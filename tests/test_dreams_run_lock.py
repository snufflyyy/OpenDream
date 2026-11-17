#!/usr/bin/env python3
"""Fixtures for the run lock and the agent wrapper's probe cleanup.

Everything here runs against fake scripts, probes, locks and /proc data only.
"""
import fcntl
import json
import os
import re
import select
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1]
PROJECT = PORT.parent
sys.path.insert(0, str(PORT / 'scripts'))
import dreams_run_lock as lock


class LaunchLockTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.port = self.root / 'port'
        (self.port / 'scripts').mkdir(parents=True)
        (self.port / 'out').mkdir()
        self.lockdir = self.root / 'shared'
        self.lockdir.mkdir()
        shutil.copy(PORT / 'run-dreams.sh', self.port)
        shutil.copy(PORT / 'scripts/dreams_run_lock.py', self.port / 'scripts')
        (self.port / 'scripts/dreams_game_check.py').write_text("import sys\nif '--serial' in sys.argv: print('FIXTURE')\n")
        (self.port / 'scripts/dreams_prepare.py').write_text('pass\n')
        (self.port / 'out/gpu-library.txt').write_text('none')
        probe = self.port / 'out/dreams-probe'
        probe.write_text('#!/usr/bin/env python3\nimport os,sys\nprint("READY",os.getpid(),flush=True)\nsys.stdin.read()\n')
        probe.chmod(0o755)
        self.children = []
        self.addCleanup(self.cleanup_children)

    def cleanup_children(self):
        for child in self.children:
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=5)

    def launch(self, role, name, **extra):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('DREAMS_', 'VCORE_', 'SHADPS4_'))}
        env.update(DREAMS_WORK=str(self.root / name), DREAMS_RUN_LOCK_DIR=str(self.lockdir),
                   DREAMS_RUN_ROLE=role, DREAMS_LOG='0', DREAMS_GAME=str(self.root / 'fake-game'), **extra)
        p = subprocess.Popen(['bash', str(self.port / 'run-dreams.sh')], env=env,
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.children.append(p)
        return p

    def line(self, p):
        self.assertTrue(select.select([p.stdout], [], [], 5)[0], "fixture did not become ready")
        return p.stdout.readline().strip()

    def ready(self, p):
        self.assertEqual(self.line(p), f'READY {p.pid}')

    def test_cross_work_exec_and_user_takeover(self):
        agent = self.launch('agent', 'one'); self.ready(agent)
        who = json.loads((self.lockdir / '.dreams-run.who').read_text())
        self.assertEqual(who['pid'], agent.pid)
        self.assertEqual(who['start_ticks'], lock.ticks(agent.pid))
        self.assertTrue(lock.owns_lock(agent.pid, who['dev'], who['ino']))
        busy = self.launch('agent', 'two'); self.assertEqual(busy.wait(timeout=5), 75)
        self.assertFalse((self.root / 'two').exists())
        user = self.launch('user', 'three'); self.ready(user)
        self.assertEqual(agent.wait(timeout=5), -15)
        self.assertEqual(json.loads((self.lockdir / '.dreams-run.who').read_text())['pid'], user.pid)
        user.stdin.close(); user.stdin = None; self.assertEqual(user.wait(timeout=5), 0)
        again = self.launch('agent', 'four'); self.ready(again)

    def test_unrelated_sentinel_stale_malformed_and_inode(self):
        agent = self.launch('agent', 'one'); self.ready(agent)
        sentinel = subprocess.Popen([sys.executable, '-c', 'import sys; print("READY",flush=True); sys.stdin.read()'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.children.append(sentinel); self.assertEqual(self.line(sentinel), 'READY')
        who = self.lockdir / '.dreams-run.who'; original = json.loads(who.read_text())
        for data in (
            dict(original, pid=sentinel.pid, start_ticks=lock.ticks(sentinel.pid)),
            dict(original, start_ticks=original['start_ticks'] + 1),
            dict(original, ino=original['ino'] + 1),
            'pid %s role agent' % sentinel.pid,
            '{broken',
        ):
            who.write_text(json.dumps(data) if isinstance(data, dict) else data)
            user = self.launch('user', 'denied'); self.assertEqual(user.wait(timeout=5), 75)
            self.assertIsNone(sentinel.poll()); self.assertIsNone(agent.poll())
        who.write_text(json.dumps(original))

    def test_before_publish_stale_sentinel_is_not_signaled(self):
        sentinel = subprocess.Popen([sys.executable, '-c', 'import sys; print("READY",flush=True); sys.stdin.read()'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.children.append(sentinel); self.assertEqual(self.line(sentinel), 'READY')
        with (self.lockdir / '.dreams-run.lock').open('w') as f:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            st = os.fstat(f.fileno())
            (self.lockdir / '.dreams-run.who').write_text(json.dumps(dict(pid=sentinel.pid, start_ticks=lock.ticks(sentinel.pid), role='agent', dev=st.st_dev, ino=st.st_ino)))
            user = self.launch('user', 'denied'); self.assertEqual(user.wait(timeout=5), 75)
            self.assertIsNone(sentinel.poll())

    def test_replaced_lock_path_is_refused(self):
        holder = self.launch('agent', 'one'); self.ready(holder)
        lockfile = self.lockdir / '.dreams-run.lock'
        # Fd 9 is bound to the inode, not the path: unlink it and leave a symlink behind.
        # A second launch would otherwise flock a fresh, unlocked inode on one GPU.
        lockfile.unlink()
        lockfile.symlink_to(self.root / 'replacement-lock')
        second = self.launch('agent', 'two')
        self.assertEqual(second.wait(timeout=5), 75)
        self.assertFalse((self.root / 'two').exists())
        self.assertIsNone(holder.poll())

    def test_fd9_without_flock_is_never_signaled(self):
        lockfile = self.lockdir / '.dreams-run.lock'
        lockfile.write_text('')
        holder = subprocess.Popen(
            [sys.executable, '-c', 'import fcntl,sys\n'
             'f=open(sys.argv[1],"r+")\n'
             'fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)\n'
             'print("READY",flush=True)\nsys.stdin.read()\n', str(lockfile)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.children.append(holder)
        self.assertEqual(self.line(holder), 'READY')
        # Holds fd 9 on the very same inode, but never flocks it: not a verified agent.
        noflock = subprocess.Popen(
            [sys.executable, '-c', 'import os,sys\n'
             'fd=os.open(sys.argv[1],os.O_RDWR)\n'
             'os.dup2(fd,9)\n'
             'if fd!=9: os.close(fd)\n'
             'print("READY",flush=True)\nsys.stdin.read()\n', str(lockfile)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.children.append(noflock)
        self.assertEqual(self.line(noflock), 'READY')
        st = os.stat(lockfile)
        (self.lockdir / '.dreams-run.who').write_text(json.dumps(dict(
            pid=noflock.pid, start_ticks=lock.ticks(noflock.pid), role='agent',
            since='fixture', dev=st.st_dev, ino=st.st_ino)))
        user = self.launch('user', 'denied')
        self.assertEqual(user.wait(timeout=10), 75)
        self.assertIsNone(noflock.poll())
        self.assertIsNone(holder.poll())

    def test_bypass_and_publication_failure_release(self):
        a = self.launch('agent', 'a', DREAMS_ALLOW_PARALLEL='1'); self.assertEqual(a.wait(timeout=5), 2)
        self.assertFalse((self.root / 'a').exists())
        (self.lockdir / '.dreams-run.who').mkdir()
        a = self.launch('agent', 'a'); self.assertEqual(a.wait(timeout=5), 75)
        self.assertFalse((self.root / 'a').exists())
        with (self.lockdir / '.dreams-run.lock').open('w') as f:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_agent_wrapper_forces_main_lock_and_no_bypass(self):
        source = (PROJECT / 'tools/agent-run.sh').read_text()
        self.assertIn('DREAMS_RUN_LOCK_DIR="$ROOT/work" DREAMS_ALLOW_PARALLEL=0', source)
        self.assertIn('${DREAMS_PORT_DIR:-$ROOT/port}/run-dreams.sh', source)

class LockPathTest(unittest.TestCase):
    def test_path_must_name_the_locked_regular_file(self):
        with tempfile.TemporaryDirectory() as d:
            lockdir = Path(d)
            lockfile = lockdir / '.dreams-run.lock'
            lockfile.write_text('')
            fd = os.open(lockfile, os.O_RDWR)
            self.addCleanup(os.close, fd)
            st = os.fstat(fd)   # keeps the inode allocated after unlink
            self.assertTrue(lock.lock_path_matches(lockdir, st.st_dev, st.st_ino))
            lockfile.unlink()
            lockfile.write_text('')
            self.assertFalse(lock.lock_path_matches(lockdir, st.st_dev, st.st_ino))
            lockfile.unlink()
            lockfile.symlink_to(lockdir / 'elsewhere')
            self.assertFalse(lock.lock_path_matches(lockdir, st.st_dev, st.st_ino))
            lockfile.unlink()
            self.assertFalse(lock.lock_path_matches(lockdir, st.st_dev, st.st_ino))


class AgentRunCleanupTest(unittest.TestCase):
    """The wrapper may SIGKILL only a probe whose --user argument is exactly this run's dir."""

    def setUp(self):
        source = (PROJECT / 'tools/agent-run.sh').read_text()
        match = re.search(r"awk -v u=\"\$private_user\" '([^']+)'", source)
        self.assertIsNotNone(match, 'agent-run.sh exact --user match not found')
        self.program = match.group(1)

    def matches(self, argv, private_user):
        # The script pipes /proc/<pid>/cmdline (NUL-separated) through tr then this awk program.
        data = '\n'.join(argv) + '\n'
        result = subprocess.run(['awk', '-v', 'u=' + private_user, self.program],
                                input=data, text=True, capture_output=True)
        return result.returncode == 0

    def test_exact_adjacent_user_argument(self):
        argv = ['dreams-probe', 'boot.bin', '--user', '/work/agent-user']
        self.assertTrue(self.matches(argv, '/work/agent-user'))
        self.assertTrue(self.matches(['--user', '/work/agent-user'], '/work/agent-user'))
        self.assertFalse(self.matches(argv, '/work/agent-user-2'))
        self.assertFalse(self.matches(['dreams-probe', '--user', '/work/agent-user-2'], '/work/agent-user'))
        self.assertFalse(self.matches(['dreams-probe', '--userx', '/work/agent-user'], '/work/agent-user'))
        self.assertFalse(self.matches(['dreams-probe', '--user'], '/work/agent-user'))
        self.assertFalse(self.matches(
            ['dreams-probe', '--content', '/work/agent-user', '--user', '/other'], '/work/agent-user'))


if __name__ == '__main__':
    unittest.main(verbosity=2)

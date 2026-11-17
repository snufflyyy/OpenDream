#!/usr/bin/env python3
"""Acquire fd 9's shared flock and safely publish/verify the launcher identity."""
import fcntl
import json
import os
from pathlib import Path
import signal
import stat
import sys
import tempfile
import time

PUBLISH_WAIT = 1.0
TERM_WAIT = 20.0
KILL_WAIT = 10.0
POLL = 0.05


def ticks(pid):
    # The parenthesized comm field can itself contain spaces and parentheses.
    text = Path(f'/proc/{pid}/stat').read_text()
    return int(text[text.rfind(')') + 2:].split()[19])


def lock_identity(fd):
    st = os.fstat(fd)
    return st.st_dev, st.st_ino


def lock_path_matches(lock_dir, dev, ino):
    """True only while the lock path still names the inode fd 9 locked (fail closed)."""
    try:
        st = os.stat(Path(lock_dir) / '.dreams-run.lock', follow_symlinks=False)
    except OSError:
        return False
    return stat.S_ISREG(st.st_mode) and (st.st_dev, st.st_ino) == (dev, ino)


def owns_lock(pid, dev, ino):
    try:
        st = os.stat(f'/proc/{pid}/fd/9')
        if (st.st_dev, st.st_ino) != (dev, ino):
            return False
        for line in Path(f'/proc/{pid}/fdinfo/9').read_text().splitlines():
            fields = line.split()
            if len(fields) >= 8 and fields[0] == 'lock:' and fields[2:5] == ['FLOCK', 'ADVISORY', 'WRITE']:
                major, minor, inode = fields[6].split(':')
                if (int(major, 16), int(minor, 16), int(inode)) == (os.major(dev), os.minor(dev), ino):
                    return True
    except (OSError, ValueError, IndexError):
        pass
    return False


def try_lock():
    try:
        fcntl.flock(9, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except BlockingIOError:
        return False


def wait_lock(seconds):
    deadline = time.monotonic() + seconds
    while True:
        if try_lock():
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(min(POLL, max(0, deadline - time.monotonic())))


def verified_agent(who, dev, ino):
    """Return pidfd + identity only after binding and verifying a live lock holder."""
    pidfd = None
    try:
        record = json.loads(who.read_text())
        pid, start = record['pid'], record['start_ticks']
        if (type(pid) is not int or pid <= 1 or type(start) is not int or
            record['role'] != 'agent' or record['dev'] != dev or record['ino'] != ino):
            return None
        pidfd = os.pidfd_open(pid, 0)
        if ticks(pid) != start or not owns_lock(pid, dev, ino) or ticks(pid) != start:
            os.close(pidfd)
            return None
        return pidfd, pid, start
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        if pidfd is not None:
            os.close(pidfd)
        return None


def publish(who, role, run_id, pid, dev, ino):
    record = dict(pid=pid, start_ticks=ticks(pid), role=role, since=run_id, dev=dev, ino=ino)
    fd, path = tempfile.mkstemp(prefix='.dreams-run.who.', dir=who.parent)
    try:
        with os.fdopen(fd, 'w') as out:
            json.dump(record, out)
            out.write('\n')
        os.replace(path, who)
    finally:
        if os.path.exists(path):
            os.unlink(path)


def acquire(lock_dir, role, run_id, pid):
    if role not in ('agent', 'user') or pid != os.getppid():
        raise ValueError('invalid launcher role or parent')
    dev, ino = lock_identity(9)
    who = Path(lock_dir) / '.dreams-run.who'
    if not try_lock():
        if role == 'agent':
            print('run-dreams.sh: another Dreams run is active', file=sys.stderr)
            return 75
        deadline = time.monotonic() + PUBLISH_WAIT
        target = None
        while time.monotonic() < deadline:
            if try_lock():
                break
            target = verified_agent(who, dev, ino)
            if target is not None:
                break
            time.sleep(POLL)
        else:
            print('run-dreams.sh: lock holder could not be verified', file=sys.stderr)
            return 75
        if target is not None:
            pidfd, holder, start = target
            try:
                if not hasattr(signal, 'pidfd_send_signal'):
                    print('run-dreams.sh: pidfd signaling unavailable', file=sys.stderr)
                    return 75
                # Recheck both identity and inherited lock immediately before signaling.
                if ticks(holder) != start or not owns_lock(holder, dev, ino):
                    return 75
                signal.pidfd_send_signal(pidfd, signal.SIGTERM)
                if not wait_lock(TERM_WAIT):
                    if ticks(holder) != start or not owns_lock(holder, dev, ino):
                        print('run-dreams.sh: holder changed or lock retained by descendant', file=sys.stderr)
                        return 75
                    signal.pidfd_send_signal(pidfd, signal.SIGKILL)
                    if not wait_lock(KILL_WAIT):
                        print('run-dreams.sh: agent lock did not release', file=sys.stderr)
                        return 75
            except (OSError, ValueError):
                # A concurrently exiting holder is harmless only if the lock is released.
                if not wait_lock(PUBLISH_WAIT):
                    print('run-dreams.sh: could not safely take over', file=sys.stderr)
                    return 75
            finally:
                os.close(pidfd)
    # The flock is bound to the inode, not the path. If the path was replaced while we
    # acquired (unlinked and recreated, or turned into a symlink/other type), a second
    # launch could flock a different inode and both would run on one GPU: fail closed.
    if not lock_path_matches(lock_dir, dev, ino):
        print('run-dreams.sh: lock file was replaced or is not a regular file', file=sys.stderr)
        return 75
    publish(who, role, run_id, pid, dev, ino)
    return 0


def main():
    try:
        return acquire(Path(sys.argv[1]), sys.argv[2], sys.argv[3], int(sys.argv[4]))
    except (OSError, ValueError, IndexError) as e:
        print(f'run-dreams.sh: lock error: {e}', file=sys.stderr)
        return 75


if __name__ == '__main__':
    sys.exit(main())

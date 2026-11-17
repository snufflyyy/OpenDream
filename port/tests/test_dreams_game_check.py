import os, shutil, struct, subprocess, sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'scripts'))
import dreams_game_check as g

class T(unittest.TestCase):
    def test_empty(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(g.check(d)[0], 2)
    def test_real(self):
        d = Path(__file__).resolve().parents[2] / 'game' / 'CUSA08010'
        if not d.is_dir():
            self.skipTest("local game dump not available")
        self.assertEqual(g.check(d)[0], 0)
    def test_region(self):
        self.assertEqual(g.region('EP9000-CUSA1_00-X'), 'EU')
def make_sfo(fields):
    keys, values, entries = bytearray(), bytearray(), bytearray()
    for key, value in fields.items():
        key_offset, value_offset = len(keys), len(values)
        keys.extend(key.encode() + b"\0")
        encoded = value.encode() + b"\0"
        values.extend(encoded)
        entries.extend(struct.pack("<HHIII", key_offset, 0x204, len(encoded), len(encoded), value_offset))
    return struct.pack("<4sIIII", b"\0PSF", 0x101, 20 + len(entries),
                       20 + len(entries) + len(keys), len(fields)) + entries + keys + values


def fixture(game, title_id="CUSA08010", title="Dreams"):
    (game / "sce_sys").mkdir(parents=True)
    (game / "sce_sys/param.sfo").write_bytes(make_sfo({"TITLE_ID": title_id, "TITLE": title, "VERSION": "99.99"}))
    for name in g.REQUIRED:
        path = game / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(g.SELF_MAGIC + b"fixture")


class GameGate(unittest.TestCase):
    def test_title_id_and_version(self):
        for tid, title, expected in [("CUSA08010", "Dreams", 0), ("CUSA12345", "Dreams", 0),
                                      ("", "Dreams", 2), ("CUSA1234x", "Dreams", 2),
                                      ("CUSA12345", "Other", 2)]:
            with self.subTest(tid=tid, title=title), tempfile.TemporaryDirectory() as d:
                fixture(Path(d), tid, title)
                self.assertEqual(g.check(d)[0], expected)

    def test_recognized_and_unsupported_magic_for_each_file(self):
        for name in g.REQUIRED:
            for magic, expected in ((g.SELF_MAGIC, 0), (g.ELF_MAGIC, 0),
                                    (b"\x7fEL", 2), (b"bad!", 2), (b"", 2)):
                with self.subTest(name=name, magic=magic), tempfile.TemporaryDirectory() as d:
                    fixture(Path(d))
                    (Path(d) / name).write_bytes(magic)
                    self.assertEqual(g.check(d)[0], expected)

    def test_recognized_magic_still_requires_detailed_parsing(self):
        from prepare import parse_self
        for magic in (g.SELF_MAGIC, g.ELF_MAGIC):
            with self.subTest(magic=magic):
                with self.assertRaises(ValueError):
                    parse_self(magic)

    def test_wrapper_gate_and_preparation_before_saves(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            port, game, work = base / "port", base / "game", base / "work"
            (port / "scripts").mkdir(parents=True)
            (port / "out").mkdir()
            shutil.copy(root / "run-dreams.sh", port)
            for name in ("dreams_game_check.py", "dreams_prepare.py", "prepare.py", "game_check.py", "dreams_run_lock.py"):
                shutil.copy(root / "scripts" / name, port / "scripts")
            # Real gate/cache, synthetic generators and probe; no runtime or game is run.
            for name in ("link_libc.py", "import_scopes.py"):
                (port / "scripts" / name).write_text("# fixture\n")
            generator = """import os, pathlib, sys
out = pathlib.Path(sys.argv[sys.argv.index('--out') + 1])
if os.environ.get('VALIDATE_FORMAT'):
    from real_prepare import parse_self
    parse_self((pathlib.Path(sys.argv[1]) / 'eboot.bin').read_bytes())
with open(os.environ['EVENTS'], 'a') as f: f.write(pathlib.Path(__file__).name + '\\n')
for name in ('boot-linked.bin', 'link.json', 'analysis.json', 'content.bin'):
    (out / name).write_text('fixture')
"""
            # prepare.py must retain the real SFO parser; append generator behavior for script execution.
            # The stand-in prepare.py imports that parser from a separately named copy.
            shutil.copy(port / "scripts/prepare.py", port / "scripts/real_prepare.py")
            (port / "scripts/prepare.py").write_text("from real_prepare import *\nif __name__ == '__main__':\n" + ''.join('    ' + line + '\n' for line in generator.splitlines()))
            for name in ("link_modules.py", "content_profile.py"):
                (port / "scripts" / name).write_text(generator)
            (port / "scripts/dreams_seed_save.py").write_text("import os\nopen(os.environ['EVENTS'], 'a').write('save\\n')\n")
            probe = port / "out/dreams-probe"
            probe.write_text("#!/bin/sh\necho probe >> \"$EVENTS\"\n")
            probe.chmod(0o755)
            (port / "out/gpu-library.txt").write_text("stub")
            fixture(game, "", "Dreams")
            events = base / "events"
            env = dict(os.environ, DREAMS_GAME=str(game), DREAMS_WORK=str(work),
                       DREAMS_USER_DIR=str(work / "private"), DREAMS_LOG="0", EVENTS=str(events))
            cmd = ["bash", str(port / "run-dreams.sh")]
            failed = subprocess.run(cmd, env=env, capture_output=True, text=True)
            self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
            self.assertFalse(events.exists())
            self.assertFalse((work / "private").exists())
            sfo_path = game / "sce_sys/param.sfo"
            sfo_path.write_bytes(b"malformed")
            failed = subprocess.run(cmd, env=env, capture_output=True, text=True)
            self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
            self.assertFalse(events.exists())
            self.assertFalse((work / "private").exists())
            sfo_path.write_bytes(make_sfo({"TITLE_ID": "CUSA08010", "TITLE": "Dreams"}))
            for name in g.REQUIRED:
                path = game / name
                path.write_bytes(b"bad!")
                failed = subprocess.run(cmd, env=env, capture_output=True, text=True)
                self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
                self.assertFalse(events.exists())
                self.assertFalse((work / "private").exists())
                path.write_bytes(g.ELF_MAGIC + b"fixture")
            # Recognized magic is only a gate: real format parsing must reject this
            # truncated ELF during preparation, before save setup.
            (game / "eboot.bin").write_bytes(g.ELF_MAGIC)
            failed = subprocess.run(cmd, env=dict(env, VALIDATE_FORMAT="1"), capture_output=True, text=True)
            self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
            self.assertFalse(events.exists())
            self.assertFalse((work / "private").exists())
            (game / "eboot.bin").write_bytes(g.ELF_MAGIC + b"fixture")
            passed = subprocess.run(cmd, env=env, capture_output=True, text=True)
            self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)
            self.assertEqual(events.read_text().splitlines(), ["prepare.py", "link_modules.py", "content_profile.py", "save", "probe"])
            cached = subprocess.run(cmd, env=env, capture_output=True, text=True)
            self.assertEqual(cached.returncode, 0, cached.stdout + cached.stderr)
            self.assertEqual(events.read_text().splitlines(), ["prepare.py", "link_modules.py", "content_profile.py", "save", "probe", "save", "probe"])

if __name__ == '__main__': unittest.main()

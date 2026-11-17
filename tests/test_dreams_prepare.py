# SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Preparation-cache contracts with tiny synthetic files and stand-in generators."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import dreams_prepare as prep


class PreparationCache(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / "game"
        self.scripts = self.root / "scripts"
        self.out = self.root / "out"
        for name in prep.GAME_INPUTS:
            path = self.game / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"input:" + name.encode())
        self.scripts.mkdir()
        for name in prep.GENERATORS:
            (self.scripts / name).write_text("# generator: " + name)
        gate = patch.object(prep, "check", return_value=(0, []))
        self.gate = gate.start()
        self.addCleanup(gate.stop)

    def generate(self, *args, **kwargs):
        self.assertTrue(kwargs["check"])
        self.assertEqual(kwargs["env"]["BB_SKIP_GAME_CHECK"], "1")
        for name in prep.OUTPUTS:
            (self.out / name).write_bytes(b"generated:" + name.encode())

    def ensure(self, **kwargs):
        return prep.ensure_prepared(self.game, self.out, self.scripts, **kwargs)

    def prime(self):
        with patch.object(prep.subprocess, "run", side_effect=self.generate) as run:
            self.assertTrue(self.ensure())
            self.assertEqual(run.call_count, 3)

    def test_unchanged_inputs_reuse_outputs(self):
        self.prime()
        with patch.object(prep.subprocess, "run") as run:
            self.assertTrue(self.ensure())
            run.assert_not_called()

    def test_all_game_inputs_invalidate_even_with_preserved_mtime(self):
        self.prime()
        for name in prep.GAME_INPUTS:
            with self.subTest(name=name):
                path = self.game / name
                before = path.stat()
                data = path.read_bytes()
                path.write_bytes(bytes([data[0] ^ 1]) + data[1:])
                os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
                with patch.object(prep.subprocess, "run", side_effect=self.generate) as run:
                    self.assertTrue(self.ensure())
                    self.assertEqual(run.call_count, 3)

    def test_all_generator_dependencies_invalidate(self):
        self.prime()
        for name in prep.GENERATORS:
            with self.subTest(name=name):
                path = self.scripts / name
                path.write_text(path.read_text() + "\n# changed")
                with patch.object(prep.subprocess, "run", side_effect=self.generate) as run:
                    self.ensure()
                    self.assertEqual(run.call_count, 3)

    def test_switching_to_identical_game_in_another_folder_invalidates(self):
        self.prime()
        import shutil
        other = self.root / "other-game"
        shutil.copytree(self.game, other)
        self.game = other
        with patch.object(prep.subprocess, "run", side_effect=self.generate) as run:
            self.ensure()
            self.assertEqual(run.call_count, 3)

    def test_missing_or_modified_outputs_invalidate(self):
        self.prime()
        for name in prep.OUTPUTS:
            with self.subTest(name=name):
                (self.out / name).unlink()
                with patch.object(prep.subprocess, "run", side_effect=self.generate) as run:
                    self.ensure()
                    self.assertEqual(run.call_count, 3)
        (self.out / prep.OUTPUTS[0]).write_bytes(b"broken")
        with patch.object(prep.subprocess, "run", side_effect=self.generate) as run:
            self.ensure()
            self.assertEqual(run.call_count, 3)

    def test_same_size_output_change_with_preserved_mtime_invalidates(self):
        self.prime()
        path = self.out / prep.OUTPUTS[0]
        stat = path.stat()
        data = path.read_bytes()
        path.write_bytes(bytes([data[0] ^ 1]) + data[1:])
        os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        with patch.object(prep.subprocess, "run", side_effect=self.generate) as run:
            self.ensure()
            self.assertEqual(run.call_count, 3)

    def test_failure_at_each_command_does_not_publish(self):
        for fail_at in range(3):
            with self.subTest(fail_at=fail_at):
                (self.out / prep.MANIFEST).unlink(missing_ok=True)
                calls = [0]
                def fail(*args, **kwargs):
                    if calls[0] == fail_at:
                        raise subprocess.CalledProcessError(1, "generator")
                    calls[0] += 1
                    self.generate(*args, **kwargs)
                with patch.object(prep.subprocess, "run", side_effect=fail):
                    with self.assertRaises(subprocess.CalledProcessError):
                        self.ensure()
                self.assertFalse((self.out / prep.MANIFEST).exists())

    def test_malformed_manifest_is_stale(self):
        self.prime()
        for text in ("{bad", "[]", "null", "{}"):
            (self.out / prep.MANIFEST).write_text(text)
            with patch.object(prep.subprocess, "run", side_effect=self.generate) as run:
                self.ensure()
                self.assertEqual(run.call_count, 3)

    def test_failed_rebuild_cannot_bless_partial_outputs(self):
        self.prime()
        path = self.game / "eboot.bin"
        original = path.read_bytes()
        path.write_bytes(b"changed")
        with patch.object(prep.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "prepare")):
            with self.assertRaises(subprocess.CalledProcessError):
                self.ensure()
        self.assertFalse((self.out / prep.MANIFEST).exists())
        path.write_bytes(original)
        with patch.object(prep.subprocess, "run", side_effect=self.generate) as run:
            self.ensure()
            self.assertEqual(run.call_count, 3)

    def test_inputs_changed_during_rebuild_are_not_recorded(self):
        def change(*args, **kwargs):
            self.generate(*args, **kwargs)
            (self.game / "eboot.bin").write_bytes(os.urandom(8))
        with patch.object(prep.subprocess, "run", side_effect=change):
            with self.assertRaisesRegex(ValueError, "inputs changed"):
                self.ensure()
        self.assertFalse((self.out / prep.MANIFEST).exists())

    def test_missing_outputs_after_success_are_not_recorded(self):
        with patch.object(prep.subprocess, "run"):
            with self.assertRaisesRegex(ValueError, "required outputs"):
                self.ensure()
        self.assertFalse((self.out / prep.MANIFEST).exists())

    def test_check_mode_never_writes_or_runs_generators(self):
        with patch.object(prep.subprocess, "run") as run:
            self.assertFalse(self.ensure(check_only=True))
            self.assertFalse(self.out.exists())
            run.assert_not_called()
        self.prime()
        with patch.object(prep.subprocess, "run") as run:
            self.assertTrue(self.ensure(check_only=True))
            run.assert_not_called()

    def test_invalid_game_fails_before_output_changes(self):
        self.gate.return_value = (2, ["FAIL fixture"])
        with patch.object(prep.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "FAIL fixture"):
                self.ensure()
            run.assert_not_called()
        self.assertFalse(self.out.exists())

    def test_launcher_validates_before_save_setup(self):
        source = (Path(__file__).resolve().parents[1] / "run-dreams.sh").read_text()
        self.assertLess(source.index('python3 -I scripts/dreams_game_check.py "$GAME"'),
                        source.index("scripts/dreams_seed_save.py"))
        self.assertIn('python3 -I scripts/dreams_prepare.py "$GAME" --out "$OUT"', source)
        self.assertNotIn("$GAME/eboot.bin -nt", source)


if __name__ == "__main__":
    unittest.main()

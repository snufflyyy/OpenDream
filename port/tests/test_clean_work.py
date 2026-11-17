# SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Cleanup safety with synthetic work trees only; never use the real work directory."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOL = Path(__file__).resolve().parents[2] / "tools" / "clean_work.py"
if TOOL.is_file():
    spec = importlib.util.spec_from_file_location("clean_work", TOOL)
    clean = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(clean)
else:
    clean = None


@unittest.skipUnless(clean, "needs project-root tools/clean_work.py")
class CleanupSafety(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.work = self.root / "work"
        self.work.mkdir()
        self.saved = self.work / "user" / "savedata"
        self.saved.mkdir(parents=True)
        (self.saved / "save.json").write_text("must survive")

    def run_cli(self, *args):
        output = io.StringIO()
        with patch.object(clean, "ROOT", str(self.root)), patch.object(clean, "WORK", str(self.work)), \
             patch.object(sys, "argv", [str(TOOL), *args]), \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            clean.main()
        return output.getvalue()

    def apply(self):
        return self.run_cli("--apply", "--keep", "0", "--older-than", "0")

    def assert_save_intact(self):
        self.assertEqual((self.saved / "save.json").read_text(), "must survive")

    def test_symlinked_category_cannot_delete_saves(self):
        (self.work / "captures").symlink_to(self.work / "user", target_is_directory=True)
        self.apply()
        self.assert_save_intact()

    def test_symlinked_category_cannot_delete_outside_work(self):
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "run-old.log").write_text("keep")
        (self.work / "logs").symlink_to(outside, target_is_directory=True)
        self.apply()
        self.assertEqual((outside / "run-old.log").read_text(), "keep")

    def test_symlinked_capture_is_preserved(self):
        (self.work / "captures").mkdir()
        link = self.work / "captures" / "old"
        link.symlink_to(self.saved, target_is_directory=True)
        self.apply()
        self.assertTrue(link.is_symlink())
        self.assert_save_intact()

    def test_dry_run_does_not_delete_ordinary_scratch(self):
        scratch = self.work / "old.log"
        scratch.write_text("disposable")
        self.run_cli("--keep", "0", "--older-than", "0")
        self.assertTrue(scratch.exists())
        self.assert_save_intact()

    def test_apply_removes_only_ordinary_scratch(self):
        scratch = self.work / "old.log"
        scratch.write_text("disposable")
        self.apply()
        self.assertFalse(scratch.exists())
        self.assert_save_intact()

    def test_category_swap_after_safe_check_cannot_delete_saves(self):
        category = self.work / "captures"
        (category / "savedata").mkdir(parents=True)
        (category / "savedata" / "dummy").write_text("dummy")
        original = clean.safe_path
        calls = 0

        def swap_after_check(path):
            nonlocal calls
            result = original(path)
            if Path(path) == category / "savedata" and result:
                calls += 1
                if calls == 2:
                    category.rename(self.work / "old-captures")
                    category.symlink_to(self.work / "user", target_is_directory=True)
            return result

        with patch.object(clean, "safe_path", side_effect=swap_after_check):
            self.apply()
        self.assertGreaterEqual(calls, 2)
        self.assert_save_intact()

    def test_category_retention_and_nested_symlink(self):
        category = self.work / "captures"
        old, new = category / "old", category / "new"
        old.mkdir(parents=True)
        new.mkdir()
        (old / "link").symlink_to(self.saved, target_is_directory=True)
        (old / "data").write_text("old")
        (new / "data").write_text("new")
        import os
        os.utime(old, (1, 1))
        os.utime(new, (2, 2))
        self.run_cli("--apply", "--keep", "1", "--older-than", "0")
        self.assertFalse(old.exists())
        self.assertTrue((new / "data").exists())
        self.assert_save_intact()

    def test_missing_safe_rmtree_support_fails_closed(self):
        scratch = self.work / "old.log"
        scratch.write_text("keep")
        with patch.object(clean.shutil.rmtree, "avoids_symlink_attacks", False):
            with self.assertRaises(SystemExit):
                self.apply()
        self.assertEqual(scratch.read_text(), "keep")

    def test_work_directory_symlink_fails_closed(self):
        alias = self.root / "alias"
        alias.symlink_to(self.work, target_is_directory=True)
        with patch.object(clean, "ROOT", str(self.root)), patch.object(clean, "WORK", str(alias)), \
             patch.object(sys, "argv", [str(TOOL), "--apply"]), \
             contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                clean.main()
        self.assert_save_intact()

    def test_invalid_retention_is_rejected(self):
        for args in (("--keep", "-1"), ("--older-than", "-1"),
                     ("--older-than", "nan"), ("--older-than", "inf")):
            with self.subTest(args=args):
                with self.assertRaises(SystemExit) as error:
                    self.run_cli(*args)
                self.assertEqual(error.exception.code, 2)
                self.assert_save_intact()


if __name__ == "__main__":
    unittest.main()

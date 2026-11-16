# SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fixtures for tools/doc_lint.py: host-independent normative docs, indexed and scanned."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

MODULE_PATH = Path(__file__).parents[1] / "tools/doc_lint.py"
spec = importlib.util.spec_from_file_location("doc_lint", MODULE_PATH)
doc_lint = importlib.util.module_from_spec(spec)
spec.loader.exec_module(doc_lint)


class DocLintTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.docs = Path(self.temp.name)

    def write_docs(self, extra=None, indexed=None):
        """Write every normative doc; EXTRA maps doc name -> body, INDEXED limits the index rows."""
        extra = extra or {}
        names = indexed if indexed is not None else doc_lint.NORMATIVE_DOCS
        rows = "\n".join(f"| `{name}` | purpose | when | normative | 2026-11-16 |" for name in names)
        (self.docs / "INDEX.md").write_text(
            "# Index\n\n| Document | Purpose | Read when | Kind | Updated |\n|---|---|---|---|---|\n"
            + rows + "\n", encoding="utf-8")
        for name in doc_lint.NORMATIVE_DOCS:
            if name == "INDEX.md":
                continue
            body = "Working directory: `$REPO`.\n" + extra.get(name, "Hardware-neutral text.\n")
            (self.docs / name).write_text(body, encoding="utf-8")

    def test_clean_tree_has_no_violations(self):
        self.write_docs()
        self.assertEqual(doc_lint.check(self.docs), [])

    def test_absolute_home_path_and_username_are_violations(self):
        self.write_docs({"BUILD.md": "Run it from /home/someone/games/port.\nBy name: "
                                     + doc_lint.HOST_USERNAME_TEXT + ".\n"})
        problems = doc_lint.check(self.docs)
        self.assertTrue(any(p.startswith("BUILD.md:2:absolute-home-path") for p in problems), problems)
        self.assertTrue(any(p.startswith("BUILD.md:3:host-username") for p in problems), problems)

    def test_checkout_written_as_home_path_is_a_violation(self):
        self.write_docs({"RUNNING.md": "See ~/Documents/Programming/Dreams-Recomp/game.\n"})
        self.assertTrue(any(p.startswith("RUNNING.md:2:absolute-home-path")
                            for p in doc_lint.check(self.docs)))

    def test_hardware_claims_fail_outside_test_hardware(self):
        self.write_docs({"BUILD.md": "Measured on an RX 7800 XT and a Ryzen 7 5700X.\n"})
        problems = doc_lint.check(self.docs)
        self.assertTrue(any(p.startswith("BUILD.md:2:host-hardware") for p in problems), problems)

    def test_hardware_doc_may_describe_the_machine(self):
        self.write_docs({doc_lint.HARDWARE_DOC: "RX 7800 XT, Fedora 44, Hyprland.\n"})
        self.assertEqual(doc_lint.check(self.docs), [])

    def test_distribution_and_compositor_fail_outside_test_hardware(self):
        self.write_docs({"RUNNING.md": "Fedora 44 packages; Hyprland users.\n"})
        problems = doc_lint.check(self.docs)
        self.assertTrue(any(p.startswith("RUNNING.md:2:host-system") for p in problems), problems)

    def test_explicit_allow_marker_and_block(self):
        self.write_docs({"BUILD.md": (
            "Fedora 44 example is a violation.\n"
            "Fedora 44 example <!-- doc-lint:allow -->\n"
            "<!-- doc-lint:allow-begin -->\n"
            "Fedora 44 and Hyprland inside the block\n"
            "<!-- doc-lint:allow-end -->\n"
            "Hyprland after the block is a violation.\n")})
        problems = doc_lint.check(self.docs)
        self.assertTrue(any(p.startswith("BUILD.md:2:host-system") for p in problems), problems)
        self.assertTrue(any(p.startswith("BUILD.md:7:host-system") for p in problems), problems)
        self.assertFalse(any(p.startswith("BUILD.md:3:") or p.startswith("BUILD.md:4:")
                             or p.startswith("BUILD.md:5:") for p in problems), problems)

    def test_missing_index_row_and_missing_doc(self):
        self.write_docs(indexed=[name for name in doc_lint.NORMATIVE_DOCS if name != "PERF.md"])
        problems = doc_lint.check(self.docs)
        self.assertTrue(any(p.startswith("PERF.md:1:index-missing") for p in problems), problems)

        (self.docs / "VCORE.md").unlink()
        problems = doc_lint.check(self.docs)
        self.assertTrue(any(p.startswith("VCORE.md:0:missing-doc") for p in problems), problems)

    def test_main_exit_codes(self):
        self.write_docs()
        original = doc_lint.DOCS
        doc_lint.DOCS = self.docs
        self.addCleanup(setattr, doc_lint, "DOCS", original)
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(doc_lint.main(), 0)
        (self.docs / "STATUS.md").write_text("Built on /home/someone/pc.\n", encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(doc_lint.main(), 2)

    def test_repository_docs_are_clean(self):
        if not (doc_lint.DOCS / "INDEX.md").is_file():
            self.skipTest(f"docs tree not found: {doc_lint.DOCS}")
        self.assertEqual(doc_lint.check(), [])


if __name__ == "__main__":
    unittest.main()

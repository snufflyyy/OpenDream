# SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""run-dreams.sh's per-game config copy (DREAMS_VCORE_CONFIG override) must not touch the template."""
import hashlib
import importlib.util
import json
import tempfile
from pathlib import Path
import unittest

PORT = Path(__file__).parents[1]
spec = importlib.util.spec_from_file_location("dreams_gpu_config", PORT / "tools/dreams_gpu_config.py")
gpu_config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpu_config)

TEMPLATE = PORT.parent / "notes/shadps4-dreams-runtime/CUSA04301.json"
TEMPLATE_SHA256 = "56a098abbc3610498214904ff4e87591ae6b82c2e3ef9b198581ec24c74c8d04"


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class GpuConfigCopyTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.out = Path(self.dir.name) / "custom_configs/CUSA08010.json"

    def test_copies_template_values_and_creates_output_dir(self):
        config = gpu_config.copy_config(TEMPLATE, self.out)
        self.assertEqual(json.loads(self.out.read_text()), config)
        self.assertEqual(config["GPU"]["readbacks_mode"], 2)  # Precise readbacks stay on
        self.assertIs(config["GPU"]["dump_shaders"], False)
        self.assertEqual(sha256(TEMPLATE), TEMPLATE_SHA256)

    def test_override_template_is_additive(self):
        override = Path(self.dir.name) / "dump.json"
        config = json.loads(TEMPLATE.read_text())
        config["GPU"]["dump_shaders"] = True
        override.write_text(json.dumps(config))
        gpu_config.copy_config(override, self.out)
        written = json.loads(self.out.read_text())
        self.assertIs(written["GPU"]["dump_shaders"], True)
        self.assertEqual(written["GPU"]["readbacks_mode"], 2)
        self.assertEqual(sha256(TEMPLATE), TEMPLATE_SHA256)

    def test_rejects_non_object_and_leaves_existing_config(self):
        self.out.parent.mkdir(parents=True)
        self.out.write_text('{"GPU": {"dump_shaders": false}}')
        for body in ["[1, 2]", '"text"', "{broken"]:
            bad = Path(self.dir.name) / "bad.json"
            bad.write_text(body)
            with self.assertRaises(ValueError):  # JSONDecodeError is a ValueError
                gpu_config.copy_config(bad, self.out)
            self.assertEqual(self.out.read_text(), '{"GPU": {"dump_shaders": false}}')
        self.assertEqual(sha256(TEMPLATE), TEMPLATE_SHA256)

    def test_main_reports_errors_without_writing(self):
        bad = Path(self.dir.name) / "bad.json"
        bad.write_text("[]")
        self.assertEqual(gpu_config.main(["tool", str(bad), str(self.out)]), 1)
        self.assertFalse(self.out.exists())
        self.assertEqual(gpu_config.main(["tool"]), 2)


if __name__ == "__main__":
    unittest.main()

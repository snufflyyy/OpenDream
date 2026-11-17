# SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("content_inventory", Path(__file__).parents[1] / "tools/dreams_content_inventory.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ContentInventoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.packs = self.root / "data/dev_levels"
        self.packs.mkdir(parents=True)
        self.metadata = [
            {"dream": {"id": "mgame", "title": "Fixture Game", "head_version": "vmap"}},
            {"version": {"id": "vmap", "dreams_in_map": [
                {"dream_version_id": {"dream_id": "dscene", "version_id": "vscene"}}],
                "blob_hashes": ["asset"], "audio_blob_hashes": ["audio", "missing"]}},
        ]
        self.content = [{"id": "mgame", "usage": "MmContentFixture"},
                        {"id": "ounknown", "usage": "OtherContent"}]

    def pack(self, name, members):
        with zipfile.ZipFile(self.packs / name, "w") as archive:
            for key, value in members.items():
                archive.writestr(key, value)

    def catalog(self):
        self.pack("dev_levels_00.zip", {"metadata.json": json.dumps(self.metadata),
                                       "content.json": json.dumps(self.content), "asset": b"fixture"})

    def test_inventory_reports_missing_assets_and_pinned_child_without_runtime_claim(self):
        self.catalog()
        self.pack("dev_levels_01.zip", {"audio": b"fixture"})
        result = module.inventory(self.root)
        game, unknown = result["entries"]
        self.assertEqual(game["title"], "Fixture Game")
        self.assertEqual(game["direct_assets"], {"declared": 3, "present": 2, "missing": ["missing"]})
        self.assertEqual(game["child_scenes"], [{"dream_id": "dscene", "version_id": "vscene"}])
        self.assertEqual(game["runtime_status"], "not_run")
        self.assertFalse(unknown["metadata_present"])
        self.assertFalse(unknown["version_present"])
        self.assertEqual(unknown["runtime_status"], "not_run")

    def test_complete_direct_payload_is_not_a_gameplay_pass(self):
        self.catalog()
        self.pack("dev_levels_01.zip", {"audio": b"fixture", "missing": b"fixture"})
        game = module.inventory(self.root)["entries"][0]
        self.assertEqual(game["direct_assets"]["missing"], [])
        self.assertEqual(game["runtime_status"], "not_run")

    def test_rejects_absent_and_ambiguous_catalogs(self):
        with self.assertRaisesRegex(ValueError, "no dev_levels"):
            module.inventory(self.root)
        self.catalog()
        self.pack("dev_levels_01.zip", {"metadata.json": "[]"})
        with self.assertRaisesRegex(ValueError, "multiple metadata"):
            module.inventory(self.root)

    def test_limits_metadata_reads(self):
        self.pack("dev_levels_00.zip", {"metadata.json": "[" + " " * 100 + "]"})
        with zipfile.ZipFile(self.packs / "dev_levels_00.zip") as archive:
            with self.assertRaisesRegex(ValueError, "exceeds inventory size limit"):
                module.read_json(archive, "metadata.json", 32)


if __name__ == "__main__":
    unittest.main()

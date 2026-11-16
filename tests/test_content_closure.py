# SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

TOOL = Path(__file__).parents[1] / "tools/dreams_content_closure.py"
spec = importlib.util.spec_from_file_location("dreams_content_closure", TOOL)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

ZERO = "0" * 32


class ContentClosureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.packs = self.root / "data/dev_levels"
        self.packs.mkdir(parents=True)
        self.metadata = [
            {"dream": {"id": "mgame", "title": "Fixture Map", "head_version": "vmap",
                       "cover_icon_hash": "cover0", "audio_preview_hash": ZERO}},
            {"dream": {"id": "dscene", "title": "Fixture Scene", "head_version": "vscene",
                       "cover_icon_hash": ZERO, "audio_preview_hash": "preview1"}},
            {"version": {
                "id": "vmap", "dream_id": "mgame", "version_type": "Bookmark",
                "dreams_in_map": [
                    {"dream_version_id": {"dream_id": "dscene", "version_id": "vscene"}}],
                "blob_hashes": ["scene0"], "icon_hash": "icon0", "background_hash": ZERO,
                "text_data_hash": "text0"}},
            {"version": {
                "id": "vscene", "dream_id": "dscene", "version_type": "Bookmark",
                "blob_hashes": ["scene1"], "audio_blob_hashes": ["audio1"],
                "icon_hash": "icon1", "background_hash": "bg1", "scribbles_hash": ZERO,
                "audio_preview_hash": ZERO,
                "tutorial_collection": {"dream_id": "", "version_id": ""},
                "default_search_collection_id": {"dream_id": "", "version_id": ""},
                "map_music_id": {"dream_id": "", "version_id": ""}}},
        ]
        self.content = [{"id": "mgame", "usage": "MmContentFixture"},
                        {"id": "mgame", "usage": "MmContentSameMap"},
                        {"id": "dscene", "usage": "MmContentScene"},
                        {"id": "other", "usage": "Unrelated"}]
        self.assets = ["scene0", "scene1", "audio1", "icon0", "icon1", "text0", "bg1",
                       "cover0", "preview1"]

    def pack(self, name, members):
        with zipfile.ZipFile(self.packs / name, "w") as archive:
            for key, value in members.items():
                archive.writestr(key, value)

    def catalog(self, extra=()):
        members = {"metadata.json": json.dumps(self.metadata),
                   "content.json": json.dumps(self.content)}
        members.update({name: b"fixture" for name in list(self.assets) + list(extra)})
        self.pack("dev_levels_00.zip", members)
        self.pack("dev_levels_01.zip", {})

    def cli(self, *args):
        return subprocess.run([sys.executable, "-I", str(TOOL), str(self.root), *args],
                              capture_output=True, text=True)

    def closure(self, dream="mgame", version="vmap"):
        return module.closure(self.root, dream, version)

    def test_resolved_map_to_scene_closure_is_not_a_runtime_claim(self):
        self.catalog()
        result = self.closure()
        self.assertEqual(result["schema"], 1)
        self.assertEqual(result["runtime_status"], "not_run")
        self.assertIn("metadata-visible closure", result["closure_scope"])
        self.assertTrue(result["unknowns"])
        self.assertIn("not a runtime pass", " ".join(result["limitations"]))
        self.assertEqual(result["root"]["content_usages"], ["MmContentFixture", "MmContentSameMap"])
        self.assertEqual([node["id"] for node in result["nodes"]["dreams"]], ["dscene", "mgame"])
        self.assertEqual([node["id"] for node in result["nodes"]["versions"]], ["vmap", "vscene"])
        self.assertEqual(result["nodes"]["dreams"][1]["versions_installed"], ["vmap"])
        self.assertEqual([(edge["field"], edge["to_version"], edge["status"])
                          for edge in result["edges"]],
                         [("dreams_in_map", "vscene", "resolved"),
                          ("tutorial_collection", "", "empty"),
                          ("default_search_collection_id", "", "empty"),
                          ("map_music_id", "", "empty")])
        assets = result["assets"]
        self.assertEqual((assets["declared"], assets["present"], assets["missing"]), (9, 9, []))
        self.assertEqual(assets["declared_references"], 9)
        self.assertEqual(assets["by_kind"]["scene_blob"],
                         {"declared": 2, "present": 2, "missing": [], "declared_references": 2})
        self.assertEqual(assets["by_kind"]["audio_blob"]["declared"], 1)
        self.assertEqual(assets["by_kind"]["special_hash"]["declared"], 6)
        self.assertEqual(assets["suffixed_variants"], [])
        run = self.cli("--root-dream", "mgame", "--root-version", "vmap")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(json.loads(run.stdout), result)

    def test_cycle_is_expanded_once(self):
        self.metadata[3]["version"]["table_of_contents"] = [
            {"dream_version_id": {"dream_id": "mgame", "version_id": "vmap"}}]
        self.catalog()
        result = self.closure()
        self.assertEqual(len(result["nodes"]["versions"]), 2)
        self.assertEqual(len(result["edges"]), 5)
        cycle = [edge for edge in result["edges"] if edge["field"] == "table_of_contents"]
        self.assertEqual([(edge["from_version"], edge["to_version"], edge["status"])
                          for edge in cycle], [("vscene", "vmap", "resolved")])

    def test_prize_edge_pulls_in_target_nodes(self):
        self.metadata.extend([
            {"dream": {"id": "dprize", "title": "Prize", "head_version": "vprize"}},
            {"version": {"id": "vprize", "dream_id": "dprize", "version_type": "Bookmark",
                         "blob_hashes": ["prize0"]}}])
        self.metadata[3]["version"]["prizes"] = [{"dream_id": "dprize", "version_id": "vprize"}]
        self.catalog(extra=["prize0"])
        result = self.closure()
        self.assertEqual(len(result["nodes"]["versions"]), 3)
        prize = [edge for edge in result["edges"] if edge["field"] == "prizes"]
        self.assertEqual([(edge["to_dream"], edge["to_version"], edge["status"])
                          for edge in prize], [("dprize", "vprize", "resolved")])
        self.assertEqual(result["assets"]["missing"], [])

    def test_missing_child_version_and_partial_ids_cross_exit_2(self):
        self.metadata[2]["version"]["dreams_in_map"] = [
            {"dream_version_id": {"dream_id": "dscene", "version_id": "vabsent"}},
            {"dream_version_id": {"dream_id": "dscene", "version_id": ""}},
            {"dream_version_id": {"dream_id": "", "version_id": "vscene"}}]
        self.catalog()
        result = self.closure()
        self.assertEqual([edge["status"] for edge in result["edges"][:3]],
                         ["missing", "missing", "missing"])
        # A partial ID pair is invalid, never an empty placeholder.
        self.assertNotIn("empty", [edge["status"] for edge in result["edges"][:3]])
        self.assertEqual(self.cli("--root-dream", "mgame", "--root-version", "vmap").returncode, 2)

    def test_version_belonging_to_another_dream_is_missing(self):
        self.metadata[2]["version"]["dreams_in_map"] = [
            {"dream_version_id": {"dream_id": "dscene", "version_id": "vmap"}}]
        self.catalog()
        result = self.closure()
        self.assertEqual(result["edges"][0]["status"], "missing")
        self.assertEqual(result["edges"][0]["to_version"], "vmap")
        self.assertEqual(self.cli("--root-dream", "mgame", "--root-version", "vmap").returncode, 2)

    def test_empty_placeholders_are_not_missing(self):
        self.catalog()
        result = self.closure()
        empties = [edge for edge in result["edges"] if edge["status"] == "empty"]
        self.assertEqual([edge["field"] for edge in empties],
                         ["tutorial_collection", "default_search_collection_id", "map_music_id"])
        self.assertEqual(result["assets"]["missing"], [])
        self.assertEqual(self.cli("--root-dream", "mgame", "--root-version", "vmap").returncode, 0)

    def test_zero_hash_is_a_sentinel_and_suffixed_member_is_only_a_variant(self):
        self.metadata[3]["version"]["blob_hashes"] = ["scene1", ZERO]
        self.metadata[3]["version"]["background_hash"] = "suffixed"
        self.assets.remove("bg1")
        self.catalog(extra=["suffixed_1"])
        result = self.closure()
        self.assertEqual(result["assets"]["missing"], ["suffixed"])
        self.assertNotIn(ZERO, json.dumps(result["assets"]))
        self.assertEqual(result["assets"]["suffixed_variants"],
                         [{"hash": "suffixed", "members": ["suffixed_1"]}])
        self.assertEqual(result["assets"]["missing_detail"][0]["field"], "background_hash")
        self.assertEqual(self.cli("--root-dream", "dscene", "--root-version", "vscene").returncode, 2)

    def test_special_hash_fields_are_checked_on_dreams_and_versions(self):
        self.metadata[0]["dream"]["cover_icon_hash"] = "coverabsent"
        self.catalog()
        result = self.closure()
        self.assertEqual(result["assets"]["missing"], ["coverabsent"])
        self.assertEqual(result["assets"]["missing_detail"],
                         [{"kind": "special_hash", "field": "cover_icon_hash",
                           "dream_id": "mgame", "version_id": "", "hash": "coverabsent"}])
        self.assertEqual(self.cli("--root-dream", "mgame", "--root-version", "vmap").returncode, 2)

    def test_asset_counts_are_unique_not_double_counted(self):
        self.metadata[0]["dream"]["cover_icon_hash"] = "dup"
        self.metadata[2]["version"]["icon_hash"] = "dup"
        self.metadata[2]["version"]["background_hash"] = "dup"
        self.assets.remove("cover0")
        self.assets.remove("icon0")
        self.catalog(extra=["dup"])
        result = self.closure()
        special = result["assets"]["by_kind"]["special_hash"]
        self.assertEqual((special["declared"], special["present"],
                          special["declared_references"]), (5, 5, 7))
        self.assertEqual(result["assets"]["declared"], 8)
        self.assertEqual(result["assets"]["missing"], [])

    def test_duplicate_dream_or_version_rows_are_ambiguous_input(self):
        self.catalog()
        self.metadata.append({"dream": {"id": "mgame", "title": "Alias", "head_version": "vmap"}})
        self.catalog()
        duplicate_dream = self.cli("--root-dream", "mgame", "--root-version", "vmap")
        self.assertEqual(duplicate_dream.returncode, 3)
        self.assertIn("duplicate dream id", duplicate_dream.stderr)
        with self.assertRaisesRegex(ValueError, "duplicate dream id"):
            self.closure()
        self.metadata.pop()
        self.catalog()
        self.metadata.append({"version": {"id": "vscene", "dream_id": "dscene"}})
        self.catalog()
        duplicate_version = self.cli("--root-dream", "mgame", "--root-version", "vmap")
        self.assertEqual(duplicate_version.returncode, 3)
        self.assertIn("duplicate version id", duplicate_version.stderr)
        # Repeated content registry entries for one dream id are legitimate aliases.
        self.metadata.pop()
        self.catalog()
        self.assertEqual(self.cli("--root-dream", "mgame", "--root-version", "vmap").returncode, 0)
        self.assertEqual(self.closure()["root"]["content_usages"],
                         ["MmContentFixture", "MmContentSameMap"])

    def test_malformed_reference_shapes_are_missing_not_ignored(self):
        cases = [
            ("dreams_in_map", [{"dream_version_id": "vchild"}]),
            ("dreams_in_map", ["vchild"]),
            ("table_of_contents", [{"note": 1}]),
            ("prizes", [{"id": "x"}]),
            ("prizes", [{"dream_id": "dscene", "version_id": None}]),
            ("tutorial_collection", "vchild"),
            ("default_search_collection_id", {"dream_id": "dscene"}),
            ("map_music_id", {"dream_id": 1, "version_id": "vscene"}),
        ]
        for field, value in cases:
            with self.subTest(field=field, value=value):
                self.metadata[2]["version"][field] = value
                self.catalog()
                result = self.closure()
                malformed = [edge for edge in result["edges"]
                             if edge["field"] == field and edge["status"] == "missing"]
                self.assertTrue(malformed, f"no missing edge for malformed {field}: {value}")
                self.assertTrue(malformed[0].get("reason"))
                self.assertNotIn("empty", [edge["status"] for edge in result["edges"]
                                           if edge["field"] == field])
                self.assertEqual(self.cli("--root-dream", "mgame",
                                          "--root-version", "vmap").returncode, 2)

    def test_malformed_reference_containers_and_metadata_rows_exit_3(self):
        self.metadata[2]["version"]["dreams_in_map"] = {
            "dream_version_id": {"dream_id": "dscene", "version_id": "vscene"}}
        self.catalog()
        container = self.cli("--root-dream", "mgame", "--root-version", "vmap")
        self.assertEqual(container.returncode, 3)
        self.assertIn("malformed reference container dreams_in_map", container.stderr)
        self.metadata[2]["version"].pop("dreams_in_map")
        self.metadata.append("garbage")
        self.catalog()
        row = self.cli("--root-dream", "mgame", "--root-version", "vmap")
        self.assertEqual(row.returncode, 3)
        self.assertIn("malformed metadata row", row.stderr)

    def test_dream_special_hash_fields_are_checked_on_dream_rows(self):
        self.metadata[0]["dream"].update(background_hash="dreambg", text_data_hash="dreamtext",
                                         scribbles_hash="dreamscribble")
        self.catalog()
        result = self.closure()
        self.assertEqual(result["assets"]["missing"],
                         ["dreambg", "dreamscribble", "dreamtext"])
        self.assertEqual({(detail["field"], detail["dream_id"], detail["version_id"])
                          for detail in result["assets"]["missing_detail"]},
                         {("background_hash", "mgame", ""),
                          ("scribbles_hash", "mgame", ""),
                          ("text_data_hash", "mgame", "")})
        self.assertEqual(self.cli("--root-dream", "mgame", "--root-version", "vmap").returncode, 2)
        self.catalog(extra=["dreambg", "dreamtext", "dreamscribble"])
        self.assertEqual(self.closure()["assets"]["missing"], [])
        self.assertEqual(self.cli("--root-dream", "mgame", "--root-version", "vmap").returncode, 0)

    def test_non_string_hash_values_are_input_errors_not_omissions(self):
        self.metadata[2]["version"]["blob_hashes"] = ["scene0", 42]
        self.catalog()
        invalid = self.cli("--root-dream", "mgame", "--root-version", "vmap")
        self.assertEqual(invalid.returncode, 3)
        self.assertIn("invalid asset hash", invalid.stderr)
        self.metadata[2]["version"]["blob_hashes"] = ["scene0", None]
        self.catalog()
        null = self.cli("--root-dream", "mgame", "--root-version", "vmap")
        self.assertEqual(null.returncode, 0, null.stderr)
        self.assertEqual(self.closure()["assets"]["declared"], 9)

    def test_absent_root_or_inconsistent_root_pair_exits_3(self):
        self.catalog()
        absent_dream = self.cli("--root-dream", "dmissing", "--root-version", "vmap")
        self.assertEqual(absent_dream.returncode, 3)
        self.assertIn("root dream not found", absent_dream.stderr)
        absent_version = self.cli("--root-dream", "mgame", "--root-version", "vabsent")
        self.assertEqual(absent_version.returncode, 3)
        wrong_pair = self.cli("--root-dream", "mgame", "--root-version", "vscene")
        self.assertEqual(wrong_pair.returncode, 3)
        self.assertIn("inconsistent root pair", wrong_pair.stderr)

    def test_absent_and_ambiguous_catalogs_exit_3(self):
        absent = self.cli("--root-dream", "mgame", "--root-version", "vmap")
        self.assertEqual(absent.returncode, 3)
        self.assertIn("no dev_levels archives", absent.stderr)
        self.catalog()
        self.pack("dev_levels_01.zip", {"metadata.json": "[]"})
        ambiguous = self.cli("--root-dream", "mgame", "--root-version", "vmap")
        self.assertEqual(ambiguous.returncode, 3)
        self.assertIn("multiple metadata catalogs", ambiguous.stderr)
        with self.assertRaisesRegex(ValueError, "multiple metadata"):
            self.closure()
        self.pack("dev_levels_01.zip", {"content.json": "[]"})
        self.assertIn("multiple content registries",
                      self.cli("--root-dream", "mgame", "--root-version", "vmap").stderr)

    def test_deterministic_json_output(self):
        self.catalog()
        first = self.root / "first.json"
        second = self.root / "second.json"
        self.assertEqual(self.cli("--root-dream", "mgame", "--root-version", "vmap",
                                  "--out", str(first)).returncode, 0)
        self.assertEqual(self.cli("--root-dream", "mgame", "--root-version", "vmap",
                                  "--out", str(second)).returncode, 0)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(json.loads(first.read_text())["schema"], 1)


if __name__ == "__main__":
    unittest.main()

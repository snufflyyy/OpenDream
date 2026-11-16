#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Inventory packaged Dreams content without extracting assets or running the game.

python3 -I tools/dreams_content_inventory.py ../game/CUSA08010 --out ../work/content-inventory.json
Catalog presence and directly declared asset presence do NOT prove playable scene coverage.
"""
import argparse
import json
from pathlib import Path
import zipfile


def read_json(archive, name, limit):
    info = archive.getinfo(name)
    if info.file_size > limit:
        raise ValueError(f"{name} exceeds inventory size limit ({limit} bytes)")
    return json.loads(archive.read(info))


def inventory(game_dir):
    packs = sorted((Path(game_dir) / "data/dev_levels").glob("dev_levels_*.zip"))
    if not packs:
        raise ValueError("no dev_levels archives found")
    members = set()
    metadata = None
    content = None
    pack_info = []
    for path in packs:
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            # Members are opaque hashes. Do not extract or interpret their binary payloads.
            members.update(names)
            pack_info.append({"name": path.name, "members": len(names)})
            if "metadata.json" in names:
                if metadata is not None:
                    raise ValueError("multiple metadata catalogs: refusing ambiguous inventory")
                metadata = read_json(archive, "metadata.json", 64 * 1024 * 1024)
            if "content.json" in names:
                if content is not None:
                    raise ValueError("multiple content registries: refusing ambiguous inventory")
                content = read_json(archive, "content.json", 4 * 1024 * 1024)
    if not isinstance(metadata, list) or not isinstance(content, list):
        raise ValueError("expected metadata and content registry arrays")
    dreams = {row["dream"]["id"]: row["dream"] for row in metadata if "dream" in row}
    versions = {row["version"]["id"]: row["version"] for row in metadata if "version" in row}
    entries = []
    for item in content:
        identifier = item["id"]
        record = dreams.get(identifier, {})
        version_id = record.get("head_version")
        version = versions.get(version_id, {})
        declared = set(version.get("blob_hashes", [])) | set(version.get("audio_blob_hashes", []))
        missing = sorted(declared - members)
        localized = record.get("content", {}).get("en-US", {})
        entries.append({
            "usage": item["usage"],
            "id": identifier,
            "title": record.get("title", localized.get("title")),
            "version": version_id,
            "metadata_present": bool(record),
            "version_present": bool(version),
            "direct_assets": {"declared": len(declared), "present": len(declared) - len(missing),
                              "missing": missing},
            "child_scenes": [child.get("dream_version_id", {})
                             for child in version.get("dreams_in_map", [])],
            "runtime_status": "not_run",
        })
    return {
        "schema": 1,
        "evidence": "local ZIP registry and metadata; no runtime observation",
        "limitations": ["Direct asset checks are not recursive dependency checks.",
                        "An empty asset list does not prove a scene is complete.",
                        "Registry entries include tools, objects, tutorials and collections, not only games.",
                        "Playable entry points and behavior require sandboxed runtime tests."],
        "packs": pack_info,
        "entries": entries,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_dir", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    result = inventory(args.game_dir)
    text = json.dumps(result, indent=2, ensure_ascii=False) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8")
        print(f"Inventoried {len(result['entries'])} registry entries -> {args.out}")
    else:
        print(text, end="")


if __name__ == "__main__":
    main()

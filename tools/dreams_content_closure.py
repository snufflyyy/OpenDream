#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Report the metadata-visible content closure of one Dream from packaged game data.

python3 -I tools/dreams_content_closure.py ../game/CUSA08010 \
    --root-dream m326284ee5d6145 --root-version vd6c7c0f6c4ad37 \
    --out ../work/content-closure-phm.json

Traverses explicit dream/version references in metadata.json, collects the assets those nodes
declare and checks their exact names in the dev_levels ZIP central directories. Blob payloads are
opaque and are never extracted or parsed: this is metadata-visible closure, not full binary
closure and not a runtime pass. Exit 0 only when every explicit non-empty reference and declared
asset resolves; 2 when one is missing; 3 for absent/ambiguous catalogs, an absent root, or a root
pair whose version does not belong to the dream.
"""
import argparse
import json
from pathlib import Path
import sys
import zipfile

# The inventory reader owns the bounded JSON read; reuse it without changing its API.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dreams_content_inventory import read_json

ZERO_HASH = "0" * 32
# The approved six special-hash fields apply to dream and version rows alike.
SPECIAL_HASH_FIELDS = ("icon_hash", "cover_icon_hash", "background_hash", "text_data_hash",
                       "scribbles_hash", "audio_preview_hash")

UNKNOWNS = [
    "References inside version.blob_hashes and version.audio_blob_hashes payloads are not "
    "inspectable from metadata.",
    "bloblist.dat is an opaque binary index and is not parsed; ZIP central-directory membership is "
    "the only asset index used.",
    "collection_icons[].icon_hash, dream.slideshow_image_hashes and tutorial_steps[].video_disk are "
    "outside the approved special-hash contract and are not validated here.",
    "A present ZIP member may still contain data this engine build cannot use; presence is not "
    "validity.",
]
LIMITATIONS = [
    "Presence is not playable-scene proof.",
    "This is metadata-visible closure, not full binary closure and not a runtime pass.",
    "Asset counts are unique hash names; one hash declared by several fields or nodes is counted "
    "once. missing_detail records each referring field.",
]


def load_catalog(game_dir):
    """Read ZIP central directories and the single metadata/content catalogs."""
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
            # Members are opaque payloads. Do not extract or interpret their bytes.
            members.update(names)
            pack_info.append({"name": path.name, "members": len(names)})
            if "metadata.json" in names:
                if metadata is not None:
                    raise ValueError("multiple metadata catalogs: refusing ambiguous closure")
                metadata = read_json(archive, "metadata.json", 64 * 1024 * 1024)
            if "content.json" in names:
                if content is not None:
                    raise ValueError("multiple content registries: refusing ambiguous closure")
                content = read_json(archive, "content.json", 4 * 1024 * 1024)
    if metadata is None:
        raise ValueError("no metadata catalog found")
    if content is None:
        raise ValueError("no content registry found")
    if not isinstance(metadata, list) or not isinstance(content, list):
        raise ValueError("expected metadata and content registry arrays")
    dreams = {}
    versions = {}
    for row in metadata:
        if not isinstance(row, dict):
            raise ValueError("malformed metadata row: expected an object")
        for kind, nodes in (("dream", dreams), ("version", versions)):
            if kind not in row:
                continue
            if not isinstance(row[kind], dict):
                raise ValueError(f"malformed {kind} metadata record: expected an object")
            identifier = row[kind].get("id")
            if not isinstance(identifier, str):
                raise ValueError(f"malformed {kind} id in metadata: expected a string")
            if identifier in nodes:
                # Duplicate rows are ambiguous input, unlike repeated content.json usages.
                raise ValueError(f"duplicate {kind} id in metadata: refusing ambiguous closure: "
                                 f"{identifier}")
            nodes[identifier] = row[kind]
    return pack_info, members, dreams, versions, content


def _pair(ref, reason):
    """Validate one dream/version pair; non-string IDs are malformed, never empty."""
    if not isinstance(ref, dict):
        return "", "", reason
    dream_id = ref.get("dream_id")
    version_id = ref.get("version_id")
    if not isinstance(dream_id, str) or not isinstance(version_id, str):
        return "", "", "reference IDs are not both strings"
    return dream_id, version_id, ""


def _reference_list(version, field):
    """Return a reference list, or raise for a malformed container (invalid input, exit 3)."""
    items = version.get(field)
    if items is None:
        return []
    if not isinstance(items, list):
        raise ValueError(f"malformed reference container {field}: expected a list")
    return items


def _version_references(version):
    """Yield (field, dream_id, version_id, reason); a non-empty reason is a malformed reference."""
    for field in ("dreams_in_map", "table_of_contents"):
        for item in _reference_list(version, field):
            if not isinstance(item, dict):
                yield field, "", "", "reference entry is not an object"
                continue
            yield field, *_pair(item.get("dream_version_id"),
                                "dream_version_id is missing or not an object")
    for item in _reference_list(version, "prizes"):
        if not isinstance(item, dict):
            yield "prizes", "", "", "reference entry is not an object"
            continue
        if "dream_id" not in item or "version_id" not in item:
            yield "prizes", "", "", "prize reference lacks dream_id/version_id"
            continue
        yield "prizes", *_pair(item, "prize reference is malformed")
    for field in ("tutorial_collection", "default_search_collection_id", "map_music_id"):
        ref = version.get(field)
        if ref is None:
            continue
        if not isinstance(ref, dict):
            yield field, "", "", "reference field is not an object"
            continue
        if "dream_id" not in ref or "version_id" not in ref:
            yield field, "", "", "reference lacks dream_id/version_id"
            continue
        yield field, *_pair(ref, "reference is malformed")


def _edge_status(dream_id, version_id, dreams, versions):
    """Classify a reference: empty placeholder, resolvable node, or explicit miss."""
    if not dream_id and not version_id:
        return "empty"  # Explicit empty slot, not a missing asset.
    if not dream_id or not version_id:
        return "missing"  # A partial ID pair is invalid, not an empty placeholder.
    version = versions.get(version_id)
    if version is None or version.get("dream_id") != dream_id:
        return "missing"  # Unknown version, or a version that belongs to another dream.
    if dream_id not in dreams:
        return "missing"  # Referenced dream has no metadata row to traverse.
    return "resolved"


def _remix_reference(dream, versions):
    """Classify dream.remix_of_version_id; None when the field is absent."""
    if "remix_of_version_id" not in dream:
        return None
    version_id = dream.get("remix_of_version_id")
    if not isinstance(version_id, str):
        return "missing", "", "", "remix_of_version_id is not a string"
    if not version_id:
        return "empty", "", "", ""
    version = versions.get(version_id)
    if version is None:
        return "missing", "", version_id, ""
    target = version.get("dream_id")
    if not isinstance(target, str) or not target:
        return "missing", "", version_id, ""
    remix_of = dream.get("remix_of")
    if "remix_of" in dream and not isinstance(remix_of, str):
        return "missing", "", version_id, "remix_of is not a string"
    if isinstance(remix_of, str) and remix_of and remix_of != target:
        return "missing", remix_of, version_id, ""  # remix_of disagrees with the version's dream.
    return "resolved", target, version_id, ""


def _declared_hash(value):
    """Return a hash that declares an asset; null, empty and all-zero 32-hex values are sentinels."""
    if value is None:
        return None
    if not isinstance(value, str):
        raise ValueError(f"invalid asset hash type: {type(value).__name__}")
    if not value:
        return None
    if len(value) == 32 and value == ZERO_HASH:
        return None
    return value


def _asset_incidences(dreams, versions, dream_ids, version_ids):
    """Yield (kind, field, dream_id, version_id, hash) for assets declared by closure nodes."""
    for version_id in sorted(version_ids):
        version = versions[version_id]
        dream_id = version.get("dream_id", "")
        for hash_value in version.get("blob_hashes") or []:
            hash_value = _declared_hash(hash_value)
            if hash_value:
                yield "scene_blob", "blob_hashes", dream_id, version_id, hash_value
        for hash_value in version.get("audio_blob_hashes") or []:
            hash_value = _declared_hash(hash_value)
            if hash_value:
                yield "audio_blob", "audio_blob_hashes", dream_id, version_id, hash_value
        for field in SPECIAL_HASH_FIELDS:
            hash_value = _declared_hash(version.get(field))
            if hash_value:
                yield "special_hash", field, dream_id, version_id, hash_value
    for dream_id in sorted(dream_ids):
        dream = dreams[dream_id]
        for field in SPECIAL_HASH_FIELDS:
            hash_value = _declared_hash(dream.get(field))
            if hash_value:
                yield "special_hash", field, dream_id, "", hash_value


def _asset_report(incidences, members):
    incidences = list(incidences)
    by_kind = {}
    all_declared = set()
    all_present = set()
    missing_detail = []
    variants = {}
    for kind, field, dream_id, version_id, hash_value in incidences:
        totals = by_kind.setdefault(kind, {"declared": set(), "present": set(), "incidences": 0})
        totals["declared"].add(hash_value)
        totals["incidences"] += 1
        all_declared.add(hash_value)
        if hash_value in members:
            totals["present"].add(hash_value)
            all_present.add(hash_value)
            continue
        missing_detail.append({"kind": kind, "field": field, "dream_id": dream_id,
                               "version_id": version_id, "hash": hash_value})
        suffixed = sorted(name for name in members if name.startswith(hash_value + "_"))
        if suffixed:
            # A suffixed member is a different asset name, never an exact-hash resolution.
            variants.setdefault(hash_value, set()).update(suffixed)
    by_kind_out = {
        kind: {"declared": len(totals["declared"]), "present": len(totals["present"]),
               "missing": sorted(totals["declared"] - totals["present"]),
               "declared_references": totals["incidences"]}
        for kind, totals in sorted(by_kind.items())
    }
    return {
        "declared": len(all_declared),
        "present": len(all_present),
        "missing": sorted(all_declared - all_present),
        "declared_references": len(incidences),
        "by_kind": by_kind_out,
        "missing_detail": missing_detail,
        "suffixed_variants": [{"hash": hash_value, "members": sorted(names)}
                              for hash_value, names in sorted(variants.items())],
    }


def closure(game_dir, root_dream, root_version):
    """Build the metadata-visible closure report; ValueError means exit status 3."""
    pack_info, members, dreams, versions, content = load_catalog(game_dir)
    if root_dream not in dreams:
        raise ValueError(f"root dream not found in metadata: {root_dream}")
    root = versions.get(root_version)
    if root is None:
        raise ValueError(f"root version not found in metadata: {root_version}")
    if root.get("dream_id") != root_dream:
        raise ValueError(f"inconsistent root pair: version {root_version} belongs to "
                         f"{root.get('dream_id')}, not {root_dream}")

    visited = set()
    visited_dreams = set()
    visited_versions = set()
    edges = []
    queue = [(root_dream, root_version)]
    remixed_dreams = set()
    while queue:
        dream_id, version_id = queue.pop(0)
        if (dream_id, version_id) in visited:
            continue  # Cycle or shared child: each node is expanded exactly once.
        visited.add((dream_id, version_id))
        visited_dreams.add(dream_id)
        visited_versions.add(version_id)
        version = versions[version_id]
        for field, to_dream, to_version, reason in _version_references(version):
            if reason:
                # A malformed explicit reference is a missing edge, never a silent success.
                edges.append({"from_version": version_id, "from_dream": dream_id, "field": field,
                              "to_dream": to_dream, "to_version": to_version,
                              "status": "missing", "reason": reason})
                continue
            status = _edge_status(to_dream, to_version, dreams, versions)
            edges.append({"from_version": version_id, "from_dream": dream_id, "field": field,
                          "to_dream": to_dream, "to_version": to_version, "status": status})
            if status == "resolved":
                queue.append((to_dream, to_version))
        if dream_id not in remixed_dreams:
            remixed_dreams.add(dream_id)
            remix = _remix_reference(dreams[dream_id], versions)
            if remix is not None:
                status, to_dream, to_version, reason = remix
                if status == "resolved":
                    queue.append((to_dream, to_version))
                edge = {"from_version": version_id, "from_dream": dream_id,
                        "field": "remix_of_version", "to_dream": to_dream,
                        "to_version": to_version, "status": status}
                if reason:
                    edge["reason"] = reason
                edges.append(edge)

    usages = sorted({item.get("usage") for item in content
                     if item.get("id") == root_dream and isinstance(item.get("usage"), str)})
    installed = {dream_id: sorted(version_id for version_id, version in versions.items()
                                  if version.get("dream_id") == dream_id)
                 for dream_id in visited_dreams}
    multi = sorted(dream_id for dream_id, ids in installed.items() if len(ids) > 1)
    dream_nodes = [{"id": dream_id, "title": dreams[dream_id].get("title"),
                    "head_version": dreams[dream_id].get("head_version"),
                    "versions_installed": installed[dream_id]}
                   for dream_id in sorted(visited_dreams)]
    version_nodes = [{"id": version_id, "dream_id": versions[version_id].get("dream_id"),
                      "version_type": versions[version_id].get("version_type")}
                     for version_id in sorted(visited_versions)]
    assets = _asset_report(_asset_incidences(dreams, versions, visited_dreams, visited_versions),
                           members)
    return {
        "schema": 1,
        "closure_scope": "metadata-visible closure: explicit JSON dream/version references and "
                         "exact ZIP member presence; blob payloads are not interpreted",
        "runtime_status": "not_run",
        "evidence": "installed dev_levels ZIP central directories plus metadata.json and "
                    "content.json; no extraction, no network",
        "root": {"dream_id": root_dream, "version_id": root_version, "content_usages": usages},
        "packs": pack_info,
        "nodes": {"dreams": dream_nodes, "versions": version_nodes},
        "edges": edges,
        "assets": assets,
        "variants": {"dreams_with_multiple_versions": multi,
                     "root_alternate_versions": [version_id for version_id
                                                 in installed[root_dream]
                                                 if version_id != root_version]},
        "unknowns": UNKNOWNS,
        "limitations": LIMITATIONS,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("game_dir", type=Path)
    parser.add_argument("--root-dream", required=True)
    parser.add_argument("--root-version", required=True)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    try:
        result = closure(args.game_dir, args.root_dream, args.root_version)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 3
    text = json.dumps(result, indent=2, ensure_ascii=False) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8")
        print(f"Closure for {args.root_dream}/{args.root_version}: "
              f"{len(result['nodes']['dreams'])} dreams, {len(result['nodes']['versions'])} "
              f"versions, {result['assets']['declared']} unique assets, "
              f"{len(result['assets']['missing'])} missing -> {args.out}")
    else:
        print(text, end="")
    missing_edges = any(edge["status"] == "missing" for edge in result["edges"])
    return 2 if missing_edges or result["assets"]["missing"] else 0


if __name__ == "__main__":
    sys.exit(main())

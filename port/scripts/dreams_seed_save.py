#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Seed a separate guest user dir for test runs, so they never write the player's own saves.

    dreams_seed_save.py SEED_DIR SOURCE_DIR SERIAL [--tutorial-skip] [--in-place-with-backup]

- SEED_DIR missing or without savedata: copies SOURCE_DIR (the player's work/user) into it.
  SOURCE_DIR is only read.
- --tutorial-skip: marks the first-run tutorial videos and cutscenes as played and every progress
  step as unlocked in SEED_DIR's SYSTEM save (savedata/1/<SERIAL>/SYSTEM/savedata.json). This is
  the same edit as the Dreams fork's SkipDreamsTutorial helper
  (notes/shadps4-dreams-runtime/SkipDreamsTutorial.cs, notes/tutorial-skip.md): three arrays,
  every false -> true, except played_tutorial_vo entries 85-87, which stay as they are. Text-level
  edit (formatting and BOM kept); refuses if an array is missing or has an unexpected length.
  Without it Dreams plays its first-time intro sequence ("The idea could be anything...") after the
  boot screens on every start. These flags also decide whether Dreams shows its first-run boot
  screens (preferences, control scheme): with them set it goes from the indreams.me card to the
  main menu (verified on CUSA08010 v2.52, docs/dreams/FPS.md).
- --in-place-with-backup: SEED_DIR may be SOURCE_DIR (the player's own dir; run-dreams.sh passes it
  only with DREAMS_TUTORIAL_SKIP_MY_SAVE=1): savedata.json is copied to
  savedata.json.before-tutorial-skip-<time> first. Without it, SEED_DIR == SOURCE_DIR is refused.
"""
import os
import re
import shutil
import sys
import time

ARRAYS = {"played_tutorial_vo": 158, "played_tutorial_cutscene": 6, "progress_unlocked": 56}
KEEP_VO = range(85, 88)


def seed(seed_dir, source_dir):
    if os.path.isdir(os.path.join(seed_dir, "savedata")):
        return False
    os.makedirs(seed_dir, exist_ok=True)
    if os.path.isdir(source_dir):
        shutil.copytree(source_dir, seed_dir, dirs_exist_ok=True, symlinks=True)
    return True


def tutorial_skip(path):
    raw = open(path, "rb").read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw[3:].decode("utf-8") if bom else raw.decode("utf-8")
    changed = 0
    for key, length in ARRAYS.items():
        pattern = re.compile(r'("%s"\s*:\s*\[)([^\]]*)(\])' % re.escape(key))
        matches = list(pattern.finditer(text))
        if len(matches) != 1:
            raise SystemExit(f"{path}: {key}: found {len(matches)} arrays, expected 1")
        body = matches[0].group(2)
        tokens = list(re.finditer(r"true|false", body))
        if len(tokens) != length:
            raise SystemExit(f"{path}: {key}: {len(tokens)} flags, expected {length}")
        out, last = [], 0
        for i, t in enumerate(tokens):
            out.append(body[last:t.start()])
            keep = key == "played_tutorial_vo" and i in KEEP_VO
            value = t.group(0) if keep else "true"
            changed += value != t.group(0)
            out.append(value)
            last = t.end()
        out.append(body[last:])
        m = matches[0]
        text = text[:m.start(2)] + "".join(out) + text[m.end(2):]
    data = text.encode("utf-8")
    open(path, "wb").write((b"\xef\xbb\xbf" if bom else b"") + data)
    return changed


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 3:
        raise SystemExit(__doc__)
    seed_dir, source_dir, serial = args
    in_place = os.path.realpath(seed_dir) == os.path.realpath(source_dir)
    if in_place and "--in-place-with-backup" not in sys.argv:
        raise SystemExit("dreams_seed_save.py: refusing to edit the source dir itself")
    if not in_place and seed(seed_dir, source_dir):
        print(f"dreams_seed_save.py: seeded {seed_dir} from {source_dir}")
    if "--tutorial-skip" in sys.argv:
        save = os.path.join(seed_dir, "savedata", "1", serial, "SYSTEM", "savedata.json")
        if not os.path.isfile(save):
            print(f"dreams_seed_save.py: no SYSTEM save in {seed_dir} yet (first boot creates it)")
            return
        if in_place:
            backup = save + time.strftime(".before-tutorial-skip-%Y%m%d-%H%M%S")
            shutil.copy2(save, backup)
            print(f"dreams_seed_save.py: backup {backup}")
        n = tutorial_skip(save)
        print(f"dreams_seed_save.py: tutorial flags set in {save} ({n} changed)")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare Dreams with a content-based cache. --check reports freshness without writing.

The caller must hold the run lock. Hash the game inputs and their Python generators,
not just mtimes: another game version can have older timestamps than the cached image.
Publish a manifest only after all three preparation commands succeed on stable inputs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

SCRIPTS = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS))
from dreams_game_check import check  # noqa: E402

GAME_INPUTS = ("eboot.bin", "sce_sys/param.sfo", "sce_module/libc.prx",
               "sce_module/libSceFios2.prx", "sce_module/libSceJobManager.prx")
GENERATORS = ("prepare.py", "link_modules.py", "link_libc.py", "content_profile.py",
              "import_scopes.py", "game_check.py", "dreams_game_check.py", "dreams_prepare.py")
OUTPUTS = ("boot-linked.bin", "link.json", "analysis.json", "content.bin")
MANIFEST = "dreams-prep.json"


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def fingerprint(game, scripts=SCRIPTS):
    game, scripts = Path(game).resolve(), Path(scripts).resolve()
    return {"schema": 1, "game": str(game),
            "inputs": {name: digest(game / name) for name in GAME_INPUTS},
            "generators": {name: digest(scripts / name) for name in GENERATORS}}


def output_stamp(out):
    try:
        stamps = {name: digest(out / name) for name in OUTPUTS
                  if (out / name).stat().st_size > 0}
        return stamps if len(stamps) == len(OUTPUTS) else None
    except OSError:
        return None


def current(out, inputs):
    try:
        saved = json.loads((out / MANIFEST).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    outputs = output_stamp(out)
    return (isinstance(saved, dict) and outputs is not None and
            saved.get("inputs") == inputs and saved.get("outputs") == outputs)


def ensure_prepared(game, out, scripts=SCRIPTS, check_only=False):
    game, out, scripts = Path(game).resolve(), Path(out), Path(scripts).resolve()
    code, lines = check(game)
    if code:
        raise ValueError("\n".join(lines))
    inputs = fingerprint(game, scripts)
    if current(out, inputs):
        return True
    if check_only:
        return False
    out.mkdir(parents=True, exist_ok=True)
    # A failed rebuild must never leave a previous manifest blessing partial output.
    (out / MANIFEST).unlink(missing_ok=True)
    env = dict(os.environ, BB_SKIP_GAME_CHECK="1")  # the Dreams check ran above
    commands = (("prepare.py",),
                ("link_modules.py", "--modules", "libc.prx", "libSceFios2.prx", "libSceJobManager.prx"),
                ("content_profile.py",))
    for script, *extra in commands:
        subprocess.run([sys.executable, str(scripts / script), str(game), "--out", str(out), *extra],
                       env=env, check=True)
    if fingerprint(game, scripts) != inputs:
        raise ValueError("preparation inputs changed during the rebuild; run again")
    outputs = output_stamp(out)
    if outputs is None:
        raise ValueError("preparation did not produce all required outputs")
    fd, temp = tempfile.mkstemp(prefix=".dreams-prep.", dir=out)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump({"inputs": inputs, "outputs": outputs}, f, sort_keys=True, indent=2)
            f.write("\n")
        os.replace(temp, out / MANIFEST)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--check", action="store_true", help="exit 1 if stale; do not rebuild")
    args = parser.parse_args()
    try:
        return 0 if ensure_prepared(args.game, args.out, check_only=args.check) else 1
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"dreams_prepare.py: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

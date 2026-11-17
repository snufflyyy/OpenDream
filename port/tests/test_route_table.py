#!/usr/bin/env python3
"""Guard: the tracked shader route table must match what the generator emits.

`ref/shadps4/src/shader_recompiler/dreams_route_table.inc` is generated from
`port/tools/dreams_routes.json` and the code hashes found in a game shader pack
(`port/tools/dreams_shader_routes.py`, docs/dreams/VERSIONS.md). Nothing else
checks it, so a stale table can silently keep a route on the wrong code hash.

This test regenerates the table from the local pack and diffs it. It skips when
the pack is absent (the game is never tracked in the repository):

    DREAMS_GAME_PACK=/path/to/CUSA08010/shaders/dd.shaders \
    SHADPS4_DIR=/path/to/shadps4 python3 -I tests/test_route_table.py
"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

PORT = Path(__file__).resolve().parent.parent
DEFAULT_PACK = PORT.parent / "game" / "CUSA08010" / "shaders" / "dd.shaders"
PACK = Path(os.environ.get("DREAMS_GAME_PACK", DEFAULT_PACK))
SHADPS4 = Path(os.environ.get("SHADPS4_DIR", PORT.parent / "ref" / "shadps4"))
TABLE = SHADPS4 / "src" / "shader_recompiler" / "dreams_route_table.inc"
GENERATOR = PORT / "tools" / "dreams_shader_routes.py"
ROUTES = PORT / "tools" / "dreams_routes.json"


class RouteTableSync(unittest.TestCase):
    def test_generated_table_matches_tracked_file(self):
        if not PACK.is_file():
            self.skipTest(f"game shader pack not found: {PACK}")
        if not TABLE.is_file():
            self.skipTest(f"tracked route table not found: {TABLE}")
        if not ROUTES.is_file():
            self.fail(f"route source of truth missing: {ROUTES}")

        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "dreams_route_table.inc"
            result = subprocess.run(
                [sys.executable, "-I", str(GENERATOR), "emit", f"2.52={PACK}", "-o", str(output)],
                cwd=PORT,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
            self.assertTrue(output.is_file(), "generator did not write the table")

            expected = TABLE.read_bytes()
            actual = output.read_bytes()
            if actual != expected:
                import difflib

                diff = difflib.unified_diff(
                    expected.decode(errors="replace").splitlines(),
                    actual.decode(errors="replace").splitlines(),
                    fromfile=str(TABLE),
                    tofile="regenerated",
                    lineterm="",
                )
                self.fail(
                    "dreams_route_table.inc is stale; run:\n"
                    f"  python3 -I tools/dreams_shader_routes.py emit 2.52={PACK} -o {TABLE}\n\n"
                    + "\n".join(list(diff)[:40])
                )


if __name__ == "__main__":
    unittest.main(verbosity=2)

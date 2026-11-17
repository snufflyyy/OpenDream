#!/usr/bin/env python3
"""Guard: run-dreams.sh disables MangoHud by default, safely.

MangoHud's Vulkan layer can leave the window on an older frame while DREAMS_CAPTURE reads the
newest rendered frame. run-dreams.sh therefore strips MangoHud from the environment unless
`DREAMS_MANGOHUD=1` asks to keep it, preserving every other `LD_PRELOAD` entry.

The test extracts the marked block (`# --- mangohud-begin` ... `# --- mangohud-end`) from
`run-dreams.sh` and evaluates it in bash with controlled environments; it does not run the game.
"""
import os
import subprocess
import unittest
from pathlib import Path

PORT = Path(__file__).resolve().parent.parent
RUN = PORT / "run-dreams.sh"
BEGIN = "# --- mangohud-begin"
END = "# --- mangohud-end"


def block() -> str:
    lines = RUN.read_text().splitlines()
    try:
        start = lines.index(BEGIN) + 1
        stop = lines.index(END)
    except ValueError as exc:  # pragma: no cover - guard against marker removal
        raise AssertionError(f"markers {BEGIN!r}/{END!r} missing from run-dreams.sh") from exc
    return "\n".join(lines[start:stop])


def evaluate(extra_env: dict[str, str]) -> dict[str, str]:
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin")}
    env.update(extra_env)
    script = block() + '\nprintf "MANGOHUD=%s\\nLD_PRELOAD=%s\\n" "${MANGOHUD-<unset>}" "${LD_PRELOAD-<unset>}"\n'
    out = subprocess.run(
        ["bash", "-c", script],
        env=env,
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert out.returncode == 0, out.stderr
    values = {}
    for line in out.stdout.splitlines():
        key, _, value = line.partition("=")
        values[key] = value
    return values


class MangoHudGate(unittest.TestCase):
    def test_strips_layer_and_keeps_other_preloads(self):
        result = evaluate({
            "MANGOHUD": "1",
            "LD_PRELOAD": "/usr/lib64/libMangoHud_shim.so:/usr/lib64/libother.so",
        })
        self.assertEqual(result["MANGOHUD"], "0")
        self.assertEqual(result["LD_PRELOAD"], "/usr/lib64/libother.so")

    def test_unsets_preload_when_only_mangohud(self):
        result = evaluate({"MANGOHUD": "1", "LD_PRELOAD": "/opt/mangohud/libMangoHud.so"})
        self.assertEqual(result["MANGOHUD"], "0")
        self.assertEqual(result["LD_PRELOAD"], "<unset>")

    def test_opt_in_keeps_mangohud(self):
        result = evaluate({
            "DREAMS_MANGOHUD": "1",
            "MANGOHUD": "1",
            "LD_PRELOAD": "/usr/lib64/libMangoHud_shim.so",
        })
        self.assertEqual(result["MANGOHUD"], "1")
        self.assertEqual(result["LD_PRELOAD"], "/usr/lib64/libMangoHud_shim.so")

    def test_no_mangohud_is_harmless(self):
        result = evaluate({"LD_PRELOAD": "/usr/lib64/libother.so"})
        self.assertEqual(result["MANGOHUD"], "0")
        self.assertEqual(result["LD_PRELOAD"], "/usr/lib64/libother.so")


if __name__ == "__main__":
    unittest.main(verbosity=2)

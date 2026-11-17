# SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only timing diagnostics must recognize both original and high-FPS code."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("fps_sample", Path(__file__).parents[1] / "tools/fps_sample.py")
sampler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sampler)


class FrameSamplerTests(unittest.TestCase):
    def test_identifies_original_and_patched_v252(self):
        for code in [bytes.fromhex("0fb605ee5ba704eb0931c0"),
                     bytes.fromhex("31c09090909090eb0931c0")]:
            def read(address, size):
                self.assertEqual((address, size), (0x800000000 + sampler.SIG_OFF, len(code)))
                return code
            self.assertEqual(sampler.find_base([0x800000000], read), 0x800000000)

    def test_rejects_unknown_code_and_unreadable_mappings(self):
        self.assertIsNone(sampler.find_base([0x1000], lambda a, n: b"\x00" * n))
        def unreadable(address, size):
            raise OSError("unmapped")
        self.assertIsNone(sampler.find_base([0x1000], unreadable))


if __name__ == "__main__":
    unittest.main()

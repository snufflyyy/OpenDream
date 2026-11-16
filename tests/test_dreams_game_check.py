import sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'scripts'))
import dreams_game_check as g

class T(unittest.TestCase):
    def test_empty(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(g.check(d)[0], 2)
    def test_real(self):
        d = Path(__file__).resolve().parents[2] / 'game' / 'CUSA08010'
        if d.is_dir(): self.assertEqual(g.check(d)[0], 0)
    def test_region(self):
        self.assertEqual(g.region('EP9000-CUSA1_00-X'), 'EU')
if __name__ == '__main__': unittest.main()

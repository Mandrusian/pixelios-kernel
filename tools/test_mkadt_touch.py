import os
import struct
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools import adt, mkadt  # noqa: E402


class SweetTouchCmu(unittest.TestCase):
    def test_cmu_props_come_from_soc_json(self):
        root = adt.parse(adt.serialize(mkadt.build(seed=b"\0" * 64, board="bluejay")))[0]
        p = next(dict((k, v) for k, _f, v in c.props) for c in root.children
                 if dict((k, v) for k, _f, v in c.props)["name"] == b"sweet-touch\0")
        self.assertEqual(p["cmu-reg"], mkadt.u64(0x10C00000, 0x10000))
        self.assertEqual(struct.unpack("<I", p["cmu-mux-usi"])[0], 0x640)
        self.assertEqual(struct.unpack("<I", p["cmu-gate-ipclk"])[0], 0x202C)
        self.assertEqual(struct.unpack("<I", p["cmu-gate-val-bit"])[0], 21)


if __name__ == "__main__":
    unittest.main()

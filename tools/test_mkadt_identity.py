import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools import adt, mkadt  # noqa: E402


@unittest.skipUnless(os.path.exists(mkadt.D22AP_ADT), 'needs research/ipsw-x/d22ap.adt (Apple-derived, extract it from the IPSW: see docs/REQUIREMENTS.md)')
class AboutIdentity(unittest.TestCase):
    def test_root_and_product_identity(self):
        root = adt.parse(adt.serialize(mkadt.build(seed=b"\0" * 64, board="bluejay")))[0]
        rp = dict((k, v) for k, _f, v in root.props)
        self.assertEqual(rp["serial-number"], b"PixOS-Project/pixelios\0")
        self.assertEqual(rp["model-number"], b"bluejay\0")
        self.assertNotIn("region-info", rp)  # absent => Model Number row is exactly the text above
        prod = next(dict((k, v) for k, _f, v in c.props) for c in root.children
                    if dict((k, v) for k, _f, v in c.props).get("name") == b"product\0")
        self.assertEqual(prod["product-description"], b"Pixel 6a\0")


if __name__ == "__main__":
    unittest.main()

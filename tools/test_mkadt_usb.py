import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools import adt, boards, mkadt  # noqa: E402


def usb_node(board, **kw):
    root = adt.parse(adt.serialize(mkadt.build(seed=b"\0" * 64, board=board, **kw)))[0]
    for c in root.children:
        props = dict((k, v) for k, _f, v in c.props)
        if props["name"] == b"sweet-usb\0":
            return props
    return None


class SweetUsbNode(unittest.TestCase):
    def test_bluejay_node_matches_board_data(self):
        p = usb_node("bluejay")
        b = boards.BOARDS["bluejay"]
        self.assertEqual(p["compatible"], b"google,gs101-dwusb3\0")
        self.assertEqual(p["reg"], mkadt.u64(b["usb_base"], b["usb_size"]))
        self.assertEqual(p["dma-pool-range"], mkadt.u64(*b["usb_dma_range"]))

    def test_sweet_and_qemu_trees_have_no_usb_node(self):
        self.assertIsNone(usb_node("sweet"))
        self.assertIsNone(usb_node("bluejay", qemu=True))


if __name__ == "__main__":
    unittest.main()

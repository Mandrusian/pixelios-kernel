import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools import adt, mkadt  # noqa: E402


def _kids(n):
    return {dict((k, v) for k, _f, v in c.props)['name']: c for c in n.children}


class WlanAdt(unittest.TestCase):
    def tree(self, **kw):
        return adt.parse(adt.serialize(mkadt.build(seed=b"\0" * 64, board="sweet", **kw)))[0]

    def test_mac_addresses_locally_administered_and_distinct(self):
        props = dict((k, v) for k, _f, v in _kids(self.tree())[b"chosen\0"].props)
        macs = [props["mac-address-" + n] for n in ("wifi0", "bluetooth0", "ethernet0")]
        self.assertEqual(len(set(macs)), 3)
        for m in macs:
            self.assertEqual(len(m), 6)
            self.assertEqual(m[0] & 3, 2)  # locally administered, unicast

    def test_wlan_node_only_when_asked(self):
        self.assertNotIn(b"wlan\0", _kids(_kids(self.tree())[b"arm-io\0"]))
        self.assertIn(b"wlan\0", _kids(_kids(self.tree(wlan_node=True))[b"arm-io\0"]))


if __name__ == "__main__":
    unittest.main()

"""Step selection of check-all.sh --quick (tools/gate_select.py)."""
import pathlib, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gate_select as g


class Select(unittest.TestCase):
    def test_docs_only(self):
        self.assertEqual(g.select(["docs/HANDOFF.md", "AGENTS.md"]), ["public-split", "validate"])

    def test_xnu_only(self):
        self.assertEqual(g.select(["xnu/patches/0092-sweet-touch-cmu-top.patch"]), ["public-split", "validate"])

    def test_xnu_patch_used_by_shim_host_tests(self):
        self.assertIn("host-tests", g.select(["xnu/patches/0088-sweet-gs101-touch.patch"]))

    def test_shim(self):
        s = g.select(["shim/ufs.c"])
        self.assertEqual(s, g.SHIM + ["public-split", "validate"])
        self.assertNotIn("unit", s)

    def test_tools_and_soc_json(self):
        self.assertEqual(g.select(["tools/qrmerge.py"]), ["unit", "phone-state", "hang-recover", "probe-run", "rootless", "public-split", "validate"])
        self.assertEqual(g.select(["soc/google/gs101/soc.json"]), g.SHIM + ["unit", "phone-state", "hang-recover", "probe-run", "rootless", "public-split", "validate"])

    def test_missing_stamp_means_full(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(g.base_from_stamp(d + "/nope"))
            p = pathlib.Path(d, "bad")
            p.write_text("deadbeef\n")
            self.assertIsNone(g.base_from_stamp(str(p)))


if __name__ == "__main__":
    unittest.main()

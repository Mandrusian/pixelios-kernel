"""The generator's checks on the soc.json "acpm" block (tools/boards.py _acpm_check): pinned constants, rail voltage windows."""
import copy
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools import boards  # noqa: E402


def acpm():
    return copy.deepcopy(boards.BOARDS["bluejay"]["acpm"])


class AcpmCheck(unittest.TestCase):
    def test_shipped_values_pass_and_flags_are_verified(self):
        a = acpm()
        boards._acpm_check(a)
        self.assertEqual((a["layout_verified"], a["elem_unit_verified"], a["elem_unit"]), (1, 1, 1))  # flipped after run acpm3 (observed)
        self.assertEqual([(r["vsel_min"], r["vsel_max"]) for r in a["rails"]], [(0x24, 0x32), (0x1C, 0x3C)])
        h = boards.c_header("bluejay")
        for line in ("#define ACPM_ELEM_UNIT 1", "#define ACPM_RAIL0_VSEL_MIN 36", "#define ACPM_RAIL1_VSEL_MAX 60",
                     "#define ACPM_INTGR1 0x40UL", "#define ACPM_INTCR0 0x24UL"):
            self.assertIn(line, h)

    def test_pinned_constants(self):
        for key, bad in (("intgr1", "0x20"), ("intcr0", "0x20"), ("intsr0", "0x28"), ("mbox_base", "0x17620000"), ("ch_id", 3),
                         ("pmic_addr", 15), ("speedy_ch", 1), ("sram_base", "0x02049000")):
            a = acpm()
            a[key] = bad
            with self.assertRaises(ValueError, msg=key):
                boards._acpm_check(a)
        a = acpm()
        a["rails"][0]["reg"] = "0x5B"
        with self.assertRaises(ValueError):
            boards._acpm_check(a)

    def test_null_window_with_a_verified_flag_fails(self):
        for flag in ("layout_verified", "elem_unit_verified"):
            for field in ("vsel_min", "vsel_max"):
                a = acpm()
                a[flag] = 1
                a["rails"][1][field] = None
                with self.assertRaises(ValueError, msg=(flag, field)):
                    boards._acpm_check(a)
        a = acpm()
        a["layout_verified"] = 1
        for r in a["rails"]:
            r["vsel_min"] = r["vsel_max"] = None
        with self.assertRaises(ValueError):
            boards._acpm_check(a)
        a = acpm()  # null windows are fine while nothing is verified: the shim then refuses (VSEL_KNOWN 0)
        a["layout_verified"] = a["elem_unit_verified"] = 0
        for r in a["rails"]:
            r["vsel_min"] = r["vsel_max"] = None
        boards._acpm_check(a)
        self.assertIn("#define ACPM_RAIL0_VSEL_KNOWN 0", "\n".join(boards._acpm_macros({"acpm": a})))

    def test_window_must_match_the_regulator_and_dt_limits(self):
        for ridx, field, bad in ((0, "vsel_min", 35), (0, "vsel_max", 51), (1, "vsel_min", 0), (1, "vsel_max", 63), (0, "vsel_max", 49)):
            a = acpm()
            a["rails"][ridx][field] = bad
            with self.assertRaises(ValueError, msg=(ridx, field, bad)):
                boards._acpm_check(a)
        a = acpm()
        a["rails"][0]["step_uv"] = 12500
        with self.assertRaises(ValueError):
            boards._acpm_check(a)
        a = acpm()
        a["rails"][0]["dt_max_uv"] = 1951000  # not on a code boundary
        with self.assertRaises(ValueError):
            boards._acpm_check(a)

    def test_elem_unit_is_bytes_or_words(self):
        for bad in (0, 2, 3, 8):
            a = acpm()
            a["elem_unit"] = bad
            with self.assertRaises(ValueError, msg=bad):
                boards._acpm_check(a)


if __name__ == "__main__":
    unittest.main()

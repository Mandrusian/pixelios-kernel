"""The generator's checks for --gpu-clk: the two GPU domains of soc.json acpm.dvfs (G3D 5 / G3DL2 6, ceiling = the firmware's init level)
and the pins in tools/boards.py (_ACPM_DVFS_DOMS, _GPU_PINS clk_khz); the CPU --dvfs requests never touch them."""
import copy
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools import boards  # noqa: E402


def acpm():
    return copy.deepcopy(boards.BOARDS["bluejay"]["acpm"])


class GpuClk(unittest.TestCase):
    def test_shipped_rows(self):
        a = acpm()
        boards._acpm_dvfs_check(a)
        d = {x["name"]: x for x in a["dvfs"]["domains"]}
        self.assertEqual((d["G3D"]["idx"], d["G3DL2"]["idx"]), (5, 6))
        self.assertEqual(d["G3D"]["rates_khz"], [848000, 762000, 701000, 572000, 510000, 471000, 400000, 351000, 302000, 251000, 202000, 151000])
        self.assertEqual(d["G3DL2"]["rates_khz"], [996000, 885000, 750000, 603000, 434000, 302000, 151000])
        self.assertEqual((d["G3D"]["ceiling_khz"], d["G3DL2"]["ceiling_khz"]), (151000, 151000))

    def test_gpu_ceiling_is_the_init_level(self):
        for name, ceil in (("G3D", 202000), ("G3DL2", 302000), ("G3D", 848000)):
            a = acpm()
            for x in a["dvfs"]["domains"]:
                if x["name"] == name:
                    x["ceiling_khz"] = ceil
            with self.assertRaises(ValueError):
                boards._acpm_dvfs_check(a)

    def test_domain_set_and_indices_are_pinned(self):
        a = acpm()
        a["dvfs"]["domains"][3]["idx"] = 7
        with self.assertRaises(ValueError):
            boards._acpm_dvfs_check(a)
        a = acpm()
        a["dvfs"]["domains"].pop()
        with self.assertRaises(ValueError):
            boards._acpm_dvfs_check(a)

    def test_pin_clk_khz(self):
        g = copy.deepcopy(boards.BOARDS["bluejay"]["gpu"])
        boards._gpu_check(g)
        g["clk_khz"] = "202000"
        with self.assertRaises(ValueError):
            boards._gpu_check(g)

    def test_macros_and_cpu_requests(self):
        h = boards.c_header("bluejay")
        for line in ("#define ACPM_DVFS_N 3", "#define ACPM_DVFS_NALL 5", "#define ACPM_DVFS_GPU_IDXS 5, 6",
                     "#define ACPM_DVFS_IDXS 2, 3, 4, 5, 6", "#define GPU_CLK_KHZ 0x24dd8UL"):
            self.assertIn(line, h)
        self.assertIn("#define ACPM_DVFS_CEILS 1803000, 1836000, 1826000, 151000, 151000", h)
        self.assertEqual(boards.dvfs_requests("bluejay", "1803000,0,0"), [1803000, 0, 0])
        with self.assertRaises(ValueError):	# the GPU clocks are not --dvfs targets: still exactly three values
            boards.dvfs_requests("bluejay", "1803000,0,0,151000,151000")


class S11(unittest.TestCase):
    """The S2MPG11 block read by --gpu-rails (soc.json acpm.s2mpg11): pinned values, a null makes the shim refuse."""

    def test_shipped_block_is_known_and_pinned(self):
        a = acpm()
        self.assertTrue(boards._acpm_s11_check(a))
        h = boards.c_header("bluejay")
        for line in ("#define ACPM_S11_KNOWN 1", "#define ACPM_S11_SPEEDY 1", "#define ACPM_S11_PMIC_ADDR 1", "#define ACPM_S11_NREG 10",
                     "#define ACPM_S11_REGS 0xc, 0x15, 0x16, 0x17, 0x25, 0x26, 0x27, 0x41, 0x42, 0x52"):
            self.assertIn(line, h)

    def test_moved_pin_fails(self):
        for key, bad in (("speedy_ch", 0), ("pmic_addr", 2), ("buck_min_uv", 250000), ("ldo1_step_uv", 6250)):
            a = acpm()
            a["s2mpg11"][key] = bad
            with self.assertRaises(ValueError):
                boards._acpm_s11_check(a)
        a = acpm()
        a["s2mpg11"]["regs"][1]["reg"] = "0x16"
        with self.assertRaises(ValueError):
            boards._acpm_s11_check(a)
        a = acpm()
        a["s2mpg11"]["regs"].pop()
        with self.assertRaises(ValueError):
            boards._acpm_s11_check(a)

    def test_null_means_unknown_and_no_macros(self):
        a = acpm()
        a["s2mpg11"]["speedy_ch"] = None
        self.assertFalse(boards._acpm_s11_check(a))
        a = acpm()
        a["s2mpg11"]["regs"][3]["reg"] = None
        self.assertFalse(boards._acpm_s11_check(a))


class GpuJob(unittest.TestCase):
    """The values shim/gpujob.c writes are derived in boards._gpu_job_values() from the kbase field layouts; they must equal the numbers of docs/research/28."""

    def test_values_match_the_design_doc(self):
        v = boards._gpu_job_values()
        self.assertEqual((v["TRANSCFG_ON"], v["TRANSCFG_OFF"]), (0x42000006, 0x42000001))
        self.assertEqual((v["MEMATTR_LO"], v["MEMATTR_HI"]), (0x888D8F88, 0x4C8D))
        self.assertEqual(v["JS_CONFIG"], 0x83B00)
        self.assertEqual((v["PTE_TABLE"], v["PTE_LEAF"]), (0x403, 0x457))
        self.assertEqual((v["HDR_W4"], v["WV_IMM64"], v["JOB_VA"], v["TARGET_VA"]), (0x10004, 7, 0x1000, 0x1100))
        self.assertEqual((v["MAGIC_HI"] << 32) | v["MAGIC_LO"], int.from_bytes(b"PIXELIOS", "big"))
        h = boards.c_header("bluejay")
        for line in ("#define GPU_JOB_TRANSCFG_ON 0x42000006u", "#define GPU_JOB_PTE_LEAF 0x457u", "#define GPU_S2MPU_G3D3 0x200b0000UL"):
            self.assertIn(line, h)

    def test_s2mpu_bases_are_pinned(self):
        g = copy.deepcopy(boards.BOARDS["bluejay"]["gpu"])
        boards._gpu_check(g)
        g["s2mpu_g3d2"] = "0x200A1000"
        with self.assertRaises(ValueError):
            boards._gpu_check(g)


if __name__ == "__main__":
    unittest.main()

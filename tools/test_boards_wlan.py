"""The WLAN_* macros of soc/google/gs101/soc.json "wlan" that the --wlan-rails / --wlan-en code relies on are pinned to the values cited in docs/research/29-wlan-stage1-design.md
(an edit of soc.json alone must not move a register that a writer touches)."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import boards  # noqa: E402

PINS = {
    # the writer (wlanen.c): gpp14 bank = PERIC0 pinctrl 0x10840000 + 0x1C0, WLAN_EN = pin 2 (DT wl_reg_on = <&gpp14 2 0x1>); gph2 = HSI2 pinctrl 0x14440000 + 0, PERST = pin 0
    "GPP14": 0x108401C0, "GPH2": 0x14440000, "GPA7": 0x174E0020, "GPP14_EN_PIN": 2, "GPH2_PERST_PIN": 0, "PMU_PCIE1_ISO": 0x17463EC4,
    # the reads (wlanrails.c): PMU+0x3C10 (rc.c:80,2692), CMU_HSI2 offsets (flexpmu_cal_system_gs101.h:355,1313-1340; rc.c:2693), S2MPG10 LDO16M..19M control registers (s2mpg10-register.h:99-103)
    "PMU_TXCO": 0x17463C10, "CMU_HSI2": 0x14400000, "CMU_OFF_PCIE_MUX": 0x620, "CMU_OFF_OSC": 0x800, "CMU_OFF_QCH_GPIO": 0x7018, "CMU_OFF_QCH_PCIE_A": 0x7060,
    "CMU_OFF_QCH_PCIE_B": 0x7064, "CMU_OFF_QCH_UFS": 0x7068, "CMU_OFF_QCH_SYSREG": 0x707C, "S10_L16M_CTRL": 0x53, "S10_L17M_CTRL": 0x54, "S10_L18M_CTRL": 0x55, "S10_L19M_CTRL": 0x56,
}


class WlanPins(unittest.TestCase):
    def test_macros_equal_the_cited_values(self):
        h = boards.c_header("bluejay")
        for k, v in PINS.items():
            self.assertIn(f"#define WLAN_{k} {v:#x}UL", h, k)

    def test_the_writer_pins_are_in_one_pinctrl_bank(self):
        self.assertLess(PINS["GPP14_EN_PIN"], 4)	# gpp14 has 4 pins (pinctrl-exynos-arm64.c:988)
        self.assertLess(PINS["GPH2_PERST_PIN"], 6)	# gph2 has 6 pins (:1016)


if __name__ == "__main__":
    unittest.main()

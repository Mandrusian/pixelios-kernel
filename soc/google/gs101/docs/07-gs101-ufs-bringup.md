# 07 — gs101 (Pixel 6a / Tensor G1) UFS host re-initialization, register-level, from Linux mainline

Desk research only; no device commands were run. All source is Linux mainline, master branch,
**commit `72a16bcff25e9f4a1d6d42a243301b1b76db2f32`** (merged 2026-10-21). All line numbers below
are at that commit. Files read (saved under `/tmp/ufsspec/`):

- `drivers/ufs/core/ufshcd.c` (11610 lines), `drivers/ufs/core/ufshcd-priv.h`, `drivers/ufs/host/ufshcd-pltfrm.c`
- `drivers/ufs/host/ufs-exynos.c` (2351 lines), `drivers/ufs/host/ufs-exynos.h`
- `drivers/phy/samsung/phy-samsung-ufs.c`, `drivers/phy/samsung/phy-samsung-ufs.h`, `drivers/phy/samsung/phy-gs101-ufs.c`
- `include/ufs/ufshci.h`, `include/ufs/unipro.h`, `include/ufs/ufs.h`, `include/ufs/ufshcd.h`, `include/uapi/scsi/scsi_bsg_ufs.h`
- `arch/arm64/boot/dts/exynos/google/gs101.dtsi`, `gs101-pixel-common.dtsi`, `drivers/clk/samsung/clk-gs101.c`, `drivers/soc/samsung/exynos-pmu.c`

Tags: **VERIFIED-FROM-SOURCE** = cited code states it directly. **INFERRED** = derived (e.g. a
masked constant expanded, or a Linux interrupt-driven wait restated as a poll with identical
condition). **UNSURE** = the mainline sources do not determine it.

## 0. Bases and derived constants used throughout

From `gs101.dtsi:1750-1787` (ufs_0 node) — VERIFIED-FROM-SOURCE:

| Name | Physical base | Size | DT reg-name | Used for |
|---|---|---|---|---|
| UFSHCI standard regs ("hci") | `0x14700000` | 0x200 | `hci` (reg index 0, used by `ufshcd_pltfrm_init` as `mmio_base`) | HCE/IS/IE/HCS/UTRLBA/UICCMD… offsets 0x00–0xA0 |
| vendor HCI ("vs_hci") | `0x14701100` | 0x200 | `vs_hci` → `ufs->reg_hci` | all `hci_readl/hci_writel()` in ufs-exynos.c |
| UniPro | `0x14780000` | 0xa000 | `unipro` → `ufs->reg_unipro` | `unipro_writel()` (COMP_CLK_PERIOD 0x44, power-mode L2 timers 0x7888–0x78C0) |
| UFS protector | `0x14600000` | 0x100 | `ufsp` → `ufs->reg_ufsp` | (unused on gs101 — `EXYNOS_UFS_OPT_UFSPR_SECURE` skips `exynos_ufs_config_smu`) |
| PHY PMA | `0x14704000` | 0x3000 | `phy-pma` | `reg_pma`; register `X` at `pma + (X << 2)` (`PHY_APB_ADDR(off) ((off) << 2)`, phy-samsung-ufs.h:27) |
| PMU syscon | `0x17460000` | 0x10000 | `samsung,pmu-syscon` | PHY isolation `0x3ec8` |
| sysreg_hsi2 | `0x14420000` | 0x10000 | `samsung,sysreg = <&sysreg_hsi2 0x710>` | IO coherency/shareability at `0x710` |
| CMU_HSI2 | `0x14400000` | 0x10000 | — | UFS clock gates |
| CMU_TOP | `0x1e080000` | 0x10000 | — | UFS clock mux/div/gate chain |

Derived mask values (VERIFIED-FROM-SOURCE components, expansion INFERRED):
- `UFSHCD_UIC_MASK = UIC_COMMAND_COMPL(0x400) | UIC_HIBERNATE_EXIT(0x20) | UIC_HIBERNATE_ENTER(0x40) | UIC_POWER_MODE(0x10) = 0x470` (ufshci.h:236-241)
- `UFSHCD_ENABLE_INTRS = UTP_TRANSFER_REQ_COMPL(0x1) | UTP_TASK_REQ_COMPL(0x200) | UFSHCD_ERROR_MASK = 0x71A85` (ufshcd.c:47-48 + ufshci.h:243-251)
- `CLK_STOP_MASK = 0x1F` (bits 0..4), `CLK_CTRL_EN_MASK = 0xF0` (bits 4..7) (ufs-exynos.c:69-84)
- `DFES_DEF_L2_ERRS = 0x2020`, `DFES_DEF_L3_ERRS = 0x7`, `DFES_DEF_L4_ERRS = 0x17`, `DFES_ERR_EN = 0x80000000` (ufs-exynos.c:86-95)
- `UFS_SW_RST_MASK = UFS_UNIPRO_SW_RST|UFS_LINK_SW_RST = 0x3` (ufs-exynos.c:47-51)
- gs101 AXIDMA WLU value `= WLU_EN | WLU_BURST_LEN(3) = 0x98000003` (ufs-exynos.c:124-126)
- `CPORT_DEF_FLAGS = CSV_N_OFF|CSD_N_OFF|E2EFC_OFF = 0x6` (unipro.h:439-442)

The gs101 driver data (VERIFIED-FROM-SOURCE, ufs-exynos.c:2278-2293):

Fields of `gs101_ufs_drvs` (type `struct exynos_ufs_drv_data`):

| Field | Value |
|---|---|
| `uic_attr` | `gs101_uic_attr` (Appendix A.5) |
| `quirks` | `UFSHCD_QUIRK_PRDT_BYTE_GRAN`, `UFSHCI_QUIRK_SKIP_RESET_INTR_AGGR`, `UFSHCI_QUIRK_BROKEN_REQ_LIST_CLR`, `UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR`, `UFSHCI_QUIRK_SKIP_MANUAL_WB_FLUSH_CTRL`, `UFSHCD_QUIRK_SKIP_DEF_UNIPRO_TIMEOUT_SETTING` (OR-ed) |
| `opts` | `EXYNOS_UFS_OPT_SKIP_CONFIG_PHY_ATTR`, `EXYNOS_UFS_OPT_UFSPR_SECURE`, `EXYNOS_UFS_OPT_TIMER_TICK_SELECT` (OR-ed) |
| `iocc_mask` | `UFS_GS101_SHARABLE` |
| `drv_init` | `gs101_ufs_drv_init` |
| `pre_link` | `gs101_ufs_pre_link` |
| `post_link` | `gs101_ufs_post_link` |
| `pre_pwr_change` | `gs101_ufs_pre_pwr_change` |
| `suspend` | `gs101_ufs_suspend` |

It is bound through the OF match entry with compatible `"google,gs101-ufs"` whose data pointer is `gs101_ufs_drvs` (ufs-exynos.c:2313-2314).

What each gs101 quirk/flag changes (all VERIFIED-FROM-SOURCE):
- `UFSHCI_QUIRK_BROKEN_HCE` is **NOT** set for gs101 → the normal HCE path (HCE 0→1 + poll) is used, **not** DME_RESET/DME_ENABLE (ufshcd.c:5201-5228).
- `UFSHCD_QUIRK_PRDT_BYTE_GRAN`: UTRD `response_upiu_offset/length` and `prd_table_offset` are programmed in **bytes**, and `prd_table_length` is byte count (`sg_entries * 16`), not dword/entry counts (ufshcd.c:4187-4204, 2733-2735).
- `UFSHCI_QUIRK_BROKEN_REQ_LIST_CLR`: `UTRLCLR`/`UTMRLCLR` polarity is inverted — to clear slot mask `m` you write `m` (bit=1 clears), where the spec says write `~m` (ufshcd.c:894-905, 910-918).
- `UFSHCI_QUIRK_SKIP_RESET_INTR_AGGR` + no `UFSHCD_CAP_INTR_AGGR` cap (gs101 `drv_init` only sets `WB_EN|CLK_GATING|HIBERN8_WITH_CLK_GATING`, ufs-exynos.c:232-236) → interrupt aggregation is *disabled*: `REG_UTP_TRANSFER_REQ_INT_AGG_CONTROL (0x4C) = 0` (ufshcd.c:5093-5096, ufshcd.h:1406-1410).
- `UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR`, `UFSHCI_QUIRK_SKIP_MANUAL_WB_FLUSH_CTRL`: error-handler/WriteBooster policy only; no effect on the bring-up sequence.
- `UFSHCD_QUIRK_SKIP_DEF_UNIPRO_TIMEOUT_SETTING`: `ufshcd_dme_change_power_mode()` does **not** write the default `DL_FC0/TC0/AFC0` timeouts before power-mode change (ufshcd.c:4918-4944) — gs101 writes its own values in `gs101_ufs_pre_pwr_change` + `exynos_ufs_pre_pwr_mode` instead.
- `EXYNOS_UFS_OPT_SKIP_CONFIG_PHY_ATTR`: `exynos_ufs_specify_phy_time_attr()`, `exynos_ufs_config_phy_time_attr()` and `exynos_ufs_config_phy_cap_attr()` are all skipped (ufs-exynos.c:662-666, 1132-1165) — the RX/TX M-PHY time-attribute tables are **not** written on gs101; the PHY is configured from the PMA register table instead.
- `EXYNOS_UFS_OPT_UFSPR_SECURE`: `exynos_ufs_config_smu()` returns immediately (ufs-exynos.c:844-848); FMP inline crypto would be enabled through SMC calls instead (`SMC_CMD_FMP_SECURITY` = SMC `0xC2011810`, `SMC_CMD_SMU` = `0xC2011850`, ufs-exynos.c:1349-1354) — not needed for plain storage.
- `EXYNOS_UFS_OPT_TIMER_TICK_SELECT`: HCI_V2P1_CTRL IA_TICK_SEL set during `exynos_ufs_fit_aggr_timeout()` (ufs-exynos.c:1160-1170).

## 1. Minimal re-init sequence (the one-page version)

Context assumed: ABL ran its UFS shutdown (`'[UFS] shutdown complete'`), so host+device are in
post-shutdown state. Linux's equivalent state transitions are `ufshcd_init()` → `ufshcd_hba_enable()`
→ `ufshcd_device_init()` (link startup) → NOP/fDeviceInit → descriptors → HS-G4 power mode, plus,
for a device that was put to SLEEP, `ufshcd_set_dev_pwr_mode(UFS_ACTIVE_PWR_MODE)` (SSU) on resume.
Steps marked **[ONLY-IF-CLOCK/PHY-OFF]** exist because Linux enables clocks/regulators before
`ufshcd_init()`; if ABL left them running they can be skipped, but see Appendix D Q1.

UIC mechanics (used by many steps) are in Appendix C. "DME_SET attr=v" means: UIC cmd 0x02,
ARG1 = `(attr << 16) | gen_sel` (gen_sel=0 for PA/DL/N/T attrs, `4+lane` for M-RX lane attrs,
`lane` for M-TX lane attrs), ARG3 = v.

1. **[ONLY-IF-CLOCK/PHY-OFF] Enable the UFS clock chain.** Gates (CMU_HSI2 @ `0x14400000`,
   set bit 21): `0x20d0` I_ACLK (parent `mout_hsi2_bus_user`), `0x20d4` I_CLK_UNIPRO
   (parent `mout_hsi2_ufs_embd_user` @ `0x0630`, from CMU_TOP `dout_cmu_hsi2_ufs_embd`
   = mux `0x10ac` → div `0x18a4` → gate `0x20cc` @ CMU_TOP `0x1e080000`), `0x20d8` I_FMP_CLK,
   `0x2090`/`0x2094` QE aclk/pclk. All are `CLK_IS_CRITICAL` in Linux.
   VERIFIED-FROM-SOURCE clk-gs101.c:3138-3150, 3071-3078, 2914-2917, 1191-1192, 1058-1060, 1377-1379.
   PHY `ref_clk` = `ext_24_5m` fixed 24.576 MHz oscclk (gs101.dtsi:1779-1787 + gs101-pixel-common.dtsi:628-630).
   Linux programs **no rate** (`freq-table-hz` is all `<0 0>`, gs101.dtsi:1764) — mclk rate is whatever
   ABL left. UNSURE what ABL leaves it at; see Appendix D Q2.

2. **[ONLY-IF-CLOCK/PHY-OFF] Un-isolate the PHY**: `PMU 0x17460000 + 0x3ec8`, mask `0x1`, set to `1`
   (`samsung_ufs_phy_ctrl_isol(phy, false)` on power-on writes `isol.en`).
   VERIFIED-FROM-SOURCE phy-samsung-ufs.h:224-229, phy-samsung-ufs.c:176-180; isol offset/mask/en from
   phy-gs101-ufs.c:11-13, 198-210. Mainline accesses PMU by plain MMIO syscon
   ("google,gs101-pmu" is in exynos-pmu.c:122-126); no SMC is used or needed for this register.
   UNSURE whether EL3 firmware on this device blocks NS access to 0x3ec8 (Appendix D Q3).

3. **IO coherency / shareability** (done once at probe by `gs101_ufs_drv_init` → `exynos_ufs_shareability`):
   `sysreg_hsi2 0x14420000 + 0x710` |= `UFS_GS101_SHARABLE (0x3)` — because the DT node is
   `dma-coherent` (`iocc_val = iocc_mask`); with bit0=RD shareable, bit1=WR shareable.
   VERIFIED-FROM-SOURCE ufs-exynos.c:216-224, 228-246, 112-117; gs101.dtsi:1761-1772.

4. **ACG disable-by-register**: `vs_hci 0x14701100 + 0x100 (HCI_IOP_ACG_DISABLE) &= ~BIT(0)`
   ("set ACG to be controlled by UFS_ACG_DISABLE").
   VERIFIED-FROM-SOURCE ufs-exynos.c:238-240, 116-118.

5. **Read controller capabilities** (before HCE in Linux: `ufshcd_hba_capabilities`):
   `hci 0x14700000 + 0x00` → `nutrs = (val & 0x1F) + 1`, `nutmrs = ((val >> 16) & 0x7) + 1`;
   `+0x08` UFS version (ABL reported 3.0 → `0x00000300`, so MCQ unsupported → legacy SDB/UTRL mode;
   `lsdb_sup = !bit29`). VERIFIED-FROM-SOURCE ufshcd.c:2501-2540.

6. **Stop the controller if it is running**: if `hci+0x34 (HCE) & 1`, write `0` to `0x34`, poll
   until bit0 == 0 (interval 10 µs, timeout 1 ms). VERIFIED-FROM-SOURCE ufshcd.c:5126-5142
   (`ufshcd_hba_stop`), 5158-5160.

7. **Vendor PRE-HCE** (`exynos_ufs_hce_enable_notify(PRE_CHANGE)`, ufs-exynos.c:1701-1725):
   a. save `vs_hci+0xB4 (HCI_MISC)`, clear `HCI_CORECLK_CTRL_EN` (bit4) in it;
   b. **host SW reset**: write `0x3 (UFS_SW_RST_MASK)` to `vs_hci+0x50 (HCI_SW_RST)`, poll until the
      register self-clears to 0 (timeout 1 ms); restore HCI_MISC
      (`exynos_ufs_host_reset`, ufs-exynos.c:1613-1635);
   c. **device HW reset**: `vs_hci+0x70 (HCI_GPIO_OUT) = 0`, wait 5 µs, `= 1`
      (`exynos_ufs_dev_hw_reset`, ufs-exynos.c:1637-1642).
   VERIFIED-FROM-SOURCE.

8. **HCE**: write `1` to `hci+0x34`; delay 1 ms (`hba_enable_delay_us` default 1000, ufshcd.c:9618-9619);
   poll `hci+0x34` bit0 == 1 (interval 1 ms, timeout 50 ms); the whole steps 6–8 are retried up to
   3 times. VERIFIED-FROM-SOURCE ufshcd.c:5153-5199; delay default `hba_enable_delay_us = 1000`
   (ufshcd.c:9607-9610).

9. **Enable UIC interrupts** (or just plan to poll IS): `hci+0x24 (IE) |= 0x470 (UFSHCD_UIC_MASK)`.
   VERIFIED-FROM-SOURCE ufshcd.c:5194, 5210; ufshci.h:236-241.

10. **Vendor POST-HCE** (`exynos_ufs_hce_enable_notify(POST_CHANGE)`): `exynos_ufs_calc_pwm_clk_div()`
    (computes only, writes nothing on gs101 because `SKIP_CONFIG_PHY_ATTR` suppresses
    `exynos_ufs_set_pwm_clk_div`); `HCI_MISC |= HCI_CORECLK_CTRL_EN (0x10)` (gs101 has no
    `BROKEN_AUTO_CLK_CTRL`). VERIFIED-FROM-SOURCE ufs-exynos.c:1726-1734, 565-573, 619-645.

11. **Pre-link, HCI part** (`exynos_ufs_pre_link`, ufs-exynos.c:1132-1158; called from
    `exynos_ufs_link_startup_notify(PRE_CHANGE)` at ufshcd.c:5346):
    a. error-intr enables (`exynos_ufs_config_intr`, ufs-exynos.c:1090-1108):
       `vs_hci+0x7C = 0x80002020` (L2), `vs_hci+0x80 = 0x80000007` (L3), `vs_hci+0x84 = 0x80000017` (L4);
    b. `exynos_ufs_setup_clocks(on=true, PRE_CHANGE)` → **ungate clocks in the HCI**:
       `vs_hci+0xB0 (HCI_CLKSTOP_CTRL) &= ~0x1F`, then `vs_hci+0xB4 (HCI_MISC) &= ~0xF0`
       (ufs-exynos.c:1111-1130, 593-603);
    c. `exynos_ufs_config_unipro` (ufs-exynos.c:1069-1089): DME_SET `PA_TXTRAILINGCLOCKS (0x1564) = 0xff`;
       DME_SET `0x956a = 0x90913C1C`; DME_SET `0x956d = 0xE01C115F` (gs101_uic_attr, ufs-exynos.c:2226-2231;
       offsets deliberately differ from TRM, ufs-exynos.h:54-58).
    VERIFIED-FROM-SOURCE. (`set_unipro_pclk_div` is skipped: gs101 lacks `HAS_APB_CLK_CTRL`.)

12. **Pre-link, gs101 vendor attributes** (`gs101_ufs_pre_link`, ufs-exynos.c:2013-2066), with
    `mclk_rate` = current `sclk_unipro_main` rate and lane counts from step 13:
    - `unipro 0x14780000 + 0x44 (COMP_CLK_PERIOD) = 16 * 1000 * 1000000 / mclk_rate`
      (`get_mclk_period_unipro_18`, ufs-exynos.c:2001-2004);
    - DME_SET `0x200 = 0x40` (enable PA test mode for the vendor MIBs);
    - for each RX lane i (i = 4 + lane, since `rx_sel_idx = PA_MAXDATALANES` — gs101 does **not**
      set `BROKEN_RX_SEL_IDX`; ufs-exynos.c:1294-1298): DME_SET `VND_RX_CLK_PRD (0x12) =
      ceil(1e9 / mclk_rate)`, `VND_RX_CLK_PRD_EN (0x11) = 0`,
      `VND_RX_LINERESET_VALUE2/1/0 (0x1B/0x1C/0x1D)` = bytes 2/1/0 of
      `(1000 [RX_LINE_RESET_TIME ns] * mclk_rate) / 1000000`, `0x2f = 0x69`, `0x84 = 0x1`, `0x25 = 0xf6`;
    - for each TX lane i (i = 0..avail-1): DME_SET `VND_TX_CLK_PRD (0xAA) = ceil(1e9 / mclk_rate)`,
      `VND_TX_CLK_PRD_EN (0xA9) = 0x02`, `VND_TX_LINERESET_PVALUE2/1/0 (0xAB/0xAC/0xAD)` = bytes of
      `(3200 [TX_LINE_RESET_TIME ns] * mclk_rate) / 1000000`, `0x04 = 1`, `0x7F = 0`;
    - DME_SET `0x200 = 0x0` (leave test mode); `PA_LOCAL_TX_LCC_ENABLE (0x155E) = 0`;
      `N_DEVICEID (0x3000) = 0`; `N_DEVICEID_VALID (0x3001) = 1`; `T_PEERDEVICEID (0x4021) = 1`;
      `T_CONNECTIONSTATE (0x4020) = CPORT_CONNECTED (1)`; DME_SET `0xA006 = 0x8000`.
    VERIFIED-FROM-SOURCE (constants ufs-exynos.h:112-116, 92-95).

13. **PHY bring-up** (`exynos_ufs_phy_init`, ufs-exynos.c:1027-1067):
    a. DME_GET `PA_AVAILRXDATALANES (0x1540)` → avail_ln_rx, DME_GET `PA_AVAILTXDATALANES (0x1520)`
       → avail_ln_tx (2 on this device);
    b. if PHY was powered: power_off + exit (PMU isol → 0);
    c. `phy_init` (lane_cnt = avail_ln_rx, state = CFG_PRE_INIT) then `phy_power_on`:
       PMU `0x3ec8` bit0 ← 1 (again), enable `ref_clk`, then run the `tensor_gs101_pre_init_cfg`
       table (Appendix A.1): COMN regs at `pma + (off<<2)`, TRSV lane0 at `pma + (off<<2)`,
       lane1 at `pma + ((off + 0x200) << 2)`; **then per lane poll** `TRSV_REG338 (0x338)` bit3
       `LN0_MON_RX_CAL_DONE` (addr `pma + ((0x338 + lane*0x200) << 2)`), 40 µs sleep / 40 ms timeout.
    VERIFIED-FROM-SOURCE ufs-exynos.c:1027-1067; phy-samsung-ufs.c:176-198, 84-117;
    phy-gs101-ufs.c:30-96, 148-170.

14. **DME_LINKSTARTUP**: UIC command `0x16`, no args. Up to 3 attempts; on UIC failure with retries
    left, Linux first re-runs `ufshcd_hba_enable()` (steps 6–10) and retries. After success check
    `hci+0x30 (HCS) & 1 (DEVICE_PRESENT)` — if 0 → fail.
    VERIFIED-FROM-SOURCE ufshcd.c:5330-5381, 4218-4232.
    Note: Linux sends DME_LINKSTARTUP a **second time** when the device was known not-active
    (`link_startup_again`, ufshcd.c:5335-5340, 5370-5374) — the resume-after-sleep analog. For the
    shim, if the first startup succeeds and HCS.DP=1, one pass matches what Linux does on a device
    it believes active; if the UIC returns an error or DP=0, repeat the whole loop once. INFERRED
    (which branch our state maps to is not in mainline).

15. **Link-up bookkeeping** (no register writes): pwr_info = PWM-G1, 1 lane, SLOW-AUTO
    (`ufshcd_init_pwr_info`, ufshcd.c:4763-4772). Validate lanes: DME_GET
    `PA_CONNECTEDTXDATALANES (0x1561)` and `PA_CONNECTEDRXDATALANES (0x1581)` must both equal 2
    (`lanes_per_direction` default 2; gs101 DT has no override).
    VERIFIED-FROM-SOURCE ufshcd.c:5287-5328, ufshcd-pltfrm.c:203-213.

16. **Post-link** (`exynos_ufs_post_link`, ufs-exynos.c:1175-1243):
    a. `exynos_ufs_establish_connt` (ufs-exynos.c:821-841): DME_SET `T_CONNECTIONSTATE = 0 (CPORT_IDLE)`,
       `N_DEVICEID = 0`, `N_DEVICEID_VALID = 1`, `T_PEERDEVICEID = 1`, `T_PEERCPORTID (0x4022) = 0`,
       `T_CPORTFLAGS (0x4025) = 0x6`, `T_TRAFFICCLASS (0x4023) = 0`, `T_CONNECTIONSTATE = 1`;
    b. `exynos_ufs_fit_aggr_timeout`: `vs_hci+0x8C (HCI_V2P1_CTRL) |= BIT(16) (IA_TICK_SEL)`;
       `vs_hci+0x0C (HCI_1US_TO_CNT_VAL) = exynos_ufs_calc_time_cntr(20000/40 = 500 ns) & 0x3FF`
       (`calc_time_cntr(period) = (period*10)/((pclk_period*10)+fraction)` with
       `pclk_period = 1e9/pclk_rate`; ufs-exynos.c:646-657, 1160-1170);
    c. `vs_hci+0x60 (HCI_DATA_REORDER) = 0xa`; `vs_hci+0x00 (HCI_TXPRDT_ENTRY_SIZE) = 12`
       (`ilog2(4096)`, no crypto prefetch); `vs_hci+0x04 (HCI_RXPRDT_ENTRY_SIZE) = 12`;
       `vs_hci+0x40 (HCI_UTRL_NEXUS_TYPE) = (1 << nutrs) - 1`;
       `vs_hci+0x44 (HCI_UTMRL_NEXUS_TYPE) = (1 << nutmrs) - 1`; `vs_hci+0x6C = 0xf` (momentarily);
    d. PA timing via debug mode (attrs filled by `exynos_ufs_parse_dt`, ufs-exynos.c:1285-1289):
       DME_SET `PA_DBG_MODE (0x9529) = 1`; DME_SET `PA_GRANULARITY (0x15AA) = 6`;
       DME_SET `PA_DBG_MODE = 0`; DME_SET `PA_TACTIVATE (0x15A8) = 3`;
       DME_SET `PA_HIBERN8TIME (0x15A7) = 0x20` (gs101 has no `USE_SW_HIBERN8_TIMER`);
    e. `phy_calibrate` at state CFG_POST_INIT → NULL table on gs101 (no writes; state advances);
    f. `gs101_ufs_post_link` (ufs-exynos.c:2069-2083):
       `vs_hci+0x6C (HCI_AXIDMA_RWDATA_BURST_LEN) = 0x98000003 (WLU_EN | WLU_BURST_LEN(3))`;
       DME_SET `PA_DBG_MODE = 1`; DME_SET `PA_SAVECONFIGTIME (0x15A4) = 0x3e8`;
       DME_SET `PA_DBG_MODE = 0`.
    VERIFIED-FROM-SOURCE.

17. **Clear UECPA**: read `hci+0x38` and discard — exactly the `0x80000010`
    (LINERESET-during-LINK_STARTUP) value the shim observed.
    VERIFIED-FROM-SOURCE ufshcd.c:5405-5408 ("Clear UECPA once due to LINERESET has happened
    during LINK_STARTUP").

18. **Make the host operational** (`ufshcd_make_hba_operational`, ufshcd.c:5084-5122):
    a. `hci+0x24 (IE) |= 0x71A85 (UFSHCD_ENABLE_INTRS)`;
    b. interrupt aggregation off: `hci+0x4C = 0` (gs101: no INTR_AGGR cap);
    c. program list bases: `hci+0x50/0x54 (UTRLBA L/H) = UTRDL phys` (1 KB-aligned, nutrs × 32 B),
       `hci+0x70/0x74 (UTMRLBA L/H) = UTMRDL phys`;
    d. read `hci+0x30 (HCS)`; require `(reg & 0xE) == 0xE` (UTRLRDY|UTMRLRDY|UCRDY);
    e. `hci+0x60 (UTRLRSR) = 1`, `hci+0x80 (UTMRLRSR) = 1`.
    VERIFIED-FROM-SOURCE. UTRDL/UCD layout with the gs101 byte-granularity quirk is in Appendix B.4.

19. **NOP OUT — verify the device UTP layer** (`ufshcd_verify_dev_init`, ufshcd.c:5428-5452):
    build UTRD+UPIU per Appendix B.1 (tag 0, no data, OCS preset `0x0F`), ring doorbell
    (`hci+0x58 |= (1 << tag)`), wait for completion (poll doorbell bit → 0 and/or IS bit0),
    require OCS=0 and response transaction code `0x20 (NOP IN)`; up to 10 retries, 50 ms timeout each.
    VERIFIED-FROM-SOURCE (timeouts ufshcd.c:56-57, 11039).

20. **fDeviceInit** (`ufshcd_complete_dev_init`, ufshcd.c:5033-5070): QUERY SET FLAG
    (opcode `0x06`, idn `0x01` fDeviceInit, index 0, selector 0, query function `0x81`), then poll
    QUERY READ FLAG (opcode `0x05`, function `0x01`) until the flag reads 0, sleeping 0.5–1 ms,
    total timeout 1500 ms. Each query: timeout 1500 ms, 3 retries (`QUERY_REQ_RETRIES`,
    `QUERY_REQ_TIMEOUT_DEFAULT`, ufshcd.c:60-66, 104).
    VERIFIED-FROM-SOURCE.

21. **Read descriptors** (needed to learn LUNs/geometry; `ufshcd_device_params_init`,
    ufshcd.c:9185-9227): QUERY READ DESCRIPTOR (opcode `0x01`) of
    Geometry (idn `0x07`) and Device (idn `0x00`), index 0, length 255 (buffer clamps to actual),
    descriptor bytes land at `response UCD + 32`; then Unit descriptors (idn `0x02`, index = LUN)
    as needed. Optionally read String descriptors. bMaxNumOfRTT fix-up applies only to UFS ≥ 4.0
    (`ufshcd_set_rtt`, ufshcd.c:8660-8690) — skip on 3.1. VERIFIED-FROM-SOURCE.

22. **Switch to HS Gear 4, Series B, 2 lanes** (`ufshcd_get_max_pwr_mode` → `exynos_ufs_negotiate_pwr_mode`
    → `ufshcd_config_pwr_mode` → `ufshcd_dme_change_power_mode`):
    a. read device caps: DME_GET `PA_CONNECTEDRX/TXDATALANES`, `PA_MAXRXHSGEAR (0x1587)` (local RX)
       and DME_PEER_GET `PA_MAXRXHSGEAR` (device's max = host TX cap) (ufshcd.c:4780-4855);
    b. host limit from `exynos_ufs_get_hs_gear`: controller major version ≥ 3 (ABL: "3.0") →
       `UFS_HS_G4` (ufs-exynos.c:900-907); host params default Series B, FAST, 2 lanes
       (ufshcd-pltfrm.c:622-637); agreed = min(dev,host) → gear 4, lanes 2, FAST/FAST, rate B;
    c. **pre-change hooks** (`exynos_ufs_pre_pwr_mode`, ufs-exynos.c:913-948):
       `exynos_ufs_config_sync_pattern_mask` does nothing for gear 4 (only g1–3 handled,
       ufs-exynos.c:864-891); `phy_calibrate` at CFG_PRE_PWR_HS writes
       `tensor_gs101_pre_pwr_hs_config` (Appendix A.2); then DME_SET
       `DL_FC0PROTTIMEOUTVAL (0x2041) = 8064`, `DL_TC0REPLAYTIMEOUTVAL (0x2042) = 28224`,
       `DL_AFC0REQTIMEOUTVAL (0x2043) = 20160`;
    d. `gs101_ufs_pre_pwr_change` (ufs-exynos.c:2086-2101): DME_SET
       `PA_PWRMODEUSERDATA0 (0x15B0) = 12000`, `PA_PWRMODEUSERDATA1 (0x15B1) = 32000`,
       `PA_PWRMODEUSERDATA2 (0x15B2) = 16000`; `unipro 0x7888 = 8064`, `0x788C = 28224`,
       `0x7890 = 20160`, `0x78B8 = 12000`, `0x78BC = 32000`, `0x78C0 = 16000`
       (`UNIPRO_DME_POWERMODE_REQ_LOCALL2TIMER0/1/2`, `REMOTEL2TIMER0/1/2`);
    e. mode attributes (`ufshcd_dme_change_power_mode`, ufshcd.c:4873-4955; default UniPro timeout
       writes skipped by quirk): DME_SET `PA_RXGEAR (0x1583) = 4`,
       `PA_ACTIVERXDATALANES (0x1580) = 2`, `PA_RXTERMINATION (0x1584) = 1`,
       `PA_TXGEAR (0x1568) = 4`, `PA_ACTIVETXDATALANES (0x1560) = 2`,
       `PA_TXTERMINATION (0x1569) = 1`, `PA_HSSERIES (0x156A) = 2 (Series B)`;
    f. **the mode change itself** (`ufshcd_uic_change_pwr_mode` → `ufshcd_uic_pwr_ctrl`):
       DME_SET `PA_PWRMODE (0x1571) = (FAST << 4) | FAST = 0x11`, and wait for **IS bit4
       (UIC_POWER_MODE, "UPMS")** — not just UCCS — then require `HCS.UPMCRS (bits 10:8) == 1
       (PWR_LOCAL)`; full polled routine in Appendix C.2. VERIFIED-FROM-SOURCE ufshcd.c:4491-4560, 4622-4651;
    g. **post-change hooks** (`exynos_ufs_post_pwr_mode` → `phy_calibrate` at CFG_POST_PWR_HS):
       write `tensor_gs101_post_pwr_hs_config` (Appendix A.3), then per lane run
       `gs101_phy_wait_for_cdr_lock`: poll `TRSV_REG339 (0x339)` bit3 up to 100 × 40 µs, and on each
       miss poke `TRSV_REG222 (0x222)`: write `BIT(4)` then `BIT(4)|BIT(3)`
       (ufs-exynos.c:951-995; phy-gs101-ufs.c:173-195).
    VERIFIED-FROM-SOURCE.

23. **Wake the SCSI device if ABL parked it** (SSU; analog of `ufshcd_set_dev_pwr_mode(UFS_ACTIVE_PWR_MODE)`
    on resume, ufshcd.c:10076-10116 / 10452): send COMMAND UPIU to **UFS device WLUN 0xD0** with CDB
    `START STOP UNIT = { 0x1B, 0, 0, 0, 1 << 4, 0 }` (power condition field `0x10` = ACTIVE),
    10 s timeout, 2 attempts (`ufshcd_execute_start_stop`, ufshcd.c:10041-10064:
    `cdb[6] = { START_STOP, 0, 0, 0, pwr_mode << 4, 0 }`, `timeout = 10 * HZ`, `allowed = 2`).
    VERIFIED-FROM-SOURCE for the CDB and target; INFERRED that ABL's shutdown leaves the device in
    SLEEP (mainline cannot say; it is the documented reason this function exists).

24. **First SCSI I/O**: TEST UNIT READY / REQUEST SENSE / READ CAPACITY on LUN 0 (normal LU).
    Expect a first CHECK CONDITION with UNIT ATTENTION (power-on/reset) — Linux's SCSI core retries
    such commands rather than treating them as errors; the UFS-specific code does no special UA
    handling. INFERRED (scsi core behavior, not in the read files). LUN addressing is in Appendix B.3.
    ABL's log said "Enabled LUNs found 4": normal LUNs 0–3 map directly to UPIU LUN bytes 0–3.

Steps NOT needed for a minimal storage path (each guarded in Linux by capability flags the shim
doesn't have): WriteBooster (`ufshcd_configure_wb`), auto-hibern8 (`ufshcd_configure_auto_hibern8`,
needs `MASK_AUTO_HIBERN8_SUPPORT` + runtime PM), bkops auto-enable (`ufshcd_force_reset_auto_bkops`,
ufshcd.c:6289-6300), FMP/SMU (inline crypto), timestamps (`wspecversion < 0x400`), bRefClkFreq
update (no `ref_clk` named clock in gs101 DT → `REF_CLK_FREQ_INVAL` → skipped, ufshcd-pltfrm.c:622-637
context + ufshcd.c:9116-9148 guarded by `dev_ref_clk_freq != REF_CLK_FREQ_INVAL`).

## Appendix A — constant tables

### A.1 PHY `tensor_gs101_pre_init_cfg` (applied at power-on, before link startup)
phy-gs101-ufs.c:30-96. Write order is table order; address = `pma + (off << 2)` for COMN and TRSV
lane 0, `pma + ((off + 0x200) << 2)` for TRSV lane 1 (both lanes get every TRSV row;
`PHY_TRSV_REG_CFG_GS101(o, v, d) = PHY_TRSV_REG_CFG_OFFSET(o, v, d, 0x200)`).

Table `tensor_gs101_pre_init_cfg` (rows in write order; the list ends with an end-of-table marker):

| # | block | offset | value | mode tag |
|--:|---|---|---|---|
| 1 | COMN | 0x43 | 0x10 | ANY |
| 2 | COMN | 0x3C | 0x14 | ANY |
| 3 | COMN | 0x46 | 0x48 | ANY |
| 4 | TRSV | 0x200 | 0x00 | ANY |
| 5 | TRSV | 0x201 | 0x06 | ANY |
| 6 | TRSV | 0x202 | 0x06 | ANY |
| 7 | TRSV | 0x203 | 0x0a | ANY |
| 8 | TRSV | 0x204 | 0x00 | ANY |
| 9 | TRSV | 0x205 | 0x11 | ANY |
| 10 | TRSV | 0x207 | 0x0c | ANY |
| 11 | TRSV | 0x2E1 | 0xc0 | ANY |
| 12 | TRSV | 0x22D | 0xb8 | ANY |
| 13 | TRSV | 0x234 | 0x60 | ANY |
| 14 | TRSV | 0x238 | 0x13 | ANY |
| 15 | TRSV | 0x239 | 0x48 | ANY |
| 16 | TRSV | 0x23A | 0x01 | ANY |
| 17 | TRSV | 0x23B | 0x25 | ANY |
| 18 | TRSV | 0x23C | 0x2a | ANY |
| 19 | TRSV | 0x23D | 0x01 | ANY |
| 20 | TRSV | 0x23E | 0x13 | ANY |
| 21 | TRSV | 0x23F | 0x13 | ANY |
| 22 | TRSV | 0x240 | 0x4a | ANY |
| 23 | TRSV | 0x243 | 0x40 | ANY |
| 24 | TRSV | 0x244 | 0x02 | ANY |
| 25 | TRSV | 0x25D | 0x00 | ANY |
| 26 | TRSV | 0x25E | 0x3f | ANY |
| 27 | TRSV | 0x25F | 0xff | ANY |
| 28 | TRSV | 0x273 | 0x33 | ANY |
| 29 | TRSV | 0x274 | 0x50 | ANY |
| 30 | TRSV | 0x284 | 0x02 | ANY |
| 31 | TRSV | 0x285 | 0x02 | ANY |
| 32 | TRSV | 0x2A2 | 0x04 | ANY |
| 33 | TRSV | 0x25D | 0x01 | ANY |
| 34 | TRSV | 0x2FA | 0x01 | ANY |
| 35 | TRSV | 0x286 | 0x03 | ANY |
| 36 | TRSV | 0x287 | 0x03 | ANY |
| 37 | TRSV | 0x288 | 0x03 | ANY |
| 38 | TRSV | 0x289 | 0x03 | ANY |
| 39 | TRSV | 0x2B3 | 0x04 | ANY |
| 40 | TRSV | 0x2B6 | 0x0b | ANY |
| 41 | TRSV | 0x2B7 | 0x0b | ANY |
| 42 | TRSV | 0x2B8 | 0x0b | ANY |
| 43 | TRSV | 0x2B9 | 0x0b | ANY |
| 44 | TRSV | 0x2BA | 0x0b | ANY |
| 45 | TRSV | 0x2BB | 0x06 | ANY |
| 46 | TRSV | 0x2BC | 0x06 | ANY |
| 47 | TRSV | 0x2BD | 0x06 | ANY |
| 48 | TRSV | 0x29E | 0x06 | ANY |
| 49 | TRSV | 0x2E4 | 0x1a | ANY |
| 50 | TRSV | 0x2ED | 0x25 | ANY |
| 51 | TRSV | 0x269 | 0x1a | ANY |
| 52 | TRSV | 0x2F4 | 0x2f | ANY |
| 53 | TRSV | 0x34B | 0x01 | ANY |
| 54 | TRSV | 0x34C | 0x23 | ANY |
| 55 | TRSV | 0x34D | 0x23 | ANY |
| 56 | TRSV | 0x34E | 0x45 | ANY |
| 57 | TRSV | 0x34F | 0x00 | ANY |
| 58 | TRSV | 0x350 | 0x31 | ANY |
| 59 | TRSV | 0x351 | 0x00 | ANY |
| 60 | TRSV | 0x352 | 0x02 | ANY |
| 61 | TRSV | 0x353 | 0x00 | ANY |
| 62 | TRSV | 0x354 | 0x01 | ANY |
| 63 | COMN | 0x43 | 0x18 | ANY |
| 64 | COMN | 0x43 | 0x00 | ANY |

### A.2 `tensor_gs101_pre_pwr_hs_config` (before HS power-mode change)
phy-gs101-ufs.c:98-101.

Table `tensor_gs101_pre_pwr_hs_config` (rows in write order; the source listing has no end-of-table marker):

| # | block | offset | value | mode tag |
|--:|---|---|---|---|
| 1 | TRSV | 0x369 | 0x11 | ANY |
| 2 | TRSV | 0x246 | 0x03 | ANY |

### A.3 `tensor_gs101_post_pwr_hs_config` (after HS power-mode change, then CDR-lock wait)
phy-gs101-ufs.c:104-109. Note: `samsung_ufs_phy_calibrate()` writes every row regardless of its
`desc` tag (phy-samsung-ufs.c:84-99 loops `for_each_phy_cfg` without testing `desc`).

Table `tensor_gs101_post_pwr_hs_config` (rows in write order; the list ends with an end-of-table marker):

| # | block | offset | value | mode tag |
|--:|---|---|---|---|
| 1 | COMN | 0x8 | 0x60 | PWM_ANY |
| 2 | TRSV | 0x222 | 0x08 | PWM_ANY |
| 3 | TRSV | 0x246 | 0x01 | ANY |

### A.4 Hibern8 tables (only if the shim uses Hibernate8 / auto-hibern8)
phy-gs101-ufs.c:111-131; applied through `notify_phystate` on every enter/exit (phy-samsung-ufs.c:248-281).

Table `tensor_gs101_post_h8_enter` (rows in write order; the list ends with an end-of-table marker):

| # | block | offset | value | mode tag |
|--:|---|---|---|---|
| 1 | TRSV | 0x262 | 0x08 | ANY |
| 2 | TRSV | 0x265 | 0x0A | ANY |
| 3 | COMN | 0x1 | 0x8 | ANY |
| 4 | COMN | 0x0 | 0x86 | ANY |
| 5 | COMN | 0x8 | 0x60 | HS_ANY |
| 6 | TRSV | 0x222 | 0x08 | HS_ANY |
| 7 | TRSV | 0x246 | 0x01 | HS_ANY |

Table `tensor_gs101_pre_h8_exit` (rows in write order; the list ends with an end-of-table marker):

| # | block | offset | value | mode tag |
|--:|---|---|---|---|
| 1 | COMN | 0x0 | 0xC6 | ANY |
| 2 | COMN | 0x1 | 0x0C | ANY |
| 3 | TRSV | 0x262 | 0x00 | ANY |
| 4 | TRSV | 0x265 | 0x00 | ANY |
| 5 | COMN | 0x8 | 0xE0 | HS_ANY |
| 6 | TRSV | 0x246 | 0x03 | HS_ANY |
| 7 | TRSV | 0x222 | 0x18 | HS_ANY |

### A.5 gs101 UIC attribute values
ufs-exynos.c:2226-2231 (+ fields filled by `exynos_ufs_parse_dt` at 1285-1289):

| Field of `gs101_uic_attr` | Value |
|---|---|
| `tx_trailingclks` | `0xff` |
| `pa_dbg_opt_suite1_val` | `0x90913C1C` |
| `pa_dbg_opt_suite1_off` | `PA_GS101_DBG_OPTION_SUITE1` = `0x956a` |
| `pa_dbg_opt_suite2_val` | `0xE01C115F` |
| `pa_dbg_opt_suite2_off` | `PA_GS101_DBG_OPTION_SUITE2` = `0x956d` |

Fields filled by `exynos_ufs_parse_dt` (not in the static struct): `rx_adv_fine_gran_sup_en` = `0x1`,
`rx_adv_fine_gran_step` = `0x3`, `rx_adv_min_actv_time_cap` = `0x9`, `pa_granularity` = `0x6`,
`pa_tactivate` = `0x3`, `pa_hibern8time` = `0x20`.

### A.6 Key vendor/standard register offsets (gs101 numbers)
- vs_hci: `HCI_TXPRDT_ENTRY_SIZE 0x00, HCI_RXPRDT_ENTRY_SIZE 0x04, HCI_1US_TO_CNT_VAL 0x0C,
  HCI_UTRL_NEXUS_TYPE 0x40, HCI_UTMRL_NEXUS_TYPE 0x44, HCI_SW_RST 0x50, HCI_DATA_REORDER 0x60,
  HCI_UNIPRO_APB_CLK_CTRL 0x68, HCI_AXIDMA_RWDATA_BURST_LEN 0x6C, HCI_GPIO_OUT 0x70,
  HCI_ERR_EN_PA/DL/N/T/DME_LAYER 0x78/0x7C/0x80/0x84/0x88, HCI_V2P1_CTRL 0x8C,
  HCI_CLKSTOP_CTRL 0xB0, HCI_MISC 0xB4, HCI_IOP_ACG_DISABLE 0x100` (ufs-exynos.c:28-118).
- unipro: `COMP_CLK_PERIOD 0x44`, `UNIPRO_DME_POWERMODE_REQ_LOCALL2TIMER0/1/2 0x7888/0x788C/0x7890`,
  `REMOTEL2TIMER0/1/2 0x78B8/0x78BC/0x78C0` (ufs-exynos.h:20, 152-158).
- standard UFSHCI (`0x14700000`): see `include/ufs/ufshci.h:29-58` (IS 0x20, IE 0x24, HCS 0x30,
  HCE 0x34, UECPA 0x38 … UECDME 0x48, UTRIACR 0x4C, UTRLBA 0x50/0x54, UTRLDBR 0x58, UTRLCLR 0x5C,
  UTRLRSR 0x60, UTMRLBA 0x70/0x74, UTMRLDBR 0x78, UTMRLCLR 0x7C, UTMRLRSR 0x80, UICCMD 0x90,
  UCMDARG1/2/3 0x94/0x98/0x9C).

## Appendix B — UPIU byte layouts as Linux builds them

All UPIU frames are 12-byte header (scsi_bsg_ufs.h:51-82) + 20-byte query/command payload
(QUERY_OSF_SIZE = 32 − 12 = 20, ufs.h:38-39). Header bytes:
`[0]=transaction_code, [1]=flags, [2]=lun, [3]=task_tag, [4]=iid<<4|command_set_type, [5]=tm_function/query_function,
[6]=response, [7]=status, [8]=ehs_length, [9]=device_information, [10..11]=data_segment_length (BE)`.

### B.1 NOP OUT (`ufshcd_prepare_utp_nop_upiu`, ufshcd.c:2895-2905)
Request UPIU (32 bytes): all zero except `[0]=0x00 (UPIU_TRANSACTION_NOP_OUT)`, `[3]=task_tag`.
UTRD (`ufshcd_prepare_req_desc_hdr`, ufshcd.c:2777-2820): DW0 = command_type 1
(`UTP_CMD_TYPE_UFS_STORAGE`) in bits [31:28]… per `struct request_desc_header` (little-endian
byte view): byte0=cci, byte1=ehs_length, byte2 = `interrupt:1 | data_direction:2 (0=no data) |
reserved1:1 | command_type:4 (=1)`, byte3 = `enable_crypto:1 | reserved2:7`; byte8 `ocs = 0x0F`;
DW4/5 = UCD physical address; DW6 = response length/offset; DW7 = PRDT length/offset (gs101: bytes).
Then doorbell `hci+0x58 |= (1<<tag)` (ufshcd.c:2439-2444).
Success: OCS=0 and response `[0] == 0x20 (UPIU_TRANSACTION_NOP_IN)`
(`ufshcd_dev_cmd_completion`, ufshcd.c:3272-3277).

### B.2 QUERY request (`ufshcd_prepare_utp_query_req_upiu`, ufshcd.c:2862-2893)
Request UPIU: header = `[0]=0x16 (QUERY_REQ)`, `[1]=0x00` (no data direction), `[2]=0`,
`[3]=task_tag`, `[5]=query_function` (`0x01` standard read / `0x81` standard write,
ufs.h:177-180), `[10..11]=0` (data_segment_length; only non-zero for WRITE_DESC).
Bytes 12..31 = `struct utp_upiu_query` (scsi_bsg_ufs.h:100-113), UPIU byte offsets (B-n):

| Field | Type | Bytes | Meaning |
|---|---|---|---|
| `opcode` | u8 | B-12 | `0x01` READ_DESC, `0x03` READ_ATTR, `0x04` WRITE_ATTR, `0x05` READ_FLAG, `0x06` SET_FLAG, `0x07` CLEAR_FLAG |
| `idn` | u8 | B-13 | descriptor/flag/attribute identifier |
| `index` | u8 | B-14 | |
| `selector` | u8 | B-15 | |
| `reserved_osf` | be16 | B-16,17 | reserved |
| `length` | be16 | B-18,19 | buffer size for READ_DESC; value for WRITE_ATTR |
| `value` | be32 | B-20..23 | attribute value for WRITE_ATTR |
| `reserved[2]` | be32 x2 | B-24..31 | reserved |- SET FLAG fDeviceInit: opcode `0x06`, idn `0x01`, function `0x81` (ufshcd_query_flag, ufshcd.c:3475-3520).
- READ FLAG: opcode `0x05`, idn, function `0x01`; result flag = `be32(response qr.value) & 0x1`
  (`MASK_QUERY_UPIU_FLAG_LOC 0xFF` then `&1`, ufshcd.c:3515-3516).
- READ DESCRIPTOR: opcode `0x01`, idn = descriptor idn, `length` = buffer size (≤ 255);
  the descriptor bytes are returned in the UCD response area starting at offset
  `GENERAL_UPIU_REQUEST_SIZE = 32` (ufshcd_copy_query_response, ufshcd.c:2471-2500); actual length =
  response header `data_segment_length` (BE16). Descriptor idns: Device `0x00`, Configuration `0x01`,
  Unit `0x02`, String `0x05`, Geometry `0x07`, Power `0x08` (ufs.h:264-275). Flag idn fDeviceInit `0x01`
  (ufs.h:186). Query opcodes (ufs.h:458-468).
- Response UPIU: `[0]=0x36 (QUERY_RSP)`, `[6]=response (0 = success)`, qr fields mirror request;
  query failure codes 0xF6..0xFF (ufs.h:543-554).

### B.3 COMMAND UPIU (`ufshcd_prepare_utp_scsi_cmd_upiu`, ufshcd.c:2830-2855)
Request UPIU: `[0]=0x01 (COMMAND)`, `[1]=flags` (`0x40` read, `0x20` write, `0x00` no data,
`|0x04` CP for RT priority), `[2]=lun`, `[3]=task_tag`, `[4] low nibble = 0 (UPIU_COMMAND_SET_TYPE_SCSI)`;
bytes 12..15 = expected data transfer length (BE32); bytes 16..31 = 16-byte CDB (UFS_CDB_SIZE=16,
truncated copy of `cmd->cmnd`).
**LUN addressing**: normal LUs use the plain unit number in `[2]` (`ufshcd_scsi_to_upiu_lun`:
`scsi_lun & 0x7F`; ufshcd-priv.h:358-365). Well-known LUNs (ufs.h:75-80):
`UFS_UPIU_REPORT_LUNS_WLUN 0x81`, `UFS_UPIU_UFS_DEVICE_WLUN 0xD0`, `UFS_UPIU_BOOT_WLUN 0xB0`,
`UFS_UPIU_RPMB_WLUN 0xC4`; a WLUN byte is `(scsi_wlun & 0x7F) | 0x80` (ufshcd.c:3034-3036).
The device WLUN 0xD0 is the target for START STOP UNIT andREPORT LUNS; the shim's root FS LUNs are 0..3.
Response UPIU: `[0]=0x21 (RESPONSE)`, `[7]=SCSI status`; bytes 12..15 residual count (BE32);
bytes 36..37 sense length (BE16); bytes 38..55 sense data (`struct utp_cmd_rsp`, ufs.h:588-599).

### B.4 UTRDL/UCD memory layout for gs101 (`ufshcd_host_memory_configure` + `PRDT_BYTE_GRAN`)
`ufshcd_memory_alloc` (ufshcd.c:4051-4134): UTRDL = `nutrs × 32 B`, 1 KB aligned; each UCD =
512 B command UPIU + 512 B response UPIU + PRDT (each PRD entry 16 B), 128 B aligned; separate
devman UCD (response area 4096 B for descriptors — `ALIGNED_DEVMAN_RSP_SIZE`, ufshci.h:20-25).
Per UTRD (ufshci.h:541-560): DW4/5 `command_desc_base_addr` = UCD phys; DW6 =
`response_upiu_length | response_upiu_offset << 16`; DW7 = `prd_table_length | prd_table_offset << 16`.
**With `UFSHCD_QUIRK_PRDT_BYTE_GRAN` (gs101): offsets and lengths are raw bytes**
(response offset 512, PRDT offset 1024, response length 512) instead of dwords
(ufshcd.c:4187-4204). PRDT length in bytes = `n_entries × 16` (ufshcd.c:2733-2735).
PRD entry: `base addr (8B), reserved (4B), size = len-1 (4B)` (ufshcd.c:2752-2760).

## Appendix C — polled UIC and power-mode-change routines

### C.1 Generic UIC command (register level)
Derived from `ufshcd_ready_for_uic_cmd` (ufshcd.c:2554-2566), `ufshcd_dispatch_uic_cmd`
(2586-2603), `ufshcd_wait_for_uic_cmd` (2609-2639), ISR `ufshcd_uic_cmd_compl` (5862-5910) and
`ufshcd_intr`'s write-1-to-clear IS handling (7453-7469). VERIFIED-FROM-SOURCE register actions,
INFERRED poll formulation:

```
uic_cmd(op, arg1, arg2, arg3):
  # HCS.UCRDY (bit3) must be set first
  poll hci+0x30 until (val & 0x8) != 0        # 500us interval, 500ms timeout
  hci+0x94 = arg1                              # UCMDARG1 = attribute selector
  hci+0x98 = arg2                              # UCMDARG2 = attr-set type << 16
  hci+0x9C = arg3                              # UCMDARG3 = value (read-back on DME_GET)
  hci+0x20 = 0x400                             # W1C any stale UIC_COMMAND_COMPL
  hci+0x90 = op                                # UICCMD: 0x01 GET, 0x02 SET, 0x03 PEER_GET,
                                               # 0x04 PEER_SET, 0x16 LINK_STARTUP,
                                               # 0x17 HIBER_ENTER, 0x18 HIBER_EXIT
  poll hci+0x20 until (val & 0x400) != 0       # IS.UCCS, timeout 500ms (uic_cmd_timeout)
  result = hci+0x98 & 0xFF                     # UCMDARG2 low byte: 0 = SUCCESS
  hci+0x20 = 0x400                             # W1C clear
  return result                                # for DME_GET: value read back from hci+0x9C
```
Argument encoding (ufshci.h:338-347): `UIC_ARG_MIB_SEL(attr, sel) = (attr << 16) | sel`;
`UIC_ARG_MIB(attr) = attr << 16`; `UIC_ARG_ATTR_TYPE(t) = t << 16` (0 for DME_SET).
DME_SET/GET for `PA_* / DL_* / N_* / T_*` attributes use gen_sel 0; M-TX lane attrs use
`sel = lane`; M-RX lane attrs use `sel = 4 + lane` (`PA_MAXDATALANES = 4`).

### C.2 Power-mode change (`ufshcd_uic_pwr_ctrl`, ufshcd.c:4491-4560; VERIFIED-FROM-SOURCE actions)
```
pwr_mode_change():
  # DME_SET PA_PWRMODE (0x1571) = (rx_mode << 4) | tx_mode   # 0x11 for FAST/FAST
  # completion is the *link* event, not just the UIC completion:
  poll hci+0x20 until (val & 0x10) != 0        # IS.UIC_POWER_MODE ("UPMS"), 500ms timeout
  hci+0x20 = val_bits_seen                     # W1C
  status = (hci+0x30 >> 8) & 0x7               # HCS.UPMCRS
  require status == 0x1 (PWR_LOCAL)            # 0x2 PWR_REMOTE is a failure here
```
Linux additionally masks `UIC_COMMAND_COMPL` during this wait (it uses the dedicated completion)
and treats a UPMCRS of `PWR_OK(0)` as `-1`; both status reads are in `ufshcd_get_upmcrs`
(ufshcd.c:2568-2571). On timeout with the UIC no longer active it still checks UPMCRS
("Power Mode Change operation has been completed, go check UPMCRS", ufshcd.c:4506-4509).

## Appendix D — open questions (things mainline does not determine)

1. **Q1 — what ABL's "shutdown" actually powers down.** Linux never reboots into this state;
   its own suspend path gates the same clocks (`exynos_ufs_suspend`/`gs101_ufs_suspend` drive
   `HCI_GPIO_OUT` low and `phy_power_off` isolates the PHY) but Linux owns all clock state by then.
   Whether ABL leaves CMU_HSI2/CMU_TOP gates, the PHY isolator, and PMU votes on cannot be read
   from mainline — the shim must read the gate registers (steps 1–2) to decide, or simply always
   perform steps 1–2 (they are idempotent).
2. **Q2 — the actual `sclk_unipro_main` (mclk) rate ABL leaves.** `freq-table-hz = <0 0>` means
   Linux never changes it; the rate feeds `COMP_CLK_PERIOD`, `VND_xx_CLK_PRD`,
   linereset-period bytes and `get_mclk_period_unipro_18()`. Mainline contains no gs101 rate
   table — the shim must read the CMU_HSI2 dividers at runtime (or dump
   `CLK_CON_DIV_CLKCMU_HSI2_UFS_EMBD 0x18a4` @ CMU_TOP and its parents) and compute.
3. **Q3 — PMU 0x3ec8 access from the shim's exception level.** Mainline writes it as a plain
   syscon MMIO (no SMC). If the shim runs at EL1/EL2 while EL3 firmware restricts the PMU region,
   this write may trap; whether a `0x82000504`-style SMC is required is a downstream/EL3 question
   mainline cannot answer.
4. **Q4 — whether a second DME_LINKSTARTUP is required after ABL shutdown** (step 14 note).
   `link_startup_again` depends on driver-internal dev-power state, which a fresh shim has no
   equivalent of. Test: if HCS.DP=1 and NOP OUT answers after one pass, one was enough.
5. **Q5 — UNIT ATTENTION behavior of the HN8T05BZGKX015 after the ABL shutdown.** The SCSI
   retry-on-UA policy is SCSI-core behavior outside the files read; a shim should send REQUEST
   SENSE on CHECK CONDITION and re-issue once on UA (29h/00h or 29h/01h).
6. **Q6 — QCH registers** (`QCH_CON_UFS_EMBD_QCH 0x3090`, `_QCH_FMP 0x3094`, `QCH_CON_QE_UFS_EMBD_HSI2_QCH 0x3068` @ CMU_HSI2,
   clk-gs101.c:2779-2796) are defined but not modeled as clocks in mainline; whether a bare-metal
   bring-up must also deassert them is UNSURE.
7. **Q7 — `vs_hci` register block gaps.** Offsets 0x308/0x30C (multi-host) exist on exynosauto
   only; nothing gs101-specific is known between 0x100 and the end of the 0x200-byte window
   beyond what is listed in A.6.
8. **Q8 — WriteBooster.** ABL enabled it, and `gs101_ufs_drv_init` sets `UFSHCD_CAP_WB_EN`, but
   WB enable (`ufshcd_configure_wb`) is descriptor-driven and optional for correctness; whether
   leaving WB disabled but previously-enabled in the device causes any persistence-side behavior
   is not determinable from mainline (assumed harmless for read-only/maintenance access).

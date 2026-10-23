# 20 - Real Wi-Fi on the Pixel 6a (bluejay) under XNU: inventory, hardware chain, staged plan, `--wlan-probe` review

Desk research plus a read-only code review. No `adb`/`fastboot`, nothing was run on a phone, nothing was extracted from a phone, no kernel patch.
Milestone 2 (AGENTS.md): real Wi-Fi (user decision, HANDOFF.md:397-398: real radio, not a USB substitute). This document builds on doc 12
(`docs/research/12-wifi-bluejay.md`, call it D12) and does not repeat its tables; it re-checks the facts the plan depends on, adds what D12 lacks,
and turns it into stages with entry criteria and success lines.

Tags (AGENTS.md rule 5): **[src]** read from source (file:line or URL; fetched 2026-10-22), **[obs]** observed on hardware (repo log, date given),
**[guess]**. `TODO(verify)` = a fact I could not source; it must not be used before it is checked. Registers that appear in this document carry a tag on the
same row or line.

Source pins (Android branch `android-gs-bluejay-6.1-android16`, same as D12/D17):

| Short name | Repo | Commit |
|---|---|---|
| `soc-gs` | https://android.googlesource.com/kernel/google-modules/soc/gs | `204078758737776959ecc85c527a00fd2cabb62f` |
| `gs101-dts` | https://android.googlesource.com/kernel/devices/google/gs101 | `a9b5edb4a1af922091683feff8f7804ec6986aea` |
| `bcmdhd4389` | https://android.googlesource.com/kernel/google-modules/wlan/bcmdhd/bcm4389 | `de86f6ec77fd3ea86aba02b8c045a4f9fac69732` |
| `linux` | https://github.com/torvalds/linux master, raw files fetched 2026-10-22 | no pin recorded |
| `topaz` | `~/pixelios-artifacts/refs/ios-topaz` (local clone of the Redmi Note 12 port) | as cloned |

`soc-gs` paths used below: `drivers/pci/controller/dwc-whi/{pcie-exynos-rc.c,pcie-exynos-rc.h,pcie-exynos-common.h,pcie-exynos-gs101-rc-cal.c}` (short:
`rc.c`, `rc.h`, `common.h`, `cal.c`), `drivers/soc/google/exynos-pd_el3.c` (short: `pd_el3.c`).

---

## Summary

- **Goal chain.** iOS Settings/`wifid` -> an `en0` that answers `apple80211` ioctls and looks like an IO80211 controller (D12 2.2) -> a Broadcom "msgbuf"
  PCIe dongle driver in our kernel (BCM4389, not in mainline brcmfmac [src: `linux` `brcm_hw_ids.h` lists 4377/4378/4387 only, `pcie.c` device table
  has no 4389]) -> a gs101 DWC PCIe root-complex bring-up (no mainline equivalent) -> power/clock/EL3 plumbing for HSI2 and the radio rails.
  Nothing in the chain exists in the repo except the read-only probe.
- **Apple's own Wi-Fi driver stack cannot be used.** Not present in anything we hold (no kernelcache: section 1.1), built for BCM4357 behind Apple's
  PCIe and DART (D12 2.1), and iOS kexts are not loadable files anyway (section 1.2). Everything must be built into our kernel by numbered patch, as
  HFS, HID, the watchdog and the framebuffer already are.
- **Precedent that the iOS side is solvable.** `topaz` (Redmi Note 12) reports iOS Settings listing networks, joining WPA2 and reconnecting after reboot
  through `wifid`, with a kernel-resident radio driver [src: `topaz` README.md:7,23,31]. Its ADT has a `wlan` node because `wifid` is only started when
  the device tree has one [src: `topaz` tools/mkadt.py:184-188]. The part that speaks to `wifid` is **not visible** in the topaz patch list we hold
  (see 1.3): treat it as unread.
- **Three biggest blockers, in order** (detail in section 5):
  1. **EL3 / ACPM gate on power.** PCIe PHY isolation (PMU `0x17463EC4`) is written by Linux through an EL3 SMC (`rmw_priv_reg`), not MMIO
     [src: `rc.c:393-407`, `pd_el3.c:66-77`]; the radio rails (S2MPG10 LDO17M/LDO19M) sit behind ACPM [src: D12 1.2]. EL3 accepting that SMC for that register,
     and the rails being on, are both unproven. Same class of blocker as the GPU (D17 section 5 item 1-2).
  2. **PHY/PCS init tables and link training with no hardware feedback.** The only source is Google's GPL `cal.c` (`exynos_pcie_rc_pcie_phy_config`,
     `cal.c:106-325`); there is no TRM and no mainline driver. A wrong table is silent: the link just never reaches L0. Licence decision (GPL reference vs
     an MIT project) is open (D12 3B item 1).
  3. **Firmware supply plus an iOS-facing layer nobody has read.** BCM4389 firmware/NVRAM/CLM live on the phone's `vendor` partition, must never enter git
     (rule 4), and our UFS path is userdata-only by design (shim/AGENTS.md); and the `apple80211`/IO80211 request set `wifid` needs on iOS 16.7 is still
     unlisted (D12 2.3, open since 2026-10-21). A fourth, hardware-only unknown follows closely: whether the HSI2 S2MPU lets the PCIe stream ids DMA into XNU RAM.
- **`--wlan-probe` review (section 3):** no store, no SMC, no unbounded loop, and every address is sourced, but there are four defects (D1 level-2 not gated,
  D2 the PHY words it reads are not the ones D12 asked for, D3 host test does not exercise the file, D4 a stale comment).
  Of D12's TODOs, the pinctrl bank layout is now **verified** (this document, 2.1).

---

## 1. What iOS needs to see Wi-Fi, and what we have

### 1.1 Inventory of the artifacts (offline, read-only)

- **No kernelcache, no kexts.** `~/pixelios-artifacts/ipsw` holds three DMGs (`087-86593-047.dmg` 2.7 GB, `087-86698-044.dmg` 4.7 GB,
  `087-86715-047.dmg` 10 MB) and `Firmware/` with only `*.root_hash`, `*.trustcache` and one `.mtree` (IM4P-compressed); `ipsw-fw/Firmware/all_flash`
  has only `DeviceTree.d22ap.im4p` [obs, `ls` 2026-10-22]. iOS 16 ships its kexts inside the kernelcache (an IM4P not in the IPSW subset we
  keep), not as files in the root DMG. So **`IO80211Family`, `AppleBCMWLAN*`, `IONetworkingFamily`, `IOSkywalkFamily` are not inspectable here** [guess for
  the root-DMG half: no `/System/Library/Extensions` on iOS 16; TODO(verify) by listing the mounted root].
- **The root DMG is not mounted this session** (needs root; stage it for the user, per AGENTS.md rule 2/global instructions). Command to stage (not run):
  `sudo mount -o ro,loop <converted-hfs-root.img> /mnt/iosroot` for the HFS image built by `tools/build-ios-root.sh`, then `ls /mnt/iosroot/usr/libexec |
  grep -i wifi`, and read `/System/Library/xpc/launchd.plist` (`com.apple.wifid` `_LimitLoadToDeviceTree`). Until then the daemon list below is partial.
- **What the dyld shared cache gives us** (image path table of `~/pixelios-artifacts/work/dsc/cache/dyld_shared_cache_arm64`, `strings` run 2026-10-22; the
  cache itself is the iPhone X 16.7 one used for the GUI) [obs: file contents, not a boot result]:
  `PrivateFrameworks/{MobileWiFi, CoreWiFi, WiFiPolicy, WiFiPeerToPeer, WiFiAnalytics, WiFiKit, WiFiKitUI, WiFiCloudSyncEngine, WiFiLogCapture, WiFiVelocity,
  IO80211, AirPortAssistant, WirelessCoexManager, WirelessProximity, WirelessDiagnostics, CaptiveNetwork, NetworkServiceProxy, NetworkStatistics}`,
  `PreferenceBundles/AirPortSettings.bundle`, `SystemConfiguration/CaptiveNetworkSupport.bundle`, `Frameworks/NetworkExtension.framework`. So every
  user-space framework Settings uses exists; only the kernel side and the `wifid` daemon's hardware view are missing.
- **Daemons** (names from D12 and our logs; the full launchd list needs the mounted root): `wifid` (owner of Wi-Fi state; `MobileWiFi` clients talk to it;
  [src: D12 2.2 citing Levin, https://newosxbook.com/articles/11208ellpA-II.html, iOS 8-era, TODO(verify) for 16.7]); `WirelessRadioManagerd` already
  runs [src: NOTES.md:410]; `networkserviceproxy` runs [src: NOTES.md:410]. `wifianalyticsd`, `apsd`, `symptomsd`, `WiFiPolicy` consumers matter only once `wifid`
  is up [guess]. `UPSTREAM_README.md:31` says Wi-Fi/BT/cellular daemons "start and fail or keep relaunching" [src].
- **What the running system looks for today** [obs, `logs/qr-20261022T063754Z.txt:94-95`, SpringBoard, 2026-10-22 06:37 UTC]:
  `IONameMatch "wlan"` -> none and `IONameMatch "marconi-wifi"` -> none. Our ADT has no `wlan` node (`grep -n "wlan\|wifi\|mac-address" tools/mkadt.py`
  is empty, D12 2.1), and `/product` still says `wifi-chipset 4357` (copied from D22AP, D12 2.1).

### 1.2 Can our XNU load kexts at all?

- **No evidence that it can, and the project never needed to.** Every driver that Apple ships as a kext is compiled into the kernel by patch:
  HFS (0026, `xnu/patches/0026-sweet-builtin-hfs.patch`, "Built in, not a loadable kext"), corecrypto replaced (0013/0014/0033, "SWEET has no corecrypto kext"),
  IOHIDFamily pieces (0058), `IOWatchdog` (0040), the framebuffer (0046/`SweetFramebuffer`) [src: patch headers, NOTES.md:479,424]. The kernel is a
  development Mach-O loaded by our shim, not a boot kernel collection [src: NOTES.md:496-502, `tools/mkpayload.py` flow in AGENTS.md]; there is no
  `kmutil`/kernelmanagerd, and iOS has no IOKit-matching daemon, which is why IOKit never quiesced until 0086 [src: HANDOFF.md:380].
- Skywalk **is** compiled in: its self-tests passed on hardware [obs, NOTES.md:500, 2026-10-15, boot of patch 0020]. That keeps Skywalk-based
  interfaces possible but does not say `wifid` on 16.7 requires one (open, D12 2.2).
- **Decision for the plan:** the driver is a set of built-in files (`iokit/Kernel/arm/Sweet*.cpp`, `bsd/dev/sweet_*.c`), numbered patches, matched through the
  ADT/`KernelConfigTables`, like `SweetWatchdog`/`SweetHID` [src: NOTES.md:702]. No loader work.

### 1.3 Options when Apple's BCM driver cannot be used with Google's BCM4389 firmware

| Option | What it is | Verdict |
|---|---|---|
| **A. Apple stack + Apple firmware** | `AppleBCMWLANCore` + `AppleBCMWLANBusInterfacePCIe` + IO80211FamilyV2 from an iPhone kernelcache | Not available (1.1); targets BCM4357 behind Apple `apcie`+DART (D12 2.1); closed; would also need Apple's firmware/NVRAM bundles for a chip Apple never shipped. Reject. |
| **B. Own msgbuf driver + own IO80211-shaped interface (recommended)** | Fresh C driver for the Broadcom PCIe "msgbuf" protocol using the phone's own `fw_bcmdhd.bin`/`bcmdhd.cal`/`bcmdhd_clm.blob` [src: D12 1.3, `bcmdhd4389` Kbuild:58-66,956-959], plus a stand-in IO80211Controller + `en0` ifnet answering `apple80211` ioctls | The only workable option; this is D12 option B and the topaz structure (radio core in kernel, iOS faces `wifid`). Reference for the protocol: mainline brcmfmac (ISC), **not** GPL bcmdhd, to keep the licence question clean [src: D12 3B item 2]. |
| **C. Port Google bcmdhd4389** | Wrap `dhd_pcie.c` (18121 lines), `dhd_msgbuf.c` (15606), `wl_cfg80211.c` (26343) behind a Linux-compat layer | Reject: GPL, 60+ kLOC, depends on cfg80211/PCI/DMA APIs we do not have [src: line counts D12 3B]. Useful as a read-only oracle for 4389 ring/IPC revisions. |
| **D. Virtual Wi-Fi over USB (D12 A2)** | fake `en0` over ECM | Not the milestone (user: real radio, HANDOFF.md:398). But its `apple80211` stub is stage 3's first half, so it is a **stepping stone**, not a rival. |

- **Firmware blobs.** Names/paths on the phone: `/vendor/firmware/fw_bcmdhd.bin`, `/vendor/etc/wifi/bcmdhd.cal` (+ `_4389_<cid>` suffix variants),
  `bcmdhd_clm.blob`, `fw_bcmdhd.map` [src: D12 1.3]. Exact variant names are `TODO(verify)` (user: `adb shell ls /vendor/firmware /vendor/etc/wifi`, read-only).
  They live inside the `super` dynamic partition [guess]. **Supply route (recommended): the user pulls them from their own phone; `tools/build-ios-root.sh`
  copies them into the iOS root at `/private/var/...` at image build time; the paths are in `.gitignore`.** Never committed (rule 4), never read by the
  shim from `super` (shim/AGENTS.md: UFS reads only inside the `userdata` GPT range; widening that audit is a separate, reviewed change).
- **BCM4389 identity** for stage 2 checks: PCI vendor `0x14E4` [guess/TODO(verify), D12 1.1]; device `0x4441` (OTP programmed) or `0x4389` (no OTP)
  [src: D12 1.1 citing `bcmdevs.h:138`, `Kbuild:944-945`, `dhd_pcie.c:1616-1631`]; ChipCommon chip id `0x4389` [src: D12 1.1, `bcmdevs.h:193`].
- **topaz gap (unread).** In the topaz patches we hold, `grep -n "80211\|IO80211\|ifnet_\|SIOC"` over 0123/0126/0129/0130 finds nothing; they expose only
  `topaz_wlan_join/joined/heard` C functions [src: `topaz` patches 0126, 0129:1122-1175, 0130:18-30] and the README says "iOS sees it through `wifid`"
  [src: README.md:23]. The layer in between is in a file we do not have. **Ask the user/topaz owner for it**; it would shortcut D12 step 1 (listing the
  `apple80211` request set) and is the cheapest high-value unknown in this plan.

---

## 2. Hardware chain on gs101

### 2.1 Verified in this review (previously `TODO(verify)` in D12)

- **Pinctrl bank layout.** gs101 `EINTG` banks (peric0, hsi1, hsi2) use `exynos850_bank_type_off` and `EINTW` banks (alive, far-alive) use
  `exynos850_bank_type_alive`; in both, `fld_width {4,1,...}`, `reg_offset {0x00, 0x04, ...}`: **CON at +0x00 (4 bits/pin), DAT at +0x04 (1 bit/pin)**
  [src: `linux` `drivers/pinctrl/samsung/pinctrl-exynos-arm64.c:56-59` (off type), `:62-67` (alive type); `pinctrl-exynos.h:217-232` (`GS101_PIN_BANK_EINTG/EINTW`)].
  This closes D12 1.2 caveat (a) and the `TODO(verify)` in `shim/wlanprobe.c:16-17` (defect D4).
- **Bank offsets.** `gpp14` = 4 pins at +0x1C0 in PERIC0 [src: `linux` `pinctrl-exynos-arm64.c:2141`]; `gph2` = 6 pins at +0x0 in HSI2 [src: `:2169`]; `gpa7` = 4 pins at
  +0x20 in FAR_ALIVE [src: `:2108`]. With controller bases `0x10840000` / `0x14440000` / `0x174E0000` (D12 1.2; `linux` `gs101.dtsi:699` for peric0) these give the
  `soc.json` `wlan` addresses `0x108401C0`, `0x14440000`, `0x174E0020` [src: `soc/google/gs101/soc.json` `wlan` object, matches]. Base addresses of the HSI2 and
  far-alive controllers are from the Google DT (D12), not re-fetched here.
- **PHY isolation is an EL3 write.** `exynos_pcie_phy_isolation()` does `rmw_priv_reg(pmu_alive_pa + pmu_offset, mask 1, val)` first and falls back to
  `regmap_update_bits` only if the SMC returned nonzero [src: `rc.c:393-407`]. `rmw_priv_reg` = `arm_smccc_smc(SMC_CMD_PRIV_REG, reg, PRIV_REG_OPTION_RMW=2, mask, val)`
  [src: `pd_el3.c:66-77`; `SMC_CMD_PRIV_REG = 0x82000504`, option values 0/1/2 read/write/rmw: D17 2.2 citing `exynos-el3_mon.h:21-23`]. `PCIE_PHY_ISOLATION = 0`, `PCIE_PHY_BYPASS = 1`
  [src: `common.h:51-52`]. Offset `0x3EC4` and PMU base `0x17460000`: D12 1.2 [src].
- **Power-up order Linux uses** (the sequence stage 1 must reproduce) [src: `rc.c:3363-3470` poweron and `rc.c:3103-3265` establish_link]:
  (1) PHY isolation -> bypass (SMC); (2) clocks (none in the clock framework for pcie_1, D12 1.2) and optional SysMMU; (3) pinctrl -> active state; (4) PHY "all power-down clear"
  (`cal.c:69`); (5) `establish_link`: PHY config tables (`exynos_pcie_rc_assert_phy_reset` -> `phy_config`, `rc.c:2657-2670`, `cal.c:106-325`), then wait PHY PLL lock
  `PMA+0x3F0` bit3, CDR lock `PMA+0xE0C` bit2, offset-cal `PMA+0xDE8 & 0xF == 0xF` [src: `rc.c:3119-3139`]; (6) soft power reset: clear then set `ELBI+0x3A4` bit1 with 1 ms between
  [src: `rc.c:3142-3153`]; (7) write `ELBI+0x80 = 0x04` (device type) [src: `rc.c:3161-3162`]; (8) non-sticky reset pulse (bit3: set, 10 us, clear, 1 ms, set) [src: `rc.c:3164-3173`];
  (9) DBI `0x890 = 0x12000` "EQ off" [src: `rc.c:3175-3176`]; (10) PERST high, then **20-22 ms** for `EP_BCM_WIFI` [src: `rc.c:3178-3187`]; (11) L1/QCH/linkdown-reset setup
  (`PCIE_APP_REQ_EXIT_L1_MODE`, `PCIE_LINKDOWN_RST_CTRL_SEL`, `PCIE_QCH_SEL`, NACK/DBI-L1-exit options) and then `dw_pcie_setup_rc` + `exynos_pcie_setup_rc` (DBI RC setup) [src: `rc.c:3189-3222`]; (12) `ELBI+0x54 = 1` (LTSSM enable), poll `ELBI+0x2C8 & 0x3F == 0x11` (`S_L0`)
  up to `MAX_TIMEOUT` = 4800 polls of 10-12 us (~48-58 ms; the header comment says "about 24 ms x 2") [src: `rc.c:3226-3243`, `common.h:20`, `rc.h:44-51`, `common.h:88`]; on timeout Linux toggles PERST low, LTSSM off, PHY clock off and retries up to 10 times [src: `rc.c:3245-3265`].
  Before all of it the RC probe drives WLAN_EN **low** [src: `rc.c:1989-1994`], and the Wi-Fi driver raises it afterwards [src: D12 1.4, `dhd_custom_google.c:529-566,638-657`].

### 2.2 Chain table

"Probe" = what `--wlan-probe` reads today; "Blocker" = what stops us from doing it from EL1.

| # | Item | Facts and source | Probe reads | Blocker / unknown |
|---|---|---|---|---|
| 1 | **HSI2 power domain** (`pd_hsi2`) | PMU config/status `0x17462180`/`+4` (DT node name says `@17462080`; inconsistent) [src: D12 1.2, gs101-pm-domains.dtsi]. UFS is in HSI2 and works [obs, HANDOFF.md:242-244, 2026-10-21 ~15:07 UTC: UFS DMA into XNU bank OK], so the domain is on. | `PD_HSI2_A/B` + `+4` (`wlanprobe.c:116-119`) | none expected; which of the two addresses is the real one is open |
| 2 | **HSI2 CMU / PCIe aclk, pclk gating** | HSI2 CMU `0x14400000` [src: D12 1.2]; `pcie-clk-num = <0>` so Linux enables no clocks via the framework [src: D12 1.2, `rc.c:1212-1229`]; which CMU bits gate the PCIe block is unknown. | nothing (PMA/PCS/ELBI/DBI reads indirectly) | `TODO(verify)` in `soc-gs` cal-if tables; a gated block may hang the bus on read |
| 3 | **PCIe PHY isolation** | PMU `0x17463EC4` bit0; EL3 SMC RMW (2.1). [obs: plain MMIO reads of PMU `0x17463EC8` work from the shim: `device/google/bluejay/hardware/ufs-gpt-read-2026-10-21.txt:294`, cited by `docs/research/10:130`] | `PMU_PCIE1_ISO` (`wlanprobe.c:115`) | **EL3 may refuse this register.** [obs, docs/archive/HANDOFF-s01-s06.md:374,382, 2026-10-21/22] EL3 accepted `0x82000504` WRITE for `0x17460810` (SYSIP_DAT0) from EL1; no evidence for `0x17463EC4`. |
| 4 | **Radio rails** S2MPG10 LDO17M (0.725-1.3 V, "L17M_PCIE1") and LDO19M (1.6-1.95 V, "L19M_PCIE1") | `regulator-always-on` in DT, `vreg1/vreg2-supply` of `pcie_1` [src: D12 1.2, gs101-pmic.dtsi]. PMIC is on the SPEEDY bus owned by ACPM; AP reaches it through the ACPM mailbox `0x17440000` [src: `soc/google/gs101/docs/02-gs101-peripherals.md:307-321`]. | none | State at XNU entry unknown. If off we need an ACPM IPC client (as GPU, D17 5.2). `02-gs101-peripherals.md:321` argues ABL leaves CPU/RAM/UFS/display rails on only [INFERRED there]. |
| 5 | **WLAN_EN** (`WL_REG_ON`) gpp14-2 | CON `0x108401C0`, DAT `+4`, bit 2 (2.1) [src] | `WLAN_EN gpp14-2 func/lvl` (`wlanprobe.c:121-122`) | Plain MMIO; [guess] writable from EL1 (the shim already drives other pinctrl banks for touch/keys? not verified: TODO(verify) with a kernel patch 0087/0088 reading) |
| 6 | **PERST / CLKREQ / DEV_WAKE** gph2-0/1/5 | PERST output func1, CLKREQ func2, dev_wake input [src: D12 1.2]; bank `0x14440000` (2.1) | `PERST/CLKREQ/DEVWAKE` (`wlanprobe.c:123-126`) | needs pd_hsi2 on (it is, row 1) |
| 7 | **Host wake** gpa7-0 (EINT func F) | `0x174E0020` (2.1) | `HOSTWAKE gpa7-0` (`wlanprobe.c:127-128`) | polled operation first; IRQ later (GIC SPI number for `IRQ_PCIE_GEN4A_1_HSI2` TODO(verify), D12 1.2) |
| 8 | **SYSREG HSI2** | `0x14420000` size `0x2000` [src: D12 1.2]. `PCIE_WIFI0_PCIE_PHY_CONTROL 0xC` with `BIFURCATION_MODE_DISABLE`=bit16, `LINK1_ENABLE`=bit15, `LINK0_ENABLE`=bit14 [src: `rc.h:213-216`]; how pcie_1 uses it: `TODO(verify)` | `SYSREG+000..01C` | which bits Linux sets for ch1 is unread |
| 9 | **PHY (PMA) / PCS** | `0x14550000`/`0x14540000` [src: D12 1.2]; tables in `cal.c:106-325` (`phy_config`), `cal.c:37-98` (power down/clear), `cal.c:98-105` (OTP). Lock bits `PMA+0x3F0`(3), `+0xE0C`(2), `+0xDE8`(`0xF`) [src: `rc.c:3119-3139`] | `PMA+000..01C`, `PCS+000..01C` (**not** the lock words, defect D2) | **tables only from GPL source** (blocker 2) |
| 10 | **ELBI (app wrapper)** | `0x14520000`; `+0x54` LTSSM enable, `+0x2C8` link/LTSSM state (`&0x3F`, L0 = `0x11`), `+0x2E8` PM_DSTATE, `+0x3A4` soft reset (bit0 core, 1 pwr, 2 sticky, 3 non-sticky, 4 phy), `+0x80` device type [src: `rc.h:44-62`, `common.h:88`, `rc.c:3161-3162`] | `ELBI+000..040`, `+054`, `+2C8`, `+2E8`, `+3A4` | none beyond power/clock |
| 11 | **DBI (DWC core)** | `0x14800000` size `0x301000` [src: D12 1.2]; the RC's own config space; Linux treats vendor/device `0x144d`/`0xecec` [src: D12 1.2 citing `dhd_custom_google.c:53-55`] | `DBI+000..00C` | DBI/NCLK gating when link off: Linux has an `#ifdef NEED_NCLKOFF` path that is "always 0 in gs101" [src: `rc.c:2088-2102`], so DBI should be readable |
| 12 | **iATU / outbound windows** | config window CPU `0x60FFE000` size `0x2000`, MEM window `0x60000000` size `0xFF0000`, I/A block `0x14500000` [src: D12 1.2]; Linux programs I/A in `rc.c:2538-2656` (link-up IRQ selection, `ia_base_physical_addr` written to I/A `0x38`, `rc.c:2556`) | none (D12: window never touched) | iATU register programming is DWC-standard [guess]; exact Google I/A sequence `TODO(verify)` |
| 13 | **DMA path** (S2MPU `s2mpu_hsi2` `0x14480000`, SSMT `0x144F0000`, stream ids `0x4 0x5`, "TODO: double check" in DT) | [src: D12 1.2]. UFS shares the S2MPU and DMAs into XNU RAM at `0x880100000` [obs, HANDOFF.md:242-244]; PCIe uses different ports. Non-coherent, no SysMMU [src: D12 1.2]. | none | hardware-only unknown; fallback = a DMA window like the USB `abl@f8800000` carve-out (D17 section 4 stage 2) |
| 14 | **Interrupts** | SPI `IRQ_PCIE_GEN4A_1_HSI2` number `TODO(verify)`; MSI optional [guess]; XNU drivers are polled (xnu/AGENTS.md) | none | none for stage 1-2 |
| 15 | **Bus type** | PCIe, **not SDIO**: `CONFIG_BCMDHD_PCIE=y`, `CONFIG_BCM4389=y` [src: D12 1.1, `bcmdhd4389` Kbuild:53-55]. BT is a separate UART HCI (out of scope). | | |

**EL3/ACPM blockers specifically** (cf. D17 2.2, 5): PMU "alive" writes must be SMCs (`0x82000504`) [src: D17 2.2 citing `pmucal_rae.c:149-155`]; only the one register above
is needed here (PHY iso), plus possibly the HSI2 domain sequence if it were off (it is not [obs]). Rail and clock *rates* belong to ACPM; PCIe needs no DVFS [guess] but the LDOs
could be sleeping (row 4). A fail-safe is to treat "SMC returned nonzero" as a stop, which is also what Linux does (it falls back to regmap, which on this phone would fault).

---

## 3. Stage 0 review: `shim --wlan-probe` (adversarial)

Files: `shim/wlanprobe.c` (173 lines), `shim/wlanprobe.h`, `shim/wlanprobe_host_test.c`, call site `shim/shim.c:645-657`, build `shim/build.sh:62-65,132-133,151-152,206`,
gate `tools/check-all.sh:12`, audit `tools/test-bluejay-boot.py:1548-1558`. Address macros come from `soc/google/gs101/soc.json` key `wlan` through `tools/boards.py:96-97`.

**What it prints.** Level 1 (`wlan_probe_l1`, `wlanprobe.c:107-129`): `PMU_PCIE1_ISO`, `PD_HSI2_A/B` and `+4`, then `gpp14-2 WLAN_EN`, `gph2-0 PERST`, `gph2-1 CLKREQ`, `gph2-5 DEVWAKE`, `gpa7-0 HOSTWAKE`
as `<name> func=X lvl=Y` plus the raw CON/DAT words. Level 2 (`wlan_probe_l2`, `wlanprobe.c:131-168`): SYSREG `00..1C`, ELBI `00..40`, `54`, `2C8`, `2E8`, `3A4`, PMA `00..1C`, PCS `00..1C`, DBI `00,04,08,0C`, and one line
`WLAN SUMMARY en=<0|1|?> perst=<..> dbi_id=<8 hex|unreadable> link=<2 hex|?>` (`link` = `ELBI+2C8 & 0x3F`; `11` means L0). All lines are `WLAN <name> pa=<8 hex> v=<8 hex>` or `... unreadable`.
Not yet run on hardware [src: HANDOFF.md:402 "(5) WLAN probe never run"].

**Checks that pass** [src: read of the files above]:
- *Can it write?* No MMIO store exists: the only I/O primitive is `try_read32` (`shim.c:127-137`, an `ldr` plus `dsb/daifclr/isb`); `try_write32` (`shim.c:143`) is not referenced from `wlanprobe.c`. No SMC, no GPIO write.
  The same property is enforced twice: source spelling (`test-bluejay-boot.py:1548-1549`) and an object-level stack-only-stores audit (`:1557`).
- *Can it loop forever?* No loops except fixed `for` bounds (`rd_words` <= 17 iterations; `put_*` bounded by buffer capacity). No wait on hardware. The only delays are the 3 s `delay_ms` in `shim.c:648,653`.
- *Are addresses cited?* Every address is a `soc.json` macro; each is sourced in D12 1.2 and re-checked above for the GPIO banks. Register offsets are in the header comment (`wlanprobe.c:9-17`) with `rc.h` references.
  The ELBI/L0 decode (`&0x3F == 0x11`) matches `rc.c:3233-3237` and `common.h:88`.
- *Fault handling.* A data abort (EC `0x25`) from the guarded load sets `probe_fault` and skips the 4-byte `ldr` (`shim.c:761-770`); an SError is absorbed because `try_read32` forces it to be taken inside the guard window (`shim.c:132`).
  Stack buffers: `rd_off` name buffer 24 bytes: worst case `pfx` (6 chars) + `+` + 3 hex + NUL = 11 (`wlanprobe.c:70-78`); `emit` clamps to `RAMLOG_LINE_MAX` (96) (`wlanprobe.c:40-45`).

**Fixes (W1, 2026-10-23, in the working tree; commit pending; host test `shim/wlanprobe_host_test.c` now builds the real `wlanprobe.c`):**
- **D1 fixed.** Level 2 is gated by `wlan_gate()` (`wlanprobe.h`, same by-construction pattern as `gpu_gate()`): the wrapper is read only if PMU `0x17463EC4` bit 0 = 1 (PCIe1 PHY not isolated: D12 1.2, `rc.h:120`, `common.h:52`) **and** `CMU_HSI2 + 0x620` (`PLL_CON0_MUX_CLKCMU_HSI2_PCIE_USER`, `flexpmu_cal_system_gs101.h:355`; base `0x14400000` = `cmu_id` of `pd_hsi2`, `gs101-pm-domains.dtsi:95`) has bit 4 set (the select convention of `embedded_g3d_on`, a *guess* as in `gpu_gate()`); otherwise `WLAN l2 gated: <pcie1-phy-isolated|pmu-iso-unreadable|cmu-hsi2-pcie-mux-off|cmu-hsi2-unreadable>` and NOTHING of SYSREG/ELBI/DBI/PMA is read. The citable PMU status word of the HSI2 domain does not exist (the DT marks `pd_hsi2` `status = "disabled"`, `gs101-pm-domains.dtsi:90-98`, and `pmucal_pd_list` has no hsi2 sequence), so none is used. `shim.c` runs the groups 0 SYSREG, 1 ELBI, 2 DBI, 3 PMA (last), each followed by `pstore_save()`, and the one-line SUMMARY (`SUMMARY gate=<name> en= perst= dbi_id= link=`) even when gated. The level-1 `gph2` read stays ungated and is documented by construction in the `wlanprobe.c` header: HSI2 is never power-gated and the UFS controller inside HSI2 (`0x14700000`) answers the shim [obs: `hardware/ufs-gpt-read-2026-10-21.txt`]; the GPIO block's own clock gate is not citable (accepted residual risk of two reads).
- **D2 fixed.** The PMA/PCS `00..1C` dumps are gone. Group 3 reads exactly the words `establish_link()` checks and logs, re-verified against the fetched `pcie-exynos-rc.c`/`rc.h` (local copies `~/pixelios-artifacts/refs/wlan/`): `PMA+0x3F0` bit 3 = PLL lock (`rc.c:3125-3126`), `+0xE0C` bit 2 = CDR lock (`rc.c:3129-3130`, `rc.h:318`), `+0xDE8` `& 0xF == 0xF` = offset calibration done (`rc.c:3134-3136`), `+0x5FC` = OC initial status (`rc.c:3115-3116`), `+0xE18` bit 7 = OC ok after the soft reset (`rc.c:3148-3149`), printed as `PHY pll_lock= cdr_lock= oc_done= oc_after_rst=`; `ELBI+0x80` = DEVICE_TYPE (`rc.c:3158-3159`); PMU `0x17463EC0` (PCIe0, comparison) in level 1. A PCS dump is not restored (no citation).
- **D3 fixed.** The host test builds the real file with a fake register window and a faulting address and asserts the exact lines, the SUMMARY, the group order, that every closed-gate case reads nothing of the wrapper, and that `wlan_gpio_func/level` are bounded (`pin < 8`, out of range reads as `0xF` / `0`). `try_write32` is not defined in the test, so a writer reference fails the link.
- **D4 fixed.** The header comment now cites the verified bank layout (`pinctrl-exynos-arm64.c:56-67`, section 2.1) and drops the stale sentence.
- **D5 fixed.** `0x17462080/84` are printed as `PD_HSI0_CFG` / `PD_HSI0_STAT` (they are the HSI0 domain: `pd-hsi0` reg `0x17462080`, `gs101-pm-domains.dtsi:78-88`; `hsi0_status` = PMU `+0x2084` bit 0, `flexpmu_cal_local_gs101.h`); `0x17462180/84` stay `PD_HSI2_A`, documented as the disabled `pd_hsi2` node and never used as a gate.
The object audit is unchanged: no new store, no `try_write32` reference (the same `check_usb_object` policy).

**Defects found** (none can write or brick; severity = how likely it costs a run; all five are fixed, see above):

| ID | Where | Defect | Severity | Fix |
|---|---|---|---|---|
| D1 | `wlanprobe.c:131-149`, `shim.c:645-657` | **Level 2 is not gated.** D12 step 5 says level 2 runs "only if level 1 row 1 says the domain is on"; the code reads SYSREG/ELBI/PMA/PCS/DBI unconditionally, unlike `gpu_gate()` in D17. A read of a clock-gated or isolated block may not fault but hang the bus (the shim cannot recover: watchdogs are off, shim/AGENTS.md). Also, level 1 itself reads `gph2` in the HSI2 pinctrl (`0x14440000`, `wlanprobe.c:123-126`) before any domain check; safe in practice because UFS proves HSI2 is on [obs], but not by construction. Also the l2 lines reach pstore only at `shim.c:657` (after l2), so after a hang the attribution rests on the last screen/UART line. | medium | Gate l2 on `PD_HSI2_A+4` status bit0 (semantics `TODO(verify)`) and print `l2 skipped: <reason>`; `pstore_save()` after each *group* (SYSREG, ELBI, PMA/PCS, DBI) so the last line is attributable; order PMA/PCS **last**. |
| D2 | `wlanprobe.c:144-145`; D12 4 rows 9, 10, 13 | **It reads the wrong PHY words.** D12 asked for PHY PLL/CDR/offset-cal lock (`PMA+0x3F0`, `+0xE0C`, `+0xDE8` [src: `rc.c:3119-3139`]), ELBI `+0x80`, and PMU `0x17463EC0` (PCIe0 for comparison). The code reads `PMA+0x00..0x1C` and `PCS+0x00..0x1C`, whose meaning is not cited anywhere, and omits the three lock words and `ELBI+0x80`. Stage 1 needs exactly the lock words as its baseline. | medium (wasted hardware run) | Replace with `PMA+0x3F0`, `+0xE0C`, `+0xDE8`, `+0x5FC`, `+0xE18` (the words `rc.c:3107-3139` logs), `ELBI+0x80`, `PMU+0x3EC0` [src: D12 4 row 2]; keep a short PCS dump only with a source citation. |
| D3 | `wlanprobe_host_test.c:14-34` | The host test never compiles `wlanprobe.c` (guard `defined(WLAN_ELBI)` is false there) and only asserts the two inline GPIO decoders. The formatting code (`put_hex`, `rd_off` buffer sizing, `emit` clamp) and the summary logic are untested; D12 step 5 promised "a fake register window". `wlan_gpio_func()` shifts by `4*pin` with no bound (`wlanprobe.h:20`): UB for pin >= 8 (callers use <= 5). | low | Build the real file in the host test with `-DWLAN_ELBI=...` etc. and a fake `try_read32` window plus a faulting address; assert the exact lines and the SUMMARY string; add `pin < 8` assert/clamp. |
| D4 | `wlanprobe.c:15-17` | Stale comment: says the CON/DAT layout "is an assumption (TODO(verify))" and that no shim code reads gpa7. Verified now (2.1, `linux` `pinctrl-exynos-arm64.c:56-67`). | low | Update the comment with the citation. |
| D5 | `wlanprobe.c:116-119`, `soc.json` | `PD_HSI2_A/B` (`0x17462180` / `0x17462080` [src: D12 1.2]): the DT is self-inconsistent (D12 1.2) so one of the four reads is a different register. Harmless (read-only) but the output must not be interpreted as "domain status" without the cross-check against UFS. | info | Note in the print label or doc; do not gate on it until the real address is known. |

Not defects, noted: PHY isolation read (`PMU_PCIE1_ISO`) is a plain MMIO read, which D12/doc 10 [obs] support for the neighbouring `0x17463EC8`; the summary's `link` field is only meaningful if `ELBI` is readable.

---

## 4. Staged plan

Rules for every stage with a write: AGENTS.md rules 1-3 hold (no storage partition touched; the user runs root/flash steps; only a device reporting `ro.product.device` = bluejay is
touched, and only with the shim, never by me in this task). Every stage ships as its own shim build flag first (`--wlan-bringup`), like `--gpu-probe`, so a hang stays attributable;
only after the shim result is [obs] does the logic move into an XNU patch. Effort is "driver-agent working days incl. CI/host tests", excluding user turnaround; a fail-and-iterate cycle on
hardware costs about one user-run each.

### Stage 0 - read-only probe (exists; fix D1-D4 first)

- **Entry:** fix D1, D2, D4 and extend the host test (D3) so the file is actually exercised; `tools/check-all.sh` green. Effort: 0.5 d.
- **Run:** `cd shim && ./build.sh --board bluejay --avb --usb-gadget --wlan-probe` -> `tools/fboot.sh --expect probe ...` -> `tools/usb-log.py capture` (HANDOFF.md:403 item 5).
- **Success line:** `WLAN SUMMARY en=0 perst=<0|1> dbi_id=<8 hex or unreadable> link=00` and no `PANIC`. Information goals: is `ELBI` readable at all (`pa=14520000.. v=`), what is `PMU_PCIE1_ISO`,
  is WLAN_EN low, is the PHY lock word zero. Expected (D12 1.4, [guess]): en=0, iso bit 0, link=00. A hang in level 2 is itself the result "HSI2 PCIe aperture needs more than domain-on" (then D1's gate matters).
- **Safety:** reads only. A bus hang is cleared by holding Power+VolDown / battery-less reset; nothing persists.

### Stage 1 - power and link training (shim experiment, writes)

- **Entry criteria:** stage 0 result recorded in HANDOFF; ELBI readable; user go-ahead for a write experiment (D12 4 step 6); `PMU_PCIE1_ISO` read; a decision on the GPL reference (D12 4 step 7);
  (a) EL3 SMC probe: first a **read-only** `get_priv_reg`-style SMC (`option 0`, [src: `pd_el3.c:54-63`]) of `0x17463EC4` [src: D12 1.2], then RMW of bit0 (one boot each) to learn whether EL3 accepts that register from EL1.
- **Steps (order per 2.1):** SMC isolation->bypass; keep WLAN_EN low and check the lock words; pinctrl CON writes (WLAN_EN output, PERST output, CLKREQ func 2); WLAN_EN high; wait the
  module turn-on delay (`WIFI_TURNON_DELAY`, value `TODO(verify)` in `dhd_custom_google.c`); PHY all-power-down-clear + `phy_config` tables (`cal.c:69,106-325`, re-derived); lock-word wait with a counter
  bound; ELBI soft-reset sequence; PERST high; 20 ms; LTSSM enable; bounded poll (e.g. 10 ms x 100) on `ELBI+0x2C8 & 0x3F == 0x11` [src: `rc.c:3233-3237`]. On timeout: LTSSM off, PERST low, WLAN_EN low, isolation back to 0 (reverse order), print the LTSSM
  state names from `common.h` and the PHY lock words, stop (no retries beyond a small fixed number).
- **Success line (proposed format):** `WLAN S1 link=11 (L0) ltssm=0x11 pll=1 cdr=1 oc=f t=<ms>` followed by `WLAN S1 teardown ok` (see below).
- **Failure lines worth keeping:** `WLAN S1 smc iso ret=<nonzero>` (EL3 refusal: blocker 1 confirmed, stop and re-plan around ACPM/EL3), `WLAN S1 rails?` (PHY lock never sets and WLAN_EN high: rails asleep, needs an ACPM client).
- **Rollback/safety:** all state is volatile (pinctrl, PMU bit, PCIe wrapper registers); a reset or power-cycle restores it. Risks: (1) a wrong SMC argument hanging EL3 or resetting the phone (recoverable with the buttons, as the
  SWRESET experiment was: HANDOFF.md:365 [obs]); (2) driving PERST/WLAN_EN into a chip with unpowered rails (a pin on a dead rail is harmless in the normal case, `TODO(verify)` against the schematic); (3) leaving the radio
  on and drawing power: always run the teardown at the end of the experiment. No UFS write, no partition touched: rule 1 unaffected.
- **Effort:** 3-5 d writing, then an unknown number of hardware iterations (PHY tables). Highest risk of the plan.

### Stage 2 - config space, BAR window, Broadcom firmware download

- **Entry:** stage 1 `link=11` [obs] and teardown verified.
- **Steps:** program the outbound iATU (config window `0x60FFE000`, then MEM window `0x60000000`, DWC-standard registers, Google sequence `TODO(verify)`); read endpoint config dword 0 and expect `0x14E4`/`0x4441`
  (or `0x4389` without OTP) [src: D12 1.1]; assign BAR0/BAR1 inside the MEM window (bus-master enable, memory enable); write `PCI_BAR0_WIN` (cfg `0x80`) = `0x18000000` and read BAR0+0 low 16 bits = chip id `0x4389` [src: D12 4 step 6, `pcicfg.h:488`, `hndsoc.h:43`, `bcmdevs.h:193`].
  Then the dongle protocol: halt the ARM core, download firmware into TCM through BAR1, append NVRAM at RAM end, release the core, find the shared structure, set up ring buffers (D12 3B item 2). Polled, no MSI at first [guess]. Firmware bytes come from the user's pulled copy placed in the iOS root (1.3),
  never committed; for the shim experiment from a gitignored file embedded into a local image only.
- **Success lines:** `WLAN S2 cfg vid=14E4 did=4441` ; `WLAN S2 chipid=4389 rev=<..>` ; `WLAN S2 fw up shared=<addr> console=<first line of dongle console>` ; `WLAN S2 ring h2d ctrl ok`.
- **Risks:** DMA ring memory must sit in a range the HSI2 S2MPU lets the PCIe stream ids reach (row 13): first test with a tiny DMA to the XNU bank, fall back to a carve-out. A wrong firmware/NVRAM pair (the `bcmdhd.cal` variant is chosen by module CID read from OTP/CIS: D12 1.3) makes the dongle crash: harmless but silent.
  Everything stays volatile; the firmware never touches the phone's flash.
- **Effort:** 6-10 d (the msgbuf protocol is the bulk; brcmfmac `pcie.c` 2795 + `msgbuf.c` 1722 + `firmware.c` 868 lines are the readable guide, D12 3B; 4389 may need newer ring/IPC revisions than brcmfmac knows: `TODO(verify)`).

### Stage 3 - IOKit nub and driver shape

- **Entry:** stage 2 firmware boots and answers an iovar over the control ring (e.g. a `cur_etheraddr`/`ver` read). Start the iOS side early and in parallel: items 1-2 below need no radio.
- **Shape (built-in, one patch family `0091+-sweet-wlan-*`):**
  1. ADT: `tools/mkadt.py` emits a `wlan` node (compatible chosen by us, device_type `wlan`) and `/chosen/mac-address-wifi0`, `/product wifi-chipset` for the real chip, from a generator table, not constants in code (AGENTS.md workflow; topaz does the same, `topaz tools/mkadt.py:184-188`).
     Without the node `wifid` is not started (topaz comment) and SpringBoard's `IONameMatch "wlan"` stays "none" [obs, `logs/qr-20261022T063754Z.txt:94`].
  2. `SweetWLAN : IOService` (provider = the ADT node) owning the controller; a stand-in class chain so `IOObjectConformsTo(controller,"IO80211Controller")` holds (D12 2.2: configd's `SCNetworkInterface.c:1780-1790,1887-1927`),
     an `IOEthernetInterface`-shaped object with `IOBSDName=en0`.
  3. `ifnet` `en0` (IFT_ETHER) whose `ioctl` callback answers `SIOCSA80211`/`SIOCGA80211` (0x802869c8 / 0xc02869c9, 0x28-byte `apple80211_ioctl_str`) [src: D12 2.2]; first version logs every `type` it is asked and returns `EOPNOTSUPP`
     (this lists the request set without a radio: D12 4 step 2), then answers the minimal set. Whether 16.7 requires a Skywalk-native interface is open; the dispatch path `ifioctl -> ifnet_ioctl` is in our tree [src: D12 2.2].
  4. Radio core: the stage-2 driver moved from the shim into `bsd/dev/sweet_bcmpcie.c`, exposing `scan`, `join(ssid, pmk|passphrase)`, `link state`, `tx`, `rx`, and event callbacks (the topaz API shape, `topaz_wlan_heard/join/joined`, `topaz patch 0126, 0129:1122-1175`).
- **Success lines:** `SWEET WLAN: en0 registered mac=<..>`; `IOK wifid: matching ... wlan -> 1`; `SWEET WLAN: ioctl type=<n> len=<n>` lines from `wifid` (first useful artefact: the request list); Settings shows a Wi-Fi row (photo/camera frame, [obs] only when taken).
- **Safety:** pure kernel software; failure = a hang or panic of an XNU boot, recoverable by the buttons. Keep a boot-arg (`sweetwlan=0`) to disable the whole thing, as `sweetaccel=0` does for the accelerator stub.
- **Effort:** 8-15 d (largest uncertainty is `wifid`'s required request/event set and any Skywalk/entitlement checks; AMFI is relaxed so entitlement checks should not bite, D12 2.2 [guess]).

### Stage 4 - association

- **Entry:** stage 3 `en0` visible to `wifid`; firmware control path works (iovars set/get).
- **Steps:** scan (iovar/ioctl scan + event decode), country/CLM load (`clmload` iovar, firmware reports the CLM version), join WPA2-PSK: either the firmware supplicant (`sup_wpa`, PMK hand-off: `wifid` hands over a ready PMK per topaz 0130:4,39) or our own 4-way handshake like topaz did in the kernel (topaz 0129:71); then DHCP through the normal
  stack on `en0`, ARP, a ping, then an HTTPS fetch. WPA3-SAE, 6 GHz, power save, MAC randomisation are out of scope initially (same limits topaz states: README.md:31).
- **Success lines:** `SWEET WLAN: scan done n=<..>`; `SWEET WLAN: assoc <ssid> bssid=<..> rssi=<..>`; `SWEET WLAN: 4way done`; `ping <gw>` reply; Settings shows the network joined (camera frame, with date + build id in the handoff).
- **Safety:** radio in a legal channel set only: load the CLM blob and the regulatory country before any transmit; do not enable 6 GHz or high-power modes; thermal is unmanaged (no TMU/ACPM thermal: `02-gs101-peripherals.md:342`), so keep duty cycles low and stop after bounded tests.
- **Effort:** 6-10 d after stage 3.

Total order of magnitude: 25-40 working days of agent effort, concentrated in stages 1-2 whose hardware iteration count is unknowable [guess]. Stage 3 items 1-3 can start now and run in QEMU (section 5), so the critical path is stage 0 -> 1.

---

## 5. Biggest blockers (restated with what would unblock them)

1. **EL3/ACPM power gate (stage 1).** Unblock by: stage-0 data; the SMC read/RMW trial for `0x17463EC4` [src: D12 1.2] (2 tiny boots); if rails are asleep, read `soc-gs: drivers/soc/google/acpm/` (not read yet, same open item as D17) and port a minimal ACPM client. Cost if it fails: the milestone is blocked until ACPM exists, exactly as the GPU.
2. **PHY/PCS tables + licence.** Unblock by: deciding the licence route before any port (D12 4 step 7); re-deriving from `cal.c:106-325` as constants in `soc/google/gs101/soc.json`-style tables (rule: tables not code constants; cite `cal.c` lines, rule "do not invent hardware constants"), and a single instrumented shim build that prints PHY lock words per step.
3. **Firmware + iOS facing layer.** Unblock by: user pulls the four vendor files (listing first); get the topaz glue source; mount the iOS root (user, root) to list `wifid`'s launchd entry and disassemble `wifid`/`MobileWiFi` for the `apple80211` request set (D12 4 step 1, REA offline).
4. (hardware-only) HSI2 S2MPU policy for the PCIe streams (row 13).

---

## 6. What can be tested on the host or in QEMU first

| What | How | Gives |
|---|---|---|
| Probe formatting, fault path, SUMMARY decode (D3) | extend `shim/wlanprobe_host_test.c` to build `wlanprobe.c` against a fake register window and a fake faulting `try_read32` | catches buffer/format bugs; runs inside `tools/check-all.sh` |
| Stage-1 sequencer logic | a pure-C state machine (`wlan_bringup_step(state, regs_ops)`) with an ops table, host-tested against a scripted fake RC: lock words appear after N reads, LTSSM walks `detect -> polling -> ... -> L0` (state names in `common.h`), timeouts and teardown order asserted | proves bounded waits, abort and reverse-order teardown before touching hardware (shim/AGENTS.md rule: every wait counter-bounded) |
| PHY table generator | `tools/gen-ufsphy-tables.py`-style generator from `cal.c` into `soc/google/gs101/shim/` header; unit test compares counts and offsets | removes hand-copy errors (rule: generator over copied constants) |
| Broadcom msgbuf rings, firmware download state machine | host test with a fake endpoint (BAR0 window, TCM array, shared struct, ring read/write pointers) | most of stage 2's logic without any radio; mainline brcmfmac `pcie.c`/`msgbuf.c` is the layout reference [src: `linux`] |
| Config-space / iATU / BAR assignment | QEMU `virt` already provides a PCIe ECAM host; a QEMU PCIe endpoint (e.g. a virtio or `edu` device) validates the *generic* enumeration and BAR math code path | generic PCI code only; DWC/Exynos specifics (ELBI, PHY) are not modelled: label this [guess] until tried |
| ADT `wlan` node, `mac-address-wifi0`, `wifi-chipset` | `tools/mkadt.py` unit test in the style of `tools/test_mkadt_usb.py` / `test_mkadt_touch.py` | stage 3 item 1 |
| iOS-facing layer: `en0` ifnet, ioctl logger, IOKit stand-ins, `wifid` start | the sweet QEMU flow (`tools/qemu-sweet.py`, docs/QEMU_TESTING.md) with a fake radio core returning canned scan results and "associated" | D12 A2 as test harness: shows whether `wifid` starts, which `apple80211` types it requests, and whether Settings row appears, with **no phone**. Note: QEMU boots the sweet identity (SM7150 ADT), so gate on the ADT `wlan` node, not board flags. |
| Skywalk requirement question | grep config in CI buildtree (`xnu-buildtree` artifact) and QEMU boot with an ioctl logger | answers "does `wifid` need a Skywalk netif" |

Caveat: none of these exercises real EL3, PHY, link training or the S2MPU; those are the blockers and stay hardware-only.

---

## 7. Open items (all `TODO(verify)`)

- Run `--wlan-probe` (after D1/D2 fixes) and record `WLAN SUMMARY` in HANDOFF.md.
- Vendor file names (`adb shell ls /vendor/firmware /vendor/etc/wifi`); module CID/NVRAM variant for the 6a.
- Mount the iOS root (user, root) and list `wifid` launchd entry, daemons, and `MobileGestalt` Wi-Fi keys; disassemble `wifid` for the `apple80211` set.
- Ask for the topaz glue between `wifid` and `topaz_wlan_*`.
- CMU_HSI2 gate bits for PCIe1; Google iATU/I-A sequence; `WIFI_TURNON_DELAY`; GIC SPI for `IRQ_PCIE_GEN4A_1_HSI2`; HSI2 S2MPU policy for stream ids 4/5.
- `acpm` IPC protocol (`soc-gs: drivers/soc/google/acpm/`).
- Whether `wifid` on 16.7 insists on a Skywalk interface.

## 8. Verification of this document's facts

[src] rows for `soc-gs` were re-fetched on 2026-10-22 (`rc.c`, `rc.h`, `common.h`, `pd_el3.c`, `cal.c` line counts rc.c 5571, rc.h 393, common.h 388, cal.c 489; the `rc.c` line numbers above are from those copies) and for `linux` from raw GitHub master the same day. Facts attributed to D12/D17 were not re-fetched unless stated in 2.1. [obs] rows cite repo logs with dates. Nothing in this document is an [obs] about the Wi-Fi hardware itself: no WLAN register has ever been read on the phone.

## 3a. W1 hardware result (2026-10-23, run w1, `logs/probe/20261023T015314Z-w1_`, `--wlan-probe` after fixes D1-D5, read-only)
*Observed on hardware.* `PMU_PCIE1_ISO (0x17463EC4)=0`, `PMU_PCIE0_ISO (0x17463EC0)=0`: level 2 correctly **gated** (`gate=pcie1-phy-isolated`), nothing behind the PHY was read, no hang. GPIOs: `WLAN_EN` gpp14-2 func=0 lvl=0 (CON 0x33, DAT 0x3), `PERST` gph2-0 func=0 lvl=0, `CLKREQ` gph2-1 lvl=1, `DEVWAKE` lvl=0, `HOSTWAKE` gpa7-0 lvl=0 (CON 0). `PD_HSI0_CFG/STAT=1/1`, `PD_HSI2_A (+4)=1/1` (not a real status word, see D5). Reading (guess): the BCM4389 is powered down (WLAN_EN low) and its PCIe1 PHY is isolated; bring-up must (1) raise WLAN_EN (and whatever supplies it), (2) release the PHY isolation (PMU alive write: probably EL3 `SMC_CMD_PRIV_REG`, research 17 section 2.2), (3) clock/reset/PHY init, (4) PERST release and link training. Everything in this chain is a writer: Opus review per step.

**W2 (2026-10-23):** the complete cited power-up sequence to L0, the EL3 write, the rail/clock reads and the staged writers are in `docs/research/29-wlan-stage1-design.md`. It corrects row 4 above: L17M/L19M are the SoC PCIe1 *PHY* supplies, not the BCM4389 supply, and the Linux driver never touches a regulator.

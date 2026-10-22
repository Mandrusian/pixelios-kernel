# Pixel 6a (bluejay / gs101) Peripheral and Hardware Research

Desk research only; no device commands were run.
- **VERIFIED-FROM-SOURCE** means a cited source (mainline Linux DT/drivers, Android gs101/bluejay trees, Linaro patch series) directly verifies the concrete address, register, or behavior.
- **INFERRED** means a deduction based on SoC architecture, shared Exynos/Tensor IP blocks, or driver handoff experience from the upstream `ios-sweet` reference port.
- Driver reuse estimates categorize how much of the existing `ios-sweet` code can be leveraged: **[reuse]**, **[adapt]**, or **[rewrite]**.

---

## Findings and Reuse Summary at a Glance

| Subsystem | gs101 / bluejay Hardware Summary | ios-sweet (SM7150) Equivalent | Reuse Estimate | Key Divergence / Action Needed |
| :--- | :--- | :--- | :---: | :--- |
| **1. CPUs** | Tri-cluster: 4x A55 + 2x A76 + 2x X1; ARMv8.2-A; no PAC; 16K granule supported | Dual-cluster: 6x A55 + 2x A76; ARMv8.2-A; no PAC | **[adapt]** | Update cluster topology to 4+2+2; adapt MPIDR maps; pure `arm64` build. |
| **2. GIC** | ARM GICv3; GICD `0x10400000`, GICR `0x10440000` (128 KB stride); Group 1 only | GICv3; GICD `0x17a00000`, GICR `0x17a60000` | **[reuse]** | Rebase register addresses; Group 1 logic from ios-sweet is 100% compatible. |
| **3. Arch Timer** | 24.576 MHz (`ext_24_5m`); standard ARM PPIs: Virt = PPI 11 (INTID 27) | 19.2 MHz; Qualcomm rewired Virt = PPI 3 (INTID 19) | **[adapt]** | Revert sweet's INTID 19 hack back to standard 27; update tick frequency. |
| **4. PSCI** | PSCI 1.0 via SMC; CPU_ON `0xc4000003`; requires PMU inform + `pmu-intr-gen` | PSCI 0.2/1.0 via SMC; standard CPU_ON | **[adapt]** | Clear `GS101_CPU_INFORM` and poke `pmu-intr-gen` before calling PSCI `CPU_ON`. |
| **5. UFS** | UFSHCI base `0x14700000`; `UFSHCD_QUIRK_PRDT_BYTE_GRAN`; FMP inline crypto; S2MPU | UFSHCI base `0x1d84000`; standard DWORD PRDT; ICE crypto | **[adapt]** | Adapt PRDTO/PRDTL for byte granularity; verify 16B vs 128B PRDT; bypass FMP. |
| **6. Display** | Samsung S6E3FC3 6.1" OLED (1080x2400); bootloader splash FB at `0xfac00000` | Tianma/Novatek 120Hz OLED (1080x2400); splash FB at `0x9c000000` | **[reuse]** | Keep simple framebuffer CPU-copy; update base address, pitch, and dimensions. |
| **7. Touch** | STMicroelectronics FTM5 over USI SPI; PMIC regulators via ACPM | FocalTech FT8719 over GENI I2C; RPMh votes | **[rewrite]** | Must write Exynos USI SPI controller driver and STM FTS protocol driver. |
| **8. Buttons** | Vol-, Vol+, Power wired to SoC Alive GPIOs (`gpa7-3`, `gpa8-1`, `gpa10-1`) | Vol keys on SoC GPIO; Power key on PMIC PON over SPMI | **[adapt]** | Much simpler: replace PMIC SPMI PON driver with plain Alive GPIO register reads. |
| **9. USB Gadget** | Synopsys DWC3 at `0x11110000`; `gs101-usb31drd-phy` at `0x11100000` | Synopsys DWC3 at `0x04e00000`; QMP/QUSB2 PHYs | **[adapt]** | DWC3 core register programming & CDC-ECM gadget are reused; adapt PHY init. |
| **10. PMIC / Battery**| S2MPG10/11 via ACPM mailbox (`0x17440000`); MAX1720x FG; MAX77759 charger | PM6150/PM6150L over SPMI; SMB1390 charger | **[reuse]** / **[rewrite]** | Reuse sweet's dummy battery stub for bringup; full PMIC requires ACPM IPC. |
| **11. System Risks** | Watchdog CL0 at `0x10060000` resets in 30s; heavy thermal load; S2MPU DMA isolation | Watchdog pet needed; moderate thermal load | **[adapt]** | Disable `watchdog_cl0` at shim entry; execute WFI on idle; avoid TZ DMA carveouts. |

---

## 1. CPUs and Architecture

### Cluster Layout and Topology
- **VERIFIED-FROM-SOURCE:** Google Tensor GS101 features an 8-core, tri-cluster CPU configuration:
  - **Cluster 0 (Little Cores - "Ananke"):** 4x ARM Cortex-A55 (`cpu0`–`cpu3`). In `gs101.dtsi`, DT nodes `cpu@0`, `cpu@100`, `cpu@200`, `cpu@300` with `reg = <0x0000>`, `<0x0100>`, `<0x0200>`, `<0x0300>`.
  - **Cluster 1 (Mid Cores - "Enyo"):** 2x ARM Cortex-A76 (`cpu4`–`cpu5`). In `gs101.dtsi`, DT nodes `cpu@400`, `cpu@500` with `reg = <0x0400>`, `<0x0500>`.
  - **Cluster 2 (Big Cores - "Hera"):** 2x ARM Cortex-X1 (`cpu6`–`cpu7`). In `gs101.dtsi`, DT nodes `cpu@600`, `cpu@700` with `reg = <0x0600>`, `<0x0700>`.
  *(Sources: Mainline Linux `arch/arm64/boot/dts/exynos/google/gs101.dtsi`, lines 30–170; Linaro GS101 patch series).*

### MPIDR Values and MT Bit
- **VERIFIED-FROM-SOURCE:** In GS101's DynamIQ topology, secondary cores are addressed by Affinity 1 in the device tree:
  - `cpu0`: `0x0000` (Aff1 = 0)
  - `cpu1`: `0x0100` (Aff1 = 1)
  - `cpu2`: `0x0200` (Aff1 = 2)
  - `cpu3`: `0x0300` (Aff1 = 3)
  - `cpu4`: `0x0400` (Aff1 = 4)
  - `cpu5`: `0x0500` (Aff1 = 5)
  - `cpu6`: `0x0600` (Aff1 = 6)
  - `cpu7`: `0x0700` (Aff1 = 7)
- **VERIFIED-FROM-SOURCE:** The **MT (Multi-Threading)** bit (bit 24 of `MPIDR_EL1`) is **`0`** across all cores. Cortex-A55, Cortex-A76, and Cortex-X1 are single-threaded execution cores (1 hardware PE per core, no simultaneous multithreading).
  *(Sources: ARM Cortex-A55 / Cortex-A76 / Cortex-X1 Technical Reference Manuals, MPIDR_EL1 register descriptions).*

### 16K Translation Granule Support
- **VERIFIED-FROM-SOURCE:** All three CPU core microarchitectures implemented in GS101 support 4 KiB, 16 KiB, and 64 KiB page translation granules:
  - `ID_AA64MMFR0_EL1.TGran16` (bits [23:20]) reports `0b0001` (16 KiB granule supported at EL1).
  - Apple's XNU kernel hard-requires a 16 KiB page size (`PAGE_SIZE = 16384`, `PAGE_SHIFT = 14`). GS101 fully satisfies this requirement natively in hardware.
  *(Sources: ARM DDI 0487 Architecture Reference Manual; ARM Cortex-A55/A76/X1 TRMs).*

### ARM Architecture Features and PAC
- **VERIFIED-FROM-SOURCE:**
  - Cortex-A55 implements **ARMv8.2-A**.
  - Cortex-A76 implements **ARMv8.2-A**.
  - Cortex-X1 implements **ARMv8.2-A** with selected ARMv8.3/v8.4 extensions (e.g., dot product).
- **VERIFIED-FROM-SOURCE:** Pointer Authentication (**PAC** / `FEAT_PAuth`) is **not supported** across the little and mid cores (A55 and A76 are ARMv8.2-A; PAC was introduced in ARMv8.3-A). Consequently, GS101 runs in non-secure EL1 without Pointer Authentication.
- **INFERRED:** The XNU kernel build must target plain `arm64` (equivalent to Apple A10 / iPhone 7 target), exactly as configured in `ios-sweet`, and must **not** target `arm64e`.

### Driver Reuse Assessment
- **[adapt]**: `ios-sweet` patch `0018-sweet-boot-cpu-driver.patch` (`SweetCPU`) and patch `0065-sweet-smp-psci.patch` can be reused in structure, but the core table and cluster topology must be adapted from sweet's 6+2 dual-cluster layout to gs101's 4+2+2 tri-cluster layout.

---

## 2. Generic Interrupt Controller (GIC)

### Architecture and Registers
- **VERIFIED-FROM-SOURCE:** GS101 integrates an ARM **GICv3** implementation:
  - Node: `gic: interrupt-controller@10400000` (`compatible = "arm,gic-v3"`)
  - **GICD (Distributor) Base:** `0x10400000`, size `0x10000` (64 KiB)
  - **GICR (Redistributors) Base:** `0x10440000`, size `0x100000` (1 MiB)
  - **Redistributor Stride:** `0x20000` (128 KiB per PE: 64 KiB `RD_base` + 64 KiB `SGI_base`). 8 PEs consume `8 * 128 KiB = 1 MiB`, exactly matching the DT reservation.
  - Maintenance Interrupt: `GIC_PPI 9 IRQ_TYPE_LEVEL_HIGH` (INTID 25).
  *(Sources: Mainline Linux `gs101.dtsi`, lines 656–664).*

### Security Configuration (Group 0 vs Group 1)
- **VERIFIED-FROM-SOURCE / INFERRED:** On GS101, ARM TrustZone security extensions are enabled (`SecurityExtn=1`, `GICD_CTLR.DS == 0`).
  - **Group 0** interrupts are reserved strictly for the Secure World (EL3 / TrustZone firmware). Any attempt by Non-Secure EL1 (XNU/Linux) to access Group 0 control registers (`ICC_IGRPEN0_EL1`, `ICC_IAR0_EL1`, `ICC_EOIR0_EL1`) results in an undefined instruction trap.
  - **Group 1 Non-Secure** interrupts must be used exclusively by Non-Secure EL1:
    - Interrupt enable: `ICC_IGRPEN1_EL1 = 1`
    - Acknowledge: Read `ICC_IAR1_EL1`
    - End-of-Interrupt: Write `ICC_EOIR1_EL1`
    - Priority filter: `ICC_PMR_EL1 = 0xff`
    - Configuration: `GICD_IGROUPR<n>` and `GICR_IGROUPR0` must mark target peripheral interrupts as Non-Secure Group 1.

### GS101 GIC Quirks
- **VERIFIED-FROM-SOURCE:** GS101 adheres to standard ARM GICv3 architecture bindings:
  - In Device Tree, SPI interrupts are specified relative to 0 (hardware INTID = `SPI_ID + 32`).
  - PPI interrupts are specified relative to 0 (hardware INTID = `PPI_ID + 16`).
  - No non-standard Qualcomm-style register remapping or vendor register banking is present.

### Driver Reuse Assessment
- **[reuse]**: `ios-sweet`'s bare-metal shim (`shim/shim.c`) and XNU GIC driver (`AppleGICv3` / `SweetInterruptController`) were specifically rewritten to interact exclusively with GICv3 Group 1 system registers. Only the MMIO base addresses (`GICD: 0x10400000`, `GICR: 0x10440000`) and the 128 KiB redistributor stride need to be updated.

---

## 3. Architected Timer (Arch Timer)

### Frequency
- **VERIFIED-FROM-SOURCE:** The ARM generic counter frequency (`CNTFRQ_EL0`) on GS101 is:
  - **`24.576 MHz`** (`24,576,000 Hz`, period `~40.69 ns`).
  - Sourced from the primary system oscillator clock `ext_24_5m` (`oscclk` / `CLK_GOUT_MISC_MCT_PCLK`).
  *(Sources: Mainline Linux `gs101-pixel-common.dtsi`, line 628; Linux dmesg logs for GS101: `arch_timer: cp15 timer(s) running at 24.57MHz`).*
  - *Contrast with SM7150:* SM7150 used a 19.200 MHz system timer.

### PPI Interrupt Assignments
- **VERIFIED-FROM-SOURCE:** The `timer` node in `gs101.dtsi` adheres strictly to standard ARM GIC architecture assignments:
  - **Secure Physical Timer:** `GIC_PPI 13` -> INTID 29 (`16 + 13`)
  - **Non-Secure Physical Timer:** `GIC_PPI 14` -> INTID 30 (`16 + 14`)
  - **Virtual Timer:** `GIC_PPI 11` -> **INTID 27** (`16 + 11`)
  - **Hypervisor Physical Timer:** `GIC_PPI 10` -> INTID 26 (`16 + 10`)
  - **Hypervisor Virtual Timer:** `GIC_PPI 12` -> INTID 28 (`16 + 12`)
  *(Sources: Mainline Linux `gs101.dtsi`, lines 1930–1940; ARM Generic Timer specification).*
  - *Contrast with SM7150:* On SM7150, Qualcomm non-standardly wired the virtual timer to PPI 3 (INTID 19), forcing `ios-sweet` patch `0017-sweet-arm-timer.patch` to override XNU's default. On GS101, standard ARM INTID 27 is preserved.

### Driver Reuse Assessment
- **[adapt]**: Revert the SM7150-specific INTID 19 override in `ios-sweet` back to standard INTID 27, and update the clock frequency calculation constant from 19,200,000 Hz to 24,576,000 Hz.

---

## 4. Power State Coordination Interface (PSCI)

### Conduit and Calling Conventions
- **VERIFIED-FROM-SOURCE:** GS101 implements PSCI v1.0 using the SMC calling convention:
  - Node: `psci { compatible = "arm,psci-1.0"; method = "smc"; };`
  - All CPU nodes specify: `enable-method = "psci";`
  - **`PSCI_CPU_ON_64`:** `0xc4000003` (arguments: `X1 = target_mpidr`, `X2 = entry_point_physical`, `X3 = context_id`)
  - **`PSCI_CPU_OFF`:** `0x84000002`
  - **`PSCI_SYSTEM_RESET`:** `0x84000009`
  - **`PSCI_SYSTEM_OFF`:** `0x84000008`
  *(Sources: Mainline Linux `gs101.dtsi`, lines 525–530; ARM PSCI Specification).*

### The GS101 CPU Hotplug / `pmu-intr-gen` Requirement
- **VERIFIED-FROM-SOURCE:** In mainline Linux upstreaming by Peter Griffin (Linaro), invoking standard `PSCI_CPU_ON` on secondary cores causes the system to hang unless two specific hardware register sequences are performed:
  1. **PMU CPU Inform Register (`GS101_CPU_INFORM`):**
     - Base address: `0x17460000` (`pmu_system_controller: system-controller@17460000`)
     - Register: `GS101_CPU_INFORM(cpu) = 0x17460000 + 0x860 + (cpu * 4)`
     - Before onlining a CPU: write `CPU_INFORM_CLEAR (0)` to this register. The EL3 secure firmware (`el3mon`) reads this hint during PSCI processing.
  2. **PMU Interrupt Generator (`pmu_intr_gen`):**
     - Base address: `0x17470000` (`pmu_intr_gen: syscon@17470000`)
     - Registers:
       - `GS101_GRP2_INTR_BID_ENABLE = 0x0200`
       - `GS101_GRP2_INTR_BID_UPEND = 0x0208`
       - `GS101_GRP2_INTR_BID_CLEAR = 0x020c`
     - Onlining sequence:
       - Clear mask bit `(1 << cpu)` in `GS101_GRP2_INTR_BID_ENABLE` (`0x17470200`).
       - Read pending bits from `GS101_GRP2_INTR_BID_UPEND` (`0x17470208`).
       - Clear pending bits by writing `reg & (1 << cpu)` to `GS101_GRP2_INTR_BID_CLEAR` (`0x1747020c`).
  *(Sources: Linux patch series `[PATCH v2 5/5] soc: samsung: exynos-pmu: enable CPU hotplug support for gs101`, `drivers/soc/samsung/exynos-pmu.c`, `include/linux/soc/samsung/exynos-regs-pmu.h`).*

### System Reset
- **VERIFIED-FROM-SOURCE:** Clean reboot is initiated via standard `PSCI_SYSTEM_RESET` (`smc(0x84000009, 0, 0, 0)`). GS101 also supports reboot reason signaling via `nvmem-reboot-mode` in PMU registers (`0x17460000`).

### Driver Reuse Assessment
- **[adapt]**: In `ios-sweet`, SMP bringup in `shim/shim.c` directly issued `PSCI_CPU_ON_64`. For GS101, this function must be adapted to write `0` to `GS101_CPU_INFORM(cpu)` and clear the corresponding bits in `pmu_intr_gen` before triggering the SMC.

---

## 5. Universal Flash Storage (UFS)

### Host Controller Addresses and Layout
- **VERIFIED-FROM-SOURCE:** The GS101 primary storage is a UFS 3.1 host controller:
  - Node: `ufs_0: ufs@14700000` (`compatible = "google,gs101-ufs"`)
  - **HCI (Standard UFSHCI Registers):** `0x14700000`, size `0x200`
  - **Vendor HCI (`vs_hci`):** `0x14701100`, size `0x200`
  - **UniPro Registers (`unipro`):** `0x14780000`, size `0xa000`
  - **UFS Protection Glue (`ufsp`):** `0x14600000`, size `0x100`
  - **PHY:** `ufs_0_phy: phy@14704000` (`compatible = "google,gs101-ufs-phy"`)
  - **Interrupt:** `GIC_SPI 532 IRQ_TYPE_LEVEL_HIGH` (INTID `532 + 32 = 564`)
  - **DMA:** Marked `dma-coherent;` in DT.
  *(Sources: Mainline Linux `gs101.dtsi`, lines 1775–1810).*

### Deviations from Standard UFSHCI
1. **`UFSHCD_QUIRK_PRDT_BYTE_GRAN`:**
   - **VERIFIED-FROM-SOURCE:** Standard UFSHCI specifies that PRDTO (PRDT Offset in the UTRD) is measured in units of 32-bit DWORDs (4 bytes), and PRDTL is the segment count. Samsung Exynos/GS101 UFSHCI interprets PRDTO with **byte granularity** and PRDTL as the **total byte length** (`sg_segments * sizeof(sg_entry)`).
   - In `ios-sweet`'s `shim/ufs.c`, `utrl[0].prdt = (1024u / 4) << 16 | 1;`. On GS101, this must be adjusted to byte units.
2. **Vendor HCI Registers (`vs_hci` at offset `0x1100`):**
   - **VERIFIED-FROM-SOURCE:** Controls automatic clock gating (`HCI_IOP_ACG_DISABLE`), IO coherency shareability (`sysreg_hsi2` offset `0x710`), and proprietary UniPro DME timeout settings.
3. **Flash Memory Protector (FMP / Inline Crypto):**
   - **VERIFIED-FROM-SOURCE:** GS101 does not implement standard UFSHCI 2.1 keyslots. Instead, inline hardware encryption keys and metadata are embedded directly inside extended PRDT scatter-gather descriptors (`struct fmp_sg_entry`, 128 bytes total: 16-byte base + 112-byte crypto metadata).
   - Enabling FMP requires an EL3 secure monitor call `SMC_CMD_FMP_SECURITY` (`0x1810`, `CFG_DESCTYPE_3`).
   - If FMP is not activated (or in bypass mode), transfers use standard 16-byte PRDT descriptors or bypass descriptors with high bits of the size field cleared.
4. **IOMMU, SysMMU, and S2MPU:**
   - **VERIFIED-FROM-SOURCE:** In mainline Linux, `ufs_0` has **no SysMMU / IOMMU phandle**; it operates with direct physical DMA (`dma-coherent`).
   - **VERIFIED-FROM-SOURCE:** However, Samsung's Stage 2 Memory Protection Unit (**`s2mpu_hsi2`**) sits between the HSI2 bus and the memory interconnect. S2MPU enforces strict physical address boundaries. If UFS DMA targets a protected carveout (e.g., GSA `0x90200000`, AOC `0x94000000`, Modem `0xf0000000`), S2MPU triggers an access violation fault. DMA to ordinary non-secure DRAM (`0x80000000`+) is permitted.

### Bootloader Handoff Status
- **VERIFIED-FROM-SOURCE / INFERRED:** Bluejay's Android Bootloader (ABL) boots directly from UFS. When ABL branches to the kernel / shim, the UFS host controller is active, clocks and power rails (`vcc_ufs`, `vccq_ufs`) are powered, and the UniPro M-PHY link is established in high-speed gear.
- As in `ios-sweet`, a minimal polled driver does not need to re-execute UniPro link negotiation or PHY training; it can immediately submit SCSI READ(10) / READ(16) commands to UFS LUNs.

### Driver Reuse Assessment
- **[adapt]**: `ios-sweet`'s polled UFS driver (`shim/ufs.c`) and XNU block driver (`SweetUFS`) can be reused after adapting the base address (`0x14700000`), adjusting PRDTO/PRDTL for byte granularity, ensuring DMA buffers are allocated in normal DRAM away from S2MPU-protected regions, and stripping Qualcomm ICE registers.

---

## 6. Display Subsystem and Framebuffer

### Display Processing Unit (DPU) and Panel
- **VERIFIED-FROM-SOURCE:** GS101 integrates the Samsung Exynos Display Processing Unit (DPU / DECON).
- **VERIFIED-FROM-SOURCE:** Pixel 6a (bluejay) features a **6.1-inch FHD+ AMOLED** panel:
  - Resolution: **1080 x 2400** pixels, 60 Hz refresh rate.
  - DDIC (Display Driver IC): Samsung **S6E3FC3**.
  - Driver module: `panel-samsung-s6e3fc3_6a.ko`.
  *(Sources: Android bluejay kernel module load configs `vendor_dlkm.modules.load`; Google bluejay device specifications).*

### Bootloader Simple Framebuffer Handoff
- **VERIFIED-FROM-SOURCE:** Bluejay's bootloader sets up a continuous splash framebuffer in DRAM:
  - Reserved memory node: `cont_splash_mem: splash@fac00000` (`reg = <0x0 0xfac00000 ...>`).
  - Chosen framebuffer node: `framebuffer-0` (`compatible = "simple-framebuffer"`, `memory-region = <&cont_splash_mem>`).
  - Power rails: `s2mpg10_ldo22m` (VCI) and `s2mpg11_bucka` (VDDI).
  *(Sources: Mainline Linux `gs101-pixel-common.dtsi`, lines 24–35, 78–85).*
- **INFERRED:** When ABL displays the Google splash logo, the DPU scanout engine is already programmed to fetch raster data continuously from `0xfac00000`. As long as the shim and XNU do not reset the DPU or gate its clocks, CPU writes to physical address `0xfac00000` immediately appear on screen.
- This replicates the exact strategy used in `ios-sweet` on SM7150 (CPU copy into bootloader framebuffer without GPU acceleration).

### Driver Reuse Assessment
- **[reuse]**: `ios-sweet`'s `SweetFramebuffer` / `AppleCLCD` stub is 100% architecturally reusable. Only the physical address (`0xfac00000`), dimensions (1080 x 2400), and pitch (`1080 * 4 = 4320` bytes) need to be supplied via Device Tree or boot arguments.

---

## 7. Touchscreen Controller

### Hardware Identity and Bus Wiring
- **VERIFIED-FROM-SOURCE:** The Pixel 6a (bluejay) touchscreen controller is:
  - Part: STMicroelectronics **FTM5** (FingerTipS series).
  - Driver: `fts_touch` / `ftm5.ko`.
  - Device Tree binding: `gs101-fts-touch.dtsi`.
  *(Sources: AOSP bluejay kernel tree `kernel/google-modules/touch/fts_touch`; `device/google/bluejay-kernel/vendor_dlkm.modules.load`).*
- **VERIFIED-FROM-SOURCE:** Bus Interface:
  - Connected via **High-Speed SPI** over an Exynos USI (Universal Serial Interface) block, **not I2C**.
  - Contrast with `ios-sweet`: The Redmi Note 10 Pro touch controller (FocalTech FT8719) was connected via I2C over Qualcomm GENI. Bluejay uses an SPI bus.

### Power Rails and Interrupts
- **VERIFIED-FROM-SOURCE:**
  - Power rails: `vci` (AVDD, ~3.0V–3.3V) and `vddi` (VDDIO, 1.8V), provided by the primary PMICs (S2MPG10/S2MPG11).
  - Interrupt: Routed via external interrupt line (EINT) to SoC Alive GPIO.

### Bare-Metal Bringup Complexity
- **INFERRED:** Bare-metal touch bringup on bluejay is significantly more complex than on sweet:
  1. An Exynos USI SPI controller driver must be implemented in XNU from scratch (clocking, FIFO control, pinmux).
  2. The STMicroelectronics FTM5 SPI protocol (event buffer parsing, multi-touch report packets, sync commands) must be implemented.
  3. If ABL shuts down touch rails before jumping to the payload, the PMIC regulators must be activated via ACPM IPC.

### Driver Reuse Assessment
- **[rewrite]**: Neither the Qualcomm GENI I2C bus driver (`SweetGENI`) nor the FocalTech touch driver (`SweetTouchController`) from `ios-sweet` can be reused. Both the SPI bus master driver and the FTM5 touch controller driver must be written from scratch.

---

## 8. Hardware Buttons

### Button Wiring and GPIO Mapping
- **VERIFIED-FROM-SOURCE:** In `gs101-pixel-common.dtsi`, all hardware buttons are mapped as standard `gpio-keys` directly to SoC Alive GPIO pins:
  - **Volume Down (`KEY_VOLUMEDOWN`):** `gpios = <&gpa7 3 GPIO_ACTIVE_LOW>` (pin `gpa7-3`, function `GS101_PIN_FUNC_EINT`)
  - **Volume Up (`KEY_VOLUMEUP`):** `gpios = <&gpa8 1 GPIO_ACTIVE_LOW>` (pin `gpa8-1`, function `GS101_PIN_FUNC_EINT`)
  - **Power Button (`KEY_POWER`):** `gpios = <&gpa10 1 GPIO_ACTIVE_LOW>` (pin `gpa10-1`, function `GS101_PIN_FUNC_EINT`)
  - Alive GPIO Controller: `pinctrl_gpio_alive: pinctrl@174d0000`, size `0x1000`.
  *(Sources: Mainline Linux `gs101-pixel-common.dtsi`, lines 37–60, 800–850).*

### Comparison with SM7150
- On SM7150 (`ios-sweet`), the power button was wired to the PMIC's internal PON (Power-On) peripheral and required an SPMI bus driver and PMIC register decoder.
- On GS101, **the power button is wired directly to SoC GPIO `gpa10-1`**. No SPMI or PMIC communication is required to read button states.

### Driver Reuse Assessment
- **[adapt]**: `ios-sweet`'s `SweetButtons` driver can be simplified. The SPMI PMIC PON polling logic is replaced with direct MMIO reads of the Alive GPIO data register at `0x174d0000`.

---

## 9. USB Device Mode (Gadget and CDC-ECM)

### Hardware Controller and PHY
- **VERIFIED-FROM-SOURCE:** GS101 integrates a Synopsys DesignWare USB3 (DWC3) core:
  - SoC Wrapper Node: `usbdrd31: usb@11110000` (`compatible = "google,gs101-dwusb3"`)
  - DWC3 Core Node: `usbdrd31_dwc3: usb@0` (`compatible = "snps,dwc3"`, registers `0x11110000 0x10000`)
  - Interrupt: `GIC_SPI 463 IRQ_TYPE_LEVEL_HIGH` (INTID `463 + 32 = 495`)
  - Clocks: Sourced from `cmu_hsi0` (`bus_early`, `susp_clk`, `link_aclk`, `link_pclk`, `ref`).
  - **PHY Node:** `usbdrd31_phy: phy@11100000` (`compatible = "google,gs101-usb31drd-phy"`), with register banks:
    - `phy`: `0x11100000`, size `0x200`
    - `pcs`: `0x110f0000`, size `0x800`
    - `pma`: `0x110e0000`, size `0x2800`
    - PMU control registers: `GS101_PHY_CTRL_USB20` (`0x17463EB0`) and `GS101_PHY_CTRL_USBDP` (`0x17463EB4`).
  - Role configuration: `dr_mode = "otg"`, `role-switch-default-mode = "peripheral"`.
  *(Sources: Mainline Linux `gs101.dtsi`, lines 1673–1725; `gs101-pixel-common.dtsi`, lines 860–895).*

### Reuse of `ios-sweet` DWC3 Gadget and CDC-ECM
- **VERIFIED-FROM-SOURCE / INFERRED:** In `ios-sweet`, patches `0072`–`0077` implement a polled Synopsys DWC3 USB gadget in XNU (`DWC3Controller` and `SweetUSBEthernetGadget`), establishing an IP-over-USB network interface (CDC-ECM).
  - Because both Qualcomm SM7150 and Google GS101 embed standard Synopsys DWC3 controller IP, the core DWC3 register offsets (offset `0xC100`+, `DCFG`, `DCTL`, `DEPCMD`, `GEVNTBUF`), descriptor ring formats (TRBs), and CDC-ECM class descriptors are **100% identical**.
  - The only platform-dependent component is the PHY and clock setup. If ABL hands off with the USB PHY clocked (e.g., in fastboot mode), minimal PHY initialization is needed.

### Driver Reuse Assessment
- **[adapt]**: The entire polled DWC3 gadget stack and CDC-ECM network driver can be directly reused. Adaptations are confined to mapping the GS101 base address (`0x11110000`), configuring the `cmu_hsi0` clocks, and implementing the `gs101-usb31drd-phy` tuning sequence if cold bringup is needed.

---

## 10. Battery, Fuel Gauge, and PMIC Access

### Power Management Architecture
- **VERIFIED-FROM-SOURCE:** The GS101 power subsystem consists of multiple chips:
  - **Master PMIC:** Samsung **S2MPG10**
  - **Slave PMIC:** Samsung **S2MPG11**
  - **Companion IF-PMIC & Charger:** Maxim **MAX77759** (I2C)
  - **Battery Fuel Gauge:** Maxim **MAX1720x** series (I2C)
  *(Sources: Mainline Linux `gs101-pixel-common.dtsi`; AOSP `kernel/google-modules/bms`).*

### ACPM (Active Clock and Power Management) Coprocessor
- **VERIFIED-FROM-SOURCE:** On GS101, the Application Processor (AP) **does not have direct MMIO or I2C access to the primary PMICs (S2MPG10/S2MPG11)**:
  - S2MPG10 and S2MPG11 are connected over Samsung's proprietary SPEEDY serial bus, which is owned exclusively by the **ACPM coprocessor** (embedded Cortex-M controller).
  - The AP communicates with ACPM via:
    - **Mailbox:** `ap2apm_mailbox` at `0x17440000`
    - **Shared SRAM:** `apm_sram` at `0x17400000`, size `0x2000`
    - Node: `acpm_ipc: power-management` (`compatible = "google,gs101-acpm-ipc"`)
  *(Sources: Mainline Linux `gs101.dtsi`, lines 490–500; `gs101-pixel-common.dtsi`, lines 90–200).*

### Features Requiring ACPM Firmware Interaction
- **VERIFIED-FROM-SOURCE:** ACPM firmware interaction is mandatory for:
  - Dynamic Voltage and Frequency Scaling (DVFS) of CPU clusters and GPU.
  - Dynamically altering PMIC regulator buck/LDO voltages.
  - Power rail sequencing during system suspend/resume (C2/C3 states).
- **INFERRED:** For initial bare-metal XNU bringup, **no ACPM driver is required**. ABL boots the SoC with CPU, RAM, UFS, and display rails fully energized at fixed nominal voltages. Operating at a fixed boot frequency bypasses ACPM completely.

### Driver Reuse Assessment
- **[reuse]**: `ios-sweet`'s mock battery service (`SweetBattery`), which publishes dummy charging status (100% battery, plugged in) to macOS/iOS power management (`IOPMPowerSource`), can be reused verbatim.
- **[rewrite]**: Writing an authentic battery driver requires implementing Maxim MAX1720x I2C driver for fuel gauge telemetry, and an ACPM IPC mailbox driver for PMIC control.

---

## 11. Critical System Risks and Peripheral Quirks

### Hardware Watchdog (`watchdog_cl0`)
- **VERIFIED-FROM-SOURCE:** GS101 contains Samsung Exynos-style watchdog timers:
  - `watchdog_cl0: watchdog@10060000` (Cluster 0 watchdog, IRQ GIC_SPI 765)
  - `watchdog_cl1: watchdog@10070000` (Cluster 1 watchdog, IRQ GIC_SPI 766)
  - Compatible: `"google,gs101-wdt"`.
  - Default timeout in DT: `timeout-sec = <30>;`.
  *(Sources: Mainline Linux `gs101.dtsi`, lines 622–645; `gs101-pixel-common.dtsi`, line 915).*
- **INFERRED / CRITICAL WARNING:** If ABL arms `watchdog_cl0` before jumping to the kernel, the SoC will undergo an emergency hardware reset after 30 seconds unless kicked. The bare-metal shim must immediately disable the watchdog upon entry by clearing the `WTCON_ENABLE` bit in register `0x10060000`.

### Thermal Runaway and Thermal Shutdown Risks
- **VERIFIED-FROM-SOURCE / INFERRED:** GS101 is manufactured on Samsung's 5LPE node and is notorious for high thermal dissipation under load (especially dual Cortex-X1 cores).
  - Without the TMU (Thermal Management Unit) driver or ACPM thermal governor active in XNU, running unthrottled spin-loops will cause rapid junction temperature rise.
  - If die temperature exceeds the hardware limit (~110°C–115°C), the PMU / PMIC triggers an immediate emergency shutdown.
  - **Mitigation:**
    1. All idle loops in the shim and XNU must issue `wfi` (Wait For Interrupt) rather than tight spin-waits.
    2. Keep secondary Cortex-X1 cores (`cpu6` and `cpu7`) offline during early bringup.
    3. Ensure display backlight is not driven at 100% continuously.

### Stage 2 Memory Protection Unit (S2MPU) DMA Violations
- **VERIFIED-FROM-SOURCE:** The high-speed interconnect includes Samsung S2MPU protection units (`s2mpu_hsi2`).
  - Even without SysMMU paging, S2MPU actively traps DMA requests issued to physical memory regions reserved for secure firmware (e.g., TrustZone GSA at `0x90200000`, TPU at `0x93000000`, AOC at `0x94000000`, Modem carveouts at `0xf0000000`).
  - DMA descriptors for UFS and DWC3 USB must strictly be allocated in normal non-secure system DRAM (`0x80000000`+). DMA to invalid addresses results in a silent transaction lockup or hardware bus fault.

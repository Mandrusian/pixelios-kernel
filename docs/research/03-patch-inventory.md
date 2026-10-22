# 03 — ios-sweet patch inventory for the Pixel 6a (gs101) port

Scope: code-reading only, no device and no network. Everything marked "repo" below comes from
`<repo>/upstream` (ios-sweet: xnu-8796 + iOS 16.7.16 iPhone X userland on a
Redmi Note 10 Pro, SM7150 "sweet"). Statements about gs101 / Pixel 6a hardware are **from memory of the
public Linux/AOSP gs101 trees, not from this repo or from the device**; they are tagged **[verify]**
and must be checked against the device (DT dump, ID registers) before anyone relies on them.

Classes used in the tables:

- **GENERIC** — works unchanged on any ARMv8.2 board that already boots the earlier stages.
- **PARAMETERIZE** — generic logic with hardcoded sweet/SM7150 constants (addresses, IRQs, geometry, names).
- **REWRITE** — Qualcomm-specific hardware driver (GENI I2C, SPMI, RPMh, QUSB2/QSCRATCH, GCC clocks, Qualcomm UFS assumptions).

Patch line numbers (`0010:99`) are line numbers inside the `.patch` file. Other references are `file:line`.

---

## 0. Executive summary

1. **66 of the 81 patches are GENERIC** (0046 and 0058 carry a minor PARAMETERIZE note); 10 are PARAMETERIZE (0004, 0010, 0012, 0016, 0018, 0028, 0061, 0065, 0073, 0078; 0028 with REWRITE risk); 5 are REWRITE (0060, 0062, 0063, 0072, 0074) (toolchain, stubs for Apple-only pieces, crypto, HFS, AMFI/entitlements, AKS,
   IOSurface/IOMobileFramebuffer, HID, diagnostics). They do not touch SoC registers.
2. **The Qualcomm-only code is small and well isolated**: 0060 (SPMI keys + GENI/Goodix touch), 0062 (RPMh + PM6150 battery),
   0063 (GENI FIFO packing + Goodix protocol), 0072/0074 (QUSB2/QSCRATCH/GCC USB probe + VBUS override). Those
   four-to-five patches are the whole REWRITE set. USB device-mode logic in 0073/0075–0077/0081 is a DWC3 gadget and is
   reusable if the Exynos DWC3 is left alive by the bootloader.
3. **The hard parts of the port are not in the patches, they are in assumptions about what the Qualcomm ABL leaves behind**:
   a running UFS host (0028 never initializes the link), a continuously scanned-out video-mode panel (0010/0046/0056/0079 just
   write RAM), an EL1 entry, a 16K-granule-capable boot core, and a single hole-free RAM range (physBase). See §4; items H1–H5 can each
   stop the port dead and should be measured in the shim *before* any XNU work.
4. **Keep the `SWEET` machine config** (§2). Select the board at runtime from the ADT and gate the Qualcomm drivers.
5. Because the iPhone X userland, the 1080x2400 panel size, HID layer, IOSurface/IOMFB services and the root image are identical between sweet
   and the Pixel 6a, **the display/HID/userland half of the work (0046, 0056–0059, 0061, 0064, 0067–0071, 0079 and all tools that build the root) transfers as-is**.

---

## 1. Inventory

### 1.1 `xnu/patches/` (applied in numeric order by CI; each patch is context-dependent on the previous ones)

| File | What it does | Class | Hardcoded constants / what must change |
| --- | --- | --- | --- |
| 0001-new-clang-drop-removed-auto-var-init-flag | Drops a clang flag that new Xcode removed | GENERIC | — |
| 0002-new-clang-backport-UIO_STACKBUF | Backports `UIO_STACKBUF` (new-clang build fix) in 12 BSD files | GENERIC | — |
| 0003-sweet-framebuffer-serial-console | Registers a "serial" device that draws kprintf on a framebuffer described by ADT node `sweet-fbcon` (reg/width/height/stride), 2x iso_font, wraps instead of scrolling | GENERIC | ADT-driven. Node name `sweet-fbcon` (0003:129) is the only sweet bit. Assumes 32 bpp; works for any byte order because it draws only white/black |
| 0004-sweet-machine-config | Adds machine config `SWEET`: `SWEET.h` board header, `-march=armv8.2-a+crypto`, plain arm64 (no PAC), GICv3 **Group 1 IRQ** path in `sleh_irq`/`pe_init_fiq` (timer + SGI as IRQ, not FIQ), `MAX_CPUS 8`, 64-byte lines, drops the `SCTLR_EL1_DEFAULT` check in `start.s` | PARAMETERIZE | `SWEET_VTIMER_INTID 19` (0004:263, Qualcomm PPI numbering; overridden at runtime by 0017); `GICR_PE_SIZE 0x20000` (0004:268, redistributor stride, would be 0x40000 with GICv4 VLPI **[verify]**); `MAX_CPUS 8`/`MAX_L2_CLINE 6` (0004:308-309, fine for gs101); `NO_ECORE`/`MAX_CPU_CLUSTERS 1`; GIC init assumes ABL left `GICD_CTLR`=ARE_NS only and `ICC_SRE` usable at EL1 |
| 0005-non-pac-build-fixes | Renames a duplicate `reserved10` field in `pmap.h` | GENERIC | — |
| 0006-sweet-master-config-open-source-asm-pmap | New `config/MASTER.arm64.sweet` (macOS config + `nos_arm_asm nos_arm_pmap`, drops closed KDK libs) | GENERIC | File name only |
| 0007-sweet-stub-apple-internal-headers | Stubs for Apple-only `sart.h`, `uat.h`, `tunables.s` (empty `APPLY_TUNABLES`) | GENERIC | If Cortex-X1/A76/A55 errata workarounds are ever wanted, `tunables.s` is where they would go (empty today) |
| 0008-sweet-fill-closed-lib-gaps-kpc-stub | Fills symbols normally in Apple's closed per-SoC lib: pmap/PPL gaps, AMCC stubs, `CleanPoC_*_nohid`, a kpc stub with no counters | GENERIC | — |
| 0009-sweet-drop-apple-soc-only-exports | Removes Apple-SoC-only symbols from the export lists | GENERIC | — |
| 0010-sweet-early-framebuffer-debug | `start.s` white marker band, a one-entry 32 MB Normal-NC block for the framebuffer in the bootstrap V=P L2 table, `sweet_early.c` text/panic output through that mapping, stage marks in `arm_init` | PARAMETERIZE | FB base `0x9c000000` (0010:99, 131, 161), row `2368`, stride `4320` (0010:99-102, 164), `1080x2400` (0010:162-163). All are compile-time because they run before the ADT is parsed. `x25 + 2*PGBYTES` assumes the FB and RAM share the one V=P L2 table (true for PA < 64 GB with 16K pages). **Fix: read `v_baseAddr/v_rowBytes/v_width/v_height` from `boot_args->Video` (x20 is already saved) instead of constants** |
| 0011-sweet-physmap-alignment-padding | Reserves VA/L1 room for physmap block-alignment slack (ARM_LARGE_MEMORY, non-PPL) | GENERIC | Comment says "all RAM below 4 GB" in 0010; highmem (`0x100000000`) already proves PA > 4 GB works. PAs ≥ 0x8_0000_0000 have never been exercised |
| 0012-sweet-keep-debug-console-after-ttbr0 | Re-maps the FB as a 32 MB Normal-NC block in the final kernel TTBR1 (`VM_MIN_KERNEL_ADDRESS + 32MB`) so the console survives the bootstrap-TTBR0 teardown; more stage marks | PARAMETERIZE | `0x9c000000` (0012:26, 104). Block mapping needs `fb & ~(32MB-1)` plus an in-block offset if the Pixel FB is not 32 MB-aligned. The whole 32 MB block becomes Normal-NC: make sure no secure/protected carve-out lies in that block (H5) |
| 0013-sweet-kmem-rng-without-corecrypto | SplitMix64-based kmem allocator RNG | GENERIC | — |
| 0014-sweet-kernel-hmac-prng | Built-in HMAC-SHA256 DRBG/entropy path replacing the missing corecrypto kext | GENERIC | Entropy source not traced in detail; mkadt emits a fresh `/chosen/random-seed` from `os.urandom` on every build |
| 0015-sweet-unavailable-trust-cache-runtime | Trust-cache runtime stays NULL when Image4/AMFI kexts are absent | GENERIC | — |
| 0016-sweet-device-tree-platform-expert | `SweetPlatformExpert` (IODTPlatformExpert without NVRAM `/options`) matched by `'xiaomi,sweet'` | PARAMETERIZE | `IONameMatch 'xiaomi,sweet'` (0016:12). Either keep the string in the Pixel ADT or switch the personality to a board-neutral name |
| 0017-sweet-device-tree-virtual-timer-irq | Reads `virtual-timer-intid` from the ADT `gic` node (range-checked PPI 16..31) instead of the compile-time 19 | GENERIC | Pixel: set ADT value to **27** (standard Arm PPI 11) **[verify]**. This is the same path the QEMU twin already exercises (`QEMU_VTIMER_INTID = 27`) |
| 0018-sweet-boot-cpu-driver | `SweetCPU` (legacy `IOCPU`): registers the boot CPU, PSCI-free single core, SGI1 IPI via `ICC_SGI1R_EL1` | PARAMETERIZE | `IONameMatch 'qcom,kryo470'` (0018:187), name "Kryo 470" (0018:60). IPI code uses MPIDR Aff3..Aff0 generically |
| 0019-sweet-builtin-pthread (+ 0019-stage-libpthread.sh) | Builds Apple's open-source libpthread (pinned commit) into the kernel as the pthread kernel provider | GENERIC | Stage script is gated `[[ $MACHINE_CONFIG == SWEET ]] \|\| exit 0` |
| 0020-sweet-portable-aes-cbc (+ 0020-stage-bearssl.sh) | Constant-time BearSSL AES (CBC/ECB) as the kernel's AES provider; later patches extend the staged BearSSL set | GENERIC | Software only (no `aes` instructions, though gs101 has them). Perf only. Stage script gated on SWEET |
| 0021-sweet-mockfs-ramdisk-root | `mockfs` in the config; fixes mockfs typed-alloc bug; `setRootMedia` tolerates a missing `IOBlockStorageDriver` | GENERIC | Needed for the RAM-disk/QEMU root path |
| 0022-sweet-ramdisk-root-without-iomedia | `publishHiddenMedia` works without an `IOMedia` class | GENERIC | — |
| 0023-sweet-builtin-sha256-for-libkern | libkern SHA-256 via built-in `ccsha256_ltc_di` | GENERIC | — |
| 0024-sweet-mockfs-virtual-ramdisk-io | mockfs RAM disk uses pager/strategy I/O instead of a bogus physical mapping | GENERIC | — |
| 0025-sweet-builtin-amfi-no-entitlements | `bsd/kern/sweet_amfi.c`: built-in AMFI interface (no entitlements) so the first `exec` does not dereference NULL | GENERIC | — |
| 0026-sweet-builtin-hfs (+ 0026-stage-hfs.sh) | Stages Apple's open-source `hfs-627.100.6` into `bsd/sweet_hfs` as a built-in filesystem | GENERIC | Stage script gated on SWEET |
| 0027-sweet-bearssl-md5 | BearSSL MD5 provider (HFS volume UUID) | GENERIC | — |
| 0028-sweet-ufs-root-device | `bsd/dev/sweet_ufs.c`: polled UFSHCI driver that **reuses the host state ABL left running**, SCSI READ(10) on **LUN 0**, finds a GPT partition by name (`ufsroot=` boot-arg, default `userdata`), exposes `/dev/ufsroot0`; `rd=ufsroot0` hook in `IOKitBSDInit.cpp` | PARAMETERIZE (REWRITE risk, H3/H4) | ADT node `/sweet-ufs` with `reg` (0028:510), LUN 0 (0028:529), 4 KiB blocks (0028:98), GPT at LBA 1, 128-byte entries, partition name (0028:507). **No `HCE` reset, no UniPro link startup, no Exynos PHY/SMU/FMP setup**: `ufs_take_over_host` simply refuses if `HCE=0`/`DP=0`/`UTRLRDY=0` or requests are outstanding. Works only if the Pixel bootloader leaves the UFS host running |
| 0029-sweet-unsealed-root-volume | Logs instead of panicking when the root volume is not sealed (`FSIOC_KERNEL_ROOTAUTH`) | GENERIC | — |
| 0030-sweet-ufs-writes | WRITE(10) + read-modify-write for partial blocks + SYNCHRONIZE CACHE, only with `ufsrw=1`, bounds-checked per write | GENERIC | Plain UFSHCI/SCSI. Valid once 0028 works |
| 0031-sweet-read-write-root | `rootrw=1` clears `MNT_RDONLY` before the root mount; HFS accepts a dirty non-journaled root | GENERIC | — |
| 0032-sweet-bearssl-sha1 | BearSSL SHA-1 provider (fixes NULL `g_crypto_funcs` in `in6_nigroup`) | GENERIC | — |
| 0033-sweet-bearssl-sha384-sha512 | BearSSL SHA-384/512 (code-directory hashing) | GENERIC | — |
| 0034-sweet-ufs-dma-above-4g | UFS refuses DMA memory above 4 GiB unless `CAP.64AS`; logs DMA address | GENERIC | Becomes relevant as soon as `physBase` ≥ 4 GiB (always on gs101 high bank). Does not know about IOMMU/S2MPU (H4) |
| 0035-sweet-amfi-entitlements-platform-binaries | `SweetEntitlements` + AMFI hooks: entitlement queries from the signature XML, every signed binary on the root mount is a platform binary | GENERIC | — |
| 0036-sweet-amfi-cs-signed | Sets `CS_SIGNED` in the AMFI hook (launchd checks `csops` status) | GENERIC | — |
| 0037-sweet-config-bindfs | Adds `bindfs` to FILESYS_BASE | GENERIC | — |
| 0038-sweet-applekeystore | Built-in `AppleKeyStore` stub (no passcode, unlocked), selector map in NOTES | GENERIC | — |
| 0039-sweet-coredump-default-off | `do_coredump = 0` on SWEET | GENERIC | — |
| 0040-sweet-iowatchdog | `IOWatchdog` + user client with all selectors succeeding (watchdogd) | GENERIC | — |
| 0041-sweet-hfs-link-notes | Rate-limits the HFS hard-link-repair printfs | GENERIC | — |
| 0042-sweet-short-exec-log | One short `exec NAME` console line | GENERIC | — |
| 0043-sweet-console-crash-log | `CRASH name ...` console crash reports from `proc_prepareexit` | GENERIC | — |
| 0044-sweet-crash-log-reason | Adds exit-reason text to the crash line | GENERIC | — |
| 0045-sweet-iowatchdog-pmu-primary | `IOWatchdog` publishes `IOPMUPrimary = 1` | GENERIC | — |
| 0046-sweet-iosurface-iomfb | `IOCoreSurfaceRoot` (IOSurface kernel side) and `AppleCLCD : IOMobileFramebuffer` on ADT `/arm-io/disp0`; swap = CPU copy of the layers into **the boot framebuffer** taken from `PE_current_console()`; 60 Hz vsync from a timer | GENERIC (PARAMETERIZE on pixel format) | Match string `'sweet,display'` (0046:23). `SWEET_FB_FORMAT_BGRA 0x42475241` (0046:1059) must equal the Pixel scanout byte order. `SWEET_FB_VSYNC_US 16667` (60 Hz timer, not a hardware vsync). `SWEET_FB_DOT_PITCH 395` is superseded by 0061. **Assumes writing to the buffer is enough to change the screen** (H11) |
| 0047-sweet-persistent-console-log | All console output goes into a 4 MiB RAM ring that a thread writes to the **last 4100 KiB of the root partition** every ~1 s (10 s after 0069); header magic `SWEETLOG` | GENERIC | Depends on UFS write path (0028/0030). Read-back needs `tools/sweet-log-pull.sh`, which assumes OrangeFox adb root (H13) |
| 0048-sweet-devnull-tee | Copies writes to `/dev/null` of listed processes to the console (`sweetnull`) | GENERIC | — |
| 0049-sweet-iokit-trace | `IOK name:` IOKit lookup/open trace for `sweetiolog` processes | GENERIC | — |
| 0050-sweet-sandbox-policy | Built-in permissive MAC policy named `Sandbox` | GENERIC | — |
| 0051-sweet-crash-heap-strings | Dumps heap text on SIGABRT/SIGTRAP to find NSException reasons | GENERIC | — |
| 0052-sweet-hfs-rename-swap | `RENAME_SWAP` via `VNOP_EXCHANGE` fallback on HFS | GENERIC | — |
| 0053-sweet-crash-scan-springboard | Adds SpringBoard to the default `sweetscan` list | GENERIC | — |
| 0054-sweet-process-sampler | `bsd/dev/sweet_sample.c` user-stack sampler, swap counter | GENERIC | — |
| 0055-sweet-aks-ext-state-sampler-depth | AKS selectors 35/101, deeper sampler | GENERIC | — |
| 0056-sweet-fb-throttled-present | Swap only records the frame; the vsync timer copies to the panel at `sweetfb_hz` (default 10) | GENERIC | Assumes continuous scanout (H11) |
| 0057-sweet-no-ext-resolver | Drops `config_ext_resolver` (ACL group entries resolved without memberd) | GENERIC | — |
| 0058-sweet-hid-event-services | Apple IOHIDFamily-1915.140.3 user client/queue/event (28 Apple files, unmodified) + `SweetHID.cpp` (`SweetHIDButtons`, `SweetHIDTouch`, C hooks `sweet_hid_key`/`sweet_hid_touch`) + 2 IOKit personalities | GENERIC (PARAMETERIZE the digitizer scale) | `SweetHID.cpp` normalizes touch by `1080` / `2400` and clamps to `1079`/`2399` (0058:21046-21047). Equal on Pixel 6a (1080x2400). Skim structure: 1M-line diff is ~97 % imported Apple headers |
| 0059-sweet-iphone-identity | `hw.machine/model/target/product` from ADT (iPhone10,3 / D22AP) instead of macOS kludge; removes SweetPlatformExpert name overrides | GENERIC | — |
| 0060-sweet-keys-touch-drivers | `sweet_keys.c` (polled PMIC key reads over the **SPMI arbiter v5 observer**) and `sweet_touch.c` (**GENI SE7 I2C** driver for Goodix GT9896 @0x5d, starts 45 s after boot); both called unconditionally from `sweet_ufs_init` | REWRITE (HID hooks are generic) | Keys: `KEYS_CORE_PA 0x0c440000`, `KEYS_OBS_PA 0x0e600000`, `KEYS_CNFG_PA 0x0c40a000`, PON `0x810`, PM6150L GPIO `0xc110`, SID 0/4 (0060:55-57). Touch: `TOUCH_SE_PA 0xa84000`, `TOUCH_GCC_PA 0x100000`, `TOUCH_NORTH_PA 0x3900000` (0060:244-246), TLMM pins 6/7/9 (0060:716-718), I2C addr 0x5d, Goodix regs `0x4022/0x4180/0x4188`. Unconditional on any SoC -> on gs101 these poke unmapped addresses. **Disable with `sweetkeys=0 sweettouch=0` until replaced** |
| 0061-sweet-iphone-x-display-size | Presents the display as the iPhone X panel 1125x2436 (@3x), centers the 1080x2400 panel in it, scales touch the same way; dot pitch 458 | PARAMETERIZE | Panel 1080x2400 is the same on the Pixel 6a, so unchanged. Constants `SWEET_FB_LOGICAL_W/H 1125/2436` (0061:17-18), hard-coded `1080/2400` offsets (0061:106-107) |
| 0062-sweet-touch-power-battery | `sweet_rpmh.c` (RPMh TCS vote to enable PM6150L LDO11 for the touch rail), `sweet_spmi.c` + `SweetBattery.cpp` (`IOPMPowerSource`, static 100 % by default, optional read-only PM6150 polling with `sweetbattery=1`) | REWRITE (SweetBattery class shell is generic) | RPMh: `RH_RSC_PA 0x18220000`, `RH_TCS_PA 0x18220d00`, `RH_DICT_PA 0x0c3f000c`, DRV 2 (0062:129-133). SPMI: `SP_CORE/OBS/CFG 0x0c440000/0x0e600000/0x0c40a000` (0062:490-492). Default battery path touches no MMIO (safe); `sweetbattery=1` is Qualcomm-only |
| 0063-sweet-goodix-geni-packing | Fixes GENI FIFO packing (register address was byte-swapped) and implements the Goodix "Yellowstone" report protocol | REWRITE | `SE_PACK0/1 0x0007f8fe/0x000ffefe`, SE register offsets, Goodix report format. Replace with the Pixel 6a touch controller's protocol (probably SPI; **[verify]** from the device tree) |
| 0064-sweet-priv-work-interval | Grants `PRIV_WORK_INTERVAL` (UIKit CoreAnimation work interval) | GENERIC | — |
| 0065-sweet-smp-psci | SMP: secondary CPUs via **PSCI CPU_ON over `smc`**; `sweet_secondary_entry` (EL1 or EL2 entry, drops to EL1), cache cleans to PoC, `ResetHandlerData` indexed by `MPIDR & 0xFFFF`, `cpus=N` clamp, `die-cluster-id`/`cluster-core-id` from ADT | PARAMETERIZE | `0xC4000003` CPU_ON64 (0065:32), `smc` conduit, MPIDR layout `Aff1 = core, Aff0 = 0` and `phys_id <= 0x700` (0078:27), SCTLR_EL1 reset `0x30d00800` (0065:371), SPSR `0x3c5` (0065:377), panics unless the **boot CPU is logical 0 and MPIDR 0** (0065:126). The EL2 branch is reusable for the *primary* CPU (H2) |
| 0066-sweet-crash-fault-region | Fault-address region in crash reports | GENERIC | — |
| 0067-sweet-console-mute | `sweetfb_noconsole=1` stops drawing console text once the GUI presents (panic still draws) | GENERIC | — |
| 0068-sweet-surface-lookup-cached | IOSurface lookup of an already-mapped surface returns the existing base | GENERIC | — |
| 0069-sweet-ufs-faster | Batched log flush (`ufslogsecs`), optional directional cache ops (`ufscache=1`) and spin-then-sleep (`ufswait=1`) | GENERIC | Cache maintenance is harmless on a coherent controller |
| 0070-sweet-pid-hibernate | Enables syscall 435 `pid_hibernate` (SIGSYS killed SpringBoard when an app left the foreground) | GENERIC | — |
| 0071-sweet-touch-edge | Edge-tip digitizer flag for touches within 48 px of an edge (home gesture) | GENERIC | `48 px` threshold depends on 1080 wide panel (same) |
| 0072-sweet-usb-probe | `sweet_usb.c` step 1: read-only probe of GCC USB30_PRIM GDSC/CBCRs, DWC3 and QUSB2 PHY, default **on** (`sweetusb=1`) | REWRITE | `GCC_PA 0x00100000`, GDSC `0xf004`, CBCR offsets, `DWC3_PA 0x0a600000`, `QUSB2_PA 0x088e2000` (0072:36-37, 52-53, 70-73). **Runs by default and reads GCC: boot with `sweetusb=0` on gs101** |
| 0073-sweet-usb-device-ep0 | DWC3 device-mode bring-up: soft reset, own event buffer (polled), EP0 control state machine, descriptors; `sweetusb=2` | PARAMETERIZE | DWC3 core registers at `base+0xc100..` are Synopsys-standard (`GCTL 0xc110`, `DCTL 0xc704` ...). Needs the Exynos DWC3 base address, and still assumes ABL left clocks/PHY/GDSC on and the PHY configured (H3). Flushes caches because the Qualcomm DT says non-coherent |
| 0074-sweet-usb-vbus-override | Forces a VBUS session through Qualcomm `QSCRATCH` (`HS_PHY_CTRL \|= VBUS_VALID\|SW_SESSVLD_SEL`, `SS_PHY_CTRL \|= LANE0_PWR_PRESENT`) | REWRITE | `QSCRATCH_PA 0x0a6f8800` (0074:13). Pixel VBUS/role detection goes through the USB-C controller (MAX77759 TCPCI/charger **[verify]**) and the Exynos wrapper/PHY |
| 0075-sweet-usb-ecm | CDC-ECM gadget (bulk 0x81/0x01, interrupt 0x82) + `ue0` kernel ifnet with 192.168.7.2/24 | GENERIC | MAC `02:00:5e:77:ee:01`, IPs, VID:PID 1209:0001 |
| 0076-sweet-usb-rx-mbuf | Single-cluster RX mbuf | GENERIC | — |
| 0077-sweet-usb-live-log | Streams the console as UDP to 192.168.7.1:5140 | GENERIC | Gives a no-recovery log channel once USB works |
| 0078-sweet-smp-8-cores | Raises `SWEET_SMP_MAX_CPUS` to 8, allows MPIDR 0x600/0x700 (the A76s) | PARAMETERIZE | `SWEET_SMP_MAX_CPUS 8` (0078:15), `phys_id > 0x700 \|\| (phys_id & 0xff) != 0` (0078:27). gs101 is also 8 cores but three clusters (4xA55, 2xA76, 2xX1) **[verify MPIDRs]**; XNU still sees one homogeneous SMP cluster |
| 0079-sweet-display-power | `RequestPowerChange(0)` fades the panel to black (AMOLED black = off) | GENERIC | Pixel 6a is also OLED; panel is not actually powered down |
| 0080-sweet-net-trace | `SWEET NET:` route/address change trace | GENERIC | — |
| 0081-sweet-usb-link-state | ECM link-state handling | GENERIC | — |

Stage scripts and notes: `0019-stage-libpthread.sh`, `0020-stage-bearssl.sh`, `0026-stage-hfs.sh` are GENERIC (pinned commits) but all three start with
`[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0`, another reason to keep the machine config name.

### 1.2 `shim/` (bare-metal boot shim, packed as an Android boot.img and started with `fastboot boot`)

| File | What it does | Class | Hardcoded constants / what must change |
| --- | --- | --- | --- |
| shim/start.S | Linux arm64 Image header (`flags 0xa`), entry at EL1 (`msr spsel`, `vbar_el1`), zeroes `.bss`, calls `shim_main`; vectors that paint a panic screen; `secondary_entry` for PSCI-started cores | PARAMETERIZE | **Assumes entry at EL1**: no `CurrentEL` check, vectors only for EL1 (H2). Comment names "Qualcomm ABL". Image is position independent (no ABL load address is baked in) |
| shim/shim.ld | Link layout: `.text` at 0, 32 KiB stack, 8x4 KiB secondary stacks, `_end` as image_size | GENERIC | — |
| shim/shim.c | `shim_main` (loader, UFS probe), exception handler, `reset()`, PSCI/SMP/GIC/timer milestone tests (mostly dead code after `loader_boot`/`reset`) | PARAMETERIZE | `IMEM_RESTART_REASON 0x146aa65c`, `RESTART_BOOTLOADER 0x77665500`, `PSHOLD 0x0c264000` (shim.c:15-17) are Qualcomm and are **written on every panic/reset** (shim.c:104-115): these writes would fault on gs101; use PSCI `SYSTEM_RESET` only. `GICD_BASE 0x17a00000`, `GICR_BASE 0x17a60000`, `GICR_SIZE`, `GICR_PE_SIZE` (shim.c:19-22), `VTIMER_INTID 19`, `PTIMER_INTID 18` (shim.c:33-34), `NR_CPUS 8`, 19.2 MHz in `to_us` (shim.c:370). The printed `ID_AA64MMFR0` / MIDR / PSCI / per-core MPIDR tests are exactly what H1/H2/H8 need: re-enable them |
| shim/console.c | 3x scaled 8x16 font text console on the framebuffer, `con_clear/con_str/con_hex/con_hex2` | PARAMETERIZE | Uses `FB_*` constants from `console.h`; assumes `a8r8g8b8` and aligned 32-bit stores (MMU off => Device memory) |
| shim/console.h | `FB_BASE 0x9c000000`, `FB_W 1080`, `FB_H 2400`, `FB_STRIDE W*4`, colour constants | PARAMETERIZE | **The first values to change**: take the FB address from the live FDT (`/reserved-memory` splash/framebuffer node or a `simple-framebuffer`) or hardcode after a DT dump. Width/height match the 6a |
| shim/font.h, shim/gen_font.py | 8x16 CP850 font table and its generator | GENERIC | — |
| shim/host_test.c | Host-side render of the console layout to `out/host.ppm` | GENERIC | — |
| shim/fdt.c, shim/fdt.h | Minimal FDT walk: read a u32/u64 property of `/chosen` | GENERIC | Good place to add "find framebuffer/reserved-memory" and a magic scan |
| shim/loader.c, shim/loader.h | "Act as iBoot": find `SWEETXNU` payload at `linux,initrd-start`, copy segments to `phys_base + (va - virt_base)`, copy ADT, build `boot_args` (Revision 2, Video = `FB_*`), jump with MMU/caches off, x0 = boot_args PA | PARAMETERIZE | Payload assumed to be **at `initrd-start`** (loader.c:88): with boot header v4 the initrd is vendor ramdisk ++ boot ramdisk, so **scan for the `SWEETXNU` magic** (H13). Video fields from `FB_*` (loader.c:126-131). Overlap check covers only the ramdisk, not the shim image or FDT (loader.c:100). Needs an EL2->EL1 drop if entered at EL2 (H2) |
| shim/mmu.c, shim/mmu.h | Milestone-3 16K-granule MMU test (T0SZ 28, MAIR/TCR, Device default, image WB, FB NC) | PARAMETERIZE | Dead in the production path (`loader_boot` runs with MMU off). `FB_PHYS 0x9c000000`; maps only the first 8 GB as Device (mmu.c:91); TCR IPS = 36-bit. Qualcomm-XPU rationale in the header comment. Useful as the H1 16K-granule test |
| shim/ufs.c, shim/ufs.h | Read-only UFSHCI probe (registers, TUR, READ CAPACITY, GPT, partition list with fs sniffing) with its own UTRL | PARAMETERIZE | `UFS_BASE 0x01d84000` (ufs.c:16). Exactly the right first test for H3/H4 on the Pixel: it prints `hce/hcs/cap/utrlba` before touching anything and stops if the host is not enabled |
| shim/build.sh | Builds the shim and wraps it with `mkbootimg --header_version 2 --dtb ../dtb/boot_unpacked/dtb --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 --tags_offset 0x100 --dtb_offset 0x1f00000` | REWRITE | Qualcomm-style header v2 with embedded DTB. Pixel 6a needs header v3/v4 (kernel + ramdisk in boot.img, DTB in the flashed `vendor_boot`) **[verify]**; `fastboot boot` flow, no DTB embedding, GKI-style layout |

### 1.3 `tools/`

| File | What it does | Class | Hardcoded constants / what must change |
| --- | --- | --- | --- |
| tools/mkadt.py | Builds the Apple Device Tree: `/chosen` (dram, random-seed, manifest hash, crypto method, display-scale, chip IDs, activation/security status, `asmb` SIP bits), `/cpus`, `/arm-io` (`gic`, `wdt`, `disp0`), `sweet-fbcon`, `sweet-ufs`, `/defaults`, `/buttons`, copied D22AP `/product`, iPhone10,3 identity | PARAMETERIZE | The **board table**: `PHYS_BASE 0xb0000000`/`MEM_SIZE 0x4d000000` (mkadt.py:13-14, XPU-driven low-bank layout), `HIGH_PHYS_BASE 0x100000000`/`HIGH_MEM_SIZE 0xfcc00000` (18-19), `SOC_BASE 0x01000000` (21), `GICD/GICR 0x17a00000/0x17a60000` + sizes (23-24), `FB 0x9c000000/0x1700000` (25), `FB_W/H/STRIDE` (27), `UFS 0x01d84000/0x3000` (29, `compatible qcom,ufshc` at 193), `TIMEBASE 19200000` (30), `VTIMER_INTID 19` (31), QEMU constants (33-34), synthetic ECID `'SWEET'+1` and chip-id `0x8015` (26, 104-105), 8 CPU nodes with `reg = core << 8` (127-132), `compatible qcom,kryo470` (131), clocks `2304000000/1804800000` (142), bus/memory freq (143-145), per-core `l2-cache-id` (138), root `compatible` includes `xiaomi,sweet`, `qcom,sm7150` (220). Keep: `disp0` 1125x2436 canvas, `display-scale 3`, `/buttons`, `/product`, security-status props |
| tools/adt.py | ADT parser/serializer, IM4P unwrapper | GENERIC | — |
| tools/mkpayload.py | Packs kernel Mach-O (fixes chained fixups), ADT, cmdline, optional RAM disk into the `SWEETXNU` payload; reads `dram-base/size` from the ADT | GENERIC | Magic `SWEETXNU`/`SWEETLOG` are shared with shim and kernel. `virt_base` is 32 MB-aligned, so **`phys_base` must be 32 MB-aligned** (true for `0x8_8000_0000` / `0x1_0000_0000`) |
| tools/mkinit.py, tools/sweet-init.S | Static ad-hoc-signed arm64 Mach-O "PID 1" (prints heartbeat) for RAM-disk/QEMU/early-root tests | GENERIC | — |
| tools/mkiosroot.sh | Builds the case-sensitive journaled HFS+ iOS root from IPSW dmgs (OS + SystemOS + AppOS cryptexes, dyld cache links, keybag, log dirs) | GENERIC | Host-only. IPSW is iPhone X 16.7.16 |
| tools/sweet-root-tweaks.py | Bring-up edits to the mounted root (PanicOnCrash off, CommCenter removed, Setup skipped, activation, migration, audio plug-in) | GENERIC | `BUILD, VERSION = 20H392, 16.7.16`; identity independent of the SoC |
| tools/patch-launchd-plist.py | Redirect selected launchd jobs to `/dev/console` and rename `_PanicOnCrash` | GENERIC | — |
| tools/unpack_bootv2.py | Unpacks Android boot image **v0-v2** | PARAMETERIZE | Not needed for the shim image, but to read a Pixel `boot.img`/`vendor_boot.img` (v3/v4) it must learn those headers |
| tools/qemu-sweet.py | Boots a `SWEETXNU` payload on QEMU `virt` (GICv3, `cortex-a76`), decodes the framebuffer, GDB stub | PARAMETERIZE | `FB_BASE 0x9c000000`, `1080x2400`, stub PA `0x40200000` (qemu-sweet.py:25-28); uses `mkadt.py --qemu`. Still the cheapest way to test kernel-only changes, including an ADT with `virtual-timer-intid 27` |
| tools/xnu-cc.py | Replays compile commands from a CI log for local syntax checks | PARAMETERIZE | `CI_DXB = '/Users/runner/work/ios-sweet/ios-sweet/dxb'` (xnu-cc.py:23) contains the **GitHub repo name**; it must become `/Users/runner/work/pixelios/pixelios/dxb` (or whatever the repo is called); uses `~/swift/usr/bin/clang` |
| tools/check-exports.py | Offline `check_all_exports` reproduction | GENERIC | — |
| tools/validate-repo.py | Checks patch numbering and script syntax | GENERIC | Keep when adding Pixel patches (`0082+`) |
| tools/remotezip.py | Extract single IPSW members over HTTP range | GENERIC | — |
| tools/sparsediff.py | Writes an Android sparse image with only changed blocks | GENERIC | — |
| tools/sweet-crash.py | Symbolicates `CRASH` console lines against the dyld cache (`ipsw`) | GENERIC | — |
| tools/sweet-log-pull.sh, tools/sweet-log.py | Pull and decode the 4 MiB log ring from the end of `userdata` via recovery adb | PARAMETERIZE | `sweet-log-pull.sh` needs an adb-root recovery and `/dev/block/by-name/userdata`; on Pixel use a recovery with adb root, or `fastboot fetch`-style access **[verify]**, or the USB-ECM live log. `sweet-log.py` (SWEETLOG format) is GENERIC |
| tools/sweet-usbnet.sh | Host side of the phone's USB Ethernet (MAC `02:00:5e:77:ee:01`, 192.168.7.1/24, UDP 5140) | GENERIC | — |

### 1.4 CI and docs

| File | What it does | Class | Constants / changes |
| --- | --- | --- | --- |
| .github/workflows/xnu.yml | Builds xnu-8796 (macOS 13.5) with blacktop/darwin-xnu-build at a pinned SHA on `macos-14`, copies `xnu/patches/*.patch *.sh` into dxb, adds `arm64` to `ARCHS`, uploads `kernel.<cfg>.<machine>` | GENERIC | `machine_config` input default `SWEET` (line ~12) and `MACHINE_CONFIG: ${{ inputs.machine_config \|\| 'SWEET' }}` (line 33); push trigger `xnu/patches/**` |
| xnu/README.md, README.md | Documentation; boot-args and layout tables | GENERIC | Mentions sweet/Redmi/Qualcomm everywhere; rewrite for the Pixel fork |

---

## 2. Where "sweet" / SM7150 is baked in, and the SWEET-config recommendation

### 2.1 Places

| Layer | Item |
| --- | --- |
| Kernel build | Machine config `SWEET` in `MakeInc.def` (`SUPPORTED_ARM64_MACHINE_CONFIGS`, `MACHINE_FLAGS_ARM64_SWEET`, plain-arm64 special case); `config/MASTER.arm64.sweet`; `pexpert/pexpert/arm64/SWEET.h`; `board_config.h` (`ARM64_BOARD_CONFIG_SWEET`); counts across all patches: 55 `defined(SWEET)`, 100 `ARM64_BOARD_CONFIG_SWEET`, 197 `SWEET_*`, 1033 `sweet_*`, 18 `MASTER.arm64.sweet` |
| Kernel runtime | IOKit personalities in `KernelConfigTables.cpp`: `SweetPlatformExpert` -> `'xiaomi,sweet'` (0016), `SweetCPU` -> `'qcom,kryo470'` (0018), `AppleCLCD` -> `'sweet,display'` (0046), `SweetBattery`, `SweetKeyStore`, `IOWatchdog`; ADT nodes `sweet-fbcon` (0003), `sweet-ufs` (0028), `sweet-io` arm-io, `disp0`; `SWEETLOG` ring magic; `config sweet_hfs` option and `bsd/sweet_hfs` dir |
| Boot-args | `sweetfb_noconsole`, `sweetfb_hz`, `sweetfb_native`, `sweetlog`, `sweetnull`, `sweetiolog`, `sweetscan`, `sweetsample`, `sweettouch`, `sweettouchdelay`, `sweettouchirq`, `sweetkeys`, `sweetbattery`, `sweetusb`, `sweethidtest`, `sweet_amfi_debug`, plus `ufsroot`, `ufsrw`, `ufscache`, `ufswait`, `ufslogsecs`, `rootrw`; and the standard `rd=ufsroot0 cpus=N launchd_unsecure_cache=1 unentitled_ios_sim_launch=1 ignition_level=1 wdt=-1 wdt_test=no_user_checkin` |
| ADT (`mkadt.py`) | root `compatible` list (`D22AP,iPhone10,3,xiaomi,sweet,qcom,sm7150,AppleARM`), `model iPhone10,3`, `firmware-version sweet-shim`, boot-manifest hash seed `'sweet boot manifest'`, ECID `'SWEET'+1`, CPU `compatible qcom,kryo470`, UFS `compatible qcom,ufshc`, `sweet-io` |
| Shim | `SWEETXNU` payload magic (shim/loader.c, mkpayload.py, qemu-sweet.py), comments "Qualcomm ABL", `sweet milestone` strings |
| Tools | `sweet-*.py/sh/S`, `qemu-sweet.py`, `xnu-cc.py` CI path `ios-sweet/ios-sweet`, `kernel.development.sweet` in the README invocation |
| CI | `machine_config` default and `MACHINE_CONFIG: SWEET` in `xnu.yml`; the three stage scripts exit early unless `MACHINE_CONFIG == SWEET`; output file name `kernel.<config>.sweet` |

### 2.2 Recommendation: **keep `SWEET` and parameterize**

- The machine config is not a SoC description. It is the "non-Apple ARMv8.2, GICv3 Group 1, 16K pages, PSCI, built-in drivers" board class, and nothing in it is Qualcomm-specific once the constants in §1 are lifted
  out of `SWEET.h` and the ADT. The Pixel needs the same build options, the same `-march`, the same IRQ model.
- Renaming touches ~1,400 identifier occurrences across 81 patches, the three `MACHINE_CONFIG == SWEET` stage scripts, the CI inputs and `files.arm64`
  hunks, every one of which must stay consistent for `git apply` to succeed. It buys nothing and makes rebasing against upstream ios-sweet harder.
- Suggested minimal-diff approach:
  1. **One new patch (0082)**: board-neutral compatibles in the three IOKit personalities (e.g. `ios-sweet,platform`, `arm,cortex`, `ios-sweet,display`) and keep the old strings as aliases; no code change.
  2. **`mkadt.py --board sweet|bluejay`** table holding every constant listed in §1.3 (DRAM, GIC, FB, UFS, timer, CPU topology, compatibles). Default stays sweet so the QEMU flow is unchanged.
  3. **Gate Qualcomm drivers at runtime**. Quick form needs no kernel change: boot with `sweetusb=0 sweetkeys=0 sweettouch=0` (the three drivers that default to on and touch MMIO unconditionally; `sweetbattery` defaults to a MMIO-free static source). Clean form: make `sweet_usb_probe`, `sweet_keys_init`, `sweet_touch_init`, `sweet_rpmh_*` return early unless the ADT root `compatible` contains `qcom,sm7150`, mirroring how `sweet_ufs_init` already bails if `/sweet-ufs` is missing.
  4. Add gs101 drivers as new files (`gs101_*.c`) selected the same way. One kernel binary for both boards.
- If a rename is ever wanted, do it last as a single mechanical commit; keep a `MACHINE_CONFIG` alias in the stage scripts first.

---

## 3. Minimal ordered patch sets per milestone

Patches are a sequential stack: apply in numeric order and treat the lists below as "what must be present", not cherry-pick targets, unless you re-diff.
"New work" means Pixel-specific code that does not exist in the repo.

### (a) Shim runs and draws on the framebuffer

- **Kernel patches: none.**
- Repo files: `shim/start.S`, `shim.ld`, `console.c/.h`, `font.h`, `fdt.c/.h`, `shim.c`, `build.sh`.
- Changes:
  1. `build.sh`: Pixel boot image (header v3/v4, `fastboot boot`, no embedded DTB) — **new work**.
  2. `console.h`: FB address/size/stride from a Pixel DT dump or from the FDT passed in x0 — **new work**.
  3. `start.S`: print/handle `CurrentEL`; if EL2, install `VBAR_EL2` and run at EL2 until the loader drops to EL1 (reuse 0065's `sweet_secondary_entry` sequence).
  4. `shim.c`: delete the Qualcomm `IMEM_RESTART_REASON`/`PSHOLD` writes in `reset()`; use PSCI `SYSTEM_RESET`; re-enable the printed `ID_AA64MMFR0`, MIDR, PSCI version, per-core MPIDR/MMFR0 (the milestone-2/3 tests).
- Exit criterion: text on the panel including `CurrentEL`, MIDR, `TGran16`, PSCI version, CNTFRQ, `GICD_TYPER`, the FDT `reserved-memory` map. These decide H1, H2, H5–H8 before any XNU work.

### (b) XNU prints to the framebuffer console

1. First light (early marks and panic text): `0001 0002 0004 0005 0006 0007 0008 0009 0010 0003 0011 0012` (always applied in numeric order).
2. Stable kprintf through kernel init to the "waiting for root" state: add `0013 0014 0015 0016 0017 0018` (RNG, trust-cache NULL runtime, platform expert, ADT timer IRQ, boot CPU) and `0019 0020` (pthread provider, AES; without them `tcp_init` faults).
- Tooling: `mkadt.py --board bluejay` (new), `mkpayload.py`, `loader.c` (magic scan).
- Pixel changes: `0010`/`0012` take the FB from `boot_args->Video`; ADT `virtual-timer-intid 27`, `timebase-frequency`, GICD/GICR PAs, `sweet-fbcon` reg, DRAM base/size, 8 CPU nodes with the real MPIDRs. Boot with `cpus=1`.
- Expected result mirrors sweet's boot 0020: kernel init completes, screen says `Waiting on <dict> ... IOMedia ... Apple_HFS`.

### (c) XNU mounts root from UFS

- Everything in (b) plus `0021 0022 0023 0024 0025 0026 0027 0028 0029` (RAM-disk plumbing is harmless and 0023/0025/0026/0027 are required: SHA-256, AMFI interface for the first exec, built-in HFS, MD5 for the HFS volume UUID; 0028 is the UFS driver; 0029 survives the unsealed root).
- For writable root and the log ring: `0030 0031 0034` and `0047 0069` (0034 matters as soon as DMA memory sits above 4 GiB).
- **New work**: make 0028 work on the Exynos UFSHCI (H3/H4): find the register base in the Pixel DT (ADT `/sweet-ufs` reg), choose the LUN, keep 4K LBA; if the bootloader tore the host down, add HCE reset + UniPro link startup + NOP-OUT/query bring-up + Exynos PHY/FMP-bypass SMCs.
- Boot-args for this step: `rd=ufsroot0 cpus=1 sweetusb=0 sweetkeys=0 sweettouch=0` (add `ufsrw=1 rootrw=1` once reads work).
- Test with the shim's `test_ufs()` first: confirm `hce`, `hcs`, `cap`, LUN 0 capacity, GPT, `userdata` partition.

### (d) launchd + SpringBoard on screen

- Add to (c): `0032 0033` (SHA-1/384/512, needed once userland loads), `0035 0036 0037 0038 0039 0040 0045` (entitlements/platform binaries, CS_SIGNED, bindfs, AppleKeyStore, coredump off, IOWatchdog), `0046` (IOSurface + AppleCLCD), `0050 0052 0055 0057` (Sandbox policy, RENAME_SWAP, AKS selectors, no ext resolver), `0056 0061 0067 0068` (throttled present, iPhone X presented size, console mute, IOSurface lookup), `0059 0064` (identity, work-interval privilege).
- Strongly recommended for a usable system: `0065` + `0078` (SMP: with one CPU, input latency was ~1 minute), `0070` (pid_hibernate, apps can return to the home screen), `0079` (display off/on).
- Debug aids worth keeping: `0041 0042 0043 0044 0048 0049 0051 0053 0054 0066 0080`.
- HID (`0058`) is not required to draw the UI: sweet reached the Setup "Hello" screen on 0057 without it.
- Outside the kernel: `mkadt.py` (the `/chosen` security/identity/display props, `asmb` SIP bits, `no-effaceable-storage`, `arm-io/wdt no-pmu`, `/buttons`, D22AP `/product`), `tools/mkiosroot.sh`, `tools/sweet-root-tweaks.py`, flashed to `userdata`.
- Boot-args (from the README, minus Qualcomm items): `debug=0xc serial=3 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin wdt=-1 ufsrw=1 rootrw=1 launchd_unsecure_cache=1 sweetfb_noconsole=1 sweetfb_hz=30` and `cpus=N`.
- The display path is the Pixel risk for this milestone (H11).

### (e) Touch and buttons

- Add `0058 0071` (HID services, edge-tip flag) and the hook layer of `0060`/`0062`/`0063` (`sweet_hid_key`, `sweet_hid_touch`, `SweetHIDButtons`, `SweetHIDTouch`).
- **New work (REWRITE)** replacing the hardware halves of 0060/0062/0063:
  - Touch controller driver for the Pixel 6a panel (identify from the DT; likely SPI, not GENI I2C) **[verify]**, including its power rail (the sweet RPMh vote has no Pixel equivalent; rails come from the S2MPG10/11 PMICs, possibly left on by the bootloader).
  - Keys: volume keys are probably plain SoC GPIO inputs (simple polled GPIO bank read), the power key is a PMIC status bit **[verify]**. The sweet SPMI-observer reads do not apply.
  - Battery: keep the static `SweetBattery` source (MMIO-free); a real fuel-gauge read is optional.
- Touch is polled, started 45 s after boot (`sweettouchdelay`), and needs no interrupt wiring.

---

## 4. Hidden assumptions that can break on gs101

Ordered roughly by how badly they stop the port. "Check" is the cheapest way to settle it.

| # | Assumption in ios-sweet | Where | Risk on gs101 | Check |
| --- | --- | --- | --- | --- |
| H1 | **The boot core supports the 16K translation granule.** XNU arm64 is 16K-only here. Measured on sweet: `ID_AA64MMFR0 = ...101122` (TGran16 = 1) on Qualcomm's A55-derived Kryo 470 Silver | NOTES "Entry EL1... 16K granule YES"; shim/shim.c milestone-3 code; `mmu.c` | My recollection (not verified) is that a plain Cortex-A55 does **not** implement 16K and the A76/X1 do. gs101 boots on core 0 = A55. If so, XNU cannot run on cores 0-3 | Print `ID_AA64MMFR0_EL1[23:20]` per core from the shim (PSCI CPU_ON each core; the old `test_smp` does this). Mitigation if A55 lacks it: have the shim PSCI-start an X1/A76 core, jump to XNU there, relax 0065's "boot CPU must be logical 0 / MPIDR 0" panic, and put only 16K-capable cores in the ADT `cpus` list |
| H2 | **Entry at EL1** (shim `msr spsel`, `vbar_el1`; XNU `start_first_cpu` is EL1 only) | shim/start.S; osfmk `start.s` | Pixel kernels are commonly entered at EL2 (nVHE KVM/pKVM) **[verify]**. At EL2 the shim's vectors are not installed and XNU would run the wrong exception level | Print `CurrentEL`. If 2: set `VBAR_EL2`, drop to EL1 in the loader using the sequence already in 0065 `sweet_secondary_entry` (`HCR_EL2.RW`, `CNTHCTL_EL2`, `CNTVOFF_EL2`, `ICC_SRE_EL2`, `SPSR_EL2=0x3c5`) |
| H3 | **The bootloader leaves the UFS host (and USB, display) running**; the XNU UFS driver never resets the host or brings the link up | 0028 `ufs_take_over_host`; 0073 "ABL leaves the DWC3 powered" | Qualcomm ABL leaves everything on after `fastboot boot`; a Samsung/Google-derived bootloader may quiesce UFS/USB/DECON before kernel handoff | Run the shim `test_ufs()` with the Pixel UFS base; if `HCE=0`/`DP=0`, the driver must do a full Exynos init (UniPro/PHY calibration, FMP/SMU bypass SMCs) = REWRITE of 0028's front half |
| H4 | **UFS programming model**: LUN 0, 4 KiB logical blocks, GPT at LBA 1 with 128-byte entries, partition named `userdata`, PRDT `dbc = bytes-1`, one slot, 64-bit DMA via `CAP.64AS`, host "not coherent" (cache ops around each command), no IOMMU on the UFS (sweet's DT had no `iommus`) | 0028/0030/0034/0069 | gs101 may put userdata on a different LUN, the Exynos host has quirks Linux handles (`UFSHCD_QUIRK_PRDT_BYTE_GRAN` etc.), and DMA may be filtered by SysMMU/S2MPU **[verify all]**. A silent DMA filter looks like timeouts | DT dump (`ufs` node, `iommus`, `dma-coherent`); try a READ on each LUN from the shim |
| H5 | **`physBase` choice**: kernel RAM = one contiguous range with cacheable 32 MB blocks from the kernel header block to `physBase+memSize`. Sweet picked the upper bank (`0x100000000 + 0xfcc00000`) because every Qualcomm XPU carve-out sits in the low bank, and the shim notes "never map carve-outs cacheable" | NOTES "XNU loader design notes"; mkadt.py:13-19; start.s patch 0010 | No XPU on Tensor, but TZ/Trusty/GSA/AoC/TPU/hypervisor carve-outs and S2MPU protection exist and may be scattered; speculative cacheable reads of a protected range fault. The high bank likely starts at `0x8_8000_0000` and has **never been exercised**: PAs > 32 GB, shim `mmu.c` maps only 8 GB, `mkpayload` needs 32 MB alignment | Read `/reserved-memory` and `memory` from the Pixel DT; pick one hole-free, 32 MB-aligned range; test a PA ≥ 0x8_0000_0000 with a tiny memtest in the shim |
| H6 | **Timer**: `TIMEBASE 19200000` in mkadt and the shim; vtimer INTID 19 (Qualcomm PPI numbering 1/2/3/0), SGI1 as IPI | mkadt.py:30-31; SWEET.h; 0017 | gs101 is likely standard Arm numbering (PPI 11 -> INTID 27) and ~24.576 MHz **[verify]**; a wrong timebase silently skews all timeouts | Read `CNTFRQ_EL0` (and `CNTFRQ` consistency per core) in the shim; ADT `timebase-frequency` and `virtual-timer-intid` from it |
| H7 | **GIC**: DS=0 (security extension present), ABL leaves `GICD_CTLR=0x10`, XNU enables Group 1 NS and takes IRQs; redistributor stride 0x20000; GICD/GICR PAs | 0004; shim/shim.c; mkadt.py:23-24 | The Group 1 NS path works whether DS is 0 or 1. Stride would differ for a GICv4 (VLPI) redistributor; PAs differ; INTID 28 "constantly pending PPI" was a sweet-specific quirk | Print `GICD_TYPER`, `GICD_CTLR`, `GICR_TYPER` (VLPIS, Last), `ICC_SRE_EL1` from the shim |
| H8 | **MPIDR layout** `Aff1 = core id, Aff0 = 0` (`0x81000n00`), max `0x700`; secondary entry indexes `ResetHandlerData` by `MPIDR & 0xFFFF`; 8 cores as one homogeneous cluster | 0065/0078; mkadt.py:127-132; 0065:126 | gs101 MPIDRs are probably the same style but verify; the three clusters differ in speed/cache (X1 pair, A76 pair, A55 quad), and the scheduler will treat them as equal | Print each core's MPIDR after CPU_ON in the shim |
| H9 | **PSCI over `smc`**, CPU_ON64 `0xC4000003`; `SYSTEM_RESET` works, `SYSTEM_RESET2`/`SYSTEM_SUSPEND` do not; XNU never sets `PE_halt_restart` | 0065:32; shim.c; NOTES | Probably `smc` on gs101 as well **[verify]**; reboot/power-off in XNU just spins ("MACH Reboot"), so recovery to the bootloader depends on holding keys | Shim already prints PSCI version/features; add `PE_halt_restart` -> PSCI `SYSTEM_RESET`/`SYSTEM_OFF` (small new patch) |
| H10 | **ABL load address `0x80080000`** and Linux Image header (`text_offset 0`, flags `0xa`) | NOTES "Milestone 3"; start.S | Not a code dependency (the shim is PIC and `build.sh` fails if it finds ABS64 relocations). Still check that the shim image, the FDT and the ramdisk do not overlap `[physBase, +memSize)`: the loader only tests the ramdisk | Add an overlap check for the shim image and FDT in `loader.c` |
| H11 | **Framebuffer**: `0x9c000000`, 1080x2400, stride 4320, `a8r8g8b8`/BGRA (`0x42475241`), kept alive by ABL, and **video-mode continuous scanout** so a RAM write shows up with no kick. The address is a compile-time constant in 7 places (console.h, mmu.h, mkadt.py, qemu-sweet.py, 0010 start.s and sweet_early.c, 0012) and `v_baseAddr` (boot_args) | NOTES "Hardware facts"; 0010/0012/0046/0056/0079 | Pixel panels are normally command-mode, TE-driven (DECON/DSIM triggers each frame) **[verify]**. If so, writing the splash buffer shows nothing, and the console and the entire GUI path need a DECON "frame update" trigger or panel refresh driver (new work, possibly large). The bootloader may also tear down DECON before kernel handoff. Byte order of the Pixel splash may be RGBA | Milestone (a) answers this: fill the FB and see. If dark, compare against a bootloader that keeps the splash (fastboot screen), and plan a DECON re-arm |
| H12 | **Watchdogs/PMU**: Qualcomm msm-watchdog was ruled out (ABL disabled it); `wdt=-1` is only an XNU flag; no SoC watchdog service in the kernel | NOTES "Watchdog ruled out..." | The Exynos/Google bootloader may leave an SoC watchdog armed, which resets the phone after tens of seconds | Observe: if the phone reboots to the bootloader after a fixed delay with a healthy screen, add a minimal WDT disable (new patch) |
| H13 | **Boot image and log retrieval**: `mkbootimg --header_version 2` with embedded DTB and Qualcomm offsets; the payload is found at `linux,initrd-start`; logs are read through OrangeFox adb root from the end of `userdata`; large flashes use `fastboot -S 512M`, with Xiaomi-specific hang workarounds | shim/build.sh, loader.c:88, sweet-log-pull.sh, README | Pixel uses header v4 + `vendor_boot` (initrd = vendor ramdisk ++ boot ramdisk, so the payload is not at `initrd-start`); no OrangeFox; `fastboot` has a different `max-download-size` **[verify]** | Build a v4 boot image with the payload as boot ramdisk and make `loader_boot` scan `[initrd-start, initrd-end)` for `SWEETXNU`; plan a log channel (USB ECM 0075-0077 once USB works, or a UART below) |
| H14 | **Debug channel**: sweet has no UART ("debug UART is SPI-mode"), so everything goes through the screen and the UFS log | NOTES | Pixel 6a has a debug UART reachable via a USB-C debug accessory **[verify]**. XNU's `pe_serial.c` already contains an S5L (Samsung-lineage) UART driver which may be register-compatible with the gs101 USI UART (unverified idea). That would give a serial console independent of the display (H11) | Check UART base and clock in the DT; try `serial=` with an ADT `uart0` node |
| H15 | **Peripheral interrupts are not used**: UFS, touch, keys, USB are polled; only the architected timer and SGI1 are wired | all drivers | Positive: no SPI numbers need porting | — |
| H16 | **Qualcomm drivers run unconditionally** from `sweet_ufs_init` (`sweet_keys_init`, `sweet_touch_init`, `sweet_usb_probe`; USB probe defaults to on) and access fixed Qualcomm PAs | 0060, 0072 | On gs101 these addresses are unmapped or are other blocks: bus error/SError, panic, or hang | Boot with `sweetusb=0 sweetkeys=0 sweettouch=0` until replaced, then add the ADT/compatible gate (§2.2) |
| H17 | **Identity props**: synthetic ECID `'SWEET'+1`, `chip-id 0x8015` (A11), `boot-manifest-hash` of `"sweet boot manifest"`, `firmware-version sweet-shim` | mkadt.py:26,36,104-105 | Not hardware dependent; keep so the iPhone X userland behaves the same. A per-device ECID (for example derived from the UFS device serial) is optional | — |
| H18 | **CPU model strings and clocks** in the ADT: `qcom,kryo470`, `clock-frequency` 1.8/2.3 GHz, bus 100 MHz, memory 1866 MHz, per-core L2, shared L3 | mkadt.py:131,138-145 | Only cosmetic/accounting, but the IOKit CPU personality matches the compatible string | Use the new neutral compatible from §2.2 |
| H19 | **Panel geometry and iOS presentation** (1080x2400 centred in 1125x2436, touch scaling 1080x2400, 458 dpi iPhone X pitch) | 0061, 0058 | None: same 1080x2400 on the Pixel 6a; OLED like the Redmi | — |
| H20 | **Non-coherent DMA and cache maintenance** (flush by PA around every command) | 0028, 0069, 0073 | Safe on a coherent controller (extra maintenance only). Slower than necessary | Optional tuning with `ufscache=1` |

---

## 5. Suggested first measurements (all possible with only the shim, no XNU)

1. Shim milestone (a) with a FB fill + text dump of: `CurrentEL`, MIDR/MPIDR per core, `ID_AA64MMFR0_EL1` (TGran16, PARange) per core, `CNTFRQ_EL0`, `GICD_TYPER/CTLR`, `GICR_TYPER`, PSCI version/features, the FDT `memory` and `/reserved-memory` nodes (H1, H2, H5-H8, H11).
2. Existing `test_ufs()` against the Pixel UFS base (H3, H4).
3. A tiny memtest at `0x8_8000_0000`-class addresses with the MMU off (H5).
4. Only then start applying patches 0001-0020 and the `mkadt.py --board bluejay` table.

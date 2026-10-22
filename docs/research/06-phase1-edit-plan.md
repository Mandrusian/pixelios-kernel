# Phase 1 Kernel Patches Audit: Qualcomm Sweet to Google Tensor Bluejay

This audit examines patches `0001` through `0030` of `xnu/patches/` to identify every hardcoded constant, board-specific assumption, and device dependency that must be addressed when porting the XNU kernel from Qualcomm Snapdragon 732G ("sweet") to Google Tensor G1 gs101 ("bluejay", Pixel 6a).

Target milestone: **XNU boots and prints to the framebuffer console, then waits for the root device.**

---

## 1. Patches Clean of Board-Specific Constants

The following 21 patches were audited and confirmed clean of Qualcomm sweet board-specific hardware constants or assumptions:
- `0001-new-clang-drop-removed-auto-var-init-flag.patch`: Compiler flag compatibility fix.
- `0002-new-clang-backport-UIO_STACKBUF.patch`: Upstream XNU BSD stack buffer backport.
- `0005-non-pac-build-fixes.patch`: Pmap struct reserved field layout fix for non-PAC ARM64.
- `0006-sweet-master-config-open-source-asm-pmap.patch`: Kernel config file `MASTER.arm64.sweet` (machine profile name only).
- `0007-sweet-stub-apple-internal-headers.patch`: Stubs for Apple internal headers (`sart.h`, `uat.h`, `tunables.h`).
- `0008-sweet-fill-closed-lib-gaps-kpc-stub.patch`: Stubs for closed Apple libraries (`kpc`, `amcc_rorgn`, `ml_enable_monitor`).
- `0009-sweet-drop-apple-soc-only-exports.patch`: Export list cleanup for open-source kernel build.
- `0011-sweet-physmap-alignment-padding.patch`: Generic non-PPL physmap calculation (`ARM64_PHYSMAP_ALIGNMENT_PADDING`).
- `0013-sweet-kmem-rng-without-corecrypto.patch`: Self-contained SplitMix64 PRNG for `kmem` before crypto kexts load.
- `0014-sweet-kernel-hmac-prng.patch`: Kernel entropy accumulator and HMAC-SHA256 DRBG using built-in XNU crypto.
- `0015-sweet-unavailable-trust-cache-runtime.patch`: Trust cache runtime stubs when AppleImage4 / AMFI are omitted.
- `0019-sweet-builtin-pthread.patch`: In-tree Apple `libpthread` provider compilation.
- `0020-sweet-portable-aes-cbc.patch`: Portable BearSSL AES-CBC provider for early crypto.
- `0021-sweet-mockfs-ramdisk-root.patch`: MockFS root support without `IOStorageFamily`.
- `0022-sweet-ramdisk-root-without-iomedia.patch`: Ramdisk unhide check when `IOMedia` class is absent.
- `0023-sweet-builtin-sha256-for-libkern.patch`: Built-in SHA-256 for `libkern` digest routines.
- `0024-sweet-mockfs-virtual-ramdisk-io.patch`: MockFS 4 KiB sector translation and page shift fix.
- `0025-sweet-builtin-amfi-no-entitlements.patch`: In-tree stub AMFI implementation returning no entitlements.
- `0026-sweet-builtin-hfs.patch`: In-tree Apple OSS HFS filesystem implementation (`hfs-627.100.6`).
- `0027-sweet-bearssl-md5.patch`: Portable BearSSL MD5 provider.
- `0029-sweet-unsealed-root-volume.patch`: Bypasses root hash authentication panic for unsigned bring-up images.

---

## 2. Inventory of Board Constants & Assumptions (Patches 0001–0030)

| Patch File | Patch Line | File & Function | Exact Quoted Constant / Assumption | Category | Bluejay Value / Source | Recommended Minimal Change |
|---|---|---|---|---|---|---|
| `0003-sweet-framebuffer-serial-console.patch` | Line 129 | `pexpert/arm/pe_serial.c:fbcon_probe` | `if (SecureDTFindEntry("name", "sweet-fbcon", &entry) != kSuccess \|\|` | `compatible-string` | `/sweet-fbcon` node in ADT (`mkadt.py:221`) | **No change needed** (ADT generates node name `sweet-fbcon`). |
| `0003-sweet-framebuffer-serial-console.patch` | Lines 134–145 | `pexpert/arm/pe_serial.c:fbcon_probe` | `SecureDTGetProperty(entry, "reg", (void const **)&reg, &size)` | `framebuffer` / `MMIO address` | Dynamic from ADT properties `reg`, `width`, `height`, `stride` | **No change needed** (ALREADY read dynamically from ADT at runtime). |
| `0004-sweet-machine-config.patch` | Line 18 | `makedefs/MakeInc.def` | `MACHINE_FLAGS_ARM64_SWEET = -DARM64_BOARD_CONFIG_SWEET                -march=armv8.2-a+crypto` | `other` | Tensor G1 (Cortex-A55/A76/X1) is ARMv8.2-A with crypto | **No change needed** for Phase 1 (reuse machine config `SWEET` or add `BLUEJAY`). |
| `0004-sweet-machine-config.patch` | Line 45 | `osfmk/arm64/proc_reg.h` | `#define MMU_I_CLINE 6` / `#define MMU_CLINE 6` | `other` | 64-byte line size (`2^6 = 64`) on Cortex-A55, A76, X1 | **No change needed** (cache line size is identical). |
| `0004-sweet-machine-config.patch` | Line 80 | `osfmk/arm64/sleh.c:sleh_irq` | `if (intid == SWEET_VTIMER_INTID) {` | `IRQ number` | INTID 27 (`boards.py:vtimer_intid`, PPI 11) | Superseded by patch 0017 (reads `sweet_vtimer_intid` from ADT). |
| `0004-sweet-machine-config.patch` | Line 153 | `pexpert/arm/pe_fiq.c:pe_init_fiq` | `gicr_write32(GICR_ISENABLER0, (1U << SWEET_VTIMER_INTID) \| (1U << SWEET_IPI_SGI));` | `IRQ number` | INTID 27 | Superseded by patch 0017 (reads `sweet_vtimer_intid` from ADT). |
| `0004-sweet-machine-config.patch` | Line 263 | `pexpert/pexpert/arm64/SWEET.h` | `#define SWEET_VTIMER_INTID        19` | `IRQ number` | 27 (`boards.py:vtimer_intid`) | Change fallback default to 27 or rely on patch 0017 ADT property. |
| `0004-sweet-machine-config.patch` | Line 264 | `pexpert/pexpert/arm64/SWEET.h` | `#define SWEET_IPI_SGI             1` | `IRQ number` | SGI 1 (cross-CPU signal) | **No change needed** (SGI 1 standard across GICv3 implementations). |
| `0004-sweet-machine-config.patch` | Line 268 | `pexpert/pexpert/arm64/SWEET.h` | `#define GICR_PE_SIZE              0x20000` | `MMIO address` | `0x20000` (128 KiB stride, `boards.py:gicr_stride`) | **No change needed** (both Sweet and Bluejay use 128 KiB redistributor stride). |
| `0004-sweet-machine-config.patch` | Line 310 | `pexpert/pexpert/arm64/board_config.h` | `#define MAX_CPU_CLUSTERS               1` | `other` | 3 clusters (`mkadt.py:146`: 4xA55 cluster 0, 2xA76 cluster 1, 2xX1 cluster 2) | Change `MAX_CPU_CLUSTERS` to 3 (or emit single cluster in ADT during single-core phase 1). |
| `0010-sweet-early-framebuffer-debug.patch` | Line 99 | `osfmk/arm64/start.s:start_first_cpu` | `MOV64	x0, (0x9c000000 + 2368 * 4320)` | `framebuffer` | `0xFAC00000 + 2368 * 4320` (`boards.py:fb_base` / `boot_args->Video.v_baseAddr`) | **Required change**: Update `0x9c000000` to `0xFAC00000` or read base from `boot_args`. |
| `0010-sweet-early-framebuffer-debug.patch` | Line 131 | `osfmk/arm64/start.s:start_first_cpu` | `MOV64	x10, 0x9c000000` | `framebuffer` / `MMIO address` | `0xFAC00000` (`boards.py:fb_base`) | **Required change**: Update early L2 block identity mapping to `0xFAC00000`. |
| `0010-sweet-early-framebuffer-debug.patch` | Line 136 | `osfmk/arm64/start.s:start_first_cpu` | `add	x13, x25, #(2 * PGBYTES)` | `DRAM base or size` / `framebuffer` | RAM bank 0 (`0x80000000`) and FB (`0xFAC00000`) are both in L1 index 0 (< 4 GiB) | **No change needed** (assumption holds: RAM bank 0 and FB are both within 32-bit physical address space). |
| `0010-sweet-early-framebuffer-debug.patch` | Line 161 | `osfmk/arm64/sweet_early.c` | `#define EARLY_FB ((volatile uint32_t *)(uintptr_t)0x9c000000)` | `framebuffer` | `0xFAC00000` (`boards.py:fb_base`) | **Required change**: Update `0x9c000000` to `0xFAC00000`. |
| `0010-sweet-early-framebuffer-debug.patch` | Lines 162–164 | `osfmk/arm64/sweet_early.c` | `#define EARLY_WIDTH 1080u` / `#define EARLY_HEIGHT 2400u` / `#define EARLY_STRIDE (4320u / 4)` | `framebuffer` | Width 1080, Height 2400, Stride 4320 (`boards.py`) | **No change needed** (dimensions and byte stride match sweet exactly). |
| `0012-sweet-keep-debug-console-after-ttbr0.patch` | Line 26 | `osfmk/arm64/arm_vm_init.c:sweet_early_map_final` | `*slot = 0x9c000000ULL \| ARM_TTE_VALID \| ARM_TTE_TYPE_BLOCK \|` | `framebuffer` / `MMIO address` | `0xFAC00000ULL` (`boards.py:fb_base`) | **Required change**: Update physical address in `*slot` from `0x9c000000ULL` to `0xFAC00000ULL`. |
| `0012-sweet-keep-debug-console-after-ttbr0.patch` | Line 104 | `osfmk/arm64/sweet_early.c` | `static volatile uint32_t *early_fb = (volatile uint32_t *)(uintptr_t)0x9c000000;` | `framebuffer` | `0xFAC00000` (`boards.py:fb_base`) | **Required change**: Update `0x9c000000` to `0xFAC00000`. |
| `0016-sweet-device-tree-platform-expert.patch` | Line 12 | `iokit/KernelConfigTables.cpp:gIOKernelConfigTables` | `"     'IONameMatch'     = 'xiaomi,sweet';"` | `compatible-string` | Root node compatible property in ADT (`mkadt.py:234`) | **No change needed** if `mkadt.py` keeps `'xiaomi,sweet'`; or change to `'google,bluejay'`. |
| `0016-sweet-device-tree-platform-expert.patch` | Line 108 | `iokit/Kernel/arm/SweetPlatformExpert.cpp:getTargetName` | `strlcpy(name, "sweet", (size_t)maxLength);` | `other` | `"bluejay"` | Change `"sweet"` to `"bluejay"` (or read target name from ADT). |
| `0016-sweet-device-tree-platform-expert.patch` | Line 116 | `iokit/Kernel/arm/SweetPlatformExpert.cpp:getProductName` | `strlcpy(name, "Redmi Note 10 Pro", (size_t)maxLength);` | `other` | `"Pixel 6a"` | Change `"Redmi Note 10 Pro"` to `"Pixel 6a"`. |
| `0017-sweet-device-tree-virtual-timer-irq.patch` | Line 13 | `pexpert/arm/pe_fiq.c` | `SECURITY_READ_ONLY_LATE(uint32_t) sweet_vtimer_intid = SWEET_VTIMER_INTID;` | `IRQ number` | 27 (`boards.py:vtimer_intid`) | Fallback default; overridden dynamically by ADT property. |
| `0017-sweet-device-tree-virtual-timer-irq.patch` | Line 33 | `pexpert/arm/pe_fiq.c:pe_init_fiq` | `if (SecureDTGetProperty(entry, "virtual-timer-intid", &timer_prop, &timer_size) == kSuccess) {` | `IRQ number` | Read from `/arm-io/gic` node `virtual-timer-intid` (27) | **No change needed** (ALREADY read dynamically from ADT at runtime). |
| `0018-sweet-boot-cpu-driver.patch` | Line 54 | `iokit/Kernel/arm/SweetCPU.cpp:SweetCPU::start` | `if (topology->num_cpus != 1) {` | `other` | ADT CPU count (`mkadt.py:144`) | Ensure `mkadt.py` emits exactly 1 CPU node for Phase 1 bringup, or remove panic. |
| `0018-sweet-boot-cpu-driver.patch` | Line 60 | `iokit/Kernel/arm/SweetCPU.cpp:SweetCPU::start` | `cpuName = OSSymbol::withCStringNoCopy("Kryo 470");` | `other` | `"Cortex-A55"` (boot CPU on Bluejay) | Cosmetic: Change `"Kryo 470"` to `"Cortex-A55"`. |
| `0018-sweet-boot-cpu-driver.patch` | Line 148 | `iokit/Kernel/arm/SweetCPU.cpp:SweetCPU::signalCPU` | `uint64_t sgi = (((mpidr >> 32) & 0xff) << 48) \|` | `MPIDR` | Aff0 = 0 (`boards.py:cpu_mpidrs`) | **No change needed** (correctly maps Aff0..Aff3; does not assume bit 31 is set or clear). |
| `0018-sweet-boot-cpu-driver.patch` | Line 187 | `iokit/KernelConfigTables.cpp:gIOKernelConfigTables` | `"     'IONameMatch'     = 'qcom,kryo470';"` | `compatible-string` | CPU compatible in ADT is `'arm,cortex-a55'` (`mkadt.py:145`) | **Required change**: Change `'qcom,kryo470'` to `'arm,cortex-a55'`. |
| `0028-sweet-ufs-root-device.patch` | Line 188 | `bsd/dev/sweet_ufs.c:ufs_scsi` | `utrd->resp = (512u / 4) << 16 \| (512u / 4);` | `UFS` | Google gs101 UFS host uses byte units (`boards.py:ufs_prdt_byte_gran = 1`) | **Required for UFS**: Change to `512u << 16 \| 512u` (byte granularity). |
| `0028-sweet-ufs-root-device.patch` | Line 197 | `bsd/dev/sweet_ufs.c:ufs_scsi` | `utrd->prdt = (1024u / 4) << 16 \| n;` | `UFS` | Google gs101 UFS host uses byte units (`boards.py:ufs_prdt_byte_gran = 1`) | **Required for UFS**: Change to `1024u << 16 \| (n * 16u)` (byte granularity). |
| `0028-sweet-ufs-root-device.patch` | Line 510 | `bsd/dev/sweet_ufs.c:sweet_ufs_init` | `if (SecureDTLookupEntry(NULL, "/sweet-ufs", &node) != kSuccess \|\|` | `MMIO address` / `compatible-string` | `/sweet-ufs` node in ADT (`mkadt.py:215`), base `0x14700000`, size 512 | **No change needed** (MMIO base and size are ALREADY read from ADT at runtime). |
| `0028-sweet-ufs-root-device.patch` | Line 516 | `bsd/dev/sweet_ufs.c:sweet_ufs_init` | `PE_parse_boot_argn("ufsroot", want, sizeof(want));` | `boot-arg` | Partition name (defaults to `"userdata"`) | **No change needed** (configurable via boot argument `ufsroot=<name>`). |
| `0030-sweet-ufs-writes.patch` | Line 197 | `bsd/dev/sweet_ufs.c:sweet_ufs_init` | `PE_parse_boot_argn("ufsrw", &rw, sizeof(rw));` | `boot-arg` | Boot-arg `ufsrw=1` gates writes | **No change needed** (read-only by default unless `ufsrw=1` supplied). |

---

## Section (A): Minimal Ordered Changes for Milestone "Kernel Prints on Framebuffer"

To achieve the milestone where XNU boots, runs through `arm_vm_init`, initializes the framebuffer console via `pe_serial.c`, prints banner text, and waits for a root filesystem, only the following changes are required in order of execution:

1. **`0010-sweet-early-framebuffer-debug.patch` (`osfmk/arm64/start.s:99, 131`)**
   - **Change**: Change physical address `0x9c000000` to `0xFAC00000` for both the early MMU-off splash bar and the bootstrap L2 block entry.
   - **Risk**: **HIGH**. Writing to `0x9c000000` on Bluejay before MMU is on causes an unhandled physical bus fault or memory corruption.
2. **`0010-sweet-early-framebuffer-debug.patch` (`osfmk/arm64/sweet_early.c:161`)**
   - **Change**: Change `#define EARLY_FB` physical address from `0x9c000000` to `0xFAC00000`.
   - **Risk**: **HIGH**. Early diagnostic marks (stages 0–7) will write to unmapped physical memory and crash the CPU.
3. **`0012-sweet-keep-debug-console-after-ttbr0.patch` (`osfmk/arm64/arm_vm_init.c:26` & `sweet_early.c:104`)**
   - **Change**: Change `*slot` physical block base from `0x9c000000ULL` to `0xFAC00000ULL`, and update `early_fb` initial pointer to `0xFAC00000`.
   - **Risk**: **HIGH**. Once bootstrap TTBR0 is torn down, stages 8–16 write through `SWEET_EARLY_FB_VA`. If the underlying physical address in the L2 entry is sweet's `0x9c000000`, the system page-faults or hangs during `arm_vm_init`.
4. **`0018-sweet-boot-cpu-driver.patch` (`iokit/KernelConfigTables.cpp:187`)**
   - **Change**: Change `IONameMatch` from `'qcom,kryo470'` to `'arm,cortex-a55'`.
   - **Risk**: **HIGH**. The boot CPU driver (`SweetCPU`) matches on the device tree compatible string. If it fails to match `'arm,cortex-a55'`, no CPU is registered, `gIOAllCPUInitializedKey` is never published, and the kernel hangs indefinitely in IOKit initialization.
5. **ADT Generation (`tools/mkadt.py:144`)**
   - **Change**: Ensure `mkadt.py` emits `range(1)` (1 CPU node) for Phase 1 bringup, or remove the `topology->num_cpus != 1` panic in `0018-sweet-boot-cpu-driver.patch` (`SweetCPU.cpp:54`).
   - **Risk**: **MEDIUM**. If ADT contains 8 CPUs but patch 0018 is built as-is, XNU immediately panics with `"SWEET CPU: only single-CPU bring-up is supported"`.
6. **`0016-sweet-device-tree-platform-expert.patch` (`iokit/KernelConfigTables.cpp:12` & `tools/mkadt.py:234`)**
   - **Change**: Keep `'xiaomi,sweet'` in the root node compatible property in `mkadt.py`, OR update both `mkadt.py` and `KernelConfigTables.cpp` to `'google,bluejay'`.
   - **Risk**: **MEDIUM**. Mismatch prevents `SweetPlatformExpert` from matching, falling back to `IOPanicPlatform`.
7. **`0028-sweet-ufs-root-device.patch` (`bsd/dev/sweet_ufs.c:188, 197`)**
   - **Change**: Update `utrd->resp` and `utrd->prdt` to byte units (`512u << 16 | 512u` and byte-granular PRDTO/PRDTL) when rooting from internal UFS (`rd=ufsroot0`).
   - **Risk**: **HIGH** if rooting from UFS; **LOW** / **NONE** if booting with a MockFS RAM disk (`rd=md0`).

---

## Section (B): Items Not Determinable from Patch Text Alone

The following hardware and firmware properties cannot be determined from the XNU patch text alone and require live validation:

1. **Live Framebuffer State Across Handoff**: Does the Tensor G1 bootloader keep the display pipeline and DSI/MIPI scanout actively refreshing from `0xFAC00000` when handing execution over to the kernel, or does the display engine need periodic VSYNC kicks from an interrupt handler?
2. **Bootloader UFS Link State**: Does Google Tensor's bootloader leave the UFSHCI host controller initialized with operational link-up and UTRL running when jumping to the kernel image (as Qualcomm ABL does), or does Tensor firmware power-gate or reset the UFS controller before booting?
3. **Exynos/Tensor UFS HCI Vendor Quirks**: Beyond the known byte granularity for response UPIU and PRDT offsets, does the gs101 UFS host require initialization of Samsung/Google-specific vendor registers (e.g. HCI version override, FMP cryptographic bypass, or vendor PRDT boundary alignments) before honoring transfer request doorbells?
4. **Firmware PSCI Implementation**: When secondary cores are booted later, is PSCI implemented in EL3 (Google Trusty / ATF) via standard SMCs (`0x84000000` / `0xC4000000`), and does `PSCI_CPU_ON` wake cores 1–7 directly at the kernel entry point?
5. **DRAM Bank 0 Carveouts**: Are there secure TrustZone, modem, or camera/AP carveouts located inside Bluejay's first 2 GiB DRAM bank (`0x80000000`–`0x100000000`) that must be excluded from `dram-size` or `physBase` to prevent synchronous external aborts during physical aperture mapping?

---

## Section (C): Cross-Patch Silent Dependencies

Several patches silently depend on constants, macros, or runtime behaviors introduced by earlier patches:

1. **`0017` depends on `0004`**:
   - `0017-sweet-device-tree-virtual-timer-irq.patch` defines `sweet_vtimer_intid = SWEET_VTIMER_INTID;`. The fallback macro `SWEET_VTIMER_INTID` is defined in `pexpert/pexpert/arm64/SWEET.h`, which is created in `0004-sweet-machine-config.patch`.
2. **`0018` depends on `0004`**:
   - `0018-sweet-boot-cpu-driver.patch` calls `__builtin_arm_wsr64("ICC_SGI1R_EL1", ... | ((uint64_t)SWEET_IPI_SGI << 24) | ...)` in `SweetCPU::signalCPU()`. The constant `SWEET_IPI_SGI` (1) is defined in `pexpert/pexpert/arm64/SWEET.h` from patch `0004`.
3. **`0012` depends on `0010`**:
   - `0012-sweet-keep-debug-console-after-ttbr0.patch` modifies `sweet_early.c` and calls `sweet_early_mark()`, `sweet_early_relocate()`, and relies on early debug framebuffer concepts and font data introduced in `0010-sweet-early-framebuffer-debug.patch`.
4. **`0030` depends on `0028`**:
   - `0030-sweet-ufs-writes.patch` directly modifies `bsd/dev/sweet_ufs.c` (adding write capability and `ufsrw` boot argument handling). The entire UFS driver file was originally created by `0028-sweet-ufs-root-device.patch`.
5. **`0028` depends on `0003` & `0004`**:
   - `0028-sweet-ufs-root-device.patch` relies on `ml_io_map()` and the DT node parsing infrastructure (`SecureDTLookupEntry`, `SecureDTGetProperty`) established in pexpert and used by the serial framebuffer console.

---

## Lead review (2026-10-21, Claude): what was verified and what the delegate missed

Verified against the patch text (grep, exact line numbers): 0010:99,131,161 and 0012:26,104 (`0x9c000000`), 0010:162-164 (1080x2400, stride 4320),
0018:54 (`num_cpus != 1` panic), 0018:60/187 (`Kryo 470`, `qcom,kryo470`), 0004:80/153/263 (`SWEET_VTIMER_INTID`=19, superseded by 0017's ADT read).
Treat the rest of the table as plausible but unverified.

Corrections / additions:
1. **Framebuffer address is the biggest edit and it is not yet known.** The bluejay value `0xFAC00000` is the Oriole (Pixel 6) value from mainline; the
   probe reads the live FDT but its screen output was never captured. Read it from the USB/serial log before editing 0010/0012. Prefer a templated
   constant (one CI step that seds a single placeholder in the patches) over scattering a second literal.
2. **Physical memory base (the Gemini question B5) is decided by the SHIM, not the kernel patches**: `tools/mkpayload.py` / `shim/loader.c` pick
   `physBase`/`memSize` for boot_args. sweet used 0xb0000000 (below Qualcomm XPU carve-outs, DRAM above 4 GiB). For bluejay it must be one hole-free,
   32 MiB-aligned range inside a `memory` bank that avoids every reserved-memory range (live FDT map). Needs the probe's memory-map output.
3. CPU: both `qcom,kryo470` (KernelConfigTables) and the `SweetCPU` name must follow the ADT CPU compatible that mkadt.py emits for the boot CPU
   (bluejay: `arm,cortex-a55`). With `cpus=1` and ONE cpu node in the ADT the single-CPU panic is not hit.
4. Out of scope here but needed before CI is worth running: gate the Qualcomm drivers off (`sweetusb=0 sweetkeys=0 sweettouch=0` boot-args as the stopgap).
5. CI cost: the workflow runs on `macos-14` (up to 120 min). On a private repo macOS minutes bill at 10x of the free allowance; decide the budget with the user
   before the first run.

# 14 - xnu patch audit for bluejay (Pixel 6a, gs101)

Scope: classify every file in `xnu/patches/` (88 patches + 3 stagers) as needed or not for the bluejay GUI path, archive
what is Qualcomm-only. Evidence tag for every row: **read from source** (= VERIFIED-FROM-SOURCE) unless it says *guess*. Nothing here was run on
hardware. "file:line" is a line in the patched tree (xnu-8796.141.3 + the stack) unless a `NNNN:` prefix says it is a line
inside the `.patch` file. "Old" numbers are the numbers before the renumbering of this change; "new" are the current
ones (map: `xnu/patches-archive/README.md`).

## Classes

- KEEP-CORE: generic XNU / iOS-userland compatibility, needed on any board that runs this stack.
- KEEP-BLUEJAY: gs101-specific, or shared code the gs101 code is built on.
- SWEET-ONLY: Qualcomm SM7150 hardware driver, unused when the ADT says google bluejay.
- DIAGNOSTIC: debug / log / trace aids; kept.
- SUPERSEDED: fully replaced by a later patch. **None** qualifies: 0035 rewrites 35 lines of 0025, 0053 rewrites 4 of 0051,
  0075 rewrites 13 of 0073, 0084 rewrites parts of 0072/0073, but each earlier patch still creates the file or the
  code the later one edits. Squashing them would only churn history, so they stay.

## How the code is gated (read from source)

Both boards build the same kernel (`MACHINE_CONFIG=SWEET`, `ARM64_BOARD_CONFIG_SWEET`, `pexpert/pexpert/arm64/board_config.h:100`),
so there is no compile-time board switch. Selection happens at run time:

- ADT root `compatible` keeps `xiaomi,sweet` on every board so `SweetPlatformExpert` matches (`tools/mkadt.py:255`,
  `iokit/KernelConfigTables.cpp:39-41`); the cpu nodes use the personality key `qcom,kryo470` on every board (`tools/mkadt.py:137`).
- gs101 selection by ADT node: `/sweet-ufs` compatible `google,gs101-ufs` (`bsd/dev/sweet_ufs.c:617`), `/sweet-usb`
  `google,gs101-dwusb3` (`bsd/dev/sweet_usb.c:889-912`), `/sweet-reset` (`bsd/dev/sweet_restart.c:7`); `tools/mkadt.py:233-248`
  only emits the last two for boards with `usb_base` / `pmu_base` in `soc.json`.
- The Qualcomm drivers had **no hardware check**: `sweet_keys_init()` and `sweet_touch_init()` were called unconditionally from
  `sweet_ufs_init()` and mapped Qualcomm physical addresses (`sweet_keys.c:126-140`); their only off switch is the boot-arg
  (`sweetkeys=0`, `sweettouch=0`), which `tools/build-xnu-image.sh:19` passes. So on bluejay they were disabled by
  boot-args, not by the ADT. That is why archiving them is safe and also removes a foot-gun (a bluejay image without those
  boot-args would have touched SM7150 MMIO).
- `sweet_usb_probe()` still runs the Qualcomm probe when no `/sweet-usb` gs101 node is present (default `sweetusb=1`, read-only,
  `sweet_usb.c:102-108`), `SweetBattery` touches SPMI only with `sweetbattery=1` (`SweetBattery.cpp:3`, `:267`).

## Table

| Old | New | Name | Class | Evidence (read from source) | Dependents | Action |
|---|---|---|---|---|---|---|
| 0001 | 0001 | `new-clang-drop-removed-auto-var-init-flag` | KEEP-CORE | Drops a clang flag the CI toolchain no longer has (makedefs/MakeInc.def); ungated build fix | none | keep |
| 0002 | 0002 | `new-clang-backport-UIO_STACKBUF` | KEEP-CORE | Backports UIO_STACKBUF for new clang (bsd/sys/uio_internal.h); ungated | none | keep |
| 0003 | 0003 | `sweet-framebuffer-serial-console` | KEEP-CORE | Framebuffer text console from ADT node "sweet-fbcon" (pexpert/arm/pe_serial.c:706); mkadt.py:197 emits it for every board | 0012 (same file), 0017 (protos.h) | keep |
| 0004 | 0004 | `sweet-machine-config` | KEEP-CORE | SWEET machine config (pexpert/pexpert/arm64/board_config.h:100); both boards build with MACHINE_CONFIG=SWEET | every later SWEET-gated hunk | keep |
| 0005 | 0005 | `non-pac-build-fixes` | KEEP-CORE | Non-PAC build fix in osfmk/arm/pmap/pmap.h; gs101 cores run without PAC (xnu/AGENTS.md) | none | keep |
| 0006 | 0006 | `sweet-master-config-open-source-asm-pmap` | KEEP-CORE | config/MASTER.arm64.sweet: open-source asm/pmap config | 0007, 0008, 0021, 0026, 0037, 0057 (same file) | keep |
| 0007 | 0007 | `sweet-stub-apple-internal-headers` | KEEP-CORE | Stubs Apple-internal headers (sart.h, uat.h, tunables.s) | none | keep |
| 0008 | 0008 | `sweet-fill-closed-lib-gaps-kpc-stub` | KEEP-CORE | Fills closed-library gaps (kpc stub, amcc_rorgn); needed to link any SWEET kernel | none | keep |
| 0009 | 0009 | `sweet-drop-apple-soc-only-exports` | KEEP-CORE | Drops Apple-SoC-only exports from config/Private*.exports | none | keep |
| 0010 | 0010 | `sweet-early-framebuffer-debug` | KEEP-CORE | Early framebuffer debug (osfmk/arm64/sweet_early.c); fb address comes from the ADT, not a constant | 0012 | keep |
| 0011 | 0011 | `sweet-physmap-alignment-padding` | KEEP-CORE | Physmap alignment padding (arm_vm_init.c); generic | none | keep |
| 0012 | 0012 | `sweet-keep-debug-console-after-ttbr0` | KEEP-CORE | Keeps the debug console alive after the TTBR0 switch; generic | none | keep |
| 0013 | 0013 | `sweet-kmem-rng-without-corecrypto` | KEEP-CORE | kmem RNG without corecrypto (osfmk/vm/vm_kern.c) | none | keep |
| 0014 | 0014 | `sweet-kernel-hmac-prng` | KEEP-CORE | Kernel HMAC PRNG without corecrypto | none | keep |
| 0015 | 0015 | `sweet-unavailable-trust-cache-runtime` | KEEP-CORE | Trust cache runtime marked unavailable (bsd/kern/kern_trustcache.c) | none | keep |
| 0016 | 0016 | `sweet-device-tree-platform-expert` | KEEP-CORE | SweetPlatformExpert matches IONameMatch xiaomi,sweet (iokit/KernelConfigTables.cpp:39-41); mkadt.py:255 keeps that string in the bluejay ADT | 0046, 0059, 0085 (same file) | keep |
| 0017 | 0017 | `sweet-device-tree-virtual-timer-irq` | KEEP-CORE | Virtual-timer IRQ read from the ADT gic node (pexpert/arm/pe_fiq.c:109); soc.json supplies vtimer_intid | none | keep |
| 0018 | 0018 | `sweet-boot-cpu-driver` | KEEP-CORE | SweetCPU matches qcom,kryo470, a personality key mkadt.py:137 uses on every board | 0065, 0078 | keep |
| 0019 | - | `0019-stage-libpthread.sh` | KEEP-CORE | Stager: fetches the pinned source and writes it under the tree before the .patch files (dxb runs patches/*.sh first); MACHINE_CONFIG=SWEET only | its .patch of the same number | keep |
| 0019 | 0019 | `sweet-builtin-pthread` | KEEP-CORE | Staging script: pinned libpthread (xnu/patches/0019-stage-libpthread.sh), runs before the .patch files | patch 0019 (same number) | keep |
| 0020 | - | `0020-stage-bearssl.sh` | KEEP-CORE | Stager: fetches the pinned source and writes it under the tree before the .patch files (dxb runs patches/*.sh first); MACHINE_CONFIG=SWEET only | its .patch of the same number | keep |
| 0020 | 0020 | `sweet-portable-aes-cbc` | KEEP-CORE | Portable BearSSL AES (libkern/crypto/sweet_aes*.c) + stager 0020-stage-bearssl.sh | 0027, 0032, 0033 (libkern/conf/files) | keep |
| 0021 | 0021 | `sweet-mockfs-ramdisk-root` | DIAGNOSTIC | mockfs ramdisk root (rd=md0): used by tools/qemu-sweet.py; bluejay boots rd=ufsroot0. Generic, harmless | 0022, 0024 | keep |
| 0022 | 0022 | `sweet-ramdisk-root-without-iomedia` | DIAGNOSTIC | Ramdisk root without IOMedia (iokit/Kernel/IOService.cpp); QEMU path | 0086 (same file) | keep |
| 0023 | 0023 | `sweet-builtin-sha256-for-libkern` | KEEP-CORE | Built-in SHA-256 for libkern (corecrypto_sha2.c) | 0033 | keep |
| 0024 | 0024 | `sweet-mockfs-virtual-ramdisk-io` | DIAGNOSTIC | mockfs virtual ramdisk I/O; QEMU path | 0026 (mockfs_vfsops.c) | keep |
| 0025 | 0025 | `sweet-builtin-amfi-no-entitlements` | KEEP-CORE | Built-in AMFI policy, no entitlements (bsd/kern/sweet_amfi.c:25) | 0035, 0036, 0050, 0064, 0087 | keep |
| 0026 | - | `0026-stage-hfs.sh` | KEEP-CORE | Stager: fetches the pinned source and writes it under the tree before the .patch files (dxb runs patches/*.sh first); MACHINE_CONFIG=SWEET only | its .patch of the same number | keep |
| 0026 | 0026 | `sweet-builtin-hfs` | KEEP-CORE | Built-in Apple HFS+ (bsd/sweet_hfs, staged by 0026-stage-hfs.sh); iOS root is HFS+ | 0031, 0041, 0052 | keep |
| 0027 | 0027 | `sweet-bearssl-md5` | KEEP-CORE | BearSSL MD5 for libkern | none | keep |
| 0028 | 0028 | `sweet-ufs-root-device` | KEEP-BLUEJAY | UFS root device (bsd/dev/sweet_ufs.c); node /sweet-ufs, gs101 quirks added by 0082 (sweet_ufs.c:617) | 0030, 0034, 0069, 0082, ... | keep |
| 0029 | 0029 | `sweet-unsealed-root-volume` | KEEP-CORE | Unsealed root volume (bsd/kern/bsd_init.c) | none | keep |
| 0030 | 0030 | `sweet-ufs-writes` | KEEP-BLUEJAY | UFS writes, only with boot-arg ufsrw=1 (sweet_ufs.c:17); used by --rw images | 0034, 0069, 0082 | keep |
| 0031 | 0031 | `sweet-read-write-root` | KEEP-CORE | rootrw=1 root mounted read-write (bsd/vfs/vfs_subr.c:1263) | none | keep |
| 0032 | 0032 | `sweet-bearssl-sha1` | KEEP-CORE | BearSSL SHA-1 | none | keep |
| 0033 | 0033 | `sweet-bearssl-sha384-sha512` | KEEP-CORE | BearSSL SHA-384/512 | none | keep |
| 0034 | 0034 | `sweet-ufs-dma-above-4g` | KEEP-BLUEJAY | UFS DMA addressing above 4 GiB in sweet_ufs.c (patch hunk 0034:1-43); docs/PLAN.md:47 lists it in the gs101 UFS adaptation set (guess: gs101 needs it too) | none | keep |
| 0035 | 0035 | `sweet-amfi-entitlements-platform-binaries` | KEEP-CORE | AMFI entitlements and platform binaries (SweetEntitlements.cpp); rewrites 35 lines of 0025 but does not replace it (0025 still creates the file) | 0036, 0050, 0064, 0087 | keep |
| 0036 | 0036 | `sweet-amfi-cs-signed` | KEEP-CORE | AMFI cs-signed handling | none | keep |
| 0037 | 0037 | `sweet-config-bindfs` | KEEP-CORE | Adds bindfs to config/MASTER.arm64.sweet | none | keep |
| 0038 | 0038 | `sweet-applekeystore` | KEEP-CORE | AppleKeyStore stub (SweetKeyStore.cpp); KernelConfigTables.cpp:51 | 0043, 0055 | keep |
| 0039 | 0039 | `sweet-coredump-default-off` | KEEP-CORE | Coredump default off (kern_core.c) | none | keep |
| 0040 | 0040 | `sweet-iowatchdog` | KEEP-CORE | IOWatchdog stub service, no hardware (SweetWatchdog.cpp) | 0045 | keep |
| 0041 | 0041 | `sweet-hfs-link-notes` | KEEP-CORE | HFS link notes (hfs_catalog.c) | none | keep |
| 0042 | 0042 | `sweet-short-exec-log` | DIAGNOSTIC | Short exec log (bsd/kern/kern_exec.c:7414, SWEET-gated printf) | none | keep |
| 0043 | 0043 | `sweet-console-crash-log` | DIAGNOSTIC | Console crash log (osfmk/kern/exception.c, kern_exit.c) | 0044, 0051, 0053, 0055, 0066 | keep |
| 0044 | 0044 | `sweet-crash-log-reason` | DIAGNOSTIC | Crash log reason | none | keep |
| 0045 | 0045 | `sweet-iowatchdog-pmu-primary` | KEEP-CORE | watchdogd needs an IOPMUPrimary service on any board (SweetWatchdog.cpp:13-15); mkadt.py wdt node has no PMU | none | keep |
| 0046 | 0046 | `sweet-iosurface-iomfb` | KEEP-CORE | Built-in IOSurface + IOMobileFramebuffer (SweetSurface/SweetFramebuffer.cpp); GUI path | 0054, 0056, 0061, 0067, 0068, 0079, 0086 | keep |
| 0047 | 0047 | `sweet-persistent-console-log` | DIAGNOSTIC | Persistent console log ring in sweet_ufs.c; polled by restart and QR (sweet_ufs.c:54) | 0069, 0088 | keep |
| 0048 | 0048 | `sweet-devnull-tee` | DIAGNOSTIC | sweetnull: /dev/null tee for named processes (bsd/dev/mem.c:161) | none | keep |
| 0049 | 0049 | `sweet-iokit-trace` | DIAGNOSTIC | IOKit user-client trace (IOUserClient.cpp:2618, boot-arg sweetiolog) | 0054 | keep |
| 0050 | 0050 | `sweet-sandbox-policy` | KEEP-CORE | Permissive Sandbox policy (sweet_amfi.c) | 0087 | keep |
| 0051 | 0051 | `sweet-crash-heap-strings` | DIAGNOSTIC | Crash heap strings | 0053 | keep |
| 0052 | 0052 | `sweet-hfs-rename-swap` | KEEP-CORE | HFS rename-swap (kpi_vfs.c) | none | keep |
| 0053 | 0053 | `sweet-crash-scan-springboard` | DIAGNOSTIC | Crash scan SpringBoard (rewrites 4 lines of 0051, does not replace it) | none | keep |
| 0054 | 0054 | `sweet-process-sampler` | DIAGNOSTIC | Process sampler (bsd/dev/sweet_sample.c:67, boot-arg sweetsample) | 0055 | keep |
| 0055 | 0055 | `sweet-aks-ext-state-sampler-depth` | DIAGNOSTIC | AKS ext-state, sampler depth | none | keep |
| 0056 | 0056 | `sweet-fb-throttled-present` | KEEP-CORE | Throttled panel copy; GUI performance | 0061/0079 (same file) | keep |
| 0057 | 0057 | `sweet-no-ext-resolver` | KEEP-CORE | Config: no external identity resolver | none | keep |
| 0058 | 0058 | `sweet-hid-event-services` | KEEP-CORE | HID event services (IOHIDFamily-1915 user client, SweetHID.cpp); iOS needs them on every board | 0061, 0071; sweet_hid_touch()/sweet_hid_key() are the entry points input drivers call | keep |
| 0059 | 0059 | `sweet-iphone-identity` | KEEP-CORE | iPhone X identity (kern_mib.c, SweetPlatformExpert.cpp); mkadt.py:255 compatible[0]=D22AP | 0085 | keep |
| 0060 | - | `sweet-keys-touch-drivers` | SWEET-ONLY | Qualcomm SPMI arbiter keys (sweet_keys.c) and GENI I2C touch (sweet_touch.c). Only gate is boot-arg sweetkeys/sweettouch (sweet_keys.c:126-136, sweet_touch.c:639-650); bluejay images pass =0 (tools/build-xnu-image.sh:19). Nothing else calls them (grep sweet_keys_init|sweet_touch_init: sweet_ufs.c only) | ARCHIVED with 0063 and the touch part of 0062. Context of 0072, 0083, 0085, 0088 (conf/files, sweet_ufs.c) regenerated | ARCHIVE |
| 0061 | 0060 | `sweet-iphone-x-display-size` | KEEP-CORE | Presents the display as 1125x2436 for UIKit (SweetFramebuffer.cpp); mkadt.py:192-195 canvas size is board independent | none | keep |
| 0062 | - | `sweet-touch-power-battery` | SWEET-ONLY | Mixed. RPMh L11C vote (sweet_rpmh.c) and sweet_touch.c edits are Qualcomm-only, called only from touch (sweet_touch.c:578). The PM6150 battery (SweetBattery.cpp) is used by bluejay (static source; SPMI read only with sweetbattery=1, SweetBattery.cpp:3) and needs sweet_spmi.c | SPLIT: touch+rpmh archived, spmi+battery kept as new 0061. 0063 and 0072 depend on the touch/rpmh files | ARCHIVE (split) |
| 0063 | - | `sweet-goodix-geni-packing` | SWEET-ONLY | GENI FIFO packing and Goodix GT9896 protocol, edits only sweet_touch.c (patch hunks 0063:3-310) | ARCHIVED with 0060; nothing else touches sweet_touch.c | ARCHIVE |
| 0064 | 0062 | `sweet-priv-work-interval` | KEEP-CORE | Grants PRIV_WORK_INTERVAL (sweet_amfi.c); UIKit needs it on any board | 0087 | keep |
| 0065 | 0063 | `sweet-smp-psci` | KEEP-CORE | SMP via PSCI CPU_ON (SweetCPU.cpp:2,30); PSCI is SoC independent, bluejay uses cpus=N (osfmk/arm64/machine_routines.c:957-962) | 0078 | keep |
| 0066 | 0064 | `sweet-crash-fault-region` | DIAGNOSTIC | Crash fault region | none | keep |
| 0067 | 0065 | `sweet-console-mute` | KEEP-CORE | Console mute once the GUI presents (serial_console.c:272, sweetfb_noconsole) | none | keep |
| 0068 | 0066 | `sweet-surface-lookup-cached` | KEEP-CORE | IOSurface lookup semantics (SweetSurface.cpp) | none | keep |
| 0069 | 0067 | `sweet-ufs-faster` | KEEP-BLUEJAY | Batched UFS log flushes; edits sweet_ufs.c lines of 0028 and 0047 | 0082 | keep |
| 0070 | 0068 | `sweet-pid-hibernate` | KEEP-CORE | pid_hibernate syscall for SpringBoard (syscalls.master:654, vm_unix.c) | none | keep |
| 0071 | 0069 | `sweet-touch-edge` | KEEP-CORE | Edge flag in SweetHIDTouch (SweetHID.cpp), a UIKit gesture requirement; HID layer is shared. Not Qualcomm | none | keep |
| 0072 | 0070 | `sweet-usb-probe` | KEEP-BLUEJAY | USB probe: Qualcomm GCC/QUSB2 path plus the gs101 branch (sweet_usb.c:107-108 calls sweet_usb_probe_gs101). Default sweetusb=1 is read-only (sweet_usb.c:102-103). Base file of the gs101 gadget | 0073-0077, 0081, 0084 all edit sweet_usb.c. CANNOT be archived | keep |
| 0073 | 0071 | `sweet-usb-device-ep0` | KEEP-BLUEJAY | DWC3 ep0 gadget; offsets identical on gs101 (sweet_usb.c:81 comment); shim/usbgadget.c ports it | 0075, 0084 (rewrites 11 of its lines) | keep |
| 0074 | 0072 | `sweet-usb-vbus-override` | SWEET-ONLY | Qualcomm QSCRATCH VBUS override (QSCRATCH_PA 0x0a6f8800). Dead on gs101 (0084 patch:271-273 skips it with ud.gs101). KEPT: 0084 edits these exact lines, so removing 0074 means regenerating 0084 by hand | 0084 (hunk at patch:271-278 uses the line 0074 adds) | keep |
| 0075 | 0073 | `sweet-usb-ecm` | KEEP-BLUEJAY | CDC-ECM function and ue0 ifnet; shared by gs101 (rewrites 13 lines of 0073) | 0076, 0077, 0081, 0084 | keep |
| 0076 | 0074 | `sweet-usb-rx-mbuf` | KEEP-BLUEJAY | RX into one cluster mbuf; generic | 0084 | keep |
| 0077 | 0075 | `sweet-usb-live-log` | KEEP-BLUEJAY | Live log over USB (sweet_log_copy) | 0084 | keep |
| 0078 | 0076 | `sweet-smp-8-cores` | KEEP-CORE | All 8 cores (SWEET.h:26, SweetCPU.cpp); bluejay ADT emits one cpu node per soc.json cpu_mpidrs entry (tools/mkadt.py cpu_nodes loop) | none | keep |
| 0079 | 0077 | `sweet-display-power` | KEEP-CORE | Display off/on via IOMFB selector 12 (SweetFramebuffer.cpp); no panel registers | none | keep |
| 0080 | 0078 | `sweet-net-trace` | DIAGNOSTIC | SWEET NET: route/address trace (rtsock.c, in.c:1221) | none | keep |
| 0081 | 0079 | `sweet-usb-link-state` | KEEP-BLUEJAY | USB link-state events (sweet_usb.c) | 0084 | keep |
| 0082 | 0080 | `sweet-ufs-gs101` | KEEP-BLUEJAY | gs101 UFS: su.gs101 from compatible google,gs101-ufs (sweet_ufs.c:617+, quirks :155-270) | none | keep |
| 0083 | 0081 | `sweet-qr-log` | KEEP-BLUEJAY | QR log channel (sweet_qr.c, sweetqr boot-arg sweet_qr.c:154); generated by tools/make-qr-patch.py; gs101 SHA3-free pragma | 0088 | keep |
| 0084 | 0082 | `sweet-usb-gs101` | KEEP-BLUEJAY | gs101 DWC3 gadget; selected by ADT /sweet-usb compatible google,gs101-dwusb3 (sweet_usb.c:889-912) | none | keep |
| 0085 | 0083 | `sweet-restart` | KEEP-BLUEJAY | Software restart through the gs101 PMU SMC; ADT node /sweet-reset only on bluejay (sweet_restart.c:7,93) | 0088 | keep |
| 0086 | 0084 | `sweet-iokit-quiet-accelerator` | KEEP-CORE | Quiet accelerator IOKit lookups (IOStartIOKit.cpp, IOService.cpp) | none | keep |
| 0087 | 0085 | `sweet-sandbox-container-path-appex` | KEEP-CORE | Sandbox container path for appex (sweet_amfi.c) | none | keep |
| 0088 | 0086 | `sweet-restart-poll` | KEEP-BLUEJAY | Restart deadline polled from log and QR threads (sweet_ufs.c:54, sweet_qr.c:14). Replaces the first attempt in 0085 ("rewritten + 0088", HANDOFF) but 0085 still creates the file | none | keep |

## Candidate analysis (requirement 2)

Candidates examined: every Qualcomm-flavoured patch (0045, 0060-0063, 0065, 0071, 0072-0081, 0078, 0084).

| Candidate | Verdict | Why |
|---|---|---|
| 0060 keys+touch | **ARCHIVE** | Only caller is `sweet_ufs_init()`; HID entry points `sweet_hid_key/touch` stay in 0058. Dependents: 0062 (touch hunks), 0063, and context in 0072, 0083, 0085, 0088 (regenerated, see below). |
| 0063 goodix | **ARCHIVE** | Edits only `sweet_touch.c`, which dies with 0060. No other dependent. |
| 0062 touch-power-battery | **SPLIT** | `sweet_rpmh.c` is called only by `sweet_touch.c:578`; touch hunks go. `SweetBattery.cpp` + `sweet_spmi.c` + its personality in `KernelConfigTables.cpp` are used by the bluejay GUI (battery source), `SweetBattery` calls `sweet_spmi_init/read`, so they stay as new 0061. The SPMI code is itself Qualcomm; removing it would mean editing `SweetBattery.cpp`, not just dropping a patch, so it is KEPT (unsure -> keep). |
| 0074 vbus override | KEEP | Qualcomm-only code, but 0084 modifies the lines it adds (`0084:271-278`: `vm_offset_t qs = ud.gs101 ? 0 : ml_io_map(QSCRATCH_PA, 0x100);` and the `if (qs != 0)` block). Archiving it means rewriting 0084 by hand. |
| 0072-0077, 0081 usb | KEEP | All edit `sweet_usb.c`, the file 0084 extends; DWC3 register offsets are the same on gs101 (`sweet_usb.c:81`). |
| 0065, 0078 smp | KEEP | PSCI CPU_ON is architectural; bluejay uses `cpus=N` (`machine_routines.c:957-962`). |
| 0045 iowatchdog pmu | KEEP | watchdogd needs an `IOPMUPrimary` service on any board (`SweetWatchdog.cpp:13-15`); no Qualcomm access. |
| 0061 display size, 0071 touch edge | KEEP | Present-size and HID semantics UIKit needs; independent of the touch controller (`sweet_hid_touch()` is the generic entry). |
| 0021/0022/0024 ramdisk root | KEEP (DIAGNOSTIC) | Used by `tools/qemu-sweet.py` (`rd=md0`); not Qualcomm hardware. |

Dependent hunks in the 4 patches that were regenerated (context only; the lines they add are unchanged), new numbers:

- 0070 (old 0072): `bsd/conf/files` context (`sweet_keys.c`, `sweet_touch.c`, `sweet_rpmh.c`), `extern` context in `sweet_ufs.c`, and the
  `sweet_usb_probe()` call site (now after `sweet_sample_start();`).
- 0081 (old 0083), 0083 (old 0085): `bsd/conf/files` context lines.
- 0086 (old 0088): `extern` context in `sweet_ufs.c`.

Result of the archive: the final tree is the old tree minus `bsd/dev/sweet_keys.c`, `sweet_rpmh.c`, `sweet_touch.c`, the two
`extern` lines and three call lines in `sweet_ufs.c`, and three `bsd/conf/files` lines (`diff -rq` of both fully patched trees, see
verification).

## Not done / unsure (kept)

- 0074 and the Qualcomm branches inside 0072-0081 (GCC clock / GDSC / QUSB2 probe, QSCRATCH): dead on gs101 but entangled with 0084.
  Removing them needs a rewritten `sweet_usb.c` (one fresh patch), not a patch deletion. Left for a separate, hardware-tested change.
- `sweet_spmi.c` (Qualcomm SPMI) only for `sweetbattery=1`: see above.
- `tools/build-xnu-image.sh` still passes `sweetkeys=0 sweettouch=0`; unknown boot-args are ignored, left as is.
- The `SWEET` naming (`SweetCPU`, `sweet_ufs.c`, ...) stays; it is the machine-config name for both boards.
- Research notes (`docs/research/0*-1*.md`, `docs/reviews`, `docs/tasks`, `UPSTREAM_README.md`, `docs/HANDOFF.md`, `NOTES.md`, `docs/CASE_STUDY.md`)
  keep the old patch numbers; use the map in `xnu/patches-archive/README.md`.

## Verification (requirement 4)

Pristine source: `apple-oss-distributions/xnu` tag `xnu-8796.141.3` (what the workflow's dxb pin resolves for macOS 13.5,
`release.json` macos-135), fetched into an empty dir. The three stagers were run first with the real network fetches (a stub
`xcrun` supplied the empty `TargetConditionals.h`), then the `.patch` files with `git apply --check` + `git apply` in order.
Run on the old stack: 88 of 88 ok. Run on the new stack: 86 of 86 ok. The dxb `sed` tweaks and dxb's own patches (not ours) were
not applied. Exact commands and outputs are in the commit message of the archive commit and the task report.

# 10 - Reset the Pixel 6a into the bootloader from software (gs101)

Goal: a test OS (shim or XNU) returns the phone to fastboot (`18d1:4ee0`) by itself, with no button press and no flash write
(`misc`/BCB is not used). Tags: **[source]** = read from source (file:line or ABL function address), **[observed]** = seen on
the phone (file + date), **[guess]**, **TODO(verify)**.

## Answer in short

- ABL decides "fastboot" from the **PMU register `SYSIP_DAT0` at physical `0x17460810`** (PMU base `0x17460000` + `0x0810`).
  Value `0xfc` = "Bootloader", `0xfa` = fastbootd, `0xff` = Recovery, `0xf9` = Rescue. ABL consumes it (writes 0) once per boot. [source]
- The register can only be **written from EL3**; non-secure code writes it with `SMC 0x82000504`: `x1 = PA, x2 = 1 (WRITE), x3 = value`.
  ABL itself does exactly this. [source]
- Then reset the SoC the way ABL and Linux do: SMC-write `PMU+0x3a00` (`SYSTEM_CONFIGURATION`) bit 1 (`SWRESET_SYSTEM`). [source]
- **Correction to the task brief:** PSCI `SYSTEM_RESET` is *not* known to work. `PSCI_FEATURES(SYSTEM_RESET)` returned `0xFFFFFFFF`
  (NOT_SUPPORTED) and a build that only called it stayed hung. [observed] Only the 3-step chain (RESET, RESET2, WDT) reset the phone, and
  *which step* worked was not recorded. [observed, NOTES.md:109]
- Nothing in this document has been run on the phone. The SMC write and the reset are **unverified on hardware**; section 3 has the experiment.

## 1. How ABL decides to enter fastboot (decompile)

**Binary.** `~/pixelios-artifacts/abl-code/abl_code.bin` = `abl.img` of factory `bluejay-cp3a.260905.009` (string `bluejay-17.0-15681480`),
16 KiB header stripped, flat AArch64 at base 0. All addresses below are file offsets in that image (= load-relative).
**The phone runs bootloader 16.3 (`bluejay-16.3-13642543`, HANDOFF.md:22): that binary was not available and was not analysed.**
The mechanism is the same one the Linux/Android kernel uses (section 2), so a different layout is unlikely, but TODO(verify) on 16.3.

**Method (no Ghidra needed, host only).** Python + capstone: scan every `adrp`+`add` pair to build string -> instruction xrefs, find the
caller of each function by scanning `bl` targets, disassemble ranges with capstone. Ghidra/REA was not started (memory).

### 1.1 The mode register

| Item | Value | Evidence |
|---|---|---|
| Register | PA `0x17460810` = PMU `SYSIP_DAT0`. ABL addresses it sign-extended (`0xffffffff17460810`: `mov x0,#-0xf7f0; movk x0,#0x1746,lsl#16`) | [source] ABL `0x76e8..0x7740`, `0x777c..0x77cc` |
| Read | plain `ldr w0,[x0]` (`0xbce4`), no SMC | [source] ABL `0xbce4` |
| Write | `0xbcec`: strips the high 32 bits (`and x9,x0,#0xffffffff`), then `smc #0` with `w0=0x82000504, x1=PA, x2=1, x3=value`; error text "failed to write to %#lx %d" | [source] ABL `0xbcec..0xbd44` |
| Format | whole 32-bit word is the mode; the value is a small magic (no bit fields): `0x00` normal, `0x50`, `0x51`, `0x60..0x67`, `0x6f`, `0xf9..0xff` | [source] jump tables at file `0x1088f6` (modes `<= 0x6f`) and `0x108966` (modes `0xf9..0xff`) |

Names printed by the "Reboot mode: 0x%x - %s" line (`0x777c`, called from the `Reboot Info:` dump at `0x3fd4`):

| Value | ABL name | Linux DT name (section 2) |
|---|---|---|
| `0x00` | Normal Boot | - |
| `0x50` | dm-verity device corrupted | `dm-verity-device-corrupted` |
| `0x51` | shutdown,thermal | `shutdown-thermal` |
| `0x60..0x67`, `0x6f` | regulator / hw-protection shutdown reasons | - |
| `0xf9` | Rescue | `rescue` |
| `0xfa` | fastbootd | `fastboot` |
| `0xfb`, anything else | Unknown | - |
| **`0xfc`** | **Bootloader** | **`bootloader`** |
| `0xfd` | Factory | - |
| `0xfe` | ROM Recovery | - |
| `0xff` | Recovery | `recovery` |

[source] ABL `0x777c..0x78f8`; the Linux column matches value for value (a strong cross-check).

### 1.2 Who reads it and what happens

1. **`0x97e0` = "enter fastboot?" decision** (called from the init sequence at `0x18ac`; if it returns 1, `0x18b4..0x18f4` is skipped).
   `0x9834: bl 0x76e8; cmp w0,#0xfc; b.ne` then logs reason string "reboot bootloader" (`0x9ea90`, a reason-string setter) and returns 1.
   Earlier exits: a "BL1 requested" state word (`0x020389c4`, value 2 or 3), later: "usb boot" (`0xb7b8() == 3`). [source]
2. **BCB / mode dispatcher at `0x11690..0x11830`.** First parses the misc BCB command ("bootloader message:%s", xref `0x1168c`;
   `bootonce-bootloader`, `boot-recovery`, `boot-fastboot`, `boot-rescue`), then `0x9894` maps the mode register to a class
   (`0x108ad8` = `[0xf9->4, 0xfa->2, 0xfb->0, 0xfc->1, 0xfd->0, 0xfe->0, 0xff->3]`; 1 = "reboot bootloader" -> fastboot, 2 = `boot-fastboot`
   = fastbootd, 3 = recovery, 4 = `boot-rescue`). **After reading, ABL clears it: `0x11810: mov w0,wzr; bl 0x9870` -> mode 0.** [source]
   The BCB path is read-only for us and is not used; if `misc` holds a stale command ABL handles it first - TODO(verify) ordering.
3. **Setter `0x9870(idx)`** (used by `fastboot reboot ...` and the halt hook `0xba80`): `idx 1..6 -> [0xfc,0xfa,0xff,0xf9,0xfe,0xf1]`
   (table `0x108ac0`), anything else -> 0, then `0x7744` (sets bit31 + stores a copy in the "BMS" tag, see 1.3, then the SMC write). [source]

### 1.3 Cold-reset save/restore (why the register alone is not always enough)

`0x76e8` (get mode): if `0x4b00()` is true, read tag `BMS_TAG_RSBM` (`0x45c0`, error string "failed to read BMS_TAG_RSBM(%d)");
if its bit31 is set and `(RST_STAT & 0x81) == 0` (not a warm/watchdog reset) ABL **restores the mode into `SYSIP_DAT0` via SMC** and clears the tag.
The setter `0x7744` stores `mode | 0x80000000` into that tag when `0x4b00()` is true. [source] ABL `0x76e8..0x7778`.
Reading: a PMIC cold reset (power-hold released) loses the alive-domain register, so the kernel/ABL park the mode in battery-gauge storage.
Linaro's DT commit says the same: "for complete support we'll also have to write the boot mode to an NVMEM storage location" (section 2). [source]
Consequence for us: a **warm reset keeps `SYSIP_DAT0`** and needs no BMS write; a **cold reset (PS_HOLD) must not be used**. [guess for "warm keeps it":
consistent with ABL using SWRESET_SYSTEM for its own bootloader reboot, 1.4; not read on hardware.] `0x4b00()` meaning: [guess] "device needs the cold-reset backup".

### 1.4 How ABL resets itself (the reset that preserves the mode)

`halt(action, reason)` at `0xbdc0`, action 1 = reboot:
- reason `== 7`, or `0x4b00() == 0`: **PMU `0x17463a00` RMW mask `0x2` value `0x2`** (`0xbfec..0xbffc`, via `0xbd54` = SMC `0x82000504`, `x2=2` RMW) = `SYSTEM_CONFIGURATION.SWRESET_SYSTEM`.
- otherwise: **PMU `0x17463e9c` (`PAD_CTRL_PWR_HOLD`) RMW mask `0x100` value `0`** (`0xbe28..0xbe40`) = power-hold release (cold reset / power off).
[source] ABL `0xbdc0..0xc044`. Same two registers as the Linux driver (section 2).

### 1.5 RST_STAT and other registers seen

- `0xc0fc` (get RST_STAT): reads PMU `0x17463b44` (`GS101_RST_STAT`), or a copy at ABL data `0x02038a2c` when PMU `0x17460834` (`GSA_INFORM(1)`) bit31 is set.
  Printed as "RST_STAT: 0x%x - %s" (`0x3f20`). "Watchdog or Warm Reset Detected." (`0x619c`) is printed when `RST_STAT & 0x11` and PMU `0x17460830` bit0.
  [source] ABL `0xc0fc..0xc13c`, `0x6160..0x619c`. Bit meanings of RST_STAT are **not decoded** (TODO(verify)).
- "Reboot reason" (separate from "Reboot mode") is a DRAM word at PA `0xd8100300` (`0xffff0000d8100300`) with values like `0xcafe` = "User Reboot(S/W Reboot)",
  `0xfafa` = Safe Kernel PANIC (`0x4fe8`, `0x5044`). It is post-mortem bookkeeping, not an input to the fastboot decision. [source] ABL `0x4fe8..0x5300`.

## 2. Cross-check with Linux / Android gs101 sources

Upstream Linux (torvalds/linux at `a804c5742bff224d4cb4d25dd383554d598be32b`):

- `arch/arm64/boot/dts/exynos/google/gs101.dtsi:1820-1832`: `reboot-mode { compatible = "syscon-reboot-mode"; offset = <0x0810>; /* EXYNOS_PMU_SYSIP_DAT0 */
  mode-bootloader = <0xfc>; mode-charge = <0x0a>; mode-dm-verity-device-corrupted = <0x50>; mode-fastboot = <0xfa>; mode-reboot-ab-update = <0x52>;
  mode-recovery = <0xff>; mode-rescue = <0xf9>; mode-shutdown-thermal = <0x51>; mode-shutdown-thermal-battery = <0x51>; }` inside the PMU node `system-controller@17460000` (`gs101.dtsi:1804-1833`). [source]
- Commit `ae32b65c9359` "add reboot-mode support (SYSIP_DAT0)": "the boot loader does use SYSIP_DAT0 (PMU + 0x0810) to determine some of the actions it should take ... we'll also have to write the boot mode to an NVMEM storage location". [source]
- `include/linux/soc/samsung/exynos-regs-pmu.h:691-697`: `GS101_INFORM0 0x0800 .. INFORM3 0x080c`, `GS101_SYSIP_DAT(n) (0x0810 + ((n)&3)*4)`; `:866` `GS101_SYSTEM_CONFIGURATION 0x3a00`; `:920` `GS101_RST_STAT 0x3b44`; `:1000` `GS101_PAD_CTRL_PWR_HOLD 0x3e9c`. [source]
- `drivers/soc/samsung/gs101-pmu.c`:
  - `:19-22` `TENSOR_SMC_PMU_SEC_REG 0x82000504`, `TENSOR_PMUREG_READ 0 / WRITE 1 / RMW 2`.
  - `:333-336` "Tensor SoCs are configured so that PMU_ALIVE registers can only be written from EL3, but are still read accessible."
  - `:343-355` write = `arm_smccc_smc(0x82000504, pmu_base + reg, 1, val, ...)`; `:359-372` RMW = `(..., reg, 2, mask, val)`; returns `-EINVAL` if EL3 refuses; `:380-383` reads are plain MMIO.
  - `:29` write table includes `INFORM0..SYSIP_DAT(0)` ("skip SYSIP_DAT1 SYSIP_DAT2", `:30`); `:149` includes `SYSTEM_CONFIGURATION..USER_DEFINED_OUT`; `:177` (in the pad-control range) covers `0x3e9c`; `:160` covers `RST_STAT`. EL3 enforces its own list, this is only Linux's copy. [source]
  - `:420-435` `SYSIP_DAT(0)` and `SYSTEM_CONFIGURATION` are *non-atomic* (use WRITE/RMW, not set/clear-bit SMCs).
- `drivers/power/reset/syscon-reboot.c:125-145` (commit `8c7cf0fc8cf5` "add gs101-specific reset"): warm/soft = `0x3a00` mask `0x2` value `0x2`; catch-all (cold/hard) = `0x3e9c` mask `0x100` value `0`. Same registers as ABL's halt path (1.4). [source]
- `drivers/power/reset/syscon-reboot-mode.c:20-35` writes the magic with `regmap_update_bits(offset, mask=0xffffffff, magic)` -> on gs101 that is the RMW SMC (cmd 2); ABL uses WRITE (cmd 1). Both reach the same register. [source]
- Commit `2d0c7ae784b4`: "Reboot of gs101 SoC can be handled by setting bit SWRESET_SYSTEM[1] of SYSTEM_CONFIGURATION (PMU + 0x3a00)"; commit `6572a93ab35e`: downstream writes the whole `0x2` and bit 1 is the only R/W bit. [source]

Android downstream (kernel/devices/google/gs101 `android-gs-bluejay-6.1-android16`, commit `a9b5edb4a1af922091683feff8f7804ec6986aea`), `dts/gs101.dtsi:446-456`:
`pixel-reboot { compatible = "google,pixel-reboot"; pshold-control-offset = <0x3E9C>; pshold-control-trigger = <0x100>; swreset-system-offset = <0x3A00>;
swreset-system-trigger = <0x2>; reboot-cmd-offset = <0x0810>; dump-gpr-offset = <0x0808>; force-warm-reboot-on-thermal-shutdown; }` - the same three offsets
(`reboot-cmd-offset 0x0810` is the mode register). [source] The driver source for `google,pixel-reboot` and its string->magic table was **not found** in
`kernel/gs` or `kernel/google-modules/soc/gs` (checked `drivers/soc/google`, `drivers/soc/samsung`, `drivers/power/reset`, `debug-reboot.c` is unrelated).
The magic values therefore come from upstream DT plus the ABL decode above, which agree. TODO(verify) the downstream table if a divergence ever shows.

`pmu-intr-gen`, CPU_INFORM (`0x0860`) etc. are the hotplug mechanism and unrelated to reboot.

## 3. Sequence for the shim (EL1, MMU off) and the experiment

Facts about this phone:
- Shim entry is EL2, drops to EL1; PSCI 1.1 over `smc` (`PSCI_VERSION 0x10001`). [observed] `device/google/bluejay/hardware/ufs-gpt-read-2026-10-21.txt:3,44`
- `PSCI_FEATURES(SYSTEM_RESET 0x84000009) = 0xFFFFFFFF` (not supported). [observed] same file `:45`.
  (`NOTES.md:663` "SYSTEM_RESET2 unsupported" is the **sweet** phone; bluejay's `SYSTEM_RESET2` support was never queried. TODO(verify): print `PSCI_FEATURES(0xc4000012)` and `(0x84000012)`.)
- A shim that called only `SYSTEM_RESET` did **not** reset (user held Power+VolDown); with `bluejay_reset_chain()` (RESET, RESET2, WDT) it did, step unrecorded. [observed] `NOTES.md:109`, `shim/shim.c:195-206`.
  The chain's `RESET2` argument `0x80000000` is a *vendor* reset type under PSCI 1.1, not the architectural warm reset the comment claims. [guess, from memory of DEN0022]
- Plain MMIO **reads** of PMU registers work from the shim (`UFSDIAG PMU 3ec8 pa=17463EC8 v=00000001`). [observed] `ufs-gpt-read-2026-10-21.txt:294`.
- The PMU `SMC 0x82000504` has **never been executed successfully on this phone** (`prepare_bluejay_cpu` only runs after a CPU_ON error, `shim/shim.c:561`; no log line). Whether EL3 accepts it from the
  shim's EL1 is unknown. It accepts it from ABL (`0xbcec`), whose exception level is unknown; the SMC conduit itself works from the shim (`CPU_ON` success, observed). [guess that EL3 does not filter by caller EL; TODO(verify)]

Proposed sequence (new function next to `bluejay_reset_chain`, `shim/shim.c:195`; `smc()` at `shim/shim.c:102` already takes `fn,x1,x2,x3`, which is all WRITE needs):

```
 1. before = try_read32(0x17460810)                      /* PMU SYSIP_DAT0, plain read */
 2. r1     = smc(0x82000504, 0x17460810, 1 /*WRITE*/, 0xfc)  /* 0 = accepted, nonzero = EL3 refused */
 3. after  = try_read32(0x17460810)                      /* must read 0xfc */
 4. print before/r1/after (screen + usb log); if after != 0xfc: stop, do not reset
 5. r2     = smc(0x82000504, 0x17463a00, 1 /*WRITE*/, 0x2)   /* SYSTEM_CONFIGURATION.SWRESET_SYSTEM; ABL uses RMW(mask 2,val 2), Linux RMW; 0x2 is the only R/W bit */
 6. fall through to the existing chain (PSCI RESET, RESET2, WDT) only if still running after ~2 s
```

Do not use the `0x3e9c` PS_HOLD path (cold reset; the register is lost, see 1.3). `SYSIP_DAT1/2` are not writable and not needed.
If step 2 returns nonzero, the next variants are: RMW (`x2=2`, `x3=0xffffffff`, `x4=0xfc`, needs a 5-argument `smc` helper, what Linux `syscon-reboot-mode` sends) and `0xfa`/`0xff` as sanity checks that any value is refused.
No `misc` write, no UFS write, no flash write: the register is volatile alive-domain state, ABL clears it on the next boot.

**Experiment (one safe test; the driver runs it).** Phone in the bootloader, `fastboot getvar product` = `bluejay`. Build a shim with the function above called at the end of the probe
(replacing the final `bluejay_reset_chain()`); boot with `tools/fboot.sh --expect shim shim/out/<img>`. Expected evidence, in order:

| Observation | Meaning |
|---|---|
| Log shows `SYSIP before=0x0..`, `SMC ret=0`, `after=0xFC` | EL3 accepted the write from non-secure EL1: **claim 3 is proven for the write**. |
| Phone resets and shows the bootloader (fastboot screen), `fastboot devices` lists it (`18d1:4ee0`) without a button press | The whole mechanism works; `SWRESET_SYSTEM` via SMC is a valid reset. |
| `ret != 0` | EL3 filters the write for this caller/address; try the RMW variant, then record the value (then ask whether the TZ blocks it). |
| `after != 0xfc` with `ret == 0` | EL3 swallowed the write or the register is shadowed; no reset is attempted; report both numbers. |
| `after == 0xfc`, but Android boots | The reset flavour or ABL 16.3 does not honour it. Compare against `RST_STAT` (`PMU+0x3b44`) and `PMU+0x830/0x834` printed before the reset (add them to the same line), and try the chain's WDT step instead of step 5. |
| `SWRESET` does nothing (still running after 2 s) | The chain's later steps run; note which step resets - that also resolves the open question in `NOTES.md:109`. |

Piggy-back (read-only, zero risk) on the same run: print `PMU+0x808`, `+0x3b44`, `+0x830`, `+0x834`, and `PSCI_FEATURES` for `0xc4000012`, `0x84000012`, `0x84000008` (SYSTEM_OFF).
Failure leaves the register at `0xfc` until ABL reads it, so the worst case is one extra boot into the bootloader.

## 4. XNU side

Current state [source, `~/pixelios-artifacts/bt-0082/tree/xnu` = xnu-8796 + patches 0001-0082; `grep PE_halt_restart|PEHaltRestart|haltRestart|kPERestartCPU xnu/patches/*` finds nothing]:
- `iokit/Kernel/IOPlatformExpert.cpp:336` `int (*PE_halt_restart)(unsigned int type) = NULL;` and `:363-367` `haltRestart()` returns `-1` when it is NULL.
- `osfmk/arm64/machine_routines.c:746-760` `halt_all_cpus()` calls `PEHaltRestart()` then `while (1);`; `osfmk/kern/debug.c:1171-1178` (`kdp_machine_reboot_type`, panic restart) the same; `osfmk/arm64/cpu.c:1095-1104` (hibernation) calls the pointer directly.
- `SweetPlatformExpert` (patch 0016, `iokit/Kernel/arm/SweetPlatformExpert.cpp`) does not override `haltRestart`.
- So on sweet/bluejay today **reboot, halt and panic-restart all spin forever**. A hang after a panic is the normal end state, which is why every test needs a button press.
- `PEHaltRestartInternal` (`IOPlatformExpert.cpp:985+`) first sends IOPM shutdown notifications and arms a 30 s timer that panics ("Halt/Restart Timed Out"); do not route the test timer through `reboot()`.

Design (new patch `0085-sweet-restart.patch`, not written here; numbering follows the contiguous rule in `xnu/AGENTS.md`):

1. **Reset primitive** `sweet_machine_reset(int to_bootloader)` in a new `bsd/dev/sweet_restart.c` (pattern of `sweet_qr.c`, patch 0083), no locks, no allocation, usable from panic context:
   `sweet_smc(0x82000504, pa, 1, val)` as an inline-asm `smc #0` (copy `sweet_psci_cpu_on`, patch 0065, lines 28-41, but with `x1..x3` as in section 3);
   `if (to_bootloader) write SYSIP_DAT0 = 0xfc`, then `write SYSTEM_CONFIGURATION = 2`, then `for(;;) wfe`. Constants come from the ADT (`soc.json` ->
   `tools/mkadt.py` -> a `/sweet-reset` node with `pmu-base`, `mode-offset 0x810`, `swreset-offset 0x3a00`, `mode-bootloader 0xfc`), gated on `compatible` `google,gs101` per `xnu/AGENTS.md`; sweet (Qualcomm) keeps `-1`. No MMIO mapping is needed: SMC takes the physical address.
2. **`PE_halt_restart` hook**: in `SweetPlatformExpert::start()` set `PE_halt_restart = sweet_halt_restart` (smaller than a `haltRestart` override and it also covers the hibernation call at `cpu.c:1099`).
   `kPERestartCPU`, `kPEPanicRestartCPU`, `kPEPanicRestartCPUNoCallouts` -> `sweet_machine_reset(armed_bootloader)`; `kPEHaltCPU` -> unchanged (PS_HOLD power-off exists in ABL `0xbec4` / Linux `syscon-poweroff` but is untested here, TODO(verify) before use).
   The base class already handles `kPEPanicSync` and `kPEHangCPU`.
3. **Boot-arg `sweetrestart=<seconds>[,bootloader]`**, absent = off (same convention as `sweetqr`). Parse with `PE_parse_boot_argn("sweetrestart", buf, sizeof buf)` into a `char[32]` and split at `,` by hand
   (a numeric type would stop at the comma). A kernel thread (`kernel_thread_start`, as `sweet_qr_start`, patch 0083:293-313) sleeps `seconds`, prints `SWEET RESTART: resetting now (bootloader=%d)` via `printf`, optionally `sync_internal()` once if the root is mounted read-write (guess: bounded risk; a stuck UFS here is covered by the watchdog below), then calls `sweet_machine_reset()`.
   Start it from `SweetPlatformExpert::start()`, **not** from `sweet_ufs_init()` (where `sweet_qr_start` lives, `0083:26`), so it also works on `--no-ufs` images and starts at the earliest IOKit point; the deadline is "seconds since IOKit start".
4. **Tool plumbing**: `tools/build-xnu-image.sh` gets `--restart SECONDS[,bootloader]` that appends `sweetrestart=...` next to the existing `sweetqr=` handling (line 20); default off.
5. **Hang/panic-proof stage (optional, UNVERIFIED)**: the thread cannot run if XNU deadlocks with interrupts off. Arming the mode register early (even in the shim: write `0xfc` right before jumping to XNU) plus a hardware watchdog that is left armed with a timeout
   (WDT CL0 `0x10060000`, which ABL leaves armed and the shim disables, `HANDOFF.md:41`) would reset the SoC into the bootloader regardless of the kernel state. A WDT reset is a "warm/watchdog reset", so `SYSIP_DAT0` should survive and ABL should skip the BMS restore (`RST_STAT & 0x81`, 1.3).
   Needs: whether the PMU reset source for the WDT is unmasked (UNSURE per `shim/shim.c:192-194`) and the `RST_STAT` bits. [guess] Do this only after section 3 shows which chain step resets the phone.

## What was checked and what is still open

Checked: ABL strings (`bootloader message:%s`, `RST_STAT`, `Watchdog or Warm Reset Detected.`, `User Reboot(S/W Reboot)`, `reboot,*`), all xrefs and the five functions above; Linux gs101 DT/PMU/reboot sources;
Android `devices/google/gs101` DT; the XNU tree for `PE_halt_restart`. Not checked: ABL 16.3 itself; the EL3 SMC handler (not in the image set: `bl31.img`/`tzsw.img` exist in `~/pixelios-artifacts/abl-in/` but were not decompiled; that is the way to answer "does EL3 block it" statically if the experiment says it does);
downstream `google,pixel-reboot` driver source; `RST_STAT` bit meanings; whether `PSCI SYSTEM_RESET2` is supported (never queried on bluejay).

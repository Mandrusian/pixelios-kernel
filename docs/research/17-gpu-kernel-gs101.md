# 17 - GPU on the Pixel 6a (bluejay): Mali-G78 MP20, a kernel-side design, and the read-only stage-0 probe

Desk research. No `adb`/`fastboot`, nothing was run on a phone, nothing was extracted from a phone. `docs/PLAN.md` calls this "research phase 6"
(GPU, after the GUI and Wi-Fi). Scope: what the GPU is, what the kernel must do to bring it up from XNU, what we can reuse from the
`ios-sailfish` port (Pixel 1, Adreno), a staged plan with risk, and the one step that is safe to build today (`--gpu-probe`).

Tags (AGENTS.md rule 5): **[src]** read from source (file:line or URL + commit, fetched 2026-10-22), **[obs]** observed on hardware (repo log),
**[guess]**. Nothing about the GPU has been observed on the phone yet; every [obs] below is about other blocks and is cited to its repo location.

Source trees (Android Open Source Project, branch `android-gs-bluejay-6.1-android16`, same pins as doc 12 where they overlap):

| Short name | Repo | Commit |
|---|---|---|
| `gs101-dts` | https://android.googlesource.com/kernel/devices/google/gs101 | `a9b5edb4a1af922091683feff8f7804ec6986aea` |
| `soc-gs` | https://android.googlesource.com/kernel/google-modules/soc/gs | `204078758737776959ecc85c527a00fd2cabb62f` |
| `kbase` | https://android.googlesource.com/kernel/google-modules/gpu (dir `mali_kbase/`) | `696fc0246c17ba4270c11f88ab742ad4040938de` |
| `kbase-5.10` | same repo, branch `android-gs-bluejay-5.10-android14-qpr1` | `8d366ed42251a1dac0fb6a62bfc0d1741f32556a` (only listed; the 6.1 tree was read) |
| `mesa` | https://gitlab.freedesktop.org/mesa/mesa (main, fetched 2026-10-22) | no pin recorded |
| `linux` | torvalds/linux master (fetched 2026-10-22) | no pin recorded |
| `sailfish` | `~/pixelios-artifacts/refs/ios-sailfish` (local clone) | as cloned |

File URL form: `<repo>/+/refs/heads/<branch>/<path>?format=TEXT` (base64).

## Summary

- GPU: **Mali-G78 MP20**, Valhall, **job-manager (JM) interface, not CSF**. kbase builds gs101 with `MALI_USE_CSF=0` and no firmware.
  The JM register file is the classic one: `GPU_CONTROL` at +0x0, `JOB_CONTROL` (job slots) at +0x1000, MMU + address spaces at +0x2000.
  Block at **0x1C500000, size 0x5000**, three SPI interrupts (job 420, mmu 421, gpu 419).
- Upstream drivers: **Arm `mali_kbase`** is what Google ships. Mainline Linux **panfrost** does *not* list G78 (only G57); **panthor** is CSF only
  (wrong generation). Mesa's panfrost compiler/Gallium stack supports Valhall (arch 9), but its model table has no entry for the G78's
  own product id (only G57 and a "G68" row), so Mesa needs a one-line model entry plus a check (section 3.5).
- **Power is the whole question.** The power-domain sequences are plain MMIO to PMU/CMU, but every write to the PMU "alive" block goes through
  an **EL3 vendor SMC** (`SMC_CMD_PRIV_REG`), then a second SMC restores the TrustZone protection controller for the domain; GPU clock *rates* and
  rail voltages are set by the **ACPM** firmware over an IPC mailbox. None of that is documented outside the Android sources, and none of it is
  verified from XNU (EL1) on this phone.
- **Biggest blocker:** the first step that actually powers the GPU needs a vendor SMC (PMU alive write, `0x82000504`) that we have never issued and that
  EL3 may refuse from our context or may reset the phone on. Second: ACPM-owned voltage/frequency (the GPU may run at an unsafe default OPP, or
  not come up at all, if ACPM is not asked). Third: the four G3D S2MPUs may block GPU DMA into XNU's RAM.
- **Built now (read-only):** `shim --gpu-probe` prints the PMU power status of the two GPU domains and the CMU_G3D mux state, and reads
  `GPU_ID`/`SHADER_PRESENT`/... **only if** the PMU says the domains are on. Otherwise it stops after the PMU/CMU words. See section 2.
- Sailfish reuse: the **user-client / libdrm-emulation / EAGL-shim structure transfers**; the Adreno-specific kernel core (about 2.6k lines of
  `sweet_gpu.c`) does not. Mesa panfrost replaces freedreno.

---

## 1. GPU identity and register interface

### 1.1 Generation, interface, driver

- **[src] Mali-G78 = kbase product `TBEX`**: `GPU_ID_PRODUCT_TBEX` is `{ .id = GPU_ID_PRODUCT_TBEX, .name = "Mali-G78" }`
  (`kbase: mali_kbase/mali_kbase_core_linux.c:3328`); `TBEX = GPU_ID_MODEL_MAKE(9, 2)` (`kbase: common/include/uapi/gpu/arm/midgard/gpu/mali_kbase_gpu_id.h:166`).
  `GPU_ID` register bits: arch_major `[31:28]`, arch_minor `[27:24]`, arch_rev `[23:20]`, product_major `[19:16]`, version `[15:0]`
  (`mali_kbase_gpu_id.h:29-44`). So the model mask `0xF00F0000` should read `0x90020000` on this phone. [guess] until the probe runs: the arch_minor/rev
  nibbles are not constrained by that table.
- **[src] JM, not CSF.** `mali_kbase/Kbuild:82-94`: `ifneq ($(CONFIG_SOC_GS101),y) CONFIG_MALI_CSF_SUPPORT ?= y`, i.e. gs101 never takes the CSF
  branch (`MALI_USE_CSF ?= 0`). CSF starts with arch 10 (G710 = `TODX` = `MODEL_MAKE(10, 2)`, `mali_kbase_gpu_id.h:169`; the JM register-map init table
  stops at arch 9.14, `hw_access/regmap/mali_kbase_regmap_jm.c:2301-2307`). G78 is arch 9. No firmware image is loaded for JM (firmware
  loading lives under `mali_kbase/csf/`).
- **[src] Core count.** Pixel 6a "MP20" = 20 shader cores: `SHADER_PRESENT` should have 20 bits set (the probe prints `popcount`). [guess] the bit pattern is
  contiguous `0xFFFFF`; the number is from the marketing name, not read from a register.
- **[src] Which upstream drivers support it**

| Driver | G78? | Evidence |
|---|---|---|
| Arm `mali_kbase` r3x-r4x (Google's `kernel/google-modules/gpu`) | yes | `hw_access`, `jm/`, `mali_kbase_hw.c:62-65` (`TBEX` features/issues tables, `LBEX`/`TBEX` share one feature table) |
| Linux `panfrost` | **no** | `linux: drivers/gpu/drm/panfrost/panfrost_gpu.c:231-244` lists only `g57` (0x9001, 0x9003, 0x9093) among Valhall |
| Linux `panthor` | no | CSF only (G310/G510/G610/G710); wrong register interface for arch 9 |
| Mesa panfrost (userspace) | partial | `mesa: src/panfrost/model/pan_model.c:89-93`: Valhall rows `PAN_PROD_ID(9,0,1)` G57, `(9,0,3)` G57, `(9,2,4)` "Mali-G68" (family G78). No `(9,2,2)` row; `pan_get_model()` returns NULL for an unlisted id (`pan_model.c:165-181`) |

  Google's tree is the only complete reference for the *kernel* side of this exact SoC (power, clocks, S2MPU, SLC). Panfrost's JM job-submit path is
  smaller and is the model for the user-visible surface (section 3.3); register-level, kbase and panfrost agree for arch 9.

### 1.2 JM register interface (what a minimal kernel driver touches)

All offsets from the GPU MMIO base `0x1C500000`. [src] `kbase: mali_kbase/hw_access/regmap/mali_kbase_regmap_jm_enums.h` (offsets are in the comments)
and `mali_kbase_regmap_jm.c:1814-1825`, `mali_kbase_regmap_legacy_jm.h`; `GPU_COMMAND` values in `mali_kbase_regmap_jm_macros.h:247-262`.

| Block | Registers (offset) |
|---|---|
| GPU control | `GPU_ID 0x0`, `L2_FEATURES 0x4`, `TILER_FEATURES 0xC`, `MEM_FEATURES 0x10`, `MMU_FEATURES 0x14`, `AS_PRESENT 0x18`, `JS_PRESENT 0x1C`, `GPU_IRQ_RAWSTAT 0x20` / `CLEAR 0x24` / `MASK 0x28` / `STATUS 0x2C`, `GPU_COMMAND 0x30`, `GPU_STATUS 0x34`, `GPU_FAULTSTATUS 0x3C`, `GPU_FAULTADDRESS 0x40` |
| Power | `SHADER/TILER/L2_PRESENT 0x100/0x110/0x120`, `*_READY 0x140/0x150/0x160`, `*_PWRON 0x180/0x190/0x1A0`, `*_PWROFF 0x1C0/0x1D0/..`, `STACK_PRESENT 0xE00` (+`PWRON 0xE20`, `PWROFF 0xE30`, `PWRTRANS 0xE40`) |
| Commands | `GPU_COMMAND`: `NOP 0`, `SOFT_RESET 1`, `HARD_RESET 2`, `CLEAN_CACHES 7`, `CLEAN_INV_CACHES 8`; reset completion is `GPU_IRQ_RAWSTAT` bit 8 `RESET_COMPLETED` |
| Job manager | `JOB_IRQ_RAWSTAT 0x1000` / `CLEAR 0x1004` / `MASK 0x1008` / `STATUS 0x100C` / `JS_STATE 0x1010`; job slot n at `0x1800 + (n << 7)`: `HEAD 0x00`, `TAIL 0x08`, `AFFINITY 0x10`, `CONFIG 0x18`, `COMMAND 0x20`, `STATUS 0x24`, `HEAD_NEXT 0x40`, `AFFINITY_NEXT 0x50`, `CONFIG_NEXT 0x58`, `COMMAND_NEXT 0x60`, `FLUSH_ID_NEXT 0x70` |
| MMU | `MMU_IRQ_RAWSTAT 0x2000` / `CLEAR 0x2004` / `MASK 0x2008` / `STATUS 0x200C`; address space n at `0x2400 + (n << 6)`: `TRANSTAB 0x00`, `MEMATTR 0x08`, `LOCKADDR 0x10`, `COMMAND 0x18`, `FAULTSTATUS 0x1C`, `FAULTADDRESS 0x20`, `STATUS 0x28`, `TRANSCFG 0x30` |

  Job chains are descriptors in GPU-visible memory; the slot registers carry only the head pointer (`HEAD_NEXT`) plus affinity and config, then
  `COMMAND_NEXT = START`. [src] panfrost `drm_panfrost_submit.jc` is "address to GPU mapping of job descriptor" (`linux: include/uapi/drm/panfrost_drm.h`). The job
  descriptor *format* (job types such as WRITE_VALUE / COMPUTE / FRAGMENT) is defined by Mesa's genxml, not by kbase; not read here ([guess] a
  WRITE_VALUE job is the simplest first job).
- **[src] Protected mode / secure rendering is not used on gs101**: the `SMC_PROTECTION_SET ... PROT_G3D` calls are compiled under
  `#if !IS_ENABLED(CONFIG_SOC_GS101) && ...` (`kbase: mali_kbase/platform/pixel/pixel_gpu_power.c:295,353`). GPU register access in kbase is plain MMIO.

---

## 2. Device tree, power, clocks (and the stage-0 probe)

### 2.1 Facts

All [src] unless tagged.

| Item | Value | Source |
|---|---|---|
| MMIO | `0x1C500000`, size `0x5000`, compatible `arm,malit6xx` | `gs101-dts: dts/gs101-gpu.dtsi` `mali@1C500000` |
| IRQs (GIC SPI numbers; INTID = 32 + n) | JOB `IRQ_G3D_IRQJOB_G3D` = 420, MMU `IRQ_G3D_IRQMMU_G3D` = 421, GPU `IRQ_G3D_IRQGPU_G3D` = 419 (also `IRQ_G3D_IRQEVENT_G3D` 418, unused by the DT node), all level-high | `gs101-gpu.dtsi interrupts/interrupt-names = "JOB","MMU","GPU"`; numbers `soc-gs: include/dt-bindings/interrupt-controller/gs101.h:435-438` |
| Power domains | `power-domains = <&pd_g3d>, <&pd_embedded_g3d>` ("top", "cores"); `pd_embedded_g3d` is a child of `pd_g3d` | `gs101-gpu.dtsi`; `gs101-dts: dts/gs101-pm-domains.dtsi` (`pd-g3d@17461E00`, `pd-embedded_g3d@17462000`) |
| PMU status word | `gpu_pmu_status_reg_offset = 0x2004`, mask `0x1` (this is `EMBEDDED_G3D_STATUS` bit 0) | `gs101-gpu.dtsi`; read in `kbase: pixel_gpu_power.c:694` (`exynos_pmu_read`) |
| `pd_g3d` | PMU regs `0x17461E00` (CONFIGURATION) / `0x17461E04` (STATUS); `cal_id = 0xB1380007`; `need_smc = 0x1C410204`; `cmu_id = 0x1C400000` | `gs101-pm-domains.dtsi`; `soc-gs: cal-if/gs101/flexpmu_cal_local_gs101.h` `g3d_on/g3d_status` |
| `pd_embedded_g3d` | PMU regs `0x17462000` / `0x17462004`; `cal_id = 0xB1380006`; no `need_smc` | same |
| Clocks | no clk-framework clocks; rates by ACPM DVFS ids `gpu0_cmu_cal_id = ACPM_DVFS_G3DL2`, `gpu1_cmu_cal_id = ACPM_DVFS_G3D`; OPP table v1: 5 rows (slowest `302000`/`151000` kHz), v2: 12 rows; `gpu_dvfs_max_freq = 800000`, `min = 250000` kHz | `gs101-gpu.dtsi`; `soc-gs: cal-if/gs101/acpm_dvfs_gs101.h:8-9,25-26` |
| CMU | `CMU_G3D` at `0x1C400000` (PLL_G3D, PLL_G3D_L2, `MUX_CLKCMU_EMBEDDED_G3D_{COREGROUP,STACKS,TOP}_USER`, dividers); sysreg G3D `0x1C420000` | `flexpmu_cal_local_gs101.h` `embedded_g3d_on`, `g3d_save` |
| Rails | S2MPG11 `BUCK2S` = `S2S_VDD_G3D`, `BUCK8S` = `S8S_VDD_G3D_L2`, `LDO1S` = `L1S_VDD_G3D_M`; all `regulator-always-on`, `regulator-initial-mode = SEC_OPMODE_SUSPEND` | `gs101-dts: dts/gs101-pmic.dtsi:760-773,850-860,936-946` |
| DMA protection | 4 x `s2mpu_g3d0..3` at `0x20080000/0x20090000/0x200A0000/0x200B0000` (size `0x10000`, `gs101-b0.dts:58-69`; `power-domain-id = 0x1C410204`); the G3D has its own page tables (AS) in addition | `gs101-dts: dts/gs101-s2mpu.dtsi:139-160`, `dts/gs101-b0.dts` |
| Thermal | TMU `G3D@100B0000`, zone `G3D_THERMAL` cooled via the mali cooling device | `gs101-dts: dts/gs101.dtsi:1686,1922-1973` |
| SLC / memory group | `physical-memory-group-manager`, SLC partition `gpu_write_alloc` (vptid 0x28, pbha 5) | `gs101-gpu.dtsi`, `gs101.dtsi:557-560` |

### 2.2 How power is actually done (and who must help)

- **[src] The AP runs the power-domain sequences; the PMU writes are an EL3 call.** `pmucal_local_enable()` runs the domain's `on` sequence with
  `pmucal_rae_handle_seq` (`soc-gs: cal-if/pmucal_local.c:42`), then, if `need_smc` is set, calls `exynos_pd_tz_restore(need_smc)` (`pmucal_local.c:52-53`). In
  `pmucal_rae.c:149-155` a write whose physical base is in the PMU alive block (`PMU_ALIVE_BASE_ADDR = 0x17460000` for gs101, `pmucal_rae.c:9-11`) is
  `set_priv_reg(base_pa + offset, val)` = an SMC; every other register (CMU, sysreg) is a plain `writel`. `set_priv_reg` is
  `arm_smccc_smc(SMC_CMD_PRIV_REG = 0x82000504, reg, PRIV_REG_OPTION_WRITE = 1, val)` (`soc-gs: drivers/soc/google/exynos-pd_el3.c:41-52`,
  `include/linux/soc/samsung/exynos-smc.h:45`, `include/soc/google/exynos-el3_mon.h:21-23`). `exynos_pd_tz_restore` is
  `SMC_CMD_PREPARE_PD_ONOFF = 0x82000410, EXYNOS_WAKEUP_PD_DOWN = 1, addr, RUNTIME_PM_TZPC_GROUP = 2` (`exynos-pd_el3.c:28-38`; `exynos-smc.h:42`).
  Reads of the PMU are `readl` (`pmucal_rae.c:131,145`), so **reading the PMU status from the AP is permitted** - the probe relies on that.
- **[src] The sequences** (`flexpmu_cal_local_gs101.h:447-483,548-555`):
  - `g3d_on`: write `0x17461E00` bit 0 = 1, wait `0x17461E04` bit 0 = 1. Then (TZPC restore SMC with `need_smc = 0x1C410204`), then the `g3d_save` entries are restored
    (CMU_G3D dividers, PLL lock times, mux users, sysreg G3D `DRCG_EN/MEMCLK`; plain `writel` to `0x1C40xxxx`, `0x1C42xxxx`).
  - `embedded_g3d_on`: `PLL_CON1_PLL_G3D`/`_L2` = `0x30000003`, `PLL_CON0` bit 4 set for both PLLs, `PLL_CON0_MUX_..._{COREGROUP,STACKS,TOP}_USER` bit 4 set, `EMBEDDED_G3D_OUT`
    (PMU `0x17462020`) |= 0x2 (atomic set), delay, write `0x17462000` bit 0 = 1, wait `0x17462004` bit 0 = 1.
  - Off paths mirror these.
- **[src] kbase's use:** `gpu_pm_power_on_top_nolock()` does `pm_runtime_get_sync` on the "top" and "cores" genpd devices (`pixel_gpu_power.c:252-262`) = the sequences above; **no
  regulator call exists** in `pixel_gpu_power.c`/`pixel_gpu.c` (grep: no `regulator`). Frequency is `cal_dfs_set_rate(cal_id, kHz)` (`pixel_gpu_dvfs.c:66`) which is
  `exynos_acpm_set_rate` -> `acpm_ipc_send_data_lazy` on the ACPM channel (`soc-gs: cal-if/acpm_dvfs.c:97-135`).
- **So:** the AP *can* power the two domains without ACPM, but not without EL3 (PMU SMC + TZPC SMC). *Rate and rail voltage* come from ACPM; if ACPM is not told,
  the GPU runs at whatever the CMU reset dividers/PLL give with whatever voltage the PMIC rail was left at. [guess] A low-voltage / high-frequency mismatch is the most
  likely way to hang the bus; a safe design programs the *lowest* OPP via ACPM first, which needs an ACPM IPC client (`soc-gs: drivers/soc/google/acpm/` is the reference, not read).
- **[guess] What ABL leaves on:** ABL drives display (DPU), UFS and USB; nothing in the boot path needs the GPU, so both domains are most likely **off** (status bit 0 = 0) and
  the probe will stop after the PMU/CMU words. The rails are `always-on` in the DT but in `SUSPEND` mode; their state is unknown.

### 2.3 Stage 0: `shim --gpu-probe` (built)

`cd shim && ./build.sh --board bluejay --avb --gpu-probe`. Source: `shim/gpuprobe.c/.h`, host test `shim/gpuprobe_host_test.c`, addresses `soc/google/gs101/soc.json` key `gpu`
(generated `GPU_*` macros through `tools/boards.py`). Not in the default build and not linked unless the flag is given; the object is compiled and audited on every build like `wlanprobe.o`.

Behaviour (all reads go through the fault-guarded `try_read32`; no stores, no SMC):

1. Always read the four PMU words: `G3D_CONFIGURATION 0x17461E00`, `G3D_STATUS 0x17461E04`, `EMBEDDED_G3D_CONFIGURATION 0x17462000`, `EMBEDDED_G3D_STATUS 0x17462004`
   (sources: section 2.1; AP reads allowed per `pmucal_rae.c:131`).
2. **Only if `G3D_STATUS` bit 0 = 1** (top domain powered; [guess] CMU_G3D lives in it: `pd_g3d` carries `cmu_id = 0x1C400000` and the embedded-domain sequence writes the CMU while only the top domain is up) read `CMU_G3D +0x600/+0x610/+0x620` (the three `MUX_..._USER`) and the two PLL_CON0 words
   (`+0x140`, `+0x180`).
3. **Gate** (`gpu_gate()` in `gpuprobe.h`, host-tested): GPU registers are read only when both PMU status bits are 1 *and* bit 4 of all three user muxes is set (the state
   Linux's own on-sequence produces; [guess] that bit 4 is the mux select). Otherwise print the reason (`pd_g3d-off`, `pd_embedded_g3d-off`, `cmu-mux-off`, `pmu-unreadable`, `cmu-unreadable`) and stop.
4. If the gate is open: `GPU_ID`, `L2/TILER/MEM/MMU_FEATURES`, `AS_PRESENT`, `JS_PRESENT`, `GPU_IRQ_RAWSTAT`, `GPU_STATUS`, `GPU_FAULTSTATUS`, `REVIDR`, `THREAD_*`, `SHADER/TILER/L2/STACK_PRESENT`,
   `SHADER_READY`, `L2_READY`, `COHERENCY_FEATURES`, `JOB_IRQ_RAWSTAT`, `MMU_IRQ_RAWSTAT` (offsets in 1.2) and the summary line
   `GPU SUMMARY gate=open id=XXXXXXXX Mali-G78 JM cores=20` (decimal).
5. The summary line is `GPU SUMMARY gate=<reason>` when the gate is closed. Either way it reaches the screen and pstore through `con_str`.

What the first run answers: (a) are the domains on after ABL (probably not), (b) the PMU status/config words are readable from EL1 at all, (c) whether CMU_G3D is readable when
the top domain is on (only if (a) is "on"). **[guess] Expected on a stock ABL boot: gate closed, `pd_g3d-off`.** That is still a result: it confirms the PMU read path and the
"off" state. Reading the GPU ID needs stage 1 (power).

### 2.4 Stage 1 (built): `shim --gpu-power`

`cd shim && ./build.sh --board bluejay --avb --pstore-fast --gpu-probe --gpu-power`; usage, output lines and the exact write allowlist: `docs/AUTOMATION.md` ("shim --gpu-power"). Source `shim/gpupower.c/.h`, host test `shim/gpupower_host_test.c`.
Scope is only the GPU's own power registers (G0 showed ABL already powers the PMU domains and clocks, doc 27): *read from source* (kbase JM `pm_driver.c`, local copy; table and omitted steps in the doc 27 addendum) the shim writes `TILER_PWRON` (which also powers the L2),
waits for `TILER_READY`/`L2_READY`, writes `SHADER_PWRON`, waits for `SHADER_READY`, reports, then always powers off (per block: drain PWRTRANS, write only if READY != 0: `SHADER_PWROFF`, then `L2_PWROFF`) back to the state ABL left. *Observed* in G0: `SHADER_READY` = `L2_READY` = 0
(`TILER_READY` and the three `*_PWRTRANS` were NOT read in G0; the precheck reads them live and refuses unless they are 0). It refuses with zero writes unless `GPU_ID`/`*_PRESENT` equal the G0 values, all `*_READY`/`*_PWRTRANS` are 0 and `GPU_STATUS`/`FAULTSTATUS` = 0 and `GPU_IRQ_RAWSTAT` = 0x100 (the G0 state;
bit 8 = RESET_COMPLETED, *read from source*, kbase macros.h:276). No SMC/PMU/CMU/ACPM write, no reset (`GPU_COMMAND`), no `STACK_*` write: `MALI_CORESTACK` is `default n` in kbase's Mconfig, but the Pixel build's setting is *unknown (guess)*, so `STACK_READY`/`STACK_PWRTRANS` are dumped read-only first.
The shim writes the LO word of each 64-bit register only; kbase uses `kbase_reg_write64` (both words, pm_driver.c:661); equivalent because the HI words of PRESENT are required to be 0.
Waits are bounded by 100000 reads and by 1 s of CNTPCT each (kbase's own bound is `PM_TIMEOUT_MS` = 5000, pm_driver.c:3244; the 1 s is our choice, *guess* that it is enough; the summary prints the measured ticks). Not yet run on hardware; writes need the user's go-ahead (section 4 stage 1).

## 3. What sailfish did, and what transfers

### 3.1 Structure in `ios-sailfish` (Pixel 1, Adreno 530) [src: local clone]

| Piece | File | Size | Role |
|---|---|---|---|
| Kernel GPU core | `bsd/dev/sweet_gpu.c` (patches `0119`-`0164`, ~34 patches) | ~2593 added lines | RPM/GDSC power + clocks, SMMU, CP microcode load, zap shader through TZ, command streams, GPU VA space (`sweet_gpu_vm_map/unmap`, 4 KB tables in 16 KB pool pages, `0137`) |
| Kernel API header | `bsd/dev/sweet_gpu.h` | ~121 lines | bounds-checked kernel entry points for the user client |
| IOKit service + user client | `iokit/Kernel/arm/SweetGPU.cpp` (`0138`, extended by `0150`) | ~677 lines | `IOService` "SweetGPU" + `SweetGPUUserClient : IOUserClient`; `externalMethod` selectors (below) |
| libdrm emulation | `tools/sweetgl/sweet_drm.c` | 529 lines | answers Mesa's `msm` DRM ioctls in user space on top of the user client; the "DRM fd" is `/dev/null` (header comment lines 1-24) |
| EAGL shim | `tools/sweetgl/eagl.c`, `sweetgl.c` | 2235 + 315 lines | CoreAnimation's GL renderer sees an EAGL/OpenGLES implementation backed by Mesa; IOSurfaces become textures through a GEM import |
| Mesa patch | `tools/sweetgl/mesa.patch` | 48 lines | hooks freedreno's one `mmap` (`freedreno_bo.c`) to `sweet_drm_bo_map` |
| Injection | `sweetgl.dylib` via `DYLD_INSERT_LIBRARIES` into backboardd | - | README: "no Metal; CoreAnimation falls back to its OpenGL ES renderer" |

User-client selectors (sailfish `0138` + `0150`, `SweetGPU.cpp`; client side `sweet_drm.c` lines 57-482): `0` map user memory, `1` map IOSurface, `2` unmap, `3` quad (test op), `10` bo new
(in: size flags; out: handle, iova, mmap address), `11` bo free, `12` bo import (in: IOSurface id; out: handle, iova, size), `13` submit (struct input: IB list + bo list, large
via `IOMemoryDescriptor`; out: sequence/fence), `14` wait (in: seq, timeout ms), `15` info (out: chip id, GMEM size/base, max freq, read-only **fence page** mapped into the task), `16` cache sync.
Completion is a 32-bit fence in a shared read-only page; `wait` is the kernel poll. The rest of the libdrm surface (GET_PARAM, GEM_INFO, SUBMITQUEUE, WAIT_FENCE) is faked in `sweet_drm.c`.

### 3.2 What transfers to Mali

| Transfers (reuse the shape) | Does not transfer |
|---|---|
| `IOService` + `IOUserClient` with `externalMethod` selectors, wired + write-combined BO memory mapped into the caller with `createMappingInTask` | everything in `sweet_gpu.c`: Qualcomm GDSC/RPM power, Adreno SMMU, CP/PFP microcode, zap shader, a5xx command encoding, GMEM, `msm` uapi |
| fence page + kernel `wait` | freedreno-specific `mesa.patch` (replace with a panfrost kmod backend) |
| user-space libdrm emulation in the injected dylib (no `/dev/dri`) | Adreno GPU-VA/SMMU layout (Mali has its own page-table format and address spaces) |
| EAGL shim (`eagl.c`) and IOSurface -> BO import | `gmem`, "zap", "RBBM" probing |
| `SweetSurface` / `SweetFramebuffer` already in our tree (patch 0046) | |
| Build: Mesa cross-build `tools/sweetgl/build.sh`, `ios-cross.txt`, `ios-dispatch.txt`, `gen-dispatch.py` | |

Mali needs *less* kernel code than Adreno in one respect: **no firmware** (JM) and **no secure zap path**; and *more* in another: the GPU's own MMU, cache
maintenance (`GPU_COMMAND` clean/invalidate), and the ACPM/EL3 power plumbing. [guess] A first Mali kernel core is 1.5k-2.5k lines (Adreno's was 2.6k plus 0.7k user client).

### 3.3 The surface a Mesa panfrost winsys needs from a minimal XNU driver

The Mesa side (current main) goes through `pan_kmod` (`mesa: src/panfrost/lib/kmod/pan_kmod.h:460-552` ops table). The panfrost backend (`panfrost_kmod.c`) issues exactly these
DRM ioctls (grep of `drmIoctl` call sites): `GET_PARAM`, `CREATE_BO`, `MMAP_BO` (returns an mmap offset), `GET_BO_OFFSET` (the **kernel-assigned GPU VA**), `WAIT_BO`, `SUBMIT`,
`MADVISE`, `SYNC_BO`, `QUERY_BO_INFO`, `SET_LABEL_BO`, `PERFCNT_ENABLE/DUMP` (`linux: include/uapi/drm/panfrost_drm.h:15-50`). Plus generic DRM: PRIME fd import/export, syncobj in/out
fences. For XNU, as in sailfish, none of these exist as kernel ioctls; they become an IOKit surface behind a libdrm emulation. The minimum selector set (a mapping of the above):

| Selector (proposed) | Panfrost op | Input / output |
|---|---|---|
| `info` | `GET_PARAM` (all `DRM_PANFROST_PARAM_*`: `GPU_PROD_ID`, `GPU_REVISION`, `SHADER_PRESENT`, `TILER_PRESENT`, `L2_PRESENT`, `STACK_PRESENT`, `AS_PRESENT`, `JS_PRESENT`, `*_FEATURES`, `THREAD_*`, `TEXTURE_FEATURES0-3`, `JS_FEATURES0-7`, `COHERENCY_FEATURES`) | out: the register words read once at bring-up (cached) + fence page |
| `bo_new` | `CREATE_BO` (flags `NOEXEC`, `HEAP`, `WB_MMAP`) | in: size, flags; out: handle, GPU VA, user mapping address |
| `bo_free` | GEM close | in: handle |
| `bo_import` / `bo_export` | PRIME / IOSurface import | in: IOSurface id; out: handle, GPU VA, size (sailfish sel 12) |
| `submit` | `SUBMIT` | struct in: job-chain head GPU VA (`jc`), BO handle list (`bo_handles`), `requirements` (FS / cycle count), optional in/out sync handles; out: sequence |
| `wait` | `WAIT_BO`, `WAIT_FENCE` | in: sequence or handle, timeout; kernel polls `JOB_IRQ_RAWSTAT` |
| `sync` | `SYNC_BO` / CPU-prep | in: handle, range, direction (cache clean/invalidate; the GPU is not assumed coherent) |
| `madvise`, `query_bo_info`, `set_label`, `perfcnt` | optional | return success/no-op |

Mesa-side work (not kernel): a model entry for the G78 id (the `pan_get_model()` NULL return above), a third `pan_kmod` backend or a libdrm emulation of the panfrost uapi, and the iOS cross-build
(`sweetgl/build.sh` style). [guess] `GET_BO_OFFSET`-style kernel-chosen VAs mean we do not need a CPU==GPU address match.

### 3.4 Not feasible (state it now)

- **Metal / AGX**: no AGX hardware, no Mali backend for Apple's Metal runtime. CoreAnimation can only render through the GLES path via the EAGL shim (the sailfish route); Metal apps will not run. [src: sailfish README "No Metal"; NOTES.md:386 QuartzCore prefers Metal and the GLES selection on a GPU-less device is "not yet established"].
- **Protected / secure rendering, DRM video paths**: not used on gs101 (section 1.2); needs EL3/Trusty cooperation. Not planned.
- **DVFS, thermal management, power gating policy**: need ACPM (rates, rails) and the TMU. Plan: one fixed low OPP, no autosuspend until proven.
- **Performance counters, hwcnt, SSCD dumps**: out of scope.
- **Hardware video decode / camera / ISP**: other blocks (MFC, ISP), not the GPU.
- GLES version: [guess] GLES 3.1 on Valhall via Mesa is plausible (sailfish ran GLES 3.1 on freedreno); not verified for panfrost here.

### 3.5 Mesa and the G78 id

Mesa's `pan_model_list` only has `(9,0,1)`, `(9,0,3)` (G57) and `(9,2,4)` ("G68", family "G78") for arch 9; `pan_get_model()` returns NULL on a miss, which panfrost treats as an unsupported GPU.
kbase says G68 is `LBEX` (product_major 4) and G78 is `TBEX` (product_major 2), so a real G78 id would be `(9,2,2)` or similar - **[guess]** that adding an entry equal to the G68 one is enough for the compiler; the first
real answer is the probe's `GPU_ID`.

---

## 4. Staged plan with risk

Every stage that writes hardware needs the user's go-ahead (a "write experiment" in the doc 12 sense) and an on-hardware result recorded in the handoff before the next stage. All state touched is volatile (PMU/CMU/GPU
registers); nothing is persisted and the power key / PSCI reset clears it. That is why this stays inside AGENTS.md rule 1, but a wrong SMC or a bus hang can still force a Power+VolDown recovery.

| Stage | What | Risk | Only hardware can answer |
|---|---|---|---|
| **0** (built) | Read-only: PMU status + CMU mux words; GPU ID/status only behind the gate (section 2.3). | low: reads only, fault-guarded. Residual: a read of a gated block hanging the bus (the gate exists to avoid exactly that) | Are the domains on after ABL (probably not)? Are the PMU words readable from EL1? Does `GPU_ID` read `0x9002....`? |
| **1** power + reset | From the shim (not XNU, to keep it attributable): PMU `G3D_CONFIGURATION` set via `SMC_CMD_PRIV_REG`, wait status; `SMC_CMD_PREPARE_PD_ONOFF` TZPC restore; CMU writes (embedded sequence); `EMBEDDED_G3D_CONFIGURATION` via SMC; then read `GPU_ID`, `GPU_COMMAND = SOFT_RESET`, wait `RESET_COMPLETED`, power on L2/tiler/shaders, poll `*_READY`. | **high**: first vendor SMC from our context; unknown ACPM voltage/frequency; a wrong argument can hang or reset the phone (recoverable with Power+VolDown) | Does EL3 accept `0x82000504` / `0x82000410` from EL1 at all, for these registers? Does the domain reach status 1 without ACPM? Does the GPU come up at the default CMU rate/rail voltage or is an ACPM call needed first? Is any extra pre-step missing (S2MPU power-domain-id, `skip-idle-ip`, PMU `*_OUT`)? |
| **2** MMU + memory | Program AS0 (`TRANSTAB`/`MEMATTR`/`TRANSCFG`, Mali LPAE-style page table with 4 KB pages built from 16 KB XNU pages), flush/`LOCKADDR` commands; a writeable test page; handle `MMU_IRQ_RAWSTAT` faults by polling. | medium: GPU DMA is checked by 4 G3D S2MPUs [src]; XNU RAM lives at `0x880000000` | Do the G3D S2MPUs allow DMA into XNU's RAM bank (UFS DMA to `0x88...` is allowed, [obs] HANDOFF.md:244, but G3D has separate S2MPUs)? If not, fall back to a carve-out like the USB DMA window (`abl@f8800000`, [obs] board.json notes) or find out how to program the S2MPUs (EL2/EL3-owned, [guess]). Is the GPU cache-coherent for CPU-cached pages (`COHERENCY_FEATURES`)? |
| **3** job submit | Smallest job chain: a WRITE_VALUE job (writes a magic word to a GPU VA), then a compute/fill job from a Mesa-built blob; slot 0 via `HEAD_NEXT`/`AFFINITY_NEXT`/`CONFIG_NEXT`/`COMMAND_NEXT`; completion by polling `JOB_IRQ_RAWSTAT` (no IRQ plumbing; xnu/AGENTS.md says drivers are polled). | medium: faults need soft/hard reset recovery; cache maintenance by hand | Does a job complete (status `DONE`) or fault? Does the fill appear in CPU-visible memory after a clean/invalidate? Does `SHADER_PRESENT` really have 20 cores? Stable at the default clock? |
| **4** user client | `SweetGPU` IOService + `SweetGPUUserClient` with the selectors of 3.3; wired BO memory; fence page; libdrm/panfrost emulation + a Mesa model entry in `sweetgl`; a user-space test like sailfish's `sweetgpu-test`. | medium: IOKit surface is XNU-side only; failure is a hang of the client, not the phone | Does a Mesa panfrost context (headless GLES clear/draw into a BO) run correctly? Which GLES version/extensions? |
| **5** IOSurface + display | Back IOSurfaces (`SweetSurface`) with GPU-visible BOs; backboardd renders through the EAGL shim into them; present: see 4.1. | medium-high: needs the EAGL shim to satisfy CoreAnimation's GL path and the CA renderer selection (open per NOTES.md:386) | Does CA pick the GL renderer? Frame rate vs the current CPU path? |

### 4.1 Render targets to the framebuffer

Today the screen is the ABL `simple-framebuffer` (`fb_base 0xfac00000`, 1080x2400, stride 4320, a8r8g8b8; [obs] board.json) and `SweetFramebuffer.cpp` (patch 0046) copies the enabled layers
into it with the CPU on every `SwapEnd` ([src] NOTES.md:386, [obs] first iOS pixels on the panel, NOTES.md:373). Two ways the GPU joins in:

1. **GPU renders to IOSurface BOs; the CPU copy stays (first step).** Surfaces are wired kernel memory already (`SweetSurface`), so the same pages are mapped into the GPU AS. After a job completes, invalidate the CPU
   cache over the surface, then the existing copy runs. Costs ~10.4 MB per full-screen frame; works regardless of display-side coherency. Needs only stages 2-4.
2. **GPU composites into the framebuffer BO.** Map the physical framebuffer (`0xfac00000`, `0x9e3400` bytes) into the GPU AS as one more BO (page tables are ours, so any PA is expressible) and make the last pass of
   the frame a GPU blit/scale into it (what sailfish does with "present"). Depends on a G3D S2MPU allowing that PA ([guess]: the framebuffer is a DPU-owned region; unknown) and on the panel being scanned continuously
   (the shim re-arms the DECON hardware trigger; no flip handshake exists today). Strictly after option 1 works.

---

## 5. Unknowns that can block the whole plan

1. **EL3 gatekeeping of the power sequence [guess].** The PMU alive block is only writable through `SMC_CMD_PRIV_REG`; EL3 may whitelist registers and/or callers. If EL3 refuses the G3D registers
   from our (non-Linux, EL1, no hypervisor) context, the GPU cannot be powered from XNU at all. Nothing in the Android source shows the monitor's policy. Stage 1 is the test.
2. **ACPM-owned voltage/frequency [src for ownership, guess for consequence].** If the rails sit at a retention/suspend voltage until ACPM raises them, powering the domain without ACPM may do nothing or hang. Mitigation:
   port the ACPM IPC client (`soc-gs: drivers/soc/google/acpm/`) and request the lowest OPP before touching the cores. This is a second large subproject (mailbox in APM SRAM, protocol).
3. **G3D S2MPU / TrustZone state [guess].** GPU DMA goes through four S2MPUs configured by EL2/EL3 (`pkvm-s2mpu` and `s2mpu` driver dirs exist under `soc-gs: drivers/soc/google/`). ABL's setting for
   `0x20080000..` is unknown. A closed S2MPU means the GPU reads/writes fault silently or raise ITMON; the MMU/page tables would be the least of the problems. (The G78 MMU itself is plain MMIO in kbase - not TZ-gated.)
4. **Mesa G78 support [src/guess].** No `(9,2,2)` model row today; trivial to add, but unverified whether G78-specific hardware issues (kbase's `base_hw_issues_tBEx_*` tables) are handled by panfrost.
5. **CoreAnimation renderer selection without Metal (open since NOTES.md:386).** If CA refuses to use GL on this userland, the EAGL shim cannot help; only a hardware/SpringBoard experiment answers it. Sailfish shows it is possible on iOS 16.7.16.
6. **Cache coherency and 16 KB pages [guess].** Mali tables use 4 KB pages; XNU pages are 16 KB. Sailfish handled the same split for Adreno (4 KB tables inside 16 KB pool pages); COHERENCY on G78 is a register (`0x300`/`0x304`) the probe reads.

Biggest blocker: **(1) with (2) behind it** - powering the GPU is not an MMIO job we can do alone.

## 6. Verification of this document's facts

Every [src] row above was fetched on 2026-10-22 from the pins in the table; the fetched copies were kept in `/tmp/gpuref` for this session only and are not committed. To re-check a fact: fetch `<repo>/+/refs/heads/android-gs-bluejay-6.1-android16/<path>?format=TEXT` and base64-decode. Nothing here is [obs] for the GPU; the only [obs] cited are about
the framebuffer, DMA windows and the display path from earlier runs.

Open items for the next session: run `--gpu-probe` (user-run, `tools/fboot.sh --expect probe`) and record `GPU SUMMARY` here; read `soc-gs: drivers/soc/google/acpm/` for the IPC protocol; read Mesa genxml for the WRITE_VALUE job layout.

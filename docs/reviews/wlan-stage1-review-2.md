# Review 2: shim --wlan-iso (W2c, EL3 RMW writer), commit 644c09c (2026-10-23)

Reviewer: Claude Opus 5.5 (adversarial, read-only; author Sonnet 5.5, the same family, so rule 6 is not met and the driver decides). Nothing was run on hardware.
I built on the host: `git archive HEAD` into /tmp, then `shim/build.sh --board bluejay --avb --pstore-fast --usb-gadget --wlan-probe --wlan-iso` (rc 0). All audits and self-tests passed,
including the wlaniso host test, the wlansmc source self-test 28/28 and the iso object self-test. I disassembled `wlansmc_iso.o`, `wlansmc.o` and `shim.elf` with aarch64-none-elf-objdump.
The refs I cite are in `~/pixelios-artifacts/refs/wlan-stage1/`. The rc.c line numbers in the author's docs are from `dwc-whi/` (whitechapel = gs101), not from `dwc/`.

## Verdict: GO-WITH-FIXES (F1 and F2 before the run; the SMC set, the ABI and the semantics hold)

## (1) Exact SMC set, from the disassembly
- `wlansmc_iso.o` has 3 `smc #0` instructions, one in each of `smc_priv_read`, `smc_priv_rmw_set` and `smc_priv_rmw_clear`.
  - read: x0 = 0x82000504, x1 = the caller's pa, x2 = 0, x3 = 0. The guard `and x1, x0, #~4; cmp 0x17463EC0; b.ne -> -1` admits only 0x17463EC0 and 0x17463EC4.
  - set: x0 = 0x82000504, x1 = 0x17463EC4, x2 = 2, x3 = 1, x4 = `mov x4, x3` (= 1). Straight-line constants, no branch.
  - clear: the same with x4 = `mov x4, #0`.
  - Call sites: `bl smc_priv_rmw_set` once (wlan_iso_run+0xe0), `bl smc_priv_rmw_clear` once (+0x1ac), `bl smc_priv_read` from `one` and once from wlan_iso_run with x0 = 0x17463EC4.
  - The precondition branches (`cbz` on each try_read32, `tbnz` bit 0 of ISO1, bit 0 of GPH2.DAT (0x14440004), bit 2 of GPP14.DAT (0x108401C4), `cbnz` and `tbnz #31` on the READ) all come before the set.
  - No other option, address, mask or value can be represented. Plain `wlansmc.o`: one smc, no RMW symbols. The linked iso image has no `wlansmc.o` in it.
- **F5 (L) doc scope.** AUTOMATION.md says "SMCs the image can issue: ... Nothing else (no WRITE option 1 ...)". That is false for the image:
  `shim.elf` also contains `prepare_bluejay_cpu`'s option-1 WRITE (`shim.elf` 0x286c: x0 = 0x82000504, x1 = CPU_INFORM 0x17460860 + 4*cpu, x2 = 1), plus the PSCI calls. Scope the sentence to `wlansmc_iso.o`.

## (2) Semantics and ABI (read from source)
- `rmw_priv_reg(reg, mask, val)` calls `arm_smccc_smc(SMC_CMD_PRIV_REG, reg, PRIV_REG_OPTION_RMW, mask, val, 0, 0, 0)` (exynos-pd_el3.c:66-77). `__arm_smccc_smc` copies a0..a7 to x0..x7 (linux-arm-smccc.h:376-390).
  So x0 = fid, x1 = reg, x2 = 2, **x3 = mask, x4 = val**. For comparison, `set_priv_reg` puts val in x3 (:40-50), and `get_priv_reg` passes x3 = 0 and **returns the value in a0** (:53-63).
  Options READ/WRITE/RMW = 0/1/2 (exynos-el3_mon.h:21-23). The fid is 0x82000504 (exynos-smc.h:45). Mainline `tensor_sec_reg_rmw` uses the same order, mask in x3 and val in x4 (linux-exynos-pmu.c:73-88).
  The caller `exynos_pcie_phy_isolation` = `rmw_priv_reg(pmu_alive_pa + 0x3EC4, PCIE_PHY_CONTROL_MASK = 1, val)` (dwc-whi rc.c:393-407, rc.h:120). The author's mapping is correct.
- The argument order is low-risk anyway. The set is (1, 1), so swapping mask and value changes nothing. A swapped clear (mask 0, val 1) is a no-op under RMW, which ends in a loud `RESTORE FAILED`, not a wrong write.
- Prior acceptance (observed, archived): `REBOOT_BL` option 1 on SYSIP_DAT0 printed `SMC ret 0 after 0xFC`, and the SWRESET via the same SMC reset the phone (docs/archive/HANDOFF-s01-s06.md:374,382).
  W2a READ of 0x17463EC4 and 0x17463EC0: a0 = 0, a1..a3 echoed unchanged, same=yes, ambiguous=1 (logs/probe/20261023T030915Z-w2a_/usb.txt:466-474). No RMW (option 2) has been seen on hardware.
  **F6 (L)** wlansmc.c:11, research 20 row 3 and AUTOMATION cite `HANDOFF.md:357,365`. HANDOFF.md is 111 lines, so fix the cite to the archive path above. CPU_INFORM "observed ret 0" has no cite I could find: tag it *guess* or cite it.
- **F7 (L)** Linux passes x5..x7 = 0. The shim leaves them as garbage, which READ tolerated on hardware (observed). Zeroing them in the two RMW helpers costs 3 movs and matches Linux exactly. If you do it, extend the object audit.
- Note (read from source): mainline treats PMU_ALIVE offsets ≤ 0x3FFF as multi-master and uses the SET/CLR alias writes, not RMW (linux-exynos-pmu.c:136-162).
  Google's kernel for this phone does RMW on this exact register, so following downstream is right. The EL3 RMW is not atomic against ACPM, but that race does not matter in the shim (*guess*).

## (3) Physics and ordering
- dwc-whi rc.c:3388-3394 has `if (use_phy_isol_con) phy_isolation(BYPASS)` as the first statement for LINK_DOWN, then `clock_enable`, with no delay in between. That is confirmed, and pcie_1 has `use-phy-isol-en = "true"` (gs101-common.dtsi:372).
  Linux's full order, though, is dhd WLAN_EN high first (review 1 section 3), then poweron, then the iso release. The shim releases with the module **off**, which is the reverse.
- Linux does release the isolation without any PHY config or link: in the panic notifier (rc.c:4996-4997), the itmon notifier (:4664-4665) and the sysfs test (:470/534, release then restore around a no-op op).
  So "released, unconfigured, PERST low" is a state Google's code produces (read from source). The bit gates the isolation cells between the PHY domain and the core logic.
  The author's "1 = released (PHY **powered**)" is wrong in kind: power comes from the LDO17/19 rails (observed on in W2), not from this bit. **F8 (L)**: reword it and tag it *guess*.
  Danger with WLAN_EN low: an un-isolated PHY in its reset/power-down state could drive REFCLK/TX into an unpowered module (back-powering through ESD diodes) for about 1 ms. Low (*guess*).

## (4) Failure branches
- The set is accepted and applied: `ok`, then one clear, read-back 0. Refused (a0 ≠ 0, nothing changed): `refused`, no clear. Lie (a0 ≠ 0 but applied): `mismatch` + clear. Fault: clear. Other bits or ISO0 moved: `mismatch`, ISO1 cleared, ISO0 not repairable (reset the phone).
- **F1 (M) The late or silent apply leaves the bit set.** If EL3 returns 0 but the bit shows after the 1 ms bound, no clear is sent. Same if RMW returns its new value (unknown: READ returns the value in a0) and the bit is still 0 at read-back.
  Fix: send the one clear **unconditionally** after any set that was sent. Under RMW, a mask-1/val-0 clear on a 0 bit is a no-op. Under overwrite it writes 0 to a word observed to be 0. Under refusal it is refused. "Never a second set" still holds.
  Then add one delayed final read (e.g. 10 ms) that prints `ISO final`. The audit pin `if(!rok||(after&1u))` and the self-test case "clear unconditional" must flip. That is a tightening, not a loosening.
- **F2 (M) Later sections still run on a bit left set.** `wlan_iso_run()`'s result is ignored (shim.c:713). The level-2 gate opens on "ISO1 bit 0 = 1 and the CMU mux bit 4 = 1", and the mux bit is already **observed 1** (w2rails usb.txt:475,482).
  So a failed restore or a late apply opens the SYSREG/ELBI/DBI/PMA reads of an unconfigured, possibly unclocked block. Those reads are unreviewed and can hang, and they are what 12.1 Q3 wants kept out.
  The same failure lets `--wlan-iso --wlan-en` drive the module on with the PHY released, which contradicts 12.1 ("W2c requires WLAN_EN low"); wlanen.c:237 only prints ISO1.
  Fix: in a `WLAN_ISO_RUN` image, skip level 2 at compile time, and skip `wlan_en_run` unless `wlan_iso_run()==0` (or have build.sh refuse `--wlan-iso` together with `--wlan-en`). Pin both in `check_shim_wlan_sequence`.
- Reset survival (*guess*): PMU alive words are kept across warm resets by design (SYSIP_DAT0 carries the reboot mode through SWRESET, research 10), so 0x3EC4 may survive a warm reset.
  Two things cover a bit left set (read from source): Android's `exynos_pcie_rc_probe` always ends with `phy_isolation(ISOLATION)` (rc.c:5409-5410), and the shim's next W2c refuses on it (`already released`). A cold power-off clears it (*guess*, U9).
- **F3 (L)** The precondition should also require `(uint32_t)rd.a0 == before`. That is the W2a consistency check, and it costs one compare.

## (5) Audits
`git diff HEAD~6 HEAD -- tools/test-bluejay-boot.py`: +639/-0 net. The -lines in 644c09c are a refactor into `_check_wlansmc_plain`/`_check_wlansmc_iso`, and the plain bans grew.
The only lost item is the W2a self-test mutation "option as a parameter". Its signature check (line 2477) remains, so re-add the mutation (**F4, L**).
The source audit is strong: exact bodies, one call each, ordered preconditions, a bounded poll, a loud restore. The object audit pins x0..x4 per helper and the smc locations.
It does not count the `bl smc_priv_rmw_set`/`_clear` sites in the object (only the source does). That is acceptable while the helpers are noipa and constant-only.

## (6) Guess tagging
Tagged correctly: the RMW semantics (wlansmc.c:17), the blind spots, and the cold power-off. Untagged or overstated: "PHY powered" (F8). "a successful READ ... is evidence it is writable" (wlansmc.c:9) is mainline's remark about mainline EL3, and a0 = 0 is ambiguous here: tag it *guess*. Also the CPU_INFORM "observed" (F6).

## Answers to 12.1
1. Yes, but unconditional rather than "whenever a0 == 0": send one clear after any sent set, then a delayed final read (F1).
2. No probe write of another value. At word 0, overwrite and RMW produce identical results for 1 then 0, and the other-bits/ISO0 checks catch drift. It is not worth a new write.
3. Not in this writer. Close the existing level-2 path in iso images too (F2). A separate flag with its own review should hold the bit, read, and clear.
4. Agreed: no plain-PMU-store fallback. That would be a new writer class (a store to PMU alive) and needs its own review.
5. No dwell. W2c answers "does EL3 accept the RMW on 0x17463EC4, and does the bit follow?". The ~1 ms pair is enough. Dwell belongs to the level-2 flag.
(Section 12's own list: Q1, ambiguous=1, is unavoidable at value 0, so allow it with F3. Q2, WLAN_EN low, is right; enforce it with F2. Q4, restore on RMW 0 with read-back 0, see F1. Q6, see the reset-survival note.)

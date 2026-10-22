# Phase 0 review 1: `t3/bluejay-probe-shim` vs `main`

Reviewer: Claude (review sub-agent), 2026-10-20. Scope: `git diff main...t3/bluejay-probe-shim`
(commits 741c6b5..6f80409). This review is read-only: nothing was built, run or flashed. The
disassembly checks used the existing `shim/out/shim.elf` in the worktree.

Checked against upstream sources (fetched today): `drivers/soc/samsung/gs101-pmu.c`
(SMC 0x82000504 args) and `drivers/ufs/host/ufs-exynos.c` (PRDT data-unit size). Other
register facts were checked against `docs/research/05-fact-check.md`.

Line numbers refer to the branch tip.

---

## Critical (will hang or misreport on first boot)

### C1. Probe fault recovery never fires: wrong vector slot, wrong EL registers, SError masked
`shim/shim.c:374-379`

```c
if (slot == 8 && probe_guard) { ... wsr("elr_el1", rsr("elr_el1") + 4); return; }
```

- The shim runs at EL1h (SPSel=1). A synchronous data abort from EL1 is taken in VBAR_EL1
  **slot 4** ("current EL, SPx, sync"). Slot 8 is "lower EL AArch64 sync", which is only taken
  from EL0. So every faulting `try_read32`/`try_write32` (watchdog, DECON, PMU intr-gen) goes
  to the blue PANIC screen and then `reset(40)`. The probe stops at the first bad register,
  which defeats the purpose of the guard.
- Slot 8 is correct only if the abort is routed to **EL2** (VBAR_EL2 = `vectors`, because of a
  stale HCR_EL2.TEA/VM/AMO; see C2). In that case the handler reads FAR_EL1 and writes
  **ELR_EL1**, but `eret` at EL2 uses ELR_EL2. The core then re-executes the faulting load, so
  the fault repeats forever: a silent hang.
- ESR is not checked. Any sync exception taken while `probe_guard` is set (UNDEF, alignment, a
  trapped sysreg) is "recovered" by skipping an instruction.
- SErrors are masked for the whole run (`daifset #0xf`, SPSR 0x3c5, never `daifclr #4`). On
  Exynos, a store to an unclocked or secure block normally completes as an **asynchronous
  SError**, and some loads do too. With PSTATE.A=1 the SError stays pending and is never
  reported. So `try_write32` reports success for a write that faulted, and `try_read32` can
  return bus garbage as success.
- A read that **hangs** on the bus (gated PCLK, a common Exynos failure) cannot be recovered
  by any exception handler.

Fix (slot 4 at EL1, ESR-checked, SError window inside the guard):

```c
/* exc_handler */
if (probe_guard && (slot == 4 || slot == 7)) {
	uint64_t ec = rsr("esr_el1") >> 26;
	if (slot == 7 || ec == 0x25) {		/* SError, or data abort same EL */
		probe_fault = 1;
		probe_fault_address = rsr("far_el1");
		probe_guard = 0;
		if (slot == 4)
			wsr("elr_el1", rsr("elr_el1") + 4);
		return;
	}
}
```

In `try_*`, after the access add `dsb sy; msr daifclr, #4; isb; msr daifset, #4` before
`probe_guard = 0`, so that a pending SError is taken inside the window. Also fix C2, so that
nothing is routed to EL2. Note that if the EL3 monitor sets SCR_EL3.EA, SErrors go to EL3 and
the result is firmware behaviour (likely a reset). The probe cannot avoid that, but it should
not claim success either.

The codegen itself is fine: in `disable_bluejay_watchdog` the volatile `ldr w8,[x6]` sits
between the two `probe_guard` stores, so `+4` lands on `str wzr,[x19,#4]`.

### C2. EL2→EL1 drop inherits whatever EL2/EL1 state the firmware left
`shim/start.S:43-60` (primary) and `:82-98` (secondary)

- **HCR_EL2 is OR'd** (`orr #(1<<31)`), not written. Any stale bit breaks the shim:
  - TGE (27) or E2H (34): the `eret` to EL1 is illegal or behaves differently.
  - TSC (19): every PSCI/PMU `smc` traps to EL2.
  - TWI (13): every `wfi` traps.
  - IMO/FMO/AMO (3-5): IRQs and SErrors route to EL2.
  - DC (12): MMU-off EL1 accesses become Normal WB, so the "MMU off ⇒ coherent" reasoning in
    `ufs.c` and in `cpu_report` polling no longer holds.
  - VM (0) with VTTBR=0: stage 2 is walked from PA 0.

  Linux writes the whole register (`HCR_HOST_NVHE_FLAGS`).
- **SCTLR_EL1 is never written** on the EL2 path. Its reset value is architecturally UNKNOWN,
  and ABL may have left M/C/A/EE/WXN set. Linux does
  `mov_q x0, INIT_SCTLR_EL1_MMU_OFF; msr sctlr_el1, x0` before dropping to EL1.
- **VPIDR_EL2/VMPIDR_EL2 are not set.** With EL2 present, EL1 reads of MIDR_EL1/MPIDR_EL1
  return these registers, and their reset values are UNKNOWN. The probe's headline MIDR/MPIDR
  output, the caller index used for CPU_INFORM (`shim.c:180`), `find_gicr()` and `sgi_self()`
  all depend on them. Linux copies `midr_el1→vpidr_el2` and `mpidr_el1→vmpidr_el2`.
- PSCI CPU_ON on TF-A-style monitors enters secondaries at **EL2** with fresh-reset EL2
  registers, so the secondary path needs the same full init.

Fix (both paths, before `eret`):

```asm
	mov	x2, #(1 << 31)			/* HCR_EL2: RW only */
	msr	hcr_el2, x2
	mrs	x2, midr_el1
	msr	vpidr_el2, x2
	mrs	x2, mpidr_el1
	msr	vmpidr_el2, x2
	mov	x2, #0x0800			/* SCTLR_EL1 RES1 bits, MMU/caches off, LE */
	movk	x2, #0x30d0, lsl #16
	msr	sctlr_el1, x2
	msr	cptr_el2, xzr			/* harmless: no FP traps if ever needed */
	msr	hstr_el2, xzr
```

`CNTHCTL_EL2=3` and `CNTVOFF_EL2=0` are right for nVHE once E2H is cleared.
`ICC_SRE_EL2 |= SRE|Enable` matches Linux. SPSR 0x3c5 (EL1h, DAIF masked) and x0/x19 DTB
preservation are correct. The EL1-entry (sweet) path is functionally unchanged.

---

## High

### H1. WFI waits can sleep forever: the GIC is never set up on the bluejay path
`shim/console.c:33-37` (`screen_room`), `shim/shim.c:287-294` (end of probe),
`shim/shim.c:324-331` (`reset()` on bluejay)

WFI wakes only when the GIC signals an interrupt to the PE. PSTATE masking does not matter
for wake-up, but the interrupt must be enabled, grouped and above PMR at the
redistributor/CPU interface. The bluejay path never enables PPI 27, never sets ICC_PMR_EL1 or
ICC_IGRPEN1_EL1, and never sets ICC_SRE_EL1.SRE, so the virtual timer firing may never wake
the core. `screen_room` runs from **every** `con_*` call once a page fills. The reserved-memory
dump fills several pages, so the first full page can freeze the probe. The final 120 s wait
and the PSCI reset path have the same problem, so the phone never reboots by itself.

Fix: spin. One core spinning for two minutes is not a thermal concern, and ABL spins too. For
a low-power wait, use WFE with the generic-timer event stream, which needs no GIC:

```c
wsr("cntkctl_el1", (1 << 2) | (13 << 4));	/* EVNTEN, ~2^14 ticks ≈ 0.67 ms @24.576 MHz */
while (now() < until) __asm__ volatile("wfe");
```

Also use one helper for all three waits instead of three copies.

### H2. UART TX polls forever
`shim/console.c:59-65`

If the USI/UART is clock-gated or TX is disabled, for example when `fastboot oem uart enable`
was skipped or does not configure this instance (05 §8 says the SBU mapping is
UNVERIFIABLE), then UTRSTAT.TXE never sets or UFSTAT.TXFULL never clears. Every `con_*` call
then hangs right after drawing its first line. A gated APB read can also hang the bus outright.

Fix: bound the poll (about 1 ms using the counter). On the first timeout, set a static
`uart_dead = 1` and stop touching the UART. The register offsets and bits match
`samsung_tty.c` (UFCON 0x08 bit 0, UTRSTAT 0x10 TXE bit 2, UFSTAT 0x18 TXFULL bit 24 for the
S5PV210/Exynos layout, UTXH 0x20, 32-bit access), so only the polling needs changing.

### H3. FDT `reg` cell-size heuristic misreads `#size-cells = <1>` trees
`shim/fdt.c:115` and `:246`. `fdt_framebuffer` at `:184-185` has the same assumption.

`stride = len % 16 == 0 ? 16 : 12` guesses the cell sizes. Mainline gs101 uses
`#address-cells = <2>; #size-cells = <1>` at the root (12-byte entries; check this against the
live FDT, which `fdt_probe_report` dumps). Four DRAM banks are 4 × 12 = 48 bytes, and 48 is a
multiple of 16. The parser then decodes them as three 16-byte entries of garbage.
`fdt_probe_report` would print wrong memory and reserved-memory bases and sizes, and Phase 1
would be planned from that output. `fdt_memory_contains` would also return wrong answers.
The reserved-memory children use the reserved-memory node's own cells.

Fix: read `#address-cells`/`#size-cells` from the root, and from `/reserved-memory` for its
children. They are only two properties to remember during the walk. Add a 4×12-byte case to
`fdt_host_test.c`.

---

## Medium

### M1. Sweet regression: unconfirmed FDT makes the image Device + PXN, which aborts on MMU enable
`shim/mmu.c:88,108`

If `fdt_memory_contains` says no (H3 misparse, the image straddling two `reg` entries, an FDT
quirk), every image page is mapped `DEV_FLAGS`, which include PXN. The first instruction
fetch after `SCTLR.M=1` takes a prefetch abort, and the vectors are PXN too. The result is a
recursive abort and a hang. `main` always mapped the image WB, and sweet worked that way.

Fix: `if (!info->img_fdt_confirmed) return -1;`. `shim_main` already handles `rc != 0` by
staying MMU-off. The MMU path is unused on bluejay, so this is purely a sweet risk.
Mapping 8-64 GiB as Device on sweet is harmless.

### M2. Watchdog: a 1 s observation delay before disabling, and the reads are unguardable
`shim/shim.c:121-145`

If ABL left a short timeout running, the shim spends up to a second watching before it
disables the watchdog. Before that, it already did a full Device-memory framebuffer clear and
the live-FDT walk. The 05 §7 notes on PMU `CNT_EN` and `MASK_RESET` mean a WDT can also be
"enabled but not counting", which the current logic handles correctly.

Fix: read WTCON/WTCNT, clear `ENABLE|RSTEN` (bits 5 and 0, correct per `s3c2410_wdt.c`)
unconditionally, then re-read and report. Writing 0 to those bits on a stopped watchdog is a
no-op. Move this before `con_clear`.

### M3. UFS: one 16 KiB PRDT entry exceeds the Exynos 4 KiB data unit
`shim/ufs.c:318-319` (GPT entries, `count*128` bytes = 4 blocks) via `scsi()` `:116-125`

Upstream `exynos_ufs_post_link()` programs
`HCI_TXPRDT_ENTRY_SIZE`/`HCI_RXPRDT_ENTRY_SIZE = ilog2(DATA_UNIT_SIZE)` = 12, and the driver
sets `max_segment_size = DATA_UNIT_SIZE` (4096). ABL probably programs the same values. A
single PRDT entry with `dbc = 16383` is then out of spec for this host: expect an OCS error,
a partial transfer or a hang. The 4 KiB GPT-header read is fine.

Fix: read the entry array one 4 KiB block per command (simplest), or emit one 4 KiB PRDT
entry per block with PRDTL = n × 16 bytes. Byte-granular RSP/PRDT offsets and lengths
(`512<<16|512`, `1024<<16|16`) match `UFSHCD_QUIRK_PRDT_BYTE_GRAN` in ufshcd.c. The residual
128-byte-entry (FMP DESCTYPE_3) risk is unchanged from 05 §6. Also:
- The timeout at `:134` hard-codes 19.2 MHz, so it is 0.78 s on bluejay. Harmless, but use
  `TIMER_FREQ`.
- `test_ufs_bluejay` reads HCI registers unguarded. If ABL gated the UFS clocks, the first
  read hangs. That is acceptable as the last probe step, but say so on screen before the read.

### M4. `fdt_framebuffer` keeps stale per-depth state and matches loosely
`shim/fdt.c:160-190`

- `found[depth]` and `simple[depth]` are not cleared at `FDT_BEGIN_NODE`. A sibling inherits
  its previous sibling's `reg` or `simple-framebuffer` flag. Mainline gs101's simplefb node
  has **no** `reg` (it uses `memory-region = <&cont_splash_mem>`), so `simple=1` leaks into
  the next sibling at that depth. A dynamically allocated `*splash*` reserved node without
  `reg` takes the previous sibling's base.
- Any node at any depth whose name contains "splash" or "framebuffer" matches, first hit wins.
- `compatible` is matched only against its first string.
- If this picks a wrong base, the screen stays black (the UART still works).

Fix: zero `found[depth]`/`simple[depth]` on BEGIN_NODE, and only accept
`/chosen/framebuffer*` with `compatible` containing `simple-framebuffer`, or
`/reserved-memory/*splash*`. Prefer the board fallback over a guess.

---

## Low

- **L1.** `stage` is never set on bluejay, so the panic path calls `con_str("STAGE", NULL)`,
  which dereferences PA 0 in `put_str` and `uart_text`. (`shim.c:60,390`) Set
  `stage = "bluejay probe"` at the start of `bluejay_probe`.
- **L2.** The FDT walkers have no `totalsize` bound. A corrupt `len` or `off_dt_struct` walks
  into MMIO with the MMU off and can hang the bus. Add `if (p >= fdt + be32(fdt + 4)) return 0;`
  per token. The `be32` and string reads are aligned, and `-mstrict-align` covers the
  compiler, so unaligned access is not an issue.
- **L3.** Mixed clock sources. `delay_ms`/`counter_freq` use the constant `TIMER_FREQ`, which
  05 §4 recommends. But the end loop (`shim.c:287`) and `screen_room` use `CNTFRQ_EL0`. If
  firmware left CNTFRQ at 0, the probe resets immediately after UFS. Use `counter_freq()`
  everywhere.
- **L4.** CPU bring-up. After the 250 ms timeout, a second CPU_ON can return ON_PENDING
  (-5) or ALREADY_ON (-4) for a slow core, and `prepare_bluejay_cpu` then pokes intr-gen for a
  core that is mid-power-up. Print the first `ret` too. It is currently overwritten, and
  ON_PENDING is the diagnostic you want. The PMU SMC matches upstream `tensor_sec_reg_write`
  (x1 = PA, x2 = 1 write, x3 = value, a0 = 0 success). The CPU_INFORM caller index and the
  intr-gen offsets match `__gs101_cpu_pmu_online`. The per-CPU report polling is coherent
  only with the MMU off and HCR_EL2.DC = 0 (C2).
- **L5.** On bluejay, CPU1 still runs the sweet GICR test (`shim.c:416-425`), which writes
  `GICR_IGROUPR0` and `IPRIORITYR` of a core that then powers off. Gate it with
  `!BOARD_BLUEJAY`.
- **L6.** The comment at `shim.c:369` is wrong: slot 9 is "lower EL IRQ", not "current EL".
  Drop slot 9 once C2 guarantees no EL2 routing.
- **L7.** Boot image. The v4 header (`header_size` 1584, kernel 0x33000 = `_end`, 4096
  page) and the Image header (text_offset 0, image_size `_end`, flags 0xa = LE, 4K,
  anywhere, magic at 0x38) are correct per booting.rst and mkbootimg. Unverified: whether
  bluejay ABL accepts an **uncompressed** `Image`, since GKI ships `Image.lz4`. Keep an lz4
  variant ready for the case where both images are rejected. On v4, ABL prepends the stock
  vendor_boot ramdisk, which justifies the `SWEETXNU` scan in `loader.c`. The scan is
  byte-wise over Device memory and slow on a large ramdisk, but correct.
- **L8.** DECON: writing `0x3061` to `0x1C300030` matches 05 §3b. Reading it back before the
  write (`before=`) gives the ABL state that §3b asks to record.

## Verified correct (no action)

- The x0 DTB pointer survives the EL2 `eret` (x19).
- Primary SP_EL2/SP_EL1 setup; `.bss` is zeroed before `probe_entry_el` is stored.
- Secondary stacks (`_sec_stacks + (idx+1)·4K`) do not overlap the primary stack.
- No ABS64 relocations.
- TCR IPS = 36 bits ≤ PARange on A55/A76/X1.
- The 64 GiB L2 table covers PA 0x8_8000_0000. This matters only for sweet's MMU path,
  because bluejay never enables the MMU.
- WTCON bits; UART registers; PMU SMC; MPIDRs `0x0..0x700` (DynamIQ, Aff1).
- The sweet `mkbootimg` arguments are identical to `main`, apart from switching to the vendored
  `mkbootimg.py`.

---

## Over-engineering worth deleting

- `tools/boards.py`: roughly 25 of 45 keys are never consumed: all `touch_*`,
  `cpu_compatibles`, `psci_conduit`, `reset_method`, `fb_format`, `gicd_size`, `ufs_size`,
  `volume_keys_bank`, and `dram_banks` except in mkadt. Move them to NOTES or the docs and
  drop the TypedDict.
- `fdt.c`: four copies of the same token walker. One walker with a per-property callback
  (plus correct cell handling, H3) is shorter and fixes H3/M4/L2 in one place.
- Three copies of the timer-WFI wait loop (`screen_room`, end of probe, `reset()`): use one
  `wait_ticks()` helper (H1).
- `con_uart_init()` is an empty function that holds a comment. Delete it.
- `extern char secondary_entry[]` is declared twice. `FB_PHYS` is just an alias of `FB_BASE`.
- `img_fdt_confirmed` on the sweet MMU path adds the M1 risk and has no bluejay user. Delete
  it, or keep it with the `return -1` fix.
- The `prepare_bluejay_cpu` retry path is unnecessary for cold boot per 05 §5. It is a
  reasonable fallback; keep it, but fix L4's reporting.

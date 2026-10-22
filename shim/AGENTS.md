# shim/

Bare-metal AArch64 code that runs under `fastboot boot` before XNU (MMU off, EL2->EL1, no libc, `-mgeneral-regs-only
-mstrict-align`). Every bug here can hang or reset the phone.

- Freestanding C only: no libc, no stdlib allocation, no static initialisers holding absolute pointers (build.sh rejects
  ABS64 relocations); build pointer tables at run time.
- Every wait is counter-bounded; the watchdogs are disabled, so a hang stays a hang.
- UFS is read-only: the only commands that may reach the doorbell are NOP OUT, TEST UNIT READY, READ CAPACITY(10) and
  READ(10) inside the `userdata` GPT range. `ufs.c` checks the real UPIU bytes at the doorbell; do not bypass it.
- MMIO writes go through the allowlisted helpers (`usbgadget.c` `w32_allowed`, `ufsinit.c` register set). Adding a
  register means citing the Linux/ABL source in the comment and extending the audit in `tools/test-bluejay-boot.py`.
- Test with host tests first (`*_host_test.c`, fake hardware), then `tools/check-all.sh`, then hardware.
- SoC-specific code lives under `soc/<vendor>/<soc>/shim/`; keep `shim/` generic and key code off `SOC_GS101`.
- `--touch-probe[=run[,mux,cs,osc|top,loop,rst,misopu][,railsr|rails]]` (`touchprobe.c`): phase 1 reads only (USI/SPI registers only if the sysreg mode is
  SPI); `run` writes only through `w32` / `w32_allowed` / `wmask` (per-bit masks: the XNU `sweettouchclk=3 sweettouchvar=1` registers plus the
  docs/research/21 experiments, each only with its build option). Adding a register or bit means extending `wmask`, `touch_host_test.c`'s
  independent expectation and the negative tests, and citing the source. Output text and options: `docs/AUTOMATION.md`.

- PMIC/ACPM/PMU: allowed since 2026-10-22 (docs/SAFETY.md). Values and addresses must come from cited Linux/ABL sources; every write goes
  through the audited allowlist; read before write; bounded waits. Fuses/OTP/PMIC trim and charger limits stay off limits.
- `railsr` / `rails` (`acpm.c`, docs/research/23; work without `run`): ask the ACPM firmware to READ (`railsr`) or also set bit 7 of (`rails`) the touch rail
  registers S2MPG10 0x5C (LDO25M) / 0x5D (LDO26M). Only `acpm.c` writes SRAM / mailbox, through its own `w32` + `w32_allowed(pa, v, guard)` (one request slot,
  the request queue front, the reply queue rear, mailbox INTGR1/INTCR0 with the channel bit; armed only after the descriptor validated, both queues are
  empty and the soc.json `acpm` gates are set). The gates: `layout_verified`, `elem_unit_verified`, per-rail `vsel_min/vsel_max` (null = unknown = refuse; `tools/boards.py` derives and checks them, and fails on null with a verified flag).
  The two flags ship at 0: the first run (`railsr`) prints the descriptor, the queue-buffer words and `ACPM unsure: ...` and writes nothing; flip a flag only after comparing the dump with
  doc 23 (+ its erratum: `q_elem_size` is BYTES, must be 16). The request is never rolled back once its front is written (`acpm_guard`: slot and front writes close at the front write). A refusal performs zero writes (`acpm_host_test.c`, `rails_host_test.c`). Same audit as `touchprobe.c` (source + object, mutation self-tests).

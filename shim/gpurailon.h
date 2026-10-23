#pragma once

/*
 * gpurailon.h - opt-in WRITER: switch the shader rail on for the power cycle, and switch it back (build.sh --gpu-rail-on, implies --gpu-rails, gs101 only).
 *
 * Run g1rails2 (doc 27) found S2MPG11 BUCK2S (S2S_VDD_G3D, the shader rail) OFF (CTRL 0x15 = 0x38, mode bits 7:6 = 0); the L2/tiler rail BUCK8S was on.
 *
 * gpu_rail_enable(): refuses with ZERO writes unless the ACPM gates, the descriptor, empty queues and the soc.json acpm.s2mpg11 + rail_on block are all fine and
 * BUCK2S_CTRL reads exactly mode 0 with OUT1 inside the pinned window; then ONE acpm_s11_rail_set() request (mode field -> on-normal, never the voltage),
 * a bounded soft-start wait, a read-back of CTRL, OUT1 and OUT2. Returns RAIL_ON_OK (rail verified on, mode 1 and OUT1 unchanged: the power cycle may run, the
 * rail MUST be restored), RAIL_REFUSED (no PMIC write was made: no power cycle, nothing to restore), RAIL_UNVERIFIED (the UPDATE was published but failed, or the
 * read-back failed) or RAIL_MISMATCH (read back, but mode != on or OUT1 moved): for both the power cycle is skipped and the rail MUST be restored.
 * gpu_rail_restore(state, power_ran): ALWAYS after the power cycle (also if it timed out): write the mode field back to off (one request), read it back, print
 * the line `GPU RAILON enable=<ok|refused|unverified> readback=<ok|mismatch|n/a> restore=<ok|FAILED|none> power=<ran|skipped|not-requested>`; says so loudly if the
 * restore failed. Nothing is done for RAIL_REFUSED. The enable value is mode 1 (0x40) like Linux (soc.json); the modes are in docs/research/27.
 * The only S2MPG11 write in the whole shim is acpm_s11_rail_set() (acpm.c, audited: reg 0x15, mask 0xC0, value 0xC0 or 0x00); this file has no MMIO of its own.
 */
enum { RAIL_ON_OK = 0, RAIL_REFUSED = 1, RAIL_UNVERIFIED = 2, RAIL_MISMATCH = 3 };
enum { RAIL_POWER_SKIPPED = 0, RAIL_POWER_RAN = 1, RAIL_POWER_NONE = 2 };	/* what the shim did with the power cycle, for the summary line */

int gpu_rail_enable(void);
void gpu_rail_restore(int state, int power);

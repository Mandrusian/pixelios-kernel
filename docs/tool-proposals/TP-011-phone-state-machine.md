---
id: TP-011
tool: "tools/phone"
status: done
proposed_by: "Gemini 3.8 Flash (delegate)"
date: 2026-10-22
exposed_by: "fragmented ad-hoc phone state checks and transitions across to-fastboot.sh, hang-recover, xrun, and fboot.sh"
extends: "tools/phone-state"
related: "TP-001, TP-006"
usage: "tools/phone goto TARGET [--timeout S] [--dry-run] [--yes-user-actions] | tools/phone state | tools/phone --self-test"
refs: "tools/phone, tools/test_phone_sm.py"
---

## checked
`tools/phone-state` (TP-001) only probes state; it does not execute transitions [read from source]. `tools/to-fastboot.sh` blindly polled `adb get-state` without product verification, safety allowlists, or self-reset handling [read from source]. `tools/hang-recover` (TP-006) had an ad-hoc 90s polling loop requiring manual Power+VolDown [read from source]. `tools/fboot.sh` and `tools/fflash-userdata.sh` are critical flash tools that must remain untouched [read from source]. Note: TP-001 and TP-006 are superseded in part by TP-011 [read from source]. Related: TP-001, TP-006.

## why
Before `tools/phone`, device transitions were fragmented across bash scripts with inconsistent timeouts, duplicated probing logic, and no centralized safety allowlist [observed]. A single state-machine layer unifies state observation (fastboot, recovery, android, booting, ios, shim-probe, hung, absent, other-device) [read from source] and drives bounded transitions with verified bluejay target checking, redaction of serial numbers, and strict refusal of non-bluejay devices with zero commands sent [observed]. Removes ad-hoc polling loops and avoids accidental 120s fastboot hangs by checking USB IDs before executing `fastboot getvar` [observed].

## output
At most 6 lines printed to console, with the final line strictly formatted as `RESULT: PASS|FAIL|PARTIAL|ERROR state=<final> target=<target> steps=<n>` [observed]. Exit codes: 0 (PASS), 1 (FAIL or PARTIAL), 2 (ERROR) [observed]. Detailed execution logs and timestamps written to `logs/phone/<timestamp>/phone.log` (gitignored) [observed].

## risks
Raw mutation or flashing commands could corrupt device partitions if executed against the wrong target device [guess]. Mitigated by a hard-coded `COMMAND_ALLOWLIST` in data asserting every adb/fastboot invocation, verifying product is `bluejay` before any command is sent, and immediate refusal on `other-device` with zero commands executed [observed]. All commands are bounded by subprocess timeouts and SIGKILL termination [read from source]. No sudo commands are executed automatically; staged host commands (e.g. USBNET) are printed as informational instructions only [observed].

## amendments
(none)

## Review and first hardware use (driver, 2026-10-22)
- Written by a delegate (Antigravity); reviewed by the driver, not trusted: read in full, 10 hermetic tests run, then used on the phone.
- **Observed on bluejay (2026-10-22):** `tools/to-fastboot.sh` (now a wrapper) took the phone from recovery to fastboot in 6.4 s, 1 step, `RESULT: PASS`; `tools/phone state` agreed with `tools/phone-state` (recovery, then fastboot).
- Fixed in review: (1) the "NEED USER: hold Power+VolDown" prompt was printed at once for wait-only steps (a phone that will reset by itself); it now appears only after the step's `bound_s` in that state (`PHONE_BOUND_SCALE` is a test hook); (2) `fastboot reboot` and the `fastboot -> android` transition were removed: no tool in the repo uses them (rule 7), they are "unknown transition: needs measuring".
- Not yet observed on hardware: the ios / shim-probe detection (ping 192.168.7.2, 1209:0001 + MAC), the wait-then-prompt paths.

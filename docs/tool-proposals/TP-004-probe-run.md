---
id: TP-004
tool: "tools/probe-run"
status: done
proposed_by: "Claude Sonnet 5.5 (driver)"
date: 2026-10-22
exposed_by: "three hand-run cycles of build + fboot + usb-log capture + grep for the touch probe"
extends: ""
related: "TP-006"
usage: "tools/probe-run --touch-probe=run,mux,... [--expect REGEX] [--timeout S]  (generic: --build-args \"...\")"
refs: "docs/AUTOMATION.md (tools/probe-run)"
---

## checked
tools/fboot.sh and tools/usb-log.py cover single steps, none chains build + boot + capture + verdict; docs/AUTOMATION.md has no probe runner; related: TP-006 (recovery after its HANG verdict)

## why
one command for build shim, check phone-state, start the USB capture, fboot, wait for the result line or a hang, write the log to logs/probe/, print one report; removes about 5 manual steps and unbounded waits

## output
`RESULT: PASS|FAIL|HANG ...`; HANG = the phone left USB before the expected line (tells the user to hold Power+VolDown); exit 0/1/2

## risks
fboot refuses stale or wrong-kind images; only bluejay; never raw `fastboot boot`

## amendments
(none)

---
id: TP-006
tool: "tools/hang-recover"
status: done
proposed_by: "Claude Sonnet 5.5 (driver)"
date: 2026-10-22
exposed_by: "the full touch probe hung the phone; recovery needs the user's Power+VolDown hold"
extends: ""
related: "TP-001, TP-004"
usage: "tools/hang-recover [--wait S]"
refs: "tools/hang-recover, docs/AUTOMATION.md (software part only; relay/hardware reset not built, still open for the user's decision)"
---

## checked
tools/to-fastboot.sh only handles the software-reset (recovery) case, not a hung phone; tools/phone-state (TP-001) is the probe it would use; related: TP-001, TP-004

## why
after a suspected hang, tell the user exactly what to press and block (bounded) until phone-state sees fastboot again, then print the result; no ad-hoc waiting loops. A hardware reset from the host (relay on the ESP32 or a USB power switch) would remove the user step entirely: needs the user's decision on hardware.

## output
`RESULT: PASS state=fastboot` or `FAIL still absent after Ns`, exit 0/1/2

## risks
none on the phone; any relay hardware needs approval

## amendments
- 2026-10-22 Antigravity: superseded in part by TP-011 (tools/phone). Hang recovery and transitions to fastboot now delegate to `tools/phone goto fastboot --timeout 45`.

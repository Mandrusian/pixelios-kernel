---
id: TP-001
tool: "tools/phone-state"
status: done
proposed_by: "Claude Sonnet 5.5 (driver)"
date: 2026-10-22
exposed_by: "a bare `fastboot getvar` hung 120 s against a phone in recovery"
extends: ""
related: ""
usage: "tools/phone-state [--expect android|recovery|fastboot|none] [--wait S]  (also tools/pix phone)"
refs: "3dcbb14, docs/AUTOMATION.md"
---

## checked
no existing state probe (to-fastboot.sh and fboot.sh each embed their own); related: none

## why
one bounded call instead of hand-run lsusb/adb/fastboot; removes a 120 s hang per misjudged state

## output
<= 4 lines, `RESULT: PASS|FAIL|ERROR state=... product=...`, exit 0/1/2, serials masked

## risks
queries only a bluejay; every probe under `timeout`

## amendments
- 2026-10-22 Antigravity: superseded in part by TP-011 (tools/phone). Device state observation and classification are unified under the tools/phone state machine layer, while tools/phone-state is retained as the internal probe backend.

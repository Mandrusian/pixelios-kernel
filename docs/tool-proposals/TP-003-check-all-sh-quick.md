---
id: TP-003
tool: "tools/check-all.sh --quick"
status: done
proposed_by: "Claude Sonnet 5.5 (driver)"
date: 2026-10-22
exposed_by: "the gate rebuilt everything for docs-only changes"
extends: "tools/check-all.sh"
related: ""
usage: "tools/check-all.sh [--quick]"
refs: "e45e02d, tools/gate_select.py"
---

## checked
extends tools/check-all.sh; no existing selective mode; related: none

## why
docs-only change 37 s -> 0.2 s; full gate 50 s -> 37 s (two lanes)

## output
one line `GATE PASS|FAIL <hash> [quick: ran N of M steps]`

## risks
`--quick` PASS is not the pre-push gate

## amendments
(none)

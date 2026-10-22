---
id: TP-002
tool: "tools/xrun --stop-on / --early / --hard-timeout"
status: done
proposed_by: "Claude Sonnet 5.5 (driver)"
date: 2026-10-22
exposed_by: "xrun waited 500+ s for the self-reset after every expected line was seen"
extends: "tools/xrun"
related: ""
usage: "tools/xrun ... --stop-on REGEX [--early] [--hard-timeout S]"
refs: "575b8b2, docs/AUTOMATION.md"
---

## checked
extends tools/xrun; no existing early-stop flag; related: none

## why
ends a run when the evidence is in (spi-clk2 replay: +337 s instead of 820 s) and reports the phone state on a timeout

## output
unchanged one report plus `EARLY:` / `PHONE:` lines

## risks
live recovery branches were replay-tested only

## amendments
(none)

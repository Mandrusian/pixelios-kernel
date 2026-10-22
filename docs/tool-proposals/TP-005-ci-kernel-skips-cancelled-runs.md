---
id: TP-005
tool: "tools/ci-kernel skips cancelled runs"
status: done
proposed_by: "Claude Sonnet 5.5 (driver)"
date: 2026-10-22
exposed_by: "`--dispatch` reported FAIL for the push run that GitHub cancels"
extends: "tools/ci-kernel"
related: ""
usage: "tools/ci-kernel [--dispatch] [--commit SHA] (unchanged flags)"
refs: "commit \"ci-kernel: skip cancelled runs\""
---

## checked
extends tools/ci-kernel; related: none

## why
removes a false FAIL on every dispatch

## output
unchanged

## risks
none

## amendments
(none)

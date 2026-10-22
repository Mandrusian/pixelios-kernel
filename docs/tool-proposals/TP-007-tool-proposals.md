---
id: TP-007
tool: "tools/tool-proposals"
status: done
proposed_by: "Claude Sonnet 5.5 (driver)"
date: 2026-10-22
exposed_by: "the register became one file per proposal with front matter; duplicate checks and numbering are manual"
extends: ""
related: ""
usage: "tools/tool-proposals list | check NAME_OR_KEYWORD | new NAME | validate"
refs: "tools/tool-proposals, tools/test_tool_proposals.py, docs/AUTOMATION.md"
---

## checked
tools/validate-repo.py checks the repo but not this register; no tool lists or checks proposals; related: none

## why
`check` greps tools/, tools/pix subcommands, docs/AUTOMATION.md and the front matter of every proposal (`tool`, `usage`) and prints
EXISTS / ALREADY-PROPOSED TP-NNN / NONE, so the dedupe rule is one command. `new` creates the next free TP-NNN file from the
template (same shape every time). `validate` (also run by tools/validate-repo.py, so by the gate) fails on missing fields, bad status
values, duplicate ids or a README index out of date. `list` prints id, tool, status, one per line (generates the README index).

## output
`list`: table. `check`: one line per hit then `RESULT: EXISTS|ALREADY-PROPOSED|NONE ...`. `new`: the path. `validate`: `RESULT: PASS|FAIL ...`. Exit 0/1/2.

## risks
Writes only under docs/tool-proposals/. No device access.

## amendments
(none)

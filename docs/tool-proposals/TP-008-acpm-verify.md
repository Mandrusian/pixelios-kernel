---
id: TP-008
tool: "tools/acpm-verify"
status: done
proposed_by: "Claude Sonnet 5.5 (delegate)"
date: 2026-10-22
exposed_by: "shim --touch-probe=railsr: the ACPM client refuses to write until a human has compared the printed channel descriptor with doc 23 and set layout_verified / elem_unit_verified (and the rail vsel range) in soc/google/gs101/soc.json"
extends: "tools/probe-run"
related: "TP-004"
usage: "tools/acpm-verify --from-dir DIR [--expect-ch 2]"
refs: "tools/acpm-verify, tools/test_acpm_verify.py"
---

## checked
`tools/tool-proposals check acpm` / `check descriptor`: nothing exists or is proposed. `tools/probe-run --dry-run --from-dir DIR` only greps expect lines; it does not
parse or judge the `SWEET TOUCH: ACPM ...` descriptor lines that shim/touchprobe.c `rails_describe` prints (cfg, init[0..7], table, desc[0..17], q, req/rsp pointers).

## why
After the first read-only hardware run the driver must decide by hand, from ~20 hex lines in usb.txt, whether doc 23's guessed layout (u32 pointers, 0x48 stride, field offsets,
q_elem_size in words) matches the real SRAM, and which of the soc.json keys may be flipped. ~5 minutes of careful reading per run and an easy place for a slip
that would let the shim write to a wrong SRAM address. A parser that re-derives every pointer/offset from the raw words and applies doc 23's sanity rules (ap_max 1..32,
id==2 at idx, offsets < 0x40000 and 4-aligned, buffers inside SRAM and disjoint, indices < q_len, elem bytes 16..64, queues empty, plus "do the next descriptors' ids look like
channel ids and the stride fit the table?") gives the verdict in one command and prints the exact soc.json lines to change (or why not).

## output
`RESULT: PASS layout consistent: set acpm.layout_verified=1 (elem_unit candidates: 4)` / `FAIL <check>: <why>` / `ERROR no ACPM lines in DIR/usb.txt`; exit 0/1/2; no log dir needed
(reads DIR/usb.txt only). It must NOT edit soc.json: the human sets the flags (the safety gate is the human comparison, the tool is the checklist).

## risks
none (offline, reads a captured log). A false PASS is the risk: it only checks internal consistency of the dump, not that the layout equals Google's firmware, so the report must say that.

## amendments
- 2026-10-22 Claude Sonnet 5.5: the review (docs/reviews/acpm-rails-review-1.md) and the fixes change what the checker must judge. (1) `q_elem_size` is BYTES: the checker must require
  `desc[14] == 16` (not "4..64 words"; the old "elem_unit candidates: 4" output is wrong, the candidate is `elem_unit=1`) and say so. (2) The describe step now also prints the first 48 words of each queue buffer
  (`ACPM req|rsp buf+OFF w w w w`, one 16-byte slot per line): the checker must confirm that the request buffer's non-empty slots look like commands at a 16-byte stride (seq in bits 21:16 of word 0, 0x01 in bits 11:8)
  and print `set acpm.elem_unit_verified=1` only then; it also needs `desc[3] == 1` (TYPE_QUEUE) and `idx == ch_id`. (3) A refused describe now prints the raw descriptor and the reason, so the checker must also explain a FAIL
  from those lines (`ACPM describe refused: ...`, `ACPM ids[..]=...`). Still offline and report-only: the human flips the flags.
- 2026-10-23 Claude Sonnet 5.5 (delegate): built as tools/acpm-verify (`--from-dir DIR [--expect-ch 2] [--sram-size 0x40000]`, ERROR if no `ACPM init[` lines). Rules re-derived from shim/acpm.c acpm_describe [read from source]: ap_max=init[6] in 1..32, table fits SRAM at the stride, desc id/index == channel, type 1, q_len 3..256, q_elem_size == 16 BYTES, 4 pointer words aligned inside SRAM, 2 buffers (q_len*16 B) inside SRAM, no overlaps, printed bases/table/q lines agree with the raw words, indices < q_len and queues empty; `elem_unit_verified=1` is suggested only if the request buffer shows non-empty slots at a 16 B stride with bits 11:8 of word 0 == 1. Report says it checks internal consistency only; never edits soc.json. Self-test tools/test_acpm_verify.py uses a SYNTHETIC fixture (not observed). Ran on three real captures in the main checkout (logs/probe, read-only) [observed on this dev machine 2026-10-23, not new hardware runs]: 20261022T145330Z-acpm3_ and 20261022T151219Z-acpm4_ PASS (ch 2, q_len 5, 5 request slots at 16 B); 20261022T172739Z-dvfsr1_ is the DVFS channel (id 1) and FAILs with the default `--expect-ch 2`, as it should. logs/runs/*/usb.txt has no ACPM lines.

---
id: TP-014
tool: "tools/lint-c"
status: done
proposed_by: "Claude Sonnet 5.5 (driver)"
date: 2026-10-23
exposed_by: "session 20: getting the W3a gate green on wip/w3a-wlan-link; the user asked whether C gets clippy/mutants-style checks"
extends: ""
related: ""
usage: "tools/lint-c [--files shim/a.c ...] [--board bluejay] [--strict]"
refs: "tools/lint-c, tools/test_lint_c.py"
---

## checked
`tools/tool-proposals check lint|cppcheck|mutation` (none), `grep -rni` over tools/ and docs/ (nothing calls cppcheck or clang-tidy), `tools/check-all.sh` and `shim/build.sh` (only `gcc -Wall -Wextra [-Werror]`, ~30 host tests, and the hand-written source/object audits in `tools/test-bluejay-boot.py`). Installed on the dev machine [observed 2026-10-23]: `/usr/bin/cppcheck`, `/usr/bin/clang-tidy` (19). `cargo-mutants` and `clippy` are also installed but are Rust-only. No C mutation tool (Mull, universalmutator, mutate_cpp) is installed. Related TP ids: none.

## why
Nothing in the gate runs a real static analyzer over the shim C, and the safety audits' own mutants are hand-written, so they only cover cases their author thought of. This session two audit pins carried a typo and one audit missed an extra `try_read32`; both were found only by running the unrun WIP code through the gate. A manual one-off on `wlanlink.c`/`wlansmc.c` took 2 commands (~10 s) and found no new issues [observed], so the check is cheap to run on every file and does not need a human. Part 2 (optional, separate decision): a mutation run over a few safety files (`wlanlink.c`, `wlansmc.c`, `w32_allowed`) to measure whether the host tests and audits kill the mutants. That needs a C mutation tool installed first (user decision).

## output
Per file: cppcheck (`--enable=warning,portability,style`, `-DSOC_GS101=1 -DWLAN_LINK_RUN`, `-Ishim -Ishim/out`, `-include shim/out/board_config.h`) and clang-tidy-19 (`clang-analyzer-*,bugprone-*`, same flags as `shim/build.sh`, `-ffreestanding`; the GCC-only `noipa` attribute warning is filtered). At most 6 console lines, ending with `RESULT: PASS|FAIL|PARTIAL|ERROR files=<n> findings=<n>`. Exit 0 = no findings, 1 = findings (`--strict` also fails on style), 2 = tool missing or board config absent. Full findings in `shim/out/lint-c/`. Intended to become one gate step in `tools/check-all.sh` after it runs clean on the whole tree.

## risks
None: read-only on source, no device touched, no adb/fastboot. Bounded runtime (timeout per file). Must not edit sources or loosen any audit in `tools/test-bluejay-boot.py`. Risk of noisy findings on old files: start in report mode (non-failing) for files outside the diff.

## amendments
- 2026-10-23 Claude Sonnet 5.5 (delegate): built as tools/lint-c (`--files`, `--strict`; no `--board`: shim/out/board_config.h must exist, exit 2 otherwise, `-DSOC_GS101=1` fixed). Self-test tools/test_lint_c.py (fake analyzers; read from source, not run on hardware). First runs [observed on this dev machine, 2026-10-23]: whole tree (27 files, `*host_test.c` skipped) 37 s, 51 findings (clang-tidy bugprone-implicit-widening-of-multiplication-result 28, bugprone-reserved-identifier 9, switch-missing-default-case 5, deadcode.DeadStores 4, assignment-in-if-condition 2, narrowing 1, branch-clone 1; cppcheck syntaxError on shim.c:116 `register ... __asm__("x0")`), 45 cppcheck style notes. shim/wlansmc.c: 1 finding (wlan_iso_run `rest` initializer never read, harmless). wlanlink.c (from wip/w3a-wlan-link): 0. bugprone-easily-swappable-parameters is disabled (noise). Not added to check-all.sh (tree is not clean yet).

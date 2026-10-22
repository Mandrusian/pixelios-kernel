# usbprobe review-1 fixes - 2026-10-20 (uncommitted report)

- H1 FIXED 741cd4b: object audit of shim/out/usbprobe.o (nm -u surface, .data/.bss=0, smc/hvc/msr/dc/ic/at/tlbi + LSE-atomic bans, CFG dataflow proving every store base is sp/x0-put_*-derived; fails closed). Source scan kept, now on usbprobe.c AND usbprobe.h with asm/mem*/__atomic/__sync token bans; volatile read-only declaration false positive fixed. Proof (bad object rejected): usbprobe_bad_store compiled from the review's non-volatile cast store *(uint32_t *)(0x11110000+0xC704)=0 -> "store 'str wzr, [x0]' base x0 is not provably stack-based". Self-tests run on every invocation: 20/20 bad objects (incl. the required non-stack store and smc) and 16/16 bad source spellings; real object: 113 stores, all stack-based.
- L1 FIXED 0c7bd13: every USB label is now "USB" (<= 8 screen columns); the register name leads the value. ramlog/pstore/UART lines byte-identical, pinned by unchanged exact-line host-test assertions.
- L2 FIXED 4f5d973: read-only pstore_zone_size() getter next to pstore_zone_addr(); "EVBUF OVERLAPS ramoops zone" / "EVBUF clear of ramoops zone" / "EVBUF ramoops zone off; no flag" second range flag; host test covers overlap + clear; object-audit allowlist deliberately extended by the two accessors.
- L3 FIXED 6a7fbc0: host tests for EVBUF addr 0, 64-bit range wrap, reserved table truncated (+1 dropped), partial overlap (covers vs overlaps: "not inside", no "in reserved"), plus a per-address read counter asserting no register is read twice within one usbprobe_report().
- SKIPPED: none.

GATE PASS 6a7fbc0

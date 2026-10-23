---
id: TP-013
tool: "tools/mg-key NAME... [--dsc CACHE]"
status: done
proposed_by: "Sonnet 5.5 (delegate, About-page identity reverse-engineering)"
date: 2026-10-22
exposed_by: "finding where MobileGestalt gets SerialNumber/ModelNumber/RegionInfo took ~25 manual ipsw/capstone/python steps"
extends: ""
related: ""
usage: "tools/mg-key SerialNumber ModelNumber RegionInfo ArtworkTraits   (or an obfuscated 22-char key)"
refs: "tools/mg-key, tools/test_mg_key.py, scratch/reports/about-identity.md (the manual procedure)"
---

## checked
`ls tools/`, `grep -il "gestalt\|dyld\|dsc" docs/tool-proposals/*`: no MobileGestalt tool and no proposal. `tools/rea` is a generic decompiler container, it does not know the MG key table.

## why
MG question -> value source is asked again and again (About rows, Home button, screen class, activation). By hand: obfuscate (base64(md5("MGCopyAnswer"+name))[:22]), find the perfect-hash entry (0xa0-byte entries at 0x1d73007e0, index from the 3 hash tables at 0x19c5ad500), read the compute/bool/access function pointers, disassemble, spot the IORegistry path and property. 25+ steps each time.

## output
One report per key: obfuscated key, table entry address, compute function vaddr, first-level source (IORegistry path + property name, syscfg key, sysctl, DT /chosen or /product prop), precache index and flags (cache can override), entitlement-protected yes/no. Ends with `RESULT: PASS|FAIL ...` (exit 0/1/2). Offline, reads the cache with `ipsw dyld` + capstone, no phone.

## risks
Table layout is read from iOS 16.7 (build 20H392); another build needs the hash-table addresses re-derived (tool should find them from `_MGCopyAnswer` code, not hard-code). Compute functions that call helpers (syscfg, EDT) need a manual second look.

## amendments

- 2026-10-23 Claude Sonnet 5.5 (delegate): built as tools/mg-key (`NAME... [--dsc CACHE] [--dylib EXTRACTED] [--out DIR]`; extracts libMobileGestalt with `ipsw dyld extract --slide`, needs python3 capstone). The perfect-hash tables, bound (0x82e), entry stride (0xa0) and entry table are NOT hard-coded: the tool finds the name-lookup function (the `cmp w1,#0x16` block of _MGCopyAnswer's helper) and symbolically executes its straight-line code [read from source: decompile of the iOS 16.7 cache at ~/pixelios-artifacts/work/dsc/cache]. Entry field offsets (+0 key, +8 compute fn, +0x18 protect hook, +0x9a precache index) are taken from the about-identity report and cross-checked at run time (the key string at the entry must equal the key, else FAIL). Run on the local cache (build 20H392) [read from source, tool output 2026-10-23]: reproduced the report's values for SerialNumber (entry 0x1d731d3e0, fn 0x19c56de30, IOPlatformSerialNumber, bool 8DHlx..., fourcc SrNm, precache 0x1f3, protected), ModelNumber (0x1d7329dc0, 0x19c5728d0, IODeviceTree:/ model-number, precache never), RegionInfo (0x1d732e960, 0x19c573ea8, IOPlatformExpertDevice region-info, 0x1db). Limits: first-level source only (strings/fourccs/helper calls of the compute function, linear scan, w2 shown only when a small immediate); helpers are not followed; the entitlement list check and cache-override behaviour are not decoded; other builds untested (the tool errors with exit 2 if the lookup block is not found). Self-test tools/test_mg_key.py uses a SYNTHETIC hand-encoded image (no Apple bytes), not run against any other cache. No cache or dylib is committed: the extracted dylib lands in logs/mg-key/ (gitignored).

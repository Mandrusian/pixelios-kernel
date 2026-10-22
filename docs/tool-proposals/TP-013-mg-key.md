---
id: TP-013
tool: "tools/mg-key NAME... [--dsc CACHE]"
status: proposed
proposed_by: "Sonnet 5.5 (delegate, About-page identity reverse-engineering)"
date: 2026-10-22
exposed_by: "finding where MobileGestalt gets SerialNumber/ModelNumber/RegionInfo took ~25 manual ipsw/capstone/python steps"
extends: ""
related: ""
usage: "tools/mg-key SerialNumber ModelNumber RegionInfo ArtworkTraits   (or an obfuscated 22-char key)"
refs: "scratch/reports/about-identity.md (the manual procedure), libMobileGestalt in the iOS 16.7 shared cache"
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
(none)

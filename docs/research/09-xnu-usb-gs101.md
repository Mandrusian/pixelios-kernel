# 09 - USB gadget (CDC-ECM + UDP console) for XNU on gs101

Goal: XNU console text over USB Ethernet on the Pixel 6a with no camera: the ios-sweet gadget (patches 0072-0077, 0081) gives `ue0`
192.168.7.2/24 and streams the console ring as UDP to 192.168.7.1:5140 (`tools/sweet-usbnet.sh` on the PC). On gs101 it cannot work as
is. This note says why, what to change, and what stays unknown. Implementation: `xnu/patches/0082-sweet-usb-gs101.patch` (+ ADT data).

Tags (AGENTS.md rule 5): **[src]** read from source (file:line), **[obs]** observed on hardware (doc/log + date), **[guess]**.
Line numbers for `sweet_usb.c` are in the patched 0082 tree (`~/pixelios-artifacts/bt-0082/tree/xnu/bsd/dev/sweet_usb.c`, the file after
0072-0077 + 0081); the patch that introduced a block is in parentheses. 0082 was named 0084 when this note was written; kernel CI compiled it (docs/HANDOFF.md: run 39665435241 green, *observed in CI*), it has never run on a phone (see "Verification" and "Status").

## Summary / recommendation

- Everything the gadget does after the DWC3 is reached (soft reset, event buffer, ep0 state machine, TRBs, ECM descriptors, `ue0`, UDP log) is
  generic. The shim already ran the same DWC3 sequence on this phone and enumerated (HS, CDC-ACM, bulk IN transfers of 1-4 MiB) [obs].
- What is Qualcomm: the GCC clock/GDSC gating, the DWC3 and QUSB2 addresses, the QSCRATCH VBUS override, and the DMA-coherence assumption.
  gs101 has none of the first four; the shim proves no VBUS/PHY/GCTL write is needed [obs].
- DMA is the one real unknown. **Recommended: option 1** (DMA page = the page ABL left the DWC3 event buffer in, inside the `abl*` reserved
  range, mapped normal non-cacheable): it is the exact memory the shim's working gadget uses [obs]. **Option 2** (`kmem_alloc_contig`, XNU
  bank `0x88...`) is built in behind boot-arg `sweetusbdma=1` purely as the decisive experiment; only UFS DMA into that bank is proven [obs].
- Another gs101-only trap, independent of DMA: `sweet_usb_probe()` is called at the very end of `sweet_ufs_init()` (sweet_ufs.c:691, 0072),
  i.e. only after the UFS root device was found. A UFS failure (the case where a log is needed most) never reaches USB. Fix belongs in
  `sweet_ufs.c` (move the call before the first `return error`), which was out of scope here: done as **patch 0091** (see Status).

## (a) Qualcomm-specific lines in 0072-0081 and their gs101 treatment

| # | `sweet_usb.c` lines (patch) | What is Qualcomm / SoC-specific | gs101 replacement or removal |
|---|---|---|---|
| 1 | 2-12 header (0072) | Describes dwc-usb3-msm, QUSB2, GDSC/GCC | Comment only; new gs101 block documents its own path (0082). |
| 2 | 45-61 `GCC_PA`, `GCC_SIZE`, `GCC_USB30_PRIM_GDSCR`, `usb_cbcr[]` (0072) | GCC clock/GDSC registers for USB30_PRIM (offsets from gcc-sdmmagpie) | **Removed on gs101**: never mapped. gs101 clocks are HSI0 CMU (docs/02 section 9, [src] soc/google/gs101/docs/02-gs101-peripherals.md:277-278); ABL leaves them on [obs, probe-log-2026-10-21-run1.txt:316-327 reads the DWC3 fine]. Replaced by read-only identity checks (GSNPSID, GCTL.PRTCAP) before any write. |
| 3 | 63-66 `DWC3_PA` 0x0a600000, `DWC3_SIZE` 0xd000, `QUSB2_PA` 0x088e2000 (0072) | Qualcomm bases | DWC3 base/size from the ADT `/sweet-usb` `reg` = 0x11110000/0x10000 [src: shim/usbprobe.h:21, soc doc line 276]. QUSB2 not used. |
| 4 | 88-105 GCC map + `clocks_on` gate (0072) | Power/clock gate guards an access that would hang | Not applicable; see row 2. Risk recorded in (f): a gated block would hang the read. |
| 5 | 106-110 `ml_io_map(DWC3_PA...)` (0072) | base | `ml_io_map(reg[0], reg[1])` from the ADT. |
| 6 | 111-118 DWC3 state dump (0072) | none (offsets 0xc1xx/0xc4xx/0xc7xx are generic DWC3; GSNPSID at 0xc120 [src: shim/usbprobe.h:23]) | Kept, factored into `usb_dump_dwc3()` so both paths print the same lines. |
| 7 | 122-134 ahb2phy gate + QUSB2 register dump (0072) | Qualcomm PHY | Removed on gs101. gs101 PHY is `google,gs101-usb31drd-phy` at 0x11100000 [src: docs/02:279-281]; the shim's read-only PHY dump (probe-log:332-336) is the model if one is wanted later (TODO(verify), not needed to enumerate). |
| 8 | 143-145, 254-258 `ud_sync` = `flush_dcache64(pa)` (0073) | Comment: "not cache coherent (DT qcom,gsi-disable-io-coherency)"; maintenance by *physical* address through `phystokv` | gs101 DWC3 coherence is unknown [guess: non-coherent, the DTS excerpt in docs/02:274-283 has no `dma-coherent` for USB, unlike UFS at docs/02:174]. Option 1: pool outside the kernel physmap (0xF8...), so `phystokv` is meaningless: `ud_sync` is a no-op for the NC mapping (0082). Option 2: unchanged (cacheable RAM + `flush_dcache64`). |
| 9 | 164-175 QSCRATCH defs (0074) and 905-915 VBUS override writes (0074) | `QSCRATCH_PA` 0x0a6f8800, HS/SS_PHY_CTRL session-valid forcing | **Removed on gs101** (`qs = 0` when gs101). gs101 has no wrapper; the shim enumerates with no GCTL/PHY/wrapper write [obs, HANDOFF:51; usbgadget.c:4-8,171-182]. |
| 10 | 856-866 `kmem_alloc_contig` DMA area (0073) | Placement assumption only (not Qualcomm) | gs101 default: ABL pool page (option 1); `sweetusbdma=1`: this code unchanged (option 2). See (c). |
| 11 | 870-879 `DCTL = CSFTRST` + wait (0073) | none; same as shim [src: usbgadget.c:632-646] | Kept; add the shim's 50 ms wait after the clear on gs101 [src: usbgadget.c:648-652, Linux core.c comment cited there]. |
| 12 | 276-289 `ud_epcmd` 1000 x 1 us (0073) | timeout chosen on Qualcomm | gs101: 10 ms like the shim [src: usbgadget.c:126]. Sweet keeps 1 ms. |
| 13 | 917 `DCTL \|= RUN_STOP` (0073) | none | gs101: mask ULSTCHNGREQ (bits 8:5) before the write, as the shim does after a review finding [src: usbgadget.c:47,556-565,690-691]. |
| 14 | 396-417 `ud_configure`: bulk OUT (phys 2), bulk IN (phys 3, FIFO 1), interrupt IN (phys 5, FIFO 2) (0075) | none; generic DWC3, **but only the bulk IN/FIFO 1 shape is proven on gs101** [obs: udump, HANDOFF:96,273; usbgadget.c:332-341]. Bulk OUT and the interrupt endpoint were never enabled by the shim (HANDOFF:57 "L8"). | Unchanged; flagged UNVERIFIED in (b)/(f). |
| 15 | 83-84 `sweetusb` boot-arg (0072/0073) | none | Semantics unchanged: 0 off, 1 (default) report only, 2 start the gadget. gs101 adds `sweetusbdma` (0 default = option 1, 1 = option 2). |
| 16 | sweet_ufs.c `sweet_log_copy/position` (0077) and 814-849 `ud_log_thread` (0077); 584-590 `ud_link_event` (0081) | none (ring is fed from `serial_console.c:285` for every console byte, from early boot [src]) | Unchanged. The thread streams the last 64 KiB first (line 821), so early boot lines are included once the link works. |
| 17 | sweet_ufs.c:691 call site (0072) | none, but runs only after a successful UFS root probe | Moved to the top of `sweet_ufs_init` by patch 0091 (once-only guard), so a UFS failure still starts USB. |

No Qualcomm-specific line remains reachable when `/sweet-usb` says gs101; the sweet path is behaviourally unchanged (see "Sweet regression surface").

## (b) gs101 registers: proven by the shim vs unverified

Controller: DWC_usb31, GSNPSID `0x33313130` (ip `0x3331`), base 0x11110000, global block at +0xC100 [obs: probe-log-2026-10-21-run1.txt:317; src: shim/usbprobe.h:10-23].
State ABL leaves (read-only probe, run 1, 2026-10-21) [obs, probe-log:317-326]: GCTL `0x00012004` (PRTCAP = 2, device), DCFG `0x00200885`,
DCTL `0x00F00000` (RUN_STOP = 0, CSFTRST = 0), DSTS `0x00D22734` (speed 4 = SS, halted), DALEPENA `0x3`, GEVNTADR `0xF8D12000` (differs per boot:
`0xF8B92000` in another run, HANDOFF:50-51), GEVNTSIZ `0x200` (512 B, interrupt unmasked), GEVNTCOUNT 0. ABL's own ep0 TRB addresses are in the same
pool (`F8D13000`, `F8D13020`, probe-log:329-331).

Proven end to end by the working shim gadget (`shim/usbgadget.c`, enumerates at HS: events `reset`, `conndone speed=0`; CDC-ACM; bulk IN dumps)
[obs, HANDOFF:51, 96, 273; udump-1mib-2026-10-21.txt]:

| Register / action | shim citation | 0082 uses it? |
|---|---|---|
| GCTL read only (PRTCAP check, device = 2); never written | usbgadget.c:623-630 | read-only, same refusal |
| DCTL.CSFTRST pulse, wait <= 500 ms, +50 ms | usbgadget.c:632-652 | yes (50 ms added on gs101) |
| DCFG: speed bits and DEVADDR cleared (forces HS) | usbgadget.c:654-659 | already in sweet code (888-889) |
| GEVNTADR LO/HI, GEVNTSIZ (INT mask \| 4096), GEVNTCOUNT drop stale | usbgadget.c:660-667 | already in sweet code (881-884) |
| DEVTEN 0x7 (disc, reset, conndone) | usbgadget.c:668-671 | already (891) |
| DEPCMD: DEPSTARTCFG, SETEPCONFIG, SETTRANSFRESOURCE, STARTTRANSFER on phys ep 0/1; ep0 TRB/ISP_IMI/IOC bits | usbgadget.c:201-312, 672-691 | already (894-903, 293-309) |
| DALEPENA 0x3 then RUN_STOP, wait for DSTS.DEVCTRLHLT clear | usbgadget.c:684-711 | already (902, 917-921) |
| Bulk IN phys 3: `DEPSTARTCFG\|(2<<16)` via ep0, `SETEPCONFIG type bulk, FIFO 1, MPS 512, ep#<<25`, resource 1, DALEPENA bit 3 | usbgadget.c:325-342 | same words in `ud_configure` for EP_TX (403-406) |
| Event decode (type, ep, status), ack of GEVNTCOUNT | usbgadget.c:544-735 | same as sweet |

**Unverified on gs101** (each is a hardware experiment, none is invented hardware data; they come from the sweet code that ran on a different DWC3):
1. Bulk OUT phys ep 2 config (`SETEPCONFIG` param1 `(1<<8)|(2<<25)`), needed for ARP replies and any RX (0075 `ud_configure`:403-404). The log needs ARP resolution of 192.168.7.1.
2. Interrupt IN phys ep 5, FIFO 2, bInterval 8 (407-409): needs a non-zero TX FIFO 2. GTXFIFOSIZ is not read anywhere in the repo; offsets not cited => TODO(verify): read GTXFIFOSIZ(0..3) and GHWPARAMS3 on the first run (Linux `core.h` offsets, not available offline here).
3. DMA into the 0x88... bank from this controller (option 2).
4. ENDTRANSFER on bus reset/disconnect (shim review M2, `usbgadget.c:251-259`): 0075 has none, so a host re-plug after configuration may hit "No Resource". Sweet shows the same weakness; first-enumeration tests do not need it.
5. Link-state events, remote wakeup, SS speed: not used (HS forced).
6. Timing of EP commands on gs101 beyond the shim's 10 ms bound.

## (c) DMA memory

Constraint [src: soc docs/02:349-352]: S2MPU (`s2mpu_hsi2`) can fault DMA into protected carve-outs; [guess] the same applies to a bus master on HSI0. Evidence for the
USB master specifically: DMA into the shim's low-DRAM `.bss` timed out for UFS (HANDOFF:47-49, 201), DMA into the ABL reserved page works for USB [obs, HANDOFF:49-51],
and UFS DMA into `0x880100000` works [obs, HANDOFF:242-244]. USB master into `0x88...`: **unknown**.

| | Option 1: ABL pool page (0082 default) | Option 2: `kmem_alloc_contig` (0082 `sweetusbdma=1`) |
|---|---|---|
| Memory | 4 KiB page of GEVNTADR + 16 KiB (`UD_SIZE`), inside the ADT `dma-pool-range` (= FDT `abl@f8800000`, 16 MiB, no-map [obs: probe-log:166-169]) | kernel RAM, 0x880000000+ |
| Proven for USB master | yes: same physical range the shim gadget uses [obs] | no |
| Mapping | `ml_io_map_wcomb` = normal non-cacheable [src: osfmk/arm64/machine_routines.c:1583, proc_reg.h:1107, pmap.c:6104-6115 honours the caller's WIMG for non-managed pages]; one `FlushPoC_DcacheRegion` first to drop lines ABL left there; zero the area; no later maintenance | cacheable, `flush_dcache64` as on sweet |
| Needs from the ADT | `dma-pool-range` (static) + `GEVNTADR` read from the live controller before any write | nothing |
| Risks | range moves with a new bootloader (re-measure); ABL's UFS list `F8C40000` sits in the same range (HANDOFF:47) - not overlapping the two observed GEVNTADR values but unchecked in code [guess]; no cache-coherency doubt | S2MPU may block the controller (symptom: `running` but no events, no setup packet) [guess]; USB 64-bit DMA addresses above 4 GiB: `GHWPARAMS0` is printed so the address width is recorded [guess] |

Checks the code does before using option 1 (mirrors `shim/usbpool.c`, fail closed): GEVNTADR != 0; the *kernel-page-rounded* span of the pool
(`io_map` maps 16 KiB pages, so the whole mapped span, not just the 16 KiB used, must lie inside the ADT range: otherwise XNU would map memory the
FDT does not call ABL's); GSNPSID ip 0x3331 and PRTCAP = device (shim M4 equivalent, usbgadget.c:623-630). The shim's `avoid` list (image, FDT, initrd,
pstore, framebuffer) does not apply: XNU's payload lives at 0x88..., the framebuffer is 0xFAC00000 and pstore 0xFD3FF000, all outside `abl@f8800000`
(0xF8800000-0xF9800000) [src: probe-log:166-169, board.json].

**Experiment that decides it** (one boot each, `fastboot boot` of the same payload, only the kernel cmdline differs):
1. `sweetusb=2` (option 1). Success = log lines `gs101: DMA from ABL's pool page`, `soft reset done`, `running`, `bus reset`, `connected, speed 0`, `setup 80 06 0100 ...`, `configured (1)`, `ECM endpoints configured`, and the PC sees VID:PID 1209:0001 and the MAC `02:00:5e:77:ee:01`.
2. Only if 1 works: `sweetusb=2 sweetusbdma=1` (option 2). Same lines but `DMA from kernel RAM`. If `running` appears but no `setup` event (GEVNTCOUNT stays 0, `SWEET USB: event` never printed), the controller cannot write the 0x88... bank: keep option 1. If it works, option 2 removes the dependency on the bootloader's carve-out and can become the default.
Recommendation: keep option 1 as default until option 2 passes with a bulk transfer, then decide whether the bootloader-independence is worth the switch.
(An alternative, cheaper to analyse but out of scope: a shim `USB_DMA_HI_BASE` build knob like `UFS_DMA_HI_BASE` (shim/ufs.c:864) so the experiment needs no kernel CI round trip. The FDT lists `xhci_dma@97000000` (4 MiB, no-map, probe-log:250-253); its owner is unknown [guess: AoC USB offload]; not used.)

## (d) ADT and detection

How sweet_ufs.c does it [src: sweet_ufs.c:616-628 + 0082]: node `/sweet-ufs`, property `compatible` = `google,gs101-ufs`, `reg` = absolute physical base/size; `su.gs101` set from it.
Same scheme for USB, with a **separate node** because the USB driver can be enabled/disabled independently of `--no-ufs` and sweet has none:

```
/sweet-usb
    compatible      "google,gs101-dwusb3"       (the Linux compatible of the wrapper, docs/02:275)
    reg             <0x11110000 0x10000>        (DWC3 core, soc.json usb_base/usb_size)
    dma-pool-range  <0xf8800000 0x1000000>      (FDT abl@f8800000, board.json usb_dma_range)
```

- `tools/mkadt.py`: emits the node only if the merged board has `usb_base` and not `--qemu`; `compatible`/`reg` from `soc/google/gs101/soc.json`, `dma-pool-range` from `device/google/bluejay/board.json` (device level: it comes from that bootloader's FDT, so a new gs101 phone must re-measure it; `tools/pix check` now fails on a board with `usb_base` but no/misaligned range). sweet trees are byte-identical to before (checked, seed fixed: sweet, sweet `--no-ufs`, sweet `--qemu`, bluejay `--qemu`); bluejay gains 256 bytes.
- `GEVNTADR` is **not** in the ADT (it moves per boot); the kernel reads it from the live controller before the first write.
- `sweet_usb_probe_gs101()`: `SecureDTLookupEntry("/sweet-usb")`, then `compatible` must start with `google,gs101-dwusb3`; anything else returns false and the Qualcomm path runs untouched.
- Payload cmdline: the staged one has `sweetusb=0` (HANDOFF:335); use `sweetusb=2` (add `sweetusbdma=1` for experiment 2, `sweetusblog=0` to disable the UDP log). Default `sweetusb=1` on bluejay just prints the DWC3 state lines (read-only).
- Host side unchanged: `tools/sweet-usbnet.sh` (matches host MAC 02:00:5e:77:ee:01, puts 192.168.7.1/24 on it, listens on udp/5140) [src: tools/sweet-usbnet.sh:16-25].

## (e) VBUS and role

- ABL hands over with GCTL.PRTCAP = device and the controller halted [obs, probe-log:318-321]. 0082 refuses to write anything if PRTCAP is not device or GSNPSID is not 0x3331.
- No VBUS/session override: the shim sets RUN_STOP only and the host sees `reset` / `conndone speed=0` [obs, HANDOFF:51]. gs101 has no QSCRATCH [src: usbgadget.c:4-8]. How session-valid reaches the core (PHY/PMU, Type-C controller role) is not understood [guess: ABL's fastboot setup, still in place].
- No GCTL writes are needed (shim never writes GCTL; its allowlist refuses it, usbgadget.c:171-182) [obs]. GUSB2PHYCFG/GUSB3PIPECTL are left as ABL set them (`0x00102400`, `0x11021102`, probe-log:326-327).
- The shim's `--boot-xnu` path does not touch USB: `loader_boot()` runs right after the display/watchdog setup and, with `XNU_UFS`, `ufs_cold_init()` [src: shim.c:500-512]; `usbprobe_report`/`usb_gadget_start` come later and never run [obs, HANDOFF:229]. So XNU finds ABL's state. [guess] ABL's state still holds some seconds later: the shim only waited ~1 s; XNU reaches `sweet_usb_probe` after the UFS root probe (tens of seconds). If the Type-C controller drops the role in between, only the report-only run (`sweetusb=1`) shows it (GCTL/DSTS lines).

## (f) Risks and hardware test order

Order (each step observable on the existing QR log `sweetqr=` or on screen; the USB step itself is the log channel we are trying to get):
1. Host: `tools/check-all.sh`, `tools/pix check bluejay`, `python3 tools/mkadt.py A.adt --board bluejay --dump | grep -A5 sweet-usb`. Kernel CI of 0082 (the patch was only syntax-checked locally).
2. `sweetusb=1` (default): expect `SWEET USB: DWC3 GSNPSID 0x33313130 GCTL 0x00012004 ...` and `GEVNTADR 0x00000000f8......`. Nothing is written. Verifies the ADT node, the map and that the block answers after the XNU handoff.
3. `sweetusb=2`: option 1, USB cable to the PC with `tools/sweet-usbnet.sh` running before boot. Watch `DMA from ABL's pool page`, `soft reset`, `running`.
4. If it stalls at `running` with no events: check DSTS/GEVNTCOUNT lines, then try `sweetusbdma=1`, then revisit (b)-1/2.
5. After enumeration: does `ue0` come up, does ARP resolve (bulk OUT), do UDP lines arrive.
6. `sweetusbdma=1` experiment (above).

Risks:
- **UFS-gated start**: fixed by patch 0091 (call moved before the first early `return` of `sweet_ufs_init`, guarded to run once). Side effect [guess]: a DWC3 hang now also blocks the UFS root, where before it only came after it; `sweetusb=0` (default) skips all of it.
- Unverified endpoints (bulk OUT, interrupt IN/FIFO 2) can fail `ud_configure` entirely (`data endpoints setup failed`), so `configured` stays false and the log thread idles. A fallback (TX-only with a static ARP entry) is not implemented. [guess]
- No ENDTRANSFER on reset; host re-plug after configuration may wedge (b)-4.
- Option 1 depends on the bootloader's `abl` carve-out layout staying as measured (board.json note); the code refuses if GEVNTADR is outside it, so the failure mode is "no USB", not memory corruption.
- Shared hardware state: the shim UFS cold init and XNU's UFS driver run before this; no overlap of their buffers with the pool was checked beyond the two observed GEVNTADR values.
- The kernel runs on cores without PAC/SHA3/SVE: `ud_*` code is plain C, but if the vectoriser misbehaves (as seen with patch 0083) compile `sweet_usb.c` with `clang optimize off` (xnu/AGENTS.md).

## Sweet regression surface (patch 0082)

gs101 behaviour is gated on `ud.gs101`, set only when `/sweet-usb` has `compatible` `google,gs101-dwusb3`. The sweet ADT has no such node, so every added branch is not taken.
Not object-identical, behaviour-identical: new includes, `usb_dump_dwc3()` extracted from `sweet_usb_probe` (same printf text/offsets), `ud_sync` early return when `dma_nc` (false), `ud_epcmd` bound `ud.gs101 ? 10000 : 1000`,
DMA allocation moved under `else` (same calls), the QSCRATCH map under `ud.gs101 ? 0 :`, DCTL masking under `ud.gs101`, one extra failed ADT lookup at probe.

## Verification status

Run on this host: `git apply --check` of 0082 on a pristine copy of the 0082 tree (and after applying 0083); applying it reproduces the edited file byte for byte; host `clang -fsyntax-only` against stub headers reports no error in the new/changed regions;
`python3 tools/mkadt.py /tmp/x.adt --board bluejay` and `--board sweet` work; unit test `tools/test_mkadt_usb.py`; `tools/pix check bluejay|sweet`; `tools/check-all.sh` (see the commit message for the result).
**Not verified:** a kernel compile (no Apple clang/SDK/buildtree here: `tools/xnu-cc.py` needs a CI log and the swift clang), any link error, any run on a phone. Specifically unverified: the `ml_io_map_wcomb`/`FlushPoC_DcacheRegion` declarations in bsd context (declared locally/exported in `arm/caches_internal.h`), `%llx`/`uintptr_t` format warnings under the kernel's `-Werror`, the NC-mapped DMA behaviour, and everything in (b) "unverified".

## Status (2026-10-22; nothing here has run on a phone)

| Item | Tag | State |
|---|---|---|
| 0070-0079 (sweet gadget, ECM, UDP log) | [obs] on the Qualcomm phone (sweet, earlier ios-sweet work); [src] only for gs101 | compile-verified in CI for the gs101 tree; never run on bluejay |
| 0082 gs101 path (ADT `/sweet-usb`, ABL pool page DMA, no QSCRATCH, identity checks) | [src] | CI green; never run |
| Start USB before the UFS probe (this note's "proposed follow-up") | [src] | done: **patch 0091** (`0091-sweet-usb-before-ufs.patch`); applies on the 0090 stack (`tools/stack-apply`); not compiled here (no kernel toolchain), CI pending |
| Boot-args / image flags | [src] | `tools/build-xnu-image.sh --usbnet` (replaces `sweetusb=0` by `sweetusb=2`), `--usbdma` (adds `sweetusbdma=1`); `tools/xrun --usbnet [--usbdma]`; without the flag the boot-args are byte-identical |
| Host side | [src] | `tools/usbnet-log.py` (stdlib UDP listener, root-free), `tools/sweet-usbnet.sh --stage` prints the one sudo line; xrun merges `usbnet.log` with the QR log in the report; unit tests `tools/test_usbnet.py` |
| Bulk OUT (ARP replies) and interrupt IN on gs101 | [guess] | never enabled by the shim (see (b) "Unverified") |
| Not done | | ENDTRANSFER on reset/disconnect; GTXFIFOSIZ read (TODO(verify)); TX-only fallback with a static ARP entry; DWC3 clock/PHY re-init if ABL's state decays; de-duplication of the 64 KiB replay after a re-plug |

## First hardware run: checklist

Prerequisites: a kernel built from a stack that contains 0082 and 0091 (CI artifact), the bluejay attached and in fastboot (driver only), `--qr` can stay on: USB is an extra channel, not a replacement.

1. No separate read-only boot: with `--usbnet` the DWC3 identity dump and checks run before the first write, and the staged default stays `sweetusb=0`. (A read-only boot would be `sweetusb=1`; `build-xnu-image.sh` has no flag for it and a second `--args sweetusb=1` would sit behind the default `sweetusb=0`, whose precedence is not verified [guess].)
2. Boot with `tools/xrun --kernel K --usbnet --qr 120 --restart 400 --expect 'SWEET USB: running' --expect 'SWEET USB: configured' --expect 'SWEET USB: ue0 .* up' --label usbnet1`.
   Boot-args become `... sweetusb=2 rd=ufsroot0 ...` (+ `sweetusbdma=1` only for the DMA experiment: `--usbdma`, second boot, only after run 1 got events).
3. xrun prints a `USBNET:` line with the one sudo command; the user runs it as soon as the host sees a new network interface (`ip -o link` shows MAC 02:00:5e:77:ee:01, ~15-60 s after boot), or `tools/sweet-usbnet.sh` interactively. `tools/usbnet-log.py` is already listening on udp/5140 (xrun started it).
4. Expected lines, in order (kernel console, QR log and `usbnet.log`):
   `SWEET USB: DWC3 GSNPSID 0x33313130 GCTL 0x00012004 ...`, `SWEET USB: gs101: DMA from ABL's pool page`, `SWEET USB: soft reset done after N ms`, `SWEET USB: running (DSTS 0x... ), DMA at 0xf8......`,
   `SWEET USB: bus reset`, `SWEET USB: connected, speed 0`, `SWEET USB: setup 80 06 0100 ...`, `SWEET USB: address N`, `SWEET USB: configured (1)`, `SWEET USB: ECM endpoints configured`, `SWEET USB: ue0 192.168.7.2/24 up (err 0)`.
   Host: `lsusb` shows 1209:0001, `ip -o link` the MAC above; after the sudo line `ping 192.168.7.2` answers (ICMP, needs bulk OUT) and `usbnet.log` fills. The report's MARKERS show `USB running / configured / ue0 up` and `USB errors`.
5. Failure signatures (what to read, and what it points to; all [guess] until seen):

| Symptom | Means | Next |
|---|---|---|
| Hang before enumeration: log stops after `DMA from ...` or `soft reset`, or at `DWC3 GSNPSID` (no `running`); screen frozen | register access hangs (clock/power gate decayed since ABL) or the soft reset never completes | Power+VolDown hold. The last line in the QR log is the evidence (QR must be on); compare GCTL/DSTS with the probe-log values. The next boot without `--usbnet` is back to `sweetusb=0` |
| `running` printed, but no `bus reset` / `connected` ever, host sees no device | no VBUS/session, or role dropped, or the event buffer DMA is blocked | check `DSTS`; host `dmesg -w` (no `new device`) => session/PHY; `lsusb` sees 18d1 only => cable/port; try `--usbdma` only if events never arrive and the host does see a reset |
| Host enumerates (VID:PID 1209:0001, `configured (1)` printed) but no interface `02:00:5e:77:ee:01` | ECM class driver or descriptors; `data endpoints setup failed` => bulk OUT / interrupt EP config rejected (unverified endpoints, (b)-1/2) | read `ep<N> cmd ... status` lines; `dmesg` on the host (`cdc_ether`) |
| Interface exists, `ue0 ... up`, but host ARP/ping gets no reply | RX path (bulk OUT) not working, or 192.168.7.1 not configured on the host | run the staged sudo line; `ip neigh`; kernel `ue0 rx N tx M frames` line: rx 0 = bulk OUT dead (add static ARP/TX-only fallback) |
| ARP resolves, ping works, but `usbnet.log` stays empty | UDP log thread idle (`sweetusblog=0`?), host firewall drops udp/5140, or listener not running | `ss -ulnp \| grep 5140`, host firewall, kernel `ue0 rx/tx` counters |
| Everything fine, then silence after a host re-plug | no ENDTRANSFER on reset (b)-4 | reboot the phone; known limitation |

6. After a run: `tools/xrun --dry-run --from-dir logs/runs/<dir>` rebuilds the report; `usbnet.log` (`<epoch>\t<line>`) is raw data, `merged.log` the USB+QR merge.

## Safety notes

- A DWC3 hang (soft reset stuck, an event or register access that never completes) freezes the core; the phone then needs **Power+VolDown held until it resets**, then `tools/to-fastboot.sh`. xrun reports this as "needs Power+VolDown hold" only before the boot; after it the run simply never resets (`reset NOT observed`).
- Nothing in the path writes storage: no partition, no bootloader state. The only writes are DWC3 registers behind the identity checks (GSNPSID ip 0x3331, GCTL.PRTCAP = device) and the pool page inside ABL's `abl` carve-out (fail closed when GEVNTADR is outside the ADT range). `sweetusbdma=1` allocates kernel RAM only.
- A wrong DMA address can corrupt memory: option 1 refuses unless the whole mapped span is inside the ADT range; option 2 uses `kmem_alloc_contig` memory the kernel owns. S2MPU blocking shows as a silent hang of events, not as corruption.
- The host side needs root only for the interface address and the user runs it (`tools/sweet-usbnet.sh --stage` prints the line); the listener is unprivileged (port 5140).
- Default behaviour is off (`sweetusb=0` in the staged boot-args); `--usbnet` is opt-in.

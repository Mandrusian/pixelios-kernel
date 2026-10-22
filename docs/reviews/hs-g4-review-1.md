# Review: `power_mode_hs()` (commit 35d7c9a), HS-G4 power-mode change

Scope: `shim/ufsinit.c:201-269` and the call at `shim/ufsinit.c:348-355`, compared with
`soc/google/gs101/docs/07-gs101-ufs-bringup.md` step 22 a-g, Appendix A.2/A.3 (`shim/ufsphy_tables.h`) and
Appendix C.1/C.2. Reviewer: Claude Opus. No files other than this report were changed.

## Verdict

**I found nothing serious and nothing that could brick the device or corrupt storage.** Every register
offset, value, UIC opcode and the order of operations matches the spec. The only finding that matters
is Medium: on failure the function logs and `ufs_cold_init()` still returns success, so the system
keeps going over a link that may be dead. The other findings are Low or Info.

## Line-by-line conformance

| Spec | Code | Result |
|---|---|---|
| 22a PEER_GET `PA_MAXRXHSGEAR` (0x1587), op 0x03, arg1 = attr<<16 | `:209` `uic(0x03, 0x1587u << 16, 0, &peer)` | OK. The UIC result is checked and the value is read from UCMDARG3 (0x9C) |
| 22a local DME_GET 0x1587 | `:211` | OK. `PA_CONNECTEDRX/TXDATALANES` were already checked == 2 at step 15 (`:312`) |
| 22b min(dev, host G4); rx = local max, tx = peer max | `:211` `g_rx = min(rxg,4)`, `g_tx = min(peer,4)` | OK. Same mapping as `ufshcd_get_max_pwr_mode` |
| 22c A.2 table: TRSV 0x369=0x11, 0x246=0x03 (both lanes) | `:216` + `ufsphy_tables.h:76-79`, `phy_write` lane 1 at (off+0x200)<<2 | OK |
| 22c DL 0x2041=8064, 0x2042=28224, 0x2043=20160 | `:217-219` | OK |
| 22d 0x15B0/1/2 = 12000/32000/16000 | `:220-222` | OK |
| 22d UniPro 0x7888/0x788C/0x7890/0x78B8/0x78BC/0x78C0 | `:223-228`, UNIPRO = 0x14780000 | OK |
| 22e 0x1583=4, 0x1580=2, 0x1584=1, 0x1568=4, 0x1560=2, 0x1569=1, 0x156A=2 | `:229-235` | OK (gear = negotiated value, 4 on this device) |
| 22f / C.2 PA_PWRMODE 0x1571 = 0x11, then UCCS (via `uic()`), then IS bit4 UPMS, W1C, HCS[10:8] == 1 | `:238-249` | OK. Stale UPMS is cleared before issuing (`:238`); the 500 ms timeout matches Linux |
| 22g A.3 table: COMN 0x8=0x60, TRSV 0x222=0x08, TRSV 0x246=0x01 | `:250` + `ufsphy_tables.h:81-85` | OK |
| 22g CDR lock: 0x339 bit3, 100 tries x 40 us, on a miss write 0x222 = 0x10 then 0x18; lane stride 0x200 words | `:251-266` | OK. The byte address is `PMA + ((0x339 + l*0x200) << 2)`, the same convention as the RX-cal poll at `:193`, which already works on hardware |

Write scope: the function writes only to PHY PMA (0x14704000 + ...), UniPro (0x14780000 + 0x78xx),
HCI IS (W1C) and the UIC command registers (0x90-0x9C). Every UIC command is DME_GET, DME_SET or
DME_PEER_GET on local or peer attributes. There is no DME_PEER_SET, no doorbell, no UPIU, no query
and no SCSI. The mode change sends PACP_PWR_req to the device, which changes only the device's
volatile M-PHY/PA state. A power cycle or the RST_n pulse in `host_enable()` (`:105-107`) resets
that state, and ABL resets the device on every boot. Nothing here can persist into flash, change a
descriptor or attribute stored in the device, or touch a partition.

Data-integrity note: if the HS link were marginal, the UniPro DL layer's CRC and retransmission
would turn bad frames into timeouts or UIC/OCS errors, not silent data corruption. The read path in
`shim/ufs.c` zeroes the UCD before each command (`:151-152`) and checks OCS. It also has no write
direction, so even a broken link cannot corrupt storage from the shim.

## Findings (severity-ranked)

### M1 - A failed HS change is reported as success; the link is left as is with no fallback
`shim/ufsinit.c:350-355`. If stage 5 (no UPMS), 6 (UPMCRS != PWR_LOCAL) or 7 (CDR not locked)
fails, `ufs_cold_init()` still returns 0.
Failure scenario: the device answers PACP with PWR_ERROR_CAP or BUSY, or the PHY does not lock in
HS. The link is then left half-switched or with RX unlocked. `ufs.c:831` sees rc 0 and goes on to
TUR, READ CAPACITY and the dump/XNU boot. Each command times out after 1 s, then UTRLCLR, then an
error. XNU then sits in its root-mount loop against a dead link. The only sign is one
`HS-G4 change FAILED` line on the screen, which (per the handoff) nobody reads in practice.
Linux handles this case by resetting the host and restoring the link (`ufshcd_host_reset_and_restore`).
Fix: for stages >= 5, return an error, or re-run the cold init with HS disabled (PWM-G1 is known
to work) and log the fallback. Stages 1-4 leave the link in PWM-G1, and carrying on there is fine.
This does not risk data (reads fail loudly), but it hides the cause behind later timeouts.

### L1 - The CDR poll reads 0x339 immediately; Linux waits 40 us first
`shim/ufsinit.c:255`. Linux does `udelay(40)` and then reads 0x339 once per try. The shim's `poll()`
reads at t=0, right after `phy_write(post_pwr_hs)` has rewritten 0x222 = 0x08.
Failure scenario (a guess, not observed): if bit3 of 0x339 still holds the pre-write or PWM-era lock
state for a few microseconds, the shim takes it as "locked" and never runs the 0x222 override poke.
The RX CDR would then run without the recovery Linux applies, which shows up later as DL
retransmits or UIC errors under load. It is harmless on the device so far (4 MiB matched). Fix: put
`udelay(40)` before the first poll so the timing matches Linux exactly.

### L2 - `fails = 0` hides a failed local DME_GET of PA_MAXRXHSGEAR
`shim/ufsinit.c:211` then `:215`. If `dme_get(0x1587)` fails with a non-zero UIC result, `uic()`
still copies UCMDARG3 into `rxg` (`:63-64`). The counter is then reset at `:215`. A garbage non-zero
value is clamped to 4 and the change goes ahead on an unverified host capability. The failure is
logged by `dme_get`, so it is not silent, just not acted on. Fix: check `fails` before resetting it,
or return 1 when the get fails.

### L3 - No UIC/PA/DL error check after the change
`shim/ufsinit.c:242-249`. Only UPMS and UPMCRS are checked. IS.UE (bit 2) and UECPA/UECDL (0x38/0x3C)
are not read after the change. Linux's error handler would see DL/PA errors raised during or right
after the switch, for example a lane that trains but takes NAC storms. Fix: after the CDR wait, read
and log IS & 0x4, UECPA and UECDL. Treat a fatal DL error as a stage-8 failure.

### L4 - When stage 5 times out, UPMCRS is never read
`shim/ufsinit.c:242-243`. Appendix C.2 notes that Linux still checks UPMCRS after a timeout, because
the operation may have completed. The shim returns 5 without logging HCS, which loses the one fact
that tells whether the link switched. Fix: log `rd(HCI+0x30)` and `rd(HCI+0x20)` on the stage-5
path. This is diagnostic only.

### L5 - Stage-3 failure leaves the HS pre-change PHY values in place while still in PWM
`shim/ufsinit.c:216` vs `:236-237`. The A.2 writes (TRSV 0x246 = 0x03, 0x369 = 0x11) and the DL
timers are applied before the DME_SET batch is checked. If any of those sets fails, the link stays
in PWM-G1 with HS-oriented RX settings. Linux applies the same table before mode attributes and does
not undo it on failure either, so this matches Linux. Its effect on PWM is unknown, though the
caller (M1) then keeps using the link. It only matters together with M1.

### I1 - Gears 1-3 would skip `exynos_ufs_config_sync_pattern_mask`
`shim/ufsinit.c:211`. The min() logic correctly allows a gear below 4. Spec 22c, however, says the
sync-pattern mask is configured only for G1-G3, and the shim does not implement it. A device that
reports max gear 3 would get a G3 change without that step. This cannot happen on this device
(HN8T05BZGKX015 reports G4; the measured 12x speedup is consistent with HS). Either refuse gears
< 4 or leave a comment.

### I2 - CDR timeout semantics
`shim/ufsinit.c:262-265`. The shim returns 7 when a lane fails to lock. Linux's
`samsung_ufs_phy_calibrate` also returns -ETIMEDOUT, but I did not check (not in spec 07) whether
`exynos_ufs_post_pwr_mode` propagates that error. Because of M1, the shim's return value has no
effect on behavior either way.

### I3 - Lane count is hard-coded to 2
`shim/ufsinit.c:230,233,251`. This is acceptable because `ufs_cold_init()` refuses to continue unless
both connected-lane counts are 2 (`:312`).

## Not issues (checked)
- PEER_GET opcode 0x03 and the arg encoding (gen_sel 0, attr set type 0) match C.1.
- UPMS W1C before and after the change, and UCCS W1C inside `uic()`. UCCS and UPMS are separate bits,
  so clearing one cannot lose the other.
- `poll(..., 40)`: TIMER_FREQ is 24 MHz (`shim/out/board_config.h:9`), so 40 us is about 960 ticks.
  `poll()` reads at least once before checking the deadline, so a 40 us window cannot falsely time
  out without a read. The worst case is about 4 ms per lane.
- IE (0x470, set at `:111`) includes UPMS, but the shim does not configure the GIC, so no stray IRQ
  is possible.
- XNU patch 0082 issues no PA_PWRMODE or hibern8 commands, so it does not conflict with the link
  being left in HS.

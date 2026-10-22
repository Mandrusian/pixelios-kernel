# Review 1: ACPM DVFS client (commit 1abf2a0: `shim/acpm.c/.h`, `touchprobe.c`, `shim.c`, `build.sh`, `tools/boards.py`, `soc.json` dvfs, tests)

Reviewer: Claude Opus 5.5 (adversarial, different model from the author Sonnet 5.5), 2026-10-22. No hardware touched.
Tags: **[src]** read from source (file:line), **[sandbox]** run on the host in `/tmp/dvfsrev` / `/tmp/dvfsmut` (copies, repo untouched),
**[obs-hw]** from an existing hardware log, **[guess]** not established.

## VERDICT

- **FIT FOR A READ-ONLY `dvfsr` RUN**, with two conditions:
  (a) build it **without `--boot-xnu`** (with `--boot-xnu` the dvfs block only runs if `loader_boot` fails, so a dvfsr+boot-xnu image
  silently does nothing, L5), and (b) check the build log for the line `PASS dvfsr object has acpm_dvfs_get and no acpm_dvfs_set`
  yourself, because the check fails open (M1). Every write a `dvfsr` run can make is a FREQ_GET on channel 1 through the same guard
  that the `railsr` runs used **[src]** + **[sandbox]** (Q2 below).
- **NOT FIT FOR `dvfs` SET RUNS** until:
  1. H1 is fixed: the `RESULT` line must come from the read-back, not from the request return codes;
  2. H2 is fixed: FREQ_REQ needs a reply, like FREQ_GET (delete the grace-period success path);
  3. a `dvfsr` run has shown on hardware: the channel 1 descriptor validates, both queues are empty, the request and reply fronts are
     equal at idle (lockstep, see Q5), and FREQ_GET returns 1197000 / 1197000 / 1106000 (the fvmap init levels, doc 25 section 1);
  4. the first set run asks for **CL0 only** (`--dvfs 1803000,0,0`), and CL1/CL2 come in a later run after the CL0 read-back matched (Q6).

## Findings

### H (could harm the phone or lie about a result)

**H1. `dvfs set: ok` is printed whatever the read-back shows.** `shim/touchprobe.c:1423`
`say_result("dvfs set: %s", e ? "REFUSED-or-FAILED" : "ok")`: `e` comes only from the request loop (`:1399-1412`). The read-back loop
(`:1415-1422`) prints lines but never changes `e`. `tools/xrun --expect` and anyone who reads the RESULT line get "ok" for a rate that was never verified.
- Scenario A, firmware takes the request but keeps the rate (for example a cluster whose cores are off, a DM constraint, or a firmware-side
  limit) **[sandbox]**: fake APM with FREQ_REQ ignored. The output is `DVFS CL0 readback rate=1197000 kHz (asked 1803000)`, `DVFS CL2 readback rate=1106000 kHz (asked 1826000)`,
  then `TOUCH RESULT: dvfs set: ok`.
- Scenario B, late reply (H2) **[sandbox]**: the fake consumes FREQ_REQ at once and replies 150000 ticks (6.1 ms) later. Both requests print
  `err=0 reply=0`, all three read-backs print `err=5 ... refused: a queue is not empty`, and RESULT is still `dvfs set: ok`.
- The opposite direction is wrong too. If a FREQ_REQ reply has a non-zero word 3, the request reports `E_RET` and RESULT says REFUSED-or-FAILED even when
  the read-back shows the new rate (L2).
- Fix: compute the result from the read-back. Count the clusters with `khz != 0` where the read-back succeeded AND equals the request. Print
  `dvfs set: ok N/N` only when all of them match, `MISMATCH` when a read-back succeeded with another rate, and `UNVERIFIED` when a read-back failed. Add
  both scenarios to `dvfs_probe_host_test.c`. Today only `never_at` covers the failure side, and the readback-ignored mutation survives every test except that one.

**H2. "A reply to FREQ_REQ is optional" contradicts the Linux source. The grace path then turns a slow reply into success and leaves channel 1 dirty.**
`shim/acpm.c:366-368` (comment), `:443-462` (`XM_DREQ` branch), `acpm.h:155-158`, `docs/research/25-cpu-dvfs-gs101.md` section 3.
- **[src]** In `__acpm_ipc_send_data`, every request, `response=false` included, sets its sequence bit (`acpm_ipc.c:756 set_bit(channel->seq_num - 1, ...)`).
  The only place that clears it for a `response=false` request is `check_response` when a reply entry with that sequence number arrives (`acpm_ipc.c:446-448`).
  The next send loops `do { ++seq } while (test_bit(...))` (`:747-750`) with the tx spinlock held. If the firmware did not reply to CPU FREQ_REQ, Android
  would hard-lock after 63 CPU frequency changes.
- **[src]** The idle check at `:712` (`(tx_front + 2) % len != rx_front`, else spin, then panic) only makes sense if the reply front follows the request front.
- So the firmware replies to every FREQ_REQ (**[src]**, inference from two independent places; the alternative would hang every Pixel 6a).
  `response=false` only means Linux does not wait for the reply and drains it on the next send.
- Consequences of the current code:
  - The `rr2 == nf` + `lim/10` (5 ms at 24.576 MHz) branch reports `ACPM_OK, had_reply=0` ("none is coming", `:457`) for a reply that is merely slower
    than 5 ms. A DVFS change with a voltage ramp and PLL relock before the reply is plausible **[guess]**.
  - The late reply then sits in the reply queue. INTSR bit 1 stays pending. Every later ACPM call in that boot is refused (`ACPM_W_BUSY`, or
    `ACPM_W_FULL` with non-lockstep fronts, **[sandbox]** first run). The readbacks are refused, and with H1 the run still says ok.
  - If the late reply lands inside the next transaction's poll window, that transaction reads the wrong entry. The seq check catches it
    (`E_REPLY`/`W_SEQ`) after it has already drained the entry **[src]**, so there is no mis-attribution. It is still a failed transaction caused by our own guess.
  - The pending reply outlives the run into XNU (no ACPM driver there) and, after a warm reset, into ABL/Linux. Linux drains it in `check_response`
    (seq 1..63, never 0, so no `panic` at `:442`) **[src]**. Volatile, but it is exactly review-2 M4's residual risk, created on purpose.
- Fix (a deletion): remove the `#ifdef TOUCH_DVFS_WRITE` block at `:443-462` and the `tc/consumed` locals. Treat `XM_DREQ` like `XM_DGET`: wait for the reply within
  `timeout_us` (50 ms), drain it, check seq/count. Keep the timeout path as it is: request published, nothing rolled back. Update doc 25 section 3 and the
  `acpm.h` comment: "the firmware replies to FREQ_REQ [src acpm_ipc.c:446-448, :747-756]". The test mode `reply_to_req = 0` then becomes the timeout case.

### M

**M1. The new build audits fail open.** `shim/build.sh:267-274` under `set -euo pipefail` (`:2`).
- In `gcc ... && nm ... | grep -q acpm_dvfs_get && ! nm ... | grep -q "acpm_dvfs_set\|dvfs_rate_ok" && echo PASS ...`, only the final `echo` can stop the script.
  A failed `!` grep (the dvfsr object DOES contain `acpm_dvfs_set`) makes the whole list false, and bash does not exit for a non-final `&&` element **[sandbox]**:
  `bash -c 'set -euo pipefail; true && ! echo acpm_dvfs_set | grep -q acpm_dvfs_set && echo PASS; echo continued'` prints `continued`.
- The same applies to `gcc ... -o out/dvfs_host_test ... && ./out/dvfs_host_test` (`:267-270`). If the host test does not compile, it is silently not run.
- Nothing greps for the PASS lines (`grep -rn "dvfsr object" tools/` finds nothing **[sandbox]**).
- The pre-existing `railsr` line `:271` has the same defect.
- The check also inspects a separate x86 host compile with hand-picked flags, not the shipped `out/acpm.o` built with the image's `$CFLAGS` (`:185`).
- And `nm` names cannot see the `XM_DREQ` branch inside `xfer` or an inlined FREQ_REQ builder (Q9).
- Fix: `if ! ...; then echo FAIL ...; exit 1; fi` for every audit and host-test line. For a `touch_dvfs=1` image without `TOUCH_DVFS_WRITE`, run
  `"$NM" out/acpm.o out/touchprobe.o` and fail on `acpm_dvfs_set`. Add a source check in `tools/test-bluejay-boot.py` that every occurrence of
  `XM_DREQ`/`FREQ_REQ` in `acpm.c` sits inside `#ifdef TOUCH_DVFS_WRITE`.

**M2. The `--boot-xnu` path asks for all three clusters in one go, before any of them has been observed.** `shim/shim.c:563-575`.
- Nothing harmful is known **[src]**: rates are firmware rows, and voltage is the firmware's job (`acpm_dvfs.c:97-135` sends no regulator call).
- But cores 4-7 are powered off while the shim runs (only cpu0 is up). Linux never sends FREQ_REQ for a cluster with no online CPU (cpufreq policy
  stopped) **[guess]**, so how the APM handles a PLL/voltage change for a powered-down cluster is unknown **[guess]**.
- A firmware fault there could wedge the APM. PSCI CPU_ON for XNU's secondaries may depend on it **[guess]**, so XNU could hang. That is volatile and recoverable with
  Power+VolDown, but it would look like an XNU bug.
- The code already supports staging (`0 = leave`). Make it the rule: first set run `--dvfs 1803000,0,0` (cpu0's own, online cluster), then CL1 and CL2 one at a time.
  Put this in doc 25 section 3 and the handoff.

**M3. The fake APM does not model "consume now, reply later", which is how a real firmware with a reply queue behaves.** `shim/dvfs_host_test.c:82-108`.
- `apm_complete` advances the request rear and posts the reply in the same instant, so the grace logic and the late-reply interleavings are never exercised.
- The `reply_to_req = 0` case (`:333-347`) models a firmware that the source says does not exist (H2).
- Add a delayed-reply mode, time-based on `now`, not poll-count-based. In `/tmp/dvfsmut/late` I needed it to show H1 and H2.

### L

**L1. The DVFS reply check is weaker than it could be.**
- `cnt != 1` for the DVFS branch is untested: mutation `nocnt` survives all 4 dvfs tests and acpm_host_test **[sandbox]**.
- Review-2 L3 (match the low 16 bits of `rep[0]` against `req[0]`) was not taken over. For DVFS this is the domain index and would name a late reply of another
  cluster. Add `(x->rep[0] & 0xffffu) != (x->req[0] & 0xffffu)` to the seq check (`acpm.c:492`) and a two-entry reply test.

**L2. Reply word 3 of FREQ_REQ is read as an error code.** `acpm.c:489-494`.
- **[src]** For CPU domains Linux never looks at the FREQ_REQ reply (`response=false`; `cmd[3]` is zeroed at `acpm_ipc.c:768`, so `exynos_acpm_set_rate` returns 0).
- For `response=true` domains, `set_rate` returns reply `cmd[3]` (`acpm_dvfs.c:133`), so treating it as a status is defensible. That it is always 0 on success for
  CPU domains is **[guess]**.
- If the firmware echoes the AP timestamp, every successful request reports `E_RET` and stops the other clusters. That is conservative, but RESULT then lies the other way (H1).
- After H1, keep the check but let the read-back decide the result.

**L3. A refusal by `acpm_dvfs_set` prints no reason.** `touchprobe.c:1407`.
- `refuse(&t->a, ...)` zeroes `t.a`, so `t.a.seq == 0`, and with `t.n == 1` neither `dvfs_xfer_line` runs.
- The log shows only `dvfs set: REFUSED-or-FAILED`, without `acpm_why(ACPM_W_DOM)`.
- It is unreachable while `boards.py --dvfs-check` and `dvfs_rate_ok` agree, but it is exactly the line you need when they do not. Print `t.a` when `t.a.err` is set.

**L4. `cfg_chan` is complete today, but nothing keeps it complete.** `acpm.c:613-638`.
- **[src]** It copies all 24 scalar fields of `struct acpm_cfg` (`acpm.h:76-83`, incl. both rails and `dvfs_ch`).
- A field added later would be left as uninitialised stack in `d`.
- Add `_Static_assert(sizeof(struct acpm_cfg) == <current>, "update cfg_chan")`. Or, smaller: pass the channel id into `xfer()` and drop the copy.

**L5. Flag combinations that silently do nothing.** `build.sh:31`, `:108-114`; `shim.c:562`.
- `--dvfs K0,K1,K2` without `--touch-probe=dvfs` is accepted and ignored.
- `--boot-xnu --touch-probe=dvfsr` builds an image whose dvfs block never runs (the probe sections are after `loader_boot`).
- Reject both in `build.sh`.

**L6. A failed `loader_boot` runs the whole dvfs sequence a second time.** `shim.c:565-582` then `:727-743`.
- Safe in every case I traced **[src]** + **[sandbox]**: after a successful set the second run hits `ACPM_E_ON`; after a pending reply it is refused BUSY.
- But it doubles the doorbells and resets `t.seq` to 1. Skip the probe-path dvfs block when the BOOT_XNU block already ran.

**L7. Cosmetic issues.**
- Both `dvfs_host_test` builds print the identical `PASS dvfs_host_test`; add `(ro)`/`(rw)`.
- `say` continuation lines lose the `DVFS` prefix when a line exceeds 82 bytes. That only happens with large `us=`/negative values, but `grep DVFS` then misses the tail.
- `req[3]` (timestamp) is never asserted: mutation `w3zero` survives. It is harmless, just untested.

## Answers to the ten questions

1. **The PMIC path is unchanged in behaviour.** **[src]** diff + **[sandbox]**
   - `pmic_w0` is the old expression byte for byte. `req[2]=req[3]=0` are passed explicitly. The write order and all pre-checks are untouched.
   - The `XM_PMIC` reply branch (`acpm.c:498-504`) is the old code: `ret = rep[1]>>24`, `value = (rep[1]>>8)&0xff`, seq/count -> `W_SEQ`, ret -> `E_RET/W_NONE`.
   - New: `had_reply = 1` is also set for PMIC. It is unused there and zeroed by `refuse()`/`acpm_xfer_zero`.
   - The `XM_DREQ` block is compiled only with `TOUCH_DVFS_WRITE` and gated by `mode == XM_DREQ`.
   - `acpm_host_test` and `rails_host_test` (verified 0/1) pass; the `nofull` mutation is caught by both acpm_host_test and dvfs_host_test.
   - Review-2 M1 (E_ON ends the loop): still fixed for rails (`touchprobe.c:1307`) and done the same way for DVFS (`:1411`).
   - Review-2 M2: still fixed (`refuse` zeroes everything incl. `had_reply`; `touch_probe_dvfs` zeroes `t.a/t.b`).
   - Review-2 M3: the analogue holds in the source, but its build check fails open (M1).
   - Review-2 M4: re-created on purpose by the grace path (H2).
2. **Writes.** **[src]**: `w32_allowed` (`acpm.c:73-86`) allows only:
   - the 16-byte slot at `req_base + 16*rf`, before the front write;
   - `req_front := nf`, once;
   - `rsp_rear := sf`, only after a reply was read;
   - `INTGR1 = 1<<ch` (bit 1, value `0x2`);
   - `INTCR0 = 1<<ch`, only after a reply.

   The DVFS guard is built from `d` (`cfg_chan`) and the descriptor of channel 1, found by `ch_id` with `idx == ch_id` enforced.
   `only_channel_writes()` and the fake's doorbell assert hold, and the `bellbit`, `cfgch` (INTGR1+4) and `cfgtimeout` mutations are caught by all four dvfs tests **[sandbox]**.
   - `dvfs_ch >= 32`: `xfer` refuses `E_ARG` before any read (`:398`).
   - `dvfs_ch == 0`: get/set refuse before any read. Step 0 would describe channel 0, which is reads only (L-grade, pinned to 1 by `boards.py:156`).
   - The early-success return of the grace path writes nothing (no drain, no INTCR0).
3. **Request validity.** **[src]** + **[sandbox]**
   - `dvfs_rate_ok` rejects 0 and anything above `dvfs_ceil[i]`, then scans only `k < dvfs_nr[i]`, so the zero padding is unreachable. Both guards are needed only together: the `nr_and_zero` mutation is caught, each alone survives harmlessly.
   - The `IDXS/CEILS/NRS/ROWS` generators all iterate the same `doms` list, the names and order are pinned (`boards.py:158-163`), and `dvfs_find` maps the index to its array position.
   - The rows equal the fvmap1 log exactly (script over `logs/runs/20261022T170051Z-fvmap1/merged.log`: 15/17/21 rows, all equal) **[obs-hw]**. Each ceiling is a row (`boards.py:167`).
   - The `ceil`, `notable` (cross-cluster row 1803000 for CL1) and `noEON` mutations are all caught.
4. **XM_DREQ completion.** See H2. The branches I walked:
   - Reply before grace: break, drain, seq check. Fine.
   - Reply after grace: OK + dirty queue. Wrong (H2).
   - Wrong seq: `E_REPLY` after draining. Fine.
   - `rr2` read fault: `E_FAULT` + snap. Fine.
   - Never consumed: `E_TIMEOUT`, request stays published, no rollback. Correct, and tested (`never_at`).
   - Consumed but no reply within 50 ms: cannot reach the timeout, because grace (5 ms) fires first.

   `lim/10`: `lim = 24576000*50000/1e6 = 1228800` ticks, so the grace is 5 ms. No overflow: the product is 1.2e12. With `counter_freq() < 20` it would give 0, so the grace would be immediate, but freq 0 is refused and the real value is pinned **[src]**. Leaving the request published on timeout is right (review-2 H1).
5. **Queue-full rule.** **[src]** `(rf+2)%qlen == sf` is Linux's wait condition (`acpm_ipc.c:712`). It only holds as an idle invariant because the firmware replies to every
   request, so the fronts move in lockstep. That is also how the acpm3 dump of channel 2 looked: 3/3 **[obs-hw]**. With the firmware replying (H2), refusing on it is correct and cannot wedge anything (zero writes).
   The test at `dvfs_host_test.c:349-357` is built on the non-replying firmware model. Keep the assertion, drop the narrative.
6. **Physical safety.**
   - Voltage comes from the firmware's own fvmap. Linux sends no regulator call **[src]**.
   - All three ceilings are mid-table rows (881 mV / 794 mV / 750 mV in fvmap1 **[obs-hw]**), well below the rows that Android's own governor uses under load **[src: same table]**.
   - I found nothing in the client that can program a rate or voltage outside the firmware table.
   - Hardware damage: no mechanism found. Thermal at these rates without a governor: low risk **[guess]**. The gs101 hardware thermal trip is not verified here **[guess]**.
   - An irrecoverable hang: everything is volatile, and Power+VolDown restores it **[src SAFETY.md]**.
   - The remaining unknown is a request for a powered-down cluster (M2), so stage CL0 first.
7. **Wiring.**
   - A failure never blocks `loader_boot`: every step's result only gates the next step, and the block always falls through **[src]**.
   - The double-run on a failed `loader_boot` is L6.
   - `TOUCH_DVFS_WRITE` without `acpm.o` is impossible: `dvfs` sets bit 512 too (`build.sh:95`), and that links `acpm.o` (`:209`).
   - Line lengths: `con_str` in `dvfs_probe_host_test.c:37` asserts `<= 82` / `< 96`, and `vsay` splits at 82 **[src]**.
8. **Mutations** (`/tmp/dvfsmut/run.sh`, 4 dvfs test builds + acpm_host_test) **[sandbox]**:
   - Caught: `ceil`, `notable`, `nopreread` (rw + probe1), `bellbit` (all), `graceflip`, `nograce`, `graceconsumedany` (rw + probe1, via the fake's progress-on-read interleaving), `noret_dvfs`, `nofull`, `noEON`, `nr_and_zero`, `cfgch`, `cfgtimeout`, `readback_ignored` (probe1, only via `never_at`).
   - Survived: `nocnt` (L1); `w3zero` (L7); `nrmax` and `nrmax_nozero` each alone (harmless, redundant guards).
   - Missing tests: the late-reply mode (M3), the request taken but rate unchanged case (H1), a two-entry reply (L1), and anything at all for the `shim.c` order and gating (only reviewable by reading).
   - I did not mutate "front before slot": a mechanical swap of the loops is caught by `expect_w` order in `test_get` **[src :265-271]** (not run).
9. **Object audit.**
   - [sandbox] The `tools/test-bluejay-boot.py` acpm object audit passes on the dvfs image's `acpm.o` (`TOUCH_DVFS_WRITE` in): "try_write32 referenced only by a bl inside w32; 215 stores outside w32, all stack/argument based".
   - [src] `cfg_chan` stores through its argument into the caller's stack struct, which the audit classifies as argument-based. No new MMIO store path.
   - The read-only check is too weak and fails open (M1).
10. **Other items, all checked fine** **[src]**:
    - `dvfs_ms`: `ticks*1000` overflows u64 only after about 24 years at 24.576 MHz, and the u32 wrap is about 49.7 days, the same as Linux.
    - SRAM offsets are checked `& 3` and against the window (`acpm_describe`), and both sides are little-endian.
    - `touch_probe_dvfs`: `v[]` is zeroed for `i < n`, and `n > 8` / `n < 1` returns before use.
    - `acpm_dvfs_ndom() == 0` gives `E_UNSURE` for steps 1/2, and step 0 is reads only.
    - `dom` is always set because `i < n`.
    - `%u` matches `uint32_t`, and `%llu` gets `unsigned long long` casts.

## Checked and found fine

- Command words against Linux:
  - FREQ_GET `{idx|seq<<16, 0, 1, ms}` and FREQ_REQ `{idx|seq<<16, kHz, 0, ms}`: `acpm_dvfs.c:117-120, :180-183`, seq at bits 21:16 per `acpm_ipc.c:758-759` **[src]**. Asserted in `dvfs_host_test.c:263, :326`.
  - GET semantics: value = reply word 1, error = reply word 3 (`acpm_dvfs.c:188-191`) **[src]**.
  - The doorbell is `1 << channel id` (`acpm_ipc.c:410-413`) and the status clear is `1 << id` to INTCR (`:458`) **[src]**.
- Domain indexes 2/3/4 and channel 1 are pinned with citations (`boards.py:148-149`). I did not re-open gs101.dtsi / dt-bindings.
- Rates: soc.json equals the fvmap1 log for all 53 rows **[obs-hw + sandbox]**. The ceilings are rows **[src boards.py:167]**, and `--dvfs-check` refuses non-rows, rows above the ceiling and negative numbers, and refuses with the wrong count **[src]**.
- Refusals before any write: unknown domain, rate 0 / non-row / above ceiling / other cluster's row, `dvfs_ch == 0`, non-empty queue, wrong element size, queue-full. All are asserted with `nw == 0` **[sandbox]**.
- Pre-read then request then read-back order, and that CL1=0 is left alone (`dvfs_probe_host_test.c:73-79`) **[sandbox]**.
- A full build with `--touch-probe=dvfs --dvfs 1803000,1836000,1826000` in a copy: all host tests, the source and object audits PASS. The only failure was `git rev-parse` because the copy has no `.git` **[sandbox]**.

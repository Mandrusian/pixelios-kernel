# 25 - gs101 CPU frequency (ACPM DVFS): what the phone runs at and how to change it

Evidence tags: **observed** (hardware, date + run), **src** (read from source, file:line), **guess**.

## 1. Observed: ABL leaves every CPU cluster at ~40 % of its maximum
- **observed** run `fvmap1` (2026-10-22, local kernel sha256 5b1e283378b0, patches 0001-0097, boot-arg `sweetfvmap=0x206A000`):
  the ACPM firmware's own DVFS table (fvmap) in APM SRAM lists, per domain (index = `ACPM_DVFS_*` & 0xFFFF):

  | dom | cluster | cores | levels | max kHz | init_lv (kHz, uV) |
  |---|---|---|---|---|---|
  | 2 CPUCL0 | Cortex-A55 | 0-3 | 15 | 2196000 | 9: 1197000, 712500 |
  | 3 CPUCL1 | Cortex-A76 | 4-5 | 17 | 2466000 | 10: 1197000, 637500 |
  | 4 CPUCL2 | Cortex-X1 | 6-7 | 21 | 3195000 | 17: 1106000, 606250 |

  Header `type` equals the index for all 14 domains and every table is monotonic: the address (APM SRAM 0x2039000 + fvmap_addr
  0x31000) and the layout are right.
- **observed** run `local096c` (patch 0096): a dependent-add loop timed against CNTVCT (24.576 MHz) gives ~1184-1196 MHz on cpu0-5 and
  ~1105 MHz on cpu6-7 (MIDR 0xd05 / 0xd0b / 0xd44). That agrees with the firmware's init levels (1197/1197/1106 MHz) to within 1 %,
  so the probe is a valid cycle-rate measurement and **the CPUs run at the ABL init level**: A55 54 %, A76 49 %, X1 35 % of max.
- XNU has no cpufreq driver, so nothing ever raises them (read in docs/research/11, 22).

## 2. How Linux changes it (src, soc/gs commit 2040787, drivers/soc/google/cal-if/)
- `exynos_acpm_set_rate(id, kHz)` (acpm_dvfs.c:97-135): message of 4 words on the DVFS channel (`ch_num` from the dvfs-manager node,
  DT `exynos_dm: acpm-ipc-channel = <1>`, gs101.dtsi:720): `cmd[0]=id` (the index, GET_IDX = id & 0xFFFF, cmucal.h:207), `cmd[1]=kHz`,
  `cmd[2]=FREQ_REQ(0)`, `cmd[3]=ms timestamp`; CPUCL0/1/2, MIF, INT are sent with `response=false`. ACPM_IPC adds the sequence number at
  bits [21:16] of cmd[0] (acpm_ipc.c:759), the same framing as the PMIC channel.
- `exynos_acpm_get_rate(id, dbg)` (acpm_dvfs.c:173-196): `cmd = {id, dbg, FREQ_GET(1), ms}`, with a response; the rate is `cmd[1]`
  of the reply when `cmd[3]` is 0.
- Async DVFS is a second channel, optional; not needed.
- Voltage is done by the ACPM DVFS plugin from the fvmap (Linux sends no regulator call for CPU rates) [src: no regulator in set_rate].

- **The firmware replies to FREQ_REQ too** (read from source, review dvfs-client-review-1 H2): `send_data` sets the request's sequence bit
  (acpm_ipc.c:756); for `response=false` the bit is cleared only when a reply entry with that number arrives (check_response, :446-448); the next
  send spins until a bit is free (:747-750). A firmware that did not reply would hang Linux after 63 CPU frequency changes. `response=false`
  only means Linux does not wait for the reply. The shim therefore waits for it (bound 50 ms) like for FREQ_GET.

## 3. Not known yet (do not guess)
- Channel 1's descriptor on this phone (elem size must be 16 bytes), that both queues are empty and the fronts equal at idle, and that FREQ_GET
  returns 1197000 / 1197000 / 1106000 (the fvmap init levels): the first read-only step (`--touch-probe=dvfsr`, build WITHOUT relying on the set path).
- What the reply words of FREQ_REQ contain (word 3 is treated as an error code; guess). The reply does not prove the rate changed: read back.
- How the firmware handles a request for a cluster whose cores are powered off (the shim runs on cpu0 only; cores 4-7 are off): unknown
  (review M2), so stage: first set run `--dvfs 1803000,0,0` (CL0, cpu0's own cluster), then CL1, then CL2 one at a time.
- Thermal behaviour at higher levels: no TMU/throttling exists in our stack. Raise in steps and watch.

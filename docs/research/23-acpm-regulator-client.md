# 23 - ACPM regulator client: how to switch the touch rails (LDO25M/LDO26M) from a bare-metal EL1/EL2 shim

Status: research only, 2026-10-22. No device touched. Tags: **[src]** read from source (file:line), **[abl]** found in the ABL
binary (`~/pixelios-artifacts/abl-code/abl_code.bin`, strings only), **[hw]** observed on hardware, **[guess]** not established.
"unknown" means not found, not guessed.

Source: `GK` = https://android.googlesource.com/kernel/gs, branch `android-gs-bluejay-5.10-android13-qpr3`. Fetched 2026-10-22
into `~/pixelios-artifacts/scratch/acpm-src/` (flat names, not in git; the branch head was not recorded, only the branch name).
Files: `drivers/soc/google/acpm/{acpm_ipc.c,acpm_ipc.h,acpm_mfd.c,fw_header/framework.h}`, `include/soc/google/{acpm_mfd.h,acpm_ipc_ctrl.h}`,
`drivers/mfd/s2mpg10-core.c`, `drivers/regulator/s2mpg10-regulator.c`, `arch/arm64/boot/dts/google/{gs101.dtsi,gs101-pmic.dtsi,gs101-fts-touch.dtsi}`.

## 0. Correction to the premise

**LDO25M and LDO26M are on S2MPG10, not S2MPG11.** The "M" suffix = main PMIC = S2MPG10 (`m_ldo25_reg: LDO25M`, `schematic-name = "L25M_TSP_DVDD"`,
`LDO26M` = `L26M_TS_AVDD`; GK `gs101-pmic.dtsi:535-553`) [src]. S2MPG11 is the sub PMIC (`LDO*S`) on speedy channel 1 [src]
(GK `gs101-pmic.dtsi:710-718`). The touch node itself (`gs101-fts-touch.dtsi`) names no supply; the supplies are bound in the
FTM5 driver (`fts_enable_reg`, see doc 21) [src]. Neither LDO has `regulator-always-on`/`boot-on` [src] (doc 21).

## 1. How a regulator enable reaches the PMIC (Q1)

Call chain [src], all GK:

| Layer | What | Where |
|---|---|---|
| regulator core | `s2m_enable()` -> `s2mpg10_update_reg(i2c, enable_reg, opmode[id], enable_mask)`; `opmode[id]` is initialised to `enable_mask` (so value 0x80, mask 0x80 for LDO25M/26M) | `s2mpg10-regulator.c:75-86`, `:890` |
| descriptors | `LDO25M`: enable_reg = `_L25M_CTRL`, mask `_7`; `LDO26M`: `_L26M_CTRL`, mask `_7` (register values 0x5C / 0x5D per doc 21, from `s2mpg10-register.h`) | `s2mpg10-regulator.c:275-278` |
| MFD | `s2mpg10_update_reg()` -> `exynos_acpm_update_reg(node, channel=0, i2c->addr, reg, val, mask)`; **`channel` is hard-coded 0** (the speedy channel); the PMIC sub-device client addr is `I2C_ADDR_PMIC = 0x01` | `s2mpg10-core.c:29-34, :127-139, :492` |
| ACPM client | builds a 4-word command and calls `acpm_ipc_send_data()` on the IPC channel from DT `acpm-ipc-channel` | `acpm_mfd.c:209-255` |
| transport | shared-memory queues in APM SRAM + doorbell write to the AP2APM mailbox | `acpm_ipc.c:680-860` |

Not a plain I2C access from the AP: the SPEEDY bus to S2MPG10/11 is owned by the APM (ACPM) core [src: DT puts both PMICs under
`google,i2c-acpm` nodes `acpm_mfd_bus0/1` with `acpm-ipc-channel = <2>`, `acpm-speedy-channel = <0|1>`, GK `gs101-pmic.dtsi:31-39, :710-718`, `gs101.dtsi:590-598`].

### 1.1 Command word format (MFD protocol) [src]

Macros: `include/soc/google/acpm_mfd.h` (`set_protocol(data, X) = (data & MASK_X) << SHIFT_X`).

| Word | Field | Bits | Notes |
|---|---|---|---|
| cmd[0] | REG | [7:0] | PMIC register |
| cmd[0] | TYPE | [11:8] (mask 0xF) | PMIC sub-device I2C address (`i2c->addr`): 0x00 TOP, **0x01 PMIC**, 0x02 RTC, 0x0A METER, 0x0B WLWP, 0x0F TRIM (`s2mpg10-core.c:29-34`) |
| cmd[0] | CHANNEL | [15:12] (mask 0xF) | speedy channel: **0 = S2MPG10**, 1 = S2MPG11 |
| cmd[0] | seq num | [21:16] | OR-ed in by the IPC layer, 1..63 (`acpm_ipc.c:754-759`, `ACPM_IPC_PROTOCOL_SEQ_NUM = 16`, `acpm_ipc_ctrl.h`) |
| cmd[1] | FUNC | [7:0] | 0 READ, 1 WRITE, 2 UPDATE, 3 BULK_READ, 4 BULK_WRITE (`enum mfd_func`) |
| cmd[1] | WRITE_VAL | [15:8] | for FUNC_WRITE |
| cmd[1] | UPDATE_VAL / UPDATE_MASK | [15:8] / [23:16] | for FUNC_UPDATE (read-modify-write done by the APM) |
| cmd[1] | RETURN (reply) | [31:24] | 0 = ok; 1 = speedy_rx fail, 2 = speedy_tx fail (`acpm_mfd.c:243-248`) |
| cmd[1] | DEST (reply) | [15:8] | read value for FUNC_READ (`acpm_mfd.c:40`) |
| cmd[2], cmd[3] | bulk data / ktime | | cmd[3] = AP ktime in ms for write/update (`acpm_mfd.c:221-224`); bulk read data in cmd[2], cmd[3] |

Example words [src, derived by hand from the macros, not run]:

- READ  PMIC reg 0x5C, speedy 0: `cmd[0] = 0x0100 | 0x5C | (0<<12) | (seq<<16)`, `cmd[1] = 0`. Reply value = `(reply[1] >> 8) & 0xFF`.
- UPDATE reg 0x5C bit 7 = 1: `cmd[1] = 2 | (0x80<<8) | (0x80<<16) = 0x00808002`.

### 1.2 Transport and wait [src]

1. Channel `i` lives in a descriptor in SRAM (`struct ipc_channel`, see Q2). The AP writes its request into the descriptor's
   **`ch.rx_*` queue** (AP TX = APM RX; the driver's `tx_ch` is built from `ch.rx_rear/rx_front/rx_base`, `acpm_ipc.c:977-983`) and
   reads replies from the **`ch.tx_*` queue** (`rx_ch` built from `ch.tx_rear/tx_front/tx_base`, `:970-975`).
2. Send sequence (`__acpm_ipc_send_data`, `acpm_ipc.c:680-773`): read `tx_front`, `rx_front` (the "other" front); wait until
   `(tx_front + 2) % len != rx_front` (`:712`); `tmp_index = (tx_front + 1) % len` must not equal `tx_rear` (`:723`); drain stale
   replies first (`check_response(channel, NULL)`, `:744`); pick `seq_num` 1..63 not in use (`:748-751`); OR it into cmd[0]
   (`:759`); copy `tx_ch.size` **bytes** (`memcpy_align_4(dest, src, n)` takes a byte count; erratum below) to `tx_ch.base + size * tx_front` (`:762`); `writel(tmp_index, tx_ch.front)` (`:770`);
   **doorbell**: `writel(1 << channel->id, intr + APM_INTGR)` (`apm_interrupt_gen`, `:410-413`, `:772`), `APM_INTGR = INTGR1 = 0x40`
   (`acpm_ipc.h`).
3. Reply wait: the PMIC channel is a polling channel if `ipc_channel.ap_poll` is set (`channel_init`, `:961`); then the AP loops `check_response()` with
   `udelay(10)` and a 10,000,000 ns (`IPC_TIMEOUT`) software timeout, retrying up to 5 times, then dumps and panics (`:775-850`). In non-polling mode it
   waits for the APM2AP mailbox IRQ (`INTSR0`/`INTCR0`, `:539-558`). A bare-metal client can simply poll (see Q4).
4. `check_response` (`:415-470`): `rx_front = read(rx_ch.front)`, `i = read(rx_ch.rear)`; for each entry take `seq = (word0 >> 16) & 0x3f`
   (0 or >= 64 panics in Linux, `:437`), copy the reply, advance; finally `write(rx_front, rx_ch.rear)` (`:455`) and **clear the doorbell
   status** `write(1 << id, intr + AP_INTCR)` (`:458`), `AP_INTCR = INTCR0 = 0x24`.

Mailbox register offsets from the mailbox base [src, `acpm_ipc.h`]: `INTGR0 0x20, INTCR0 0x24, INTMR0 0x28, INTSR0 0x2c, INTMSR0 0x30,
INTGR1 0x40, INTMR1 0x48, INTSR1 0x4c, INTMSR1 0x50, SR0..SR3 0x80..0x8c`. AP uses INTGR1 to ring the APM and INTSR0/INTCR0/INTMR0 for replies.

## 2. Physical addresses (Q2)

From the `acpm_ipc` node, GK `gs101.dtsi:491-507` [src]:

| Item | Value | Source |
|---|---|---|
| AP2APM mailbox base | **0x17610000**, size 0x1000 | `reg` #0 `/* AP2APM MAILBOX */` |
| APM SRAM base / size | **0x02039000** (written `0x2039000`), size **0x40000** | `reg` #1 `/* APM SRAM */` |
| ALIVE_FRC_CTRL (timestamp, optional) | 0x1742048c, size 0x14 | `reg` #2 |
| initdata (acpm_framework) | SRAM + **0xA000** = **0x02043000** | `initdata-base = <0xA000>` |
| Doorbell (AP -> APM) | mailbox + 0x40 (INTGR1), bit = IPC channel id | `acpm_ipc.h`, `acpm_ipc.c:412` |
| Reply status/clear (APM -> AP) | mailbox + 0x2c (INTSR0) / + 0x24 (INTCR0) / + 0x28 (INTMR0) | `acpm_ipc.h`, `acpm_ipc.c:458, :1001` |
| APM2AP interrupt | `IRQ_MAILBOX_APM2AP_ALIVE` (GIC SPI number: **unknown**, macro not resolved; not needed when polling) | `gs101.dtsi:495` |
| IPC channel id for the PMIC | **2** (`acpm-ipc-channel = <2>` on both `s2mpg10mfd` and `s2mpg11mfd`); speedy channel is inside the command word | `gs101-pmic.dtsi:38-39, :717-718` |
| Other fixed IPC ids (for context) | 0 = cal_if/clock, 4 = acpm, 10/11 = slc, 12 = acpm_ipc | `gs101.dtsi:456, 485, 504, 523-526` |

Note [src]: `soc/google/gs101/docs/02-gs101-peripherals.md:23, :309` give the mailbox as `0x17440000`; in GK that address is `TIMER_APM`
(`acpm` node, `gs101.dtsi:481-489`), not the mailbox. Treat `0x17610000` as the mailbox.

**How the kernel discovers channel offsets** [src]: it does not hard-code them. `acpm_ipc_probe` maps SRAM, sets
`initdata = sram_base + initdata-base` (`acpm_ipc.c:1084-1093`); `channel_init` reads `initdata->ipc_ap_max` (number of AP channels) and the
descriptor array at `sram_base + initdata->ipc_channels` (`:945-999`). All queue pointers are **SRAM-relative 32-bit offsets**.

Layout (built with `CONFIG_GS_ACPM_MODULE`, where pointers are `u32`; if the config were not set the layout would use 64-bit pointers: **guess** that the module config is on,
because `acpm_mfd.h` and the Android build gate everything on `IS_ENABLED(CONFIG_GS_ACPM_MODULE)`) `fw_header/framework.h:36-133` [src]:

| `struct acpm_framework` (at SRAM + 0xA000) | offset |
|---|---|
| `plugins` | 0x00 |
| `num_plugins` | 0x04 |
| `ipc_channels` (SRAM offset of descriptor array) | 0x08 |
| `num_ipc_channels` | 0x0C |
| `pid_framework`, `pid_max` | 0x10, 0x14 |
| `ipc_ap_max` (number of AP descriptors) | 0x18 |

| `struct ipc_channel` (stride **0x48**, derived: id 4 + bitfield word 4 + owner 4 + type 4 + `channel_info` 13 x u32 = 0x34 + ap_poll 4) | offset |
|---|---|
| `id` | 0x00 |
| bitfield word (`field`, `late_init_source`, `use_sacpm`), `owner`, `type` | 0x04, 0x08, 0x0C |
| `ch.rx_rear, rx_front, rx_base, rx_indr_buf, rx_indr_buf_base, rx_indr_buf_size` | 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24 |
| `ch.tx_rear, tx_front, tx_base` | 0x28, 0x2C, 0x30 |
| `ch.q_len, q_elem_size, credit, mbox_addr` | 0x34, 0x38, 0x3C, 0x40 |
| `ap_poll` | 0x44 |

(offsets derived by hand from the struct, `framework.h:97-133`; verify against a SRAM dump before trusting them.) The actual values
(`q_len`, `q_elem_size`, queue bases, `ap_poll` of channel id 2, and `ipc_ap_max`) are **unknown** until read from the phone.

## 3. Can a bare-metal client do it without Linux init? (Q3)

- **Linux does almost no ACPM-side init.** `acpm_ipc_probe`/`channel_init` only reads initdata and the descriptors, then writes the mailbox mask
  `INTMR0 = polling-channel mask` (`acpm_ipc.c:1001`) and a timestamp word (`time_index = 0xffffffff`, `:938`, debug log). It writes no
  channel table and no queue pointers [src]. Hence the channel table must be created by the APM firmware itself (it owns the SRAM
  layout; the AP only consumes `initdata`) [src for the AP side; guess that nothing else is required of the AP].
- **Is ACPM running at hand-off?** Strongly implied but not directly observed: the Pixel ABL contains ACPM/PMIC strings
  (`"[ACPM] "`, `"ACPM R0~R3 : %08x ..."`, `"failed to initialize S2MPG pmic %d"`, `"failed to initialize if-pmic %d"`, `"Error reading pmic topsys int ret:%d"`,
  `"acpm locks regulators"`, `"failed to set rate, ACPM error %d"`) [abl, my `strings` over `abl_code.bin`, 2026-10-22], so the ABL itself reads/writes PMIC and
  clocks through ACPM, and the rails for UFS/display are on when the shim runs [hw: the shim already talks to UFS and display]. The APM firmware is therefore
  running when the shim starts [guess, high confidence]. Where the ABL's mailbox/SRAM constants are (and whether it uses the same queue protocol as Linux): **unknown**
  (the ABL was not disassembled for this; no literal `0x17610000`/`0x02039000` was found by a byte search, so it builds addresses with movz/movk or reads them from a table).
- **Who loads the ACPM firmware?** unknown. Not Linux (Linux only talks to it) [src]; so an earlier stage (BL1/BL2, before the ABL) [guess].
- **Access from EL1/EL2:** `0x17610000` and `0x02039000` are normal MMIO/SRAM; whether any protection unit (S2MPU/TZASC/APM firewall) blocks non-secure
  EL1 reads or writes there is **unknown**. The alive block `0x174E....` is already accessed by the shim [hw, doc 21], which is a hint, not proof.
- **Linux state the ABL may leave behind:** non-empty reply queues (`rx_front != rx_rear`) and `INTSR0` bits. Linux copes via `check_response()` before every send
  (`:744`); the shim should do the same (sync `rx_rear := rx_front`, `INTCR0 := 1 << 2`) [guess, mirrors `:455-458`].
- **Linux-only prerequisites that may not apply:** none found in `acpm_ipc.c`. Bits "GS_ACPM clock/PD gating of the mailbox" (e.g. `pd-apm` string in the ABL [abl]) are **unknown**.

The earlier doc 02 statement "no ACPM driver required for initial bring-up" ([guess] there) holds only because the bootloader left those rails on;
touch rails are not among them [guess, doc 21].

## 4. Minimal pseudo-code (Q4)

Constants: `MBOX = 0x17610000`, `SRAM = 0x02039000`, `INIT = SRAM + 0xA000`, `CH_ID = 2`. Everything is 32-bit MMIO (`ldr w` / `str w`, device memory, `dsb sy` after the stores).

```
# --- A. discover (reads only) -------------------------------------------------
n    = rd32(INIT + 0x18)                    # ipc_ap_max            [src framework.h]   sanity: 1..32
tab  = SRAM + rd32(INIT + 0x08)             # ipc_channels offset   [src]               sanity: inside SRAM
for i in 0..n-1: d = tab + 0x48*i           # stride 0x48           [src, hand-derived: verify]
    if rd32(d + 0x00) == CH_ID: break       # match DT acpm-ipc-channel = 2   [src]
q_len = rd32(d+0x34); esz = rd32(d+0x38)    # sanity: q_len >= 3, esz >= 4  [src acpm_ipc.c:697]
txq   = {rear: SRAM+rd32(d+0x10), front: SRAM+rd32(d+0x14), base: SRAM+rd32(d+0x18)}   # AP -> APM  [src :977-983]
rxq   = {rear: SRAM+rd32(d+0x28), front: SRAM+rd32(d+0x2c), base: SRAM+rd32(d+0x30)}   # APM -> AP  [src :970-975]
poll  = rd32(d + 0x44)                      # [src]   if 0 the IRQ would fire; we only poll INTSR0 / rxq.front

# --- B. send one PMIC command (generic) ----------------------------------------
ack_stale: rd32(rxq.front)->f; wr32(rxq.rear, f); wr32(MBOX+0x24, 1<<CH_ID)           # [guess, mirrors :455-458]
tf = rd32(txq.front); rf = rd32(rxq.front)
require ((tf + 2) % q_len) != rf and ((tf + 1) % q_len) != rd32(txq.rear)             # [src :712, :723]
w0 = (0x01 << 8) | reg | (0 << 12) | (seq << 16)    # TYPE=PMIC addr 0x01, speedy ch 0, seq 1..63   [src]
w[0..3] = {w0, w1, 0, 0}                             # 4 words = 16 bytes, unused words 0 [src :762, cmd[3] is only a timestamp]
store w to txq.base + esz*tf                         # element stride = esz BYTES (esz must be 16) [src :762; ERRATUM 1]
dsb sy
wr32(txq.front, (tf + 1) % q_len)                    # [src :770]
wr32(MBOX + 0x40, 1 << CH_ID)                        # doorbell INTGR1                 [src :412, :772]
loop until rd32(rxq.front) != rd32(rxq.rear)         # poll, bounded (e.g. 100 ms)     [src :775-785; guess: bounded abort instead of panic]
    r = rxq.base + esz*rd32(rxq.rear)                # esz in bytes (ERRATUM 1)
    check ((rd32(r) >> 16) & 0x3f) == seq            # [src :429-437]
    reply1 = rd32(r + 4)
wr32(rxq.rear, rd32(rxq.front)); wr32(MBOX + 0x24, 1 << CH_ID)                          # [src :455-458]
ret = reply1 >> 24;  value = (reply1 >> 8) & 0xff;  require ret == 0                   # [src acpm_mfd.c:40-41]

# --- C. the actual sequence ------------------------------------------------------
READ   reg 0x5C : w1 = 0                      -> value is L25M_CTRL    # read-only first step  [src]
READ   reg 0x5D : w1 = 0                      -> value is L26M_CTRL
UPDATE reg 0x5C : w1 = 0x00808002             # FUNC_UPDATE, val 0x80, mask 0x80   [src]
UPDATE reg 0x5D : w1 = 0x00808002
# order in Linux: DVDD (LDO25M, 1.8 V class, 1.6-1.95 V) before AVDD (LDO26M, 2.5-3.3 V)  [guess: fts_enable_reg order, doc 21 ref fts.c:6156-6181]
mdelay(~10)                                   # [guess] settle; Linux does regulator enable_time then 10 ms reset pulse
READ   reg 0x5C and 0x5D again : require (value & 0x80)                                 # verify  [src mask]
```

Notes:

- The voltage select bits of reg 0x5C/0x5D (bits 5:0 in this PMIC family, `_N_VOLTAGES(64)`) are **not touched** by an UPDATE with mask 0x80. The reset/boot
  voltage is then whatever the PMIC OTP/ABL left; Linux's regulator framework sets `regulator-min/max` from DT after enable via `vsel_reg`
  (`s2mpg10-regulator.c:130-150`) [src]. Whether the OTP default is inside the FTM5 range (DVDD 1.6-1.95 V; AVDD 2.5-3.3 V) is **unknown**: read the registers first and compute the voltage from the
  `s2mpg10` step formula (not looked up here; unknown) before setting bit 7.
- The enable field mask is `_MASK(_7)` for both (single bit). `_MASK(_7_6)` registers use two-bit opmodes; do not copy this recipe to those.
- The driver stack does **not** use FUNC_WRITE for enables (it uses UPDATE) [src]; use UPDATE so the voltage bits are preserved.

## 5. Risks and a read-only first attempt (Q5)

| Risk | Consequence | Mitigation |
|---|---|---|
| Wrong `initdata` base / SRAM offset (stale layout assumption: u32 vs 64-bit pointers, stride 0x48) | Store goes to an arbitrary SRAM word: could corrupt a live APM queue, plugin data or log; APM may fault/watchdog-reset the SoC (`APM_CPU_WDTRESET` string in ABL [abl]) | Phase A is read-only and prints raw words (framework header, the descriptor, plausibility checks: `ipc_ap_max <= 32`, every offset `< 0x40000`, `q_len >= 3`); abort if any fails; dump the descriptor as hex into the USB log so the layout is verified against real data **before** any store |
| Wrong channel id (e.g. 0 = cal_if/clock, or 12) or wrong speedy channel | A PMIC-protocol word interpreted by a different APM plugin: unknown effect (clock/DVFS plugin) | Hard-code channel 2 / speedy 0, and require `descriptor.id == 2` with `type`/`owner` plausible; never iterate over other channels |
| Wrong PMIC addr/reg in an UPDATE | Could switch off a live rail (e.g. DRAM/UFS/display LDO, a buck) -> instant hang or reset. TOP (0x00) and PMIC (0x01) share the bus. | Only the two constants `0x5C`, `0x5D` under `TYPE = 0x01`, `CHANNEL = 0`, mask `0x80`, value `0x80` (set, never clear); no generic "poke" interface; read the register first, and refuse if the read value does not look like a LDO control byte (bit 7 should be 0 and bits 5:0 a plausible voltage code) |
| Poll never completes (APM not running, queue mismatch) | Shim hangs in the poll loop | Bounded wait (the doc'd Linux one is 10 ms x 6 tries then panic; use e.g. 100 ms then log and continue), and nothing after the doorbell depends on the answer |
| Stale entries / sequence confusion | Mis-parse a reply; Linux panics on seq 0 or >= 64 | Drain stale entries first; match `seq` exactly; ignore others |
| Queue full / `front` race with the APM | Wrap bug | Respect `(front+2) % len != other_front` and `(front+1) % len != rear` like Linux (`:712, :723`) |
| Mailbox/SRAM not accessible at EL1/EL2 (protection unit) | Synchronous external abort (SError) | Do the first SRAM read as the very first step with a fault-tolerant handler already installed by the shim (unknown whether the shim has one: check `shim/`) |
| Repo rule | `docs/SAFETY.md` "Never": "Write ... PMIC regulator voltages, ACPM, or anything secure-world" | **Needs the user's explicit approval** before Phase C. Phase A (SRAM/mailbox reads) and Phase B READ (it does write the queue slot, `front` and the doorbell, so it is a write to ACPM state, but not to the PMIC) also touch ACPM: ask for approval for both. The rail enable is a reversible, volatile write (a reboot restores the OTP/ABL state) [guess] |

**Staging, safest first:**

1. A: reads only (no stores at all): print `INIT[0..0x40]`, the channel descriptor of id 2, the `front/rear` words of both queues, `INTSR0`/`INTMSR0`/`SR0..SR3`. Compare to the layout in section 2.
2. B: one `FUNC_READ` of PMIC `0x01:0x00` (TOP/ID, harmless) or directly `0x5C`/`0x5D`. Success proves the whole path (doorbell, SRAM queues, speedy channel, reply format) and also tells whether the rails are already on (bit 7 = 1 would mean the premise in doc 21 is wrong and the touch stall has another cause).
3. C: UPDATE 0x5C then 0x5D, read back.

## 6. Unknowns (all need a device read or ABL disassembly)

- Values in the live channel table: `ipc_ap_max`, the descriptor for id 2 (`q_len`, `q_elem_size`, bases, `ap_poll`), exact stride (0x48 is derived, not read).
- Whether Linux's `CONFIG_GS_ACPM_MODULE` u32 layout is the build in use (affects every offset above).
- GIC number of `IRQ_MAILBOX_APM2AP_ALIVE` (irrelevant when polling).
- Whether the ABL leaves queue state clean, and which mailbox constants/protocol the ABL uses.
- EL1/EL2 accessibility of mailbox and SRAM (protection units).
- The OTP default of reg 0x5C/0x5D (the code-to-volt mapping is known now: erratum 2).
- Who loads the ACPM firmware and when.

## Erratum (2026-10-22, from docs/reviews/acpm-rails-review-1.md, re-derived from the Google kernel sources)

1. **`q_elem_size` is in BYTES, not words (sections 1.2 step 2, 2 and 4 B said words: wrong).** *Read from source:* `acpm_ipc.c:762-764` is
   `memcpy_align_4(tx_ch.base + tx_ch.size * tx_front, cfg->cmd, tx_ch.size)`; `tx_ch.base` is a `void __iomem *` (pointer arithmetic in bytes) and
   `memcpy_align_4(dest, src, n)` (Google `acpm.c:257-271`) copies `n >> 2` words and warns when `n % 4`, so `n` is bytes. The MFD callers pass
   `unsigned int command[4]` (`acpm_mfd.c:20,66,115,163,214`) and `check_response` copies `size` bytes back into it (`acpm_ipc.c:446,468`): a size of
   16 *words* would overrun that 16-byte array by 48 bytes on every PMIC call, so for channel 2 `q_elem_size` = 16 (bytes) = 4 words is the only
   consistent value. (The `sizeof(int) * size` allocations at `:754, :984, :990` merely over-allocate.) Consequences, all applied: `soc.json` `elem_unit` = 1; the shim
   computes every slot offset in bytes (`base + 16 * index`), requires `q_elem_size * elem_unit == 16` and refuses otherwise (`ACPM_W_ESZ`); with the old
   `elem_unit = 4` and a real 16 the shim would have written 16 words at `base + 64 * front` while the APM reads 4 words at `base + 16 * front`
   (a stale slot executed: an unrequested PMIC write). This is still *read from source*: `elem_unit_verified` stays 0 until a `railsr` dump shows
   16-byte slots (`ACPM req buf+...` lines: one 4-word element per line, command words with seq in bits 21:16 and 0x01 in bits 11:8 of word 0).
2. **Voltage windows (section 6 listed the mapping as unknown): *read from source*.** LDO25M is voltage group 4 = 700 mV + 25 mV * code
   (`s2mpg10-register.h:319-320`), LDO26M group 5 = 1800 mV + 25 mV * code (`:322-323`); `vsel_mask` = 63 (`s2mpg10-regulator.c:219`, 64 voltages). The
   device-tree limits (`gs101-pmic.dtsi:535-553`) give L25M 1.60-1.95 V = codes 0x24-0x32 and L26M 2.5-3.3 V = codes 0x1C-0x3C. `soc.json` carries both the window and the numbers it
   is derived from; `tools/boards.py _acpm_check` recomputes it and fails the build on a mismatch or on a null window while a verified flag is 1.
3. **No rollback after publish (section 4 B / the shim's "undo" were wrong in spirit).** The APM reads front, reads the slot, runs the SPEEDY transaction and
   only then advances `rear`; an unmoved `rear` does not mean "not taken". Writing `front` back after the front write made the APM see `q_len - 1` stale
   pending commands. Linux never rolls back (`acpm_ipc.c:762-772`). The shim now stops, prints the queue state and leaves the channel as it is.

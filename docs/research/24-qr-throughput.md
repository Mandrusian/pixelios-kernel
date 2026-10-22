# 24 - QR log channel: QL1 vs QL2, measured offline

Status: 2026-10-22. No device touched. Tags: **[hw]** observed on hardware earlier (docs/HANDOFF.md, QR LOG CHANNEL sections), **[sim]** measured
with `tools/qrsim.py` (the real C encoder `qrlog/` rendered as the phone draws it, seen through a camera model, decoded by the real host
decoder), **[src]** read from source, **[guess]**. Nothing here is a hardware result for QL2: the first hardware cycle is listed at the end.

## 0. How the numbers were made, and how far to trust them

`tools/qrsim.py modules|deliver`. Screen 1080x2400 (as `qrlog_draw`/`qs_draw` paint it: 48 px margin, 4-module quiet zone, integer module size).
Camera model: area-averaged downscale, rotation 3 deg, 1% tilt, vignette, defocus blur sigma, Gaussian noise, black/white at ~35/175 gray, JPEG.
Two presets: **typical** (blur 0.9 px, noise 4, JPEG 85, span 140) and **poor** (1.3 px, 6, 75, 110). Calibration against the only camera
numbers we had before ([hw]-derived sim numbers in HANDOFF: 720p portrait, QL1 chunk 140 reads 100 %, chunk 240 43 %, chunk 480 11 %): with
the old whole-image decoder `poor` gives 50-100 % / 0 % / 0 %, `typical` 100 / 0 / 0 (`e1_ql1_whole` below), i.e. **poor is about the
camera that produced the old numbers, typical is a little better**. Hardware QL1 chunk 200 worked (60/60 frames) [hw], so the real camera is
at least `poor`-like at its distance. Cells are 12-24 trials: treat differences under ~15 points as noise. Orientation/size: "720p portrait" = 1280x720 frame, phone upright
filling 90 % of the height (0.27 camera px per panel px); "720p landscape" = phone on its side, long edge along the width (0.48); "1200p portrait" 0.45;
"1200p landscape" 0.60 (1600x1200). The real camera's resolution is set on the board (see `.env` / reference-esp32-cam: `/schema`; framesize 13 = 1280x720, 15 legible
per HANDOFF); I did not query it (no device access).

## 1. QL1, the current setup [hw] + [sim]

| | |
|---|---|
| Frame | `QL1 <sid> <idx>/<n> <crc>\n<chunk>`, text QR byte mode, ECC M (boosted), chunk 200 B -> version 11 (61 modules), 800 ms/frame |
| Payload rate | 200 B x 1.25 frames/s = **250 B/s at best** (every frame read, useful bytes before any loss) |
| What it can show | the last 12000 B of a log whose digit-only-different lines were dropped and whose lines are cut at 159 chars: 61 frames, a 49 s loop |
| Completion | the snapshot is rebuilt every loop and its id is the crc of its text, so a log that grows gets a new id every loop and the reader restarts: "13/60 frames, restarting forever" [hw]. Only a snapshot whose 61 frames were all read under one id counts. Idle log (no new lines): 32/32 and 60/60 frames completed [hw] |
| Effective rate, idle log [sim] | time to collect all 61 frames at per-frame read probability p: p=0.9 **134 s** (90 B/s), 0.7 220 s (55 B/s), 0.5 344 s (35 B/s) |
| Effective rate, growing log | P(all 61 frames in one loop) = p^61 = 0.0016 % at p=0.9 -> **0** |
| Early boot | not deliverable: only the tail exists. 50 KB / 200 KB / 4 MiB: **not possible** (12 KB window) |

Decode of QL1 frames, 720p portrait (v11, 3.78 px/module), 12 trials [sim `e1_ql1`]: typical whole-image decode 92 %, **poor 0 %**; with the new decoder (section 3.8) 100 % / 92 %.

## 2. QL2 in one paragraph

Console bytes (from the first byte of the ring, read in the background) -> reducer (shape dedupe with counted repeats, panics/`CRASH`/`SWEET*` protected,
no line cut) -> units of 8 KiB, each compressed independently (LZ77 + trained Huffman + 12 KiB preset dictionary, no deflate header on the wire)
-> append-only transport stream with absolute offsets and a stable 16-bit session id -> blocks (one per QR) -> generations of K=64 blocks
-> frames: SRC (a plain block) or REP (XOR of a pseudo-random half of a generation) -> base45 in an alphanumeric-mode QR of a fixed version, ECC L, 2 QR codes per
screen -> `tools/qr-log.py` (ROI decode, worker threads) -> `tools/qrl2.py` (fountain solver, record parser) -> `qr2-<sid>.txt`.
Layout and limits: `qrlog/qrstream.h`.

## 3. Candidates, measured

### 3.1 Compression before framing (idea 1)

Offline, on 12 real phone logs (9 xrun merged logs + tools/testdata), leave-one-run-out (dictionary and Huffman table trained on the other runs; ratio = log bytes / bytes
sent) [sim]:

| unit | no dict | 2 KiB dict | 4 KiB | 8 KiB | 12 KiB |
|---|---|---|---|---|---|
| 4096 B | 4.24 | 4.90 | 5.48 | 6.40 | 7.01 |
| 8192 B | 4.80 | 5.45 | 6.09 | 7.09 | **7.75** |

For scale: python zlib -9 dynamic Huffman, no dictionary, 4 KiB units: 3.9; 16 KiB units 5.3; one stream for the whole corpus 8.0 (xz 12.0; the corpus overlaps itself, so the
large-window numbers are inflated). Independent units are the point: a lost block costs only the units it touches. **Chosen: 8 KiB units, 12 KiB dictionary (the shipped table is trained on all 12 files, so
the leave-one-out column is the honest one).** The dictionary is log phrases only (checked for serials, MACs, URLs: none). Cost in the kernel: `qrz.c` 176 lines, `qrstream.c` 486 lines, no allocation; static state ~80 KB for the compressor and 2.1 MiB for `qs_t` (mostly the 2 MiB transport ring).
On the synthetic log (hex digits randomised, `qrsim.synth_log`) the whole chain gives 6.1-6.2x, on the real logs 7.6-7.8x (in-sample dictionary) [sim].

### 3.2 Byte/alphanumeric payloads, QR version, ECC (idea 2)

* Text decoders (OpenCV) mangle binary: payload is **base45** (RFC 9285) in QR **alphanumeric mode**: 2 bytes -> 3 chars of 5.5 bits = 97 % of raw binary, ASCII-safe. (Byte mode + base64 would be 75 %.)
* Frame overhead: 13-16 B header + 2 B block pointer + 4 B CRC = ~8 % at version 10.
* Per-frame payload B (bytes of stream, ECC L, alphanumeric): v8 166, v10 **243**, v11 292, v12 336, v14 424, v16 549, v18 677, v20 812, v22 953, v24 1116, v26 1306, v28 1462 [sim, `qs_init`].
* ECC [sim, 720p portrait s=0.27, 12 trials, 1 tile]: v10 at 4.05 px/module: L/M/Q all 100 % (typical and poor); at 3.60 px/module poor: L 92 %, M 100 %, Q 100 %; v12 (3.12 px/module) poor: L 33 %, M 33 %, Q 50 %.
  Per frame L carries 243 B vs M 187 B vs Q 127 B (v10): ECC buys ~0.1-0.2 px/module of margin for 23-48 % of the capacity; one version more (L) is worth more. **ECC L**, `sweetqr_ecc` to change.
* **Version 21 cannot be decoded by OpenCV** (1/6 frames on a clean render; 22 and 20 are fine, every version 4-30 otherwise 6/6): the encoder maps 21 -> 22.
* The limit is camera pixels per module: whole-image decode needs ~3.9 px (poor) / ~3.3 (typical); with the ROI decoder (3.8) ~3.6 / ~2.7.

Decode rate by camera setup, 2 tiles, ECC L, ROI decoder, 12 trials per cell; B = payload bytes per QR [sim]:

| camera | v | B | px/module | typical | poor |
|---|---|---|---|---|---|
| 720p portrait (s=0.27) | 8 | 166 | 4.59 | 100 | 100 |
| | 10 | 243 | 4.05 | 100 | 92 |
| | 11 | 292 | 3.78 | 100 | 100 |
| | 12 | 336 | 3.51 | 100 | 71-79 |
| | 13 | 392 | 3.24 | 100 | 46-58 |
| | 14 | 424 | 3.24 | 100 | 46 |
| 720p landscape (s=0.48) | 14-22 | 424-953 | 5.8-3.8 | 100 | 96-100 |
| | 24 | 1116 | 3.84 | 100 | 79 |
| 1200p portrait (s=0.45) | 14-20 | 424-812 | 5.4-4.05 | 100 | 96-100 |
| | 22 | 953 | 3.60 | 100 | 46 |
| | 24 | 1116 | 3.60 | 100 | 75 |
| 1200p landscape (s=0.60) | 18-28 | 677-1462 | 6.0-4.2 | 100 | 100 |

(`tools/qrsim.py modules --cam 720|1200 [--landscape] --scale S --ver ... --tiles 2 --quality typical poor`; files `c720p/c720l/c1200p/c1200l` of the run. The 1200p v21 rows (38 %/29 %) are the OpenCV v21 problem.)
Defaults chosen: **v10, ECC L, 2 tiles** (safe on a `poor` 720p-portrait view: 243 B per QR); the table shows what to raise it to for a better view
(`--args "sweetqr_ver=14"` etc.). A QL1 chunk-200 frame is v11 at 200 B: QL2 v10 carries 243 B per QR AND 6-7x more log per byte.

### 3.3 Fountain coding (idea 3)

Dense random linear code over GF(2) inside generations of K blocks; systematic first pass (SRC), then repair frames (REP) whose subset of blocks
is a nonlinear hash of (generation, seed, block): the host knows which blocks a REP covers without any header. Decoding = Gaussian elimination on K-bit ints,
any K+~2 independent frames per generation. Expected overhead 1.6 frames per generation (2.5 % at K=64).
Bug found by the tests: my first subset generator (xorshift32) is GF(2)-linear, its masks span at most 32 dimensions, so K > 32 never decoded; replaced
by a murmur3 finaliser per block (selftest: repair frames alone rebuild generations of 4, 32, 128, 512 blocks).
Time to complete a 600 KB real log, 2 tiles v10, mean of 2 seeds, ideal = 147 s (p=.9) / 220 s (p=.6) [sim, `sweepk`]:

| K | inline repairs | p=0.9 | p=0.6 |
|---|---|---|---|
| 16 | 0 % | 198 s | 333 s |
| 32 | 0 % | 182 s | 291 s |
| 64 | 0 % | **174 s** | **246 s** |
| 128 | 0 % | 165 s | 248 s |
| 64 | 12 % | 192 s | 238 s |
| 128 | 12 % | 182 s | 248 s |

Chosen K=64, no inline repairs (`sweetqr_k`, `sweetqr_red`). The rotation-only alternative (same stream, plain blocks re-sent in a cycle, `sweetqr_fec=0`) versus fountain, complete-log times [sim]:
see the delivery table: rotation is 2.2x slower at p=0.9 for 200 KB, 3.0x at p=0.5, 2.0x at 4 MiB p=0.9 and 6x at 4 MiB p=0.5, because the last missing blocks of a rotation must each come around again.
The first scheduler (K=16, repair frames spread evenly, half of them on the newest generation even for a static log, nothing for the partly filled last generation) needed 3293 s for the 4 MiB synthetic log at p=0.9; the final one 1661 s (ideal at this link rate: ~1100 s).

### 3.4 Stable append-only stream (idea 4)

Done by construction: absolute block numbers and unit offsets, a session id fixed for the boot (16 bit), transport ring of 2 MiB (about 11-16 MB of log; when it wraps, `lo` advances and
the report says so), `hi`/`lo` in every frame so the host knows what exists. New blocks are sent first (2 of 3 slots), the third slot rotates over repair frames of all generations;
while the log is still growing every other repair slot goes to the newest generation. Early boot is kept: the encoder reads the ring from its first byte while the
boot runs (before the takeover delay) instead of keeping a tail. Selftest: late joiner (150 frames missed), two sessions interleaved, a log growing while frames are on air, wrapped ring.

### 3.5 Log-content reduction (idea 5)

`qs_feed` reducer: shape-hash (digits ignored) window of 32; **protected** lines (start with `SWEET`, or contain panic/PANIC/Panic/CRASH/Assertion/esr/Kernel trap) are dropped only when
exactly repeated; dropped runs are counted in a `[~N similar lines dropped]` line in front of the next kept line; no 159-char cut (lines are split at 1024 with a forced newline); CR dropped, NUL -> `.`.
Property tested: kept + counted = input lines, no kept line altered. Timestamps are not added (the log lines carry their own); `qr2-<sid>.json` has the time each part arrived.

### 3.6 Tiles (idea 6)

The screen is 1:2.2, a square QR uses 41 % of its height: two stacked QRs keep the module size, so a 720p portrait view gets 2x the data per screen (3 do not fit at v10: 2.97 px/module).
Decode per tile [sim, 720p portrait v10 at 4.05 px/module, ROI decoder, 12 trials]: typical 100 % (1 and 2 tiles), poor 100 % (1 tile) / 92 % (2 tiles). `qs_draw` supports up to 4 tiles.
Delivery of 200 KB at p=0.9, 1 vs 2 tiles [sim]: synthetic 144 s vs 74 s (1.9x), real logs 106 s vs 54 s (2.0x).

### 3.7 Pacing and iOS redraws (idea 7)

Root cause read from source [src] (`iokit/Kernel/arm/SweetFramebuffer.cpp`): `AppleCLCD::vsyncFired` copies the latest iOS frame (`present`) into the panel memory every `60/sweetfb_hz`
vsyncs whenever iOS swapped, and on a power-off request fades the panel (halving every channel per vsync, then black); the QR thread draws into the same memory
and `sweet_console_muted` only stops *text*. That explains "the QR flashes a few seconds then the logo returns" (a redraw by backboardd) and the black screen ~580 s into a run (idle display-off) [hw, cause then unknown].
Patch 0094 adds `sweet_qr_hold` (set at takeover, `sweetqr_hold=0` disables): vsync still notifies clients, but nothing is presented or faded. **Not yet confirmed on hardware.**
Not done: black/white calibration frames and border sync patterns (the sequence is in every frame header, duplicates are free, a torn frame fails its CRC); see TP-010 for a calibration mode.

### 3.8 Host decoder (idea 8)

* **ROI decode** (`tools/qrcam.py`): OpenCV `detectMulti` finds QR corners even when decoding fails (24/24 at 2.7 px/module); the code is rectified to 3x / cropped and magnified 4x, unsharp-masked and decoded by both OpenCV decoders.
  Whole-image decode vs ROI, v10 single tile, 720p, 12 trials [sim `e5_curve`]:

  | px/module | whole typical | ROI typical | whole poor | ROI poor |
  |---|---|---|---|---|
  | 2.4 | 0 | 50 | 0 | 0 |
  | 2.7 | 0 | **100** | 0 | 0 |
  | 3.0 | 0 | 92 | 0 | 0 |
  | 3.3 | 0 | 100 | 0 | 67 |
  | 3.6 | 42 | 100 | 0 | 92 |
  | 3.9 | 25 | 100 | 0 | 100 |

  (the `whole` column is the QL1-era decoder: 1x, 0.5x, 0.35x). QL1 chunk 200 (3.78 px/module): typical 92 -> 100 %, poor 0 -> 92 %; chunk 240: 0 -> 100 % / 0 -> 100 %; chunk 480: 0 -> 100 % / 0 -> 0 %.
* **Averaging** several camera frames of one screen before decoding: one hand-run test (3.3 px/module, poor, same pose, new noise): 8/16 -> 13/16 with 3 frames, 14/16 with 5; the `modules --average` matrix (12 trials) shows no
  consistent gain at 3.2-3.5 px/module (67 -> 50 -> 75 %): noise-limited cases gain, blur-limited do not. Kept as `--average N` (off by default).
* Multi-code: up to `--tiles` codes per frame (`detectMulti`, then erase-and-retry); worker threads (`--workers`), frames that arrive while all are busy are dropped (the camera repeats a screen anyway).
* ESP32-CAM: higher resolution is the most effective lever (table 3.2: 1200p allows 2-3x the module count). Resolution/quality are set over HTTP `/control` with the password from `.env` (never printed here); `/schema` lists the values. The reader prints camera img/s, decoded img/s and dropped frames.
* Live "what is missing": every 5 s `QL2 <sid>: N frames (D dup), blocks have/need (missing), units, log B, gaps k: @offset+len`.

## 4. Delivery time, 2 tiles, v10, ECC L, 800 ms per screen [sim `deliver`]

p = chance that a displayed QR is read at least once (several camera frames per screen: **[guess]** p=0.9 is what a `poor` camera at ~3.6-3.9 px/module might give with ~2.4 captures per screen), burst = chance a whole screen is lost.
Seconds until the host has the **complete** log (every byte). QL1 can only ever show the last 12 kB and only when the log is idle.

| log | size | p | QL1 (12 kB tail, idle) | QL2 rotation only | **QL2 fountain** |
|---|---|---|---|---|---|
| synthetic 6.1x | 50 KB | 0.9 | 134 s | 29 s | **16 s** |
| | | 0.5 | 344 s | 77 s | **29 s** |
| | 200 KB | 0.9 | n/a (tail only) | 163 s | **74 s** |
| | | 0.7 | n/a | 224 s | **93 s** |
| | | 0.5 | n/a | 394 s | **131 s** |
| | 4 MiB | 0.9 | n/a | 3325 s | **1661 s** |
| | | 0.5 | n/a | 16499 s | **2707 s** |
| real logs 7.7x (in-sample dict) | 50 KB | 0.9 | 134 s | 22 s | **13 s** |
| | 200 KB | 0.9 | n/a | 166 s | **54 s** |
| | | 0.5 | n/a | 349 s | **96 s** |
| | 4 MiB | 0.9 | n/a | 3277 s | **1315 s** |
| | | 0.5 | n/a | 10339 s | **2141 s** |

Effective rate for the 200 KB case (real logs, p=0.9): 200 KB / 54 s = **3.8 KB/s of log** against QL1's 90 B/s on an idle log (**42x**) and 0 on a growing one.
Redraw bursts (p=0.7, 15 % of screens blanked, 200 KB): fountain 86-122 s, rotation 243-429 s.
Growing log (real logs, produced at 1 / 2 / 4 KB/s for 120 s; seconds after the log stopped until the receiver has everything): 5 / 5 / 46 s at p=0.9 and 5 / 14 / 75 s at p=0.7 (4 KB/s exceeds the ~3.7 KB/s the link carries at v10 and lags by minutes).

Steady-state capacity (log bytes/s = tiles x B / 0.8 s x ratio 6.2 (synthetic) .. 7.7, p = 1) by camera view and quality [sim table 3.2]:

| view | version | B | log KB/s (`poor` view) | log KB/s (`typical` view) |
|---|---|---|---|---|
| 720p portrait | v10 (default) | 243 | 3.8-4.7 | 3.8-4.7 |
| | v11 (poor) / v14 (typical) | 292 / 424 | 4.5-5.6 | 6.6-8.2 |
| 720p landscape | v22 | 953 | 14.8-18.3 | 14.8-18.3 |
| 1200p portrait | v18 (poor) / v24 (typical) | 677 / 1116 | 10.5-13.0 | 17.4-21.5 |
| 1200p landscape | v28 | 1462 | 22.7-28.1 | 22.7-28.1 |
| QL1 chunk 200 (any view) | v11 | 200 | **0.25 nominal, 0.09 effective, idle log only** | |

## 5. What was not measured

* Anything on the real camera/phone (fps of the ESP32-CAM at each framesize; glare; moire between the OLED and the sensor; focus drift). **[guess]**: the real p per displayed QR is 0.7-0.95 at v10 on a view like the old one.
* The real compression ratio of future logs (the dictionary is trained on today's boot).
* Kernel cost of the encoder at `-O0` (the `#pragma clang optimize off` the eor3 workaround needs): the pump feeds 64 KB per screen (256 KB per second before takeover); a 4 MiB backlog should take seconds to a minute **[guess]**; check `SWEET QR2:` stat lines.
* Compile of patch 0094 inside XNU (it applies on the stack and its sources pass `clang -fsyntax-only -Wall -Wextra -Werror` against stub headers, `tools/test_qrl2.py`; CI compile is the real check).

## 6. First hardware cycle (driver)

See the end of the session report / HANDOFF "QL2 QR LOG": CI kernel with patch 0094, then `tools/xrun ... --qr 70 --args "sweetqr_fmt=2" --expect "SWEET QR: taking over the screen \(QL2"`, camera reader status line, `sweetqr_fmt=1` as the fallback.

## Reproduce

```sh
PYTHONPATH=~/pixelios-artifacts/pylib python3 tools/qrsim.py modules --cam 720 --scale 0.27 --ver 8 9 10 11 12 13 14 --tiles 2 --quality typical poor --trials 12
PYTHONPATH=~/pixelios-artifacts/pylib python3 tools/qrsim.py modules --proto ql1 --ver 140 200 240 480 --scale 0.27 --decode whole   # QL1 baseline, old decoder
PYTHONPATH=~/pixelios-artifacts/pylib python3 tools/qrsim.py deliver --p 0.9 0.7 0.5 --sizes 51200 204800 4194304
PYTHONPATH=~/pixelios-artifacts/pylib python3 tools/qr-log.py --selftest        # frame-level faults + rendered screens
python3 tools/qrz-train.py --dict 12288 --unit 8192 <dir of logs>                  # retrain the compressor tables (then tools/make-qr2-patch.py)
```

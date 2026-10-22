---
id: TP-010
tool: "tools/qr-calibrate"
status: proposed
proposed_by: "Claude Sonnet 5.5 (delegate)"
date: 2026-10-22
exposed_by: "QL2 design session: tools/qrsim.py decode matrix, docs/research/24-qr-throughput.md"
extends: "tools/xrun"
related: ""
usage: "tools/qr-calibrate --kernel K [--cam-res 720|1200] [--ms 800] [--label NAME]"
refs: "-"
---

## checked
Searched tools, docs and proposals for calibrate/qr: nothing proposed or built (`tools/tool-proposals check calibrate` = NONE). `tools/qrsim.py` models
a camera but cannot know THIS ESP32-CAM, its focus, its distance from the phone or its frame rate; `tools/xrun` runs one boot with fixed boot-args.

## why
The QL2 defaults (QR version, tiles, frame time) are chosen from a simulated camera. The right values depend on how many camera pixels one QR module
covers (decode threshold ~2.7-3.9 px/module in the simulator, sharp) and on the camera's real frame rate: each wrong guess costs one hardware
cycle (xrun is ~10-12 min with `--restart 600`) and a human re-aim. One calibration boot replaces the trial-and-error loop and also yields the
camera fps, which only the live reader can measure.

## output
One boot with `sweetqr_calib=1` (new kernel mode: shows known test frames, one version/tile combination for ~6 s each: v6..v28 x 1,2 tiles),
the reader decodes them, then ONE report: table of version x tiles -> decode % per displayed frame, camera img/s, recommended boot-args line
(`sweetqr_ver=N sweetqr_tiles=T sweetqr_ms=M`), last line `RESULT: PASS recommend v12 x2 tiles, 800 ms` (PARTIAL when nothing above 90 % decodes, ERROR when
the camera or phone is not reachable). Exit 0/1/2. Logs in `logs/runs/<UTC>-calib-*/`.

## risks
Same as `tools/xrun` (boots an XNU image from fastboot, bluejay only, no partition writes, `--restart` self-reset). Needs the kernel-side calibration
mode (small: reuses qs_draw). Must not print the camera credentials (.env).

## amendments
(none)

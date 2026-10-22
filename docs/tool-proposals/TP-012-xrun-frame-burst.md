---
id: TP-012
tool: "tools/xrun --burst-on REGEX"
status: proposed
proposed_by: "Sonnet 5.5 (delegate, screen-off investigation)"
date: 2026-10-22
exposed_by: "cannot see a sub-second screen-off fade: camera frames are kept every 4 s (luma) / ~16 s (jpg)"
extends: "tools/xrun"
related: "TP-010"
usage: "tools/xrun ... --burst-on 'SWEET FB: display off' [--burst-s 3]"
refs: "tools/qr-log.py (--save-every), logs/runs/20261022T180139Z-about1"
---

## checked
`grep -il "burst\|fps\|camera" docs/tool-proposals/*` only hits TP-010 (QR calibration, camera fps as a side result). `tools/qr-log.py --save-dir/--save-every` keeps one frame per N seconds; nothing keys off a kernel log line.

## why
A power-button screen-off lasts well under a second. Every run so far has luma at ~4 s and jpg at ~16 s spacing, so no frame can show the fade (logs/runs/*-about*/frames). Each hypothesis test costs a ~10 min hardware cycle.

## output
While the usbnet listener sees REGEX, qr-log saves every camera frame for --burst-s seconds (default 3) into `frames/burst-<n>/` with the usbnet timestamp in the name; xrun report gets one line `BURST: n frames, camera fps X, luma first/last`. RESULT line unchanged.

## risks
More jpg on disk (a few MB per burst). Camera fps is unknown (maybe 5-15); if <10 fps a 100-300 ms fade still gives only 1-3 frames.

## amendments
(none)

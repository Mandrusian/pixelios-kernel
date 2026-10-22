
## Read-only dump result (2026-10-22, observed on hardware, build 6693257)
`probe-run --touch-probe=railsr` did NOT hang: the camera shows the whole dump on screen (ends with `TOUCH: verified`). The
`RESULT: HANG` came from `--expect` strings that only exist on the screen console, not in the USB capture (usb.txt ends at the UFS/USB
register dump). Observed: `ACPM q len=5 elem=16 words=4`, req/rsp base 0x2071 2f0 / 0x2071290, buffers hold plausible 0x1c9..0x1cb
words, tool prints "descriptor validated" but "not verified against a dump". Next: carry the TOUCH/ACPM lines to the USB log, then flip
layout_verified/elem_unit_verified after reading the full camera dump. The first run's "hang" was probably the same false verdict (the phone
also needed a button press only because the shim idles after the dump).

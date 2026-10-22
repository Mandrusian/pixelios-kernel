# device/

One directory per phone: `device/<vendor>/<codename>/{board.json,docs/,hardware/}`. `board.json` selects the SoC and
holds per-device facts (panel, framebuffer, DRAM layout). `hardware/` holds sanitized captures only.

- Strip serial numbers, IMEI, MAC addresses and account data from anything committed here.
- Raw partition dumps, backups and firmware never go in git.
- New phone: `tools/pix new-device`, then fill `docs/PORTING.md` honestly, including what is unknown.

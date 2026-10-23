# Wi-Fi coverage: how far are we from a working BCM4389 on the Pixel 6a

Goal (user): real Wi-Fi inside the iOS experience (Settings lists networks, a WPA2 join passes traffic), no USB-Ethernet substitute.
Same format as `docs/GPU-COVERAGE.md`: each line is `- [X] ID text`, X = the highest evidence reached:
`" "` not started, `r` researched from source/RE (rule 5: read from source), `b` built/host-tested (not run on the phone), `h` verified on hardware (add date + run label).
Percent = items at that level or above. Keep the evidence after the text. Last updated 2026-10-23 (kernel `wip/w3a-wlan-link`).

## A. Hardware bring-up (shim, pre-XNU, `--wlan-link=N`)
- [h] A1 PCIe1 power-up chain: rails, WLAN_EN, clocks, PHY, PERST, LTSSM to L0 (S00-S23; run w3a-n23, 2026-10-23, restore=ok)
- [h] A2 Link report: Gen1 x1 DLL-active, LNKCAP Gen4 x2, capability chain of the RC (S24; run w3a-n24)
- [h] A3 iATU layout read: unrolled iATU, outbound region 0 at reset (S25; run w3a-n25)
- [h] A4 Outbound iATU region 0 = CFG0 programmed, endpoint answers: vendor 0x14E4, device 0x4441, class 0x028000 (S26; run w3a-n26, review 7)
- [h] A5 Endpoint config-space dump + iATU region 1 reset values, read-only (S27; run w3a-n27, 2026-10-23, kernel 2f54b00, review 8 SAFE WITH CONDITIONS, 63 rows, restore=ok): BAR0/BAR2 = 0x4 (64-bit memory BARs, unassigned), capability chain 0x48 PM -> 0x58 MSI -> 0x68 vendor -> 0xAC PCIe, EP LNKCAP Gen2 x1, link Gen1 x1, subsystem 0x4389/0x14E4, PCI_BAR0_WIN 0x18001000, AER at 0x100, L1SS at 0x240, ATU region 1 at reset
- [ ] A6 (next) BAR sizing and assignment inside 0x60000000+0xFF0000, EP command MEM|MASTER (first CFG writes: new hardware writer, Opus review)
- [ ] A7 Outbound MEM iATU region 1 and first BAR0 register read (chip id via the backplane window)
- [ ] A8 RC setup S22 (bus numbers, command bits, Gen2 retrain): deferred, not needed to enumerate

## B. Reference research (host only)
- [r] B1 wifid / IO80211 / ADT requirements (research 26)
- [r] B2 Power-up sequence from Google sources: cal.c, rc.c, DT, pinctrl (research 29)
- [r] B3 RC setup and outbound ATU recipe (research 31)
- [r] B4 BAR assignment plan and Broadcom DHD source facts: BAR/window/chipid (research 32)
- [ ] B5 Firmware + NVRAM files for the BCM4389 identified, licence/extraction path decided
- [ ] B6 DHD firmware download protocol (backplane, ARM core reset, TCM download, NVRAM tail) cited per step
- [ ] B7 apple80211 ioctl surface needed for Settings scan/join (from the wifid RE)

## C. Shim: firmware boot
- [ ] C1 Backplane window setup and core enumeration (EROM) from the shim
- [ ] C2 Firmware + NVRAM download over BAR2/TCM
- [ ] C3 ARM core released, firmware alive (shared-info / console ring readable)
- [ ] C4 Hand the link to XNU (shim leaves it up, or XNU brings it up itself)

## D. XNU kernel side
- [ ] D1 PCIe root complex driver in XNU (iATU, BAR, interrupts)
- [ ] D2 Broadcom dongle protocol driver (msgbuf rings, IOCTL, events)
- [ ] D3 IO80211 family nub + apple80211 ioctls (scan first)
- [ ] D4 Settings lists networks (scan)
- [ ] D5 WPA2 join and traffic (milestone 2)

## E. Safety and process
- [h] E1 Every hardware step table-driven with an independent allowlist and always-run teardown (wlanlink.c; mutants fail the host test, review 8)
- [h] E2 Every step reviewed (Opus 5.5, adversarial) before its first run (reviews 1-8)

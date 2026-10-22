# Gate 0 results (Pixel 6a, serial redacted), 2026-10-20

Observations (seen on the device) are separated from guesses.

## Observed
- Phone: `bluejay`, factory build `google/bluejay_beta/bluejay:16/BP31.250610.004`, bootloader `bluejay-16.3-13642543`,
  baseband `g5123b-145971-250708-B-13746081`, slot b active, slot a unbootable.
- `fastboot flashing unlock` needed the OEM-unlocking toggle first. After unlock: `unlocked: yes`, data wiped.
- `fastboot fetch persist|efs` refused: "not allowed when locked", and after unlock "Fetching restricted partition".
- `fastboot oem uart enable`: OKAY.
- `fastboot boot` of the **unsigned** v4 image: transfer OKAY, phone stays in fastboot (did not run).
- `fastboot boot` of the **AVB-footed** v4 image (`shim-boot-avb.img`): phone left fastboot and the shim **ran** and drew its report.
  (LZ4 variants not tried, not needed.)
- Report tail as read by the user: UFS `hce=0x1 hcs=0x10F cap=0x1303FF1F ver=0x300`;
  READ CAPACITY on LUN 0,1,2,3 -> `rc=1` (= timeout in `scsi()`); then `PROBE complete; reset in 120s`.
- Earlier report pages (EL, MIDR/ID regs, CNTFRQ, GIC, PSCI, FDT memory map, DECON, watchdog, CPUs) were not captured.

## Interpretation (guesses)
- ABL accepts only the AVB-footed image for `fastboot boot`; the host controller is left up by ABL (H3 resolved in our favour).
- UFS commands time out: candidates are DMA blocked by S2MPU for the shim's buffer addresses, UTRD/UPIU layout details on the
  Exynos host, or the doorbell/IS handling. Next probe must dump IS, UTRLDBR, UTRD/OCS after the timeout.

## Run 2 (2026-10-21): USB serial log works; first full probe report captured

Observed: the `--usb-gadget` AVB image (commit 2b0b68b) enumerated on the PC (`1209:0001`, `/dev/ttyACM0`, events `reset` and `conndone speed=0`) and
streamed the whole probe report (348 lines, `device/google/bluejay/hardware/probe-log-2026-10-21-run1.txt`). Facts: entry EL2; MPIDR 0x81000n00; cores 0-3 A55, 4-5 A76,
6-7 X1; 16K granule on all cores; no PAC; CNTFRQ 24.576 MHz; PSCI 1.1 with plain CPU_ON for all secondaries; both watchdogs armed by ABL (disabled by the
shim); DECON trigger 0x3070 -> 0x3061; no framebuffer node in the FDT; DRAM banks 0x80000000/0x880000000/0x900000000 (2 GiB each), many no-map carve-outs in
the low bank, none in 0x880000000-0x980000000; UFS host up but every command from buffers at 0x8002A800 timed out (OCS 0x0F, doorbell stuck, uecpa 80000010).
ABL's own UFS list was at UTRLBA F8C40000 (abl range). Guess (untested): UFS DMA to low DRAM is blocked; the next build tries buffers in the abl range (A0/A7).
Not captured: whether the panel displayed anything (the screen was not watched).

## Phase 1 XNU image on the Pixel (2026-10-21, session 2 -> 3)

Observed (user read the screen): the upstream sweet XNU kernel boots on the Pixel 6a with the Pixel memory layout and draws kernel text on the panel
(black background). Reached: Skywalk self-tests PASSED, bsd_autoconf, ifnet/DLIL init, AMFI/Sandbox stand-in policies, HFS registered, SWEET SURF/HID/BATTERY
shims, `SWEET FB: AppleCLCD 1080x2408 ... at 0xfac00000`, cpu-0 started (1 CPU, SGI1 OK), mcache/mbinit, DLIL self-tests passed, and then it sits at
`Waiting on <IOProviderClass IOMedia, Content Apple_HFS>` (no root device yet; expected without UFS). No panic seen. The text could not be copied exactly
(read from a photo/transcription, digits unreliable). Phone was recovered by hand into fastboot.
Interpretation: XNU bring-up is not the blocker; storage (UFS cold re-init) is.

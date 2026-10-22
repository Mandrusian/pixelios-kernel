# Pixel 6a Gate 0 runbook

This is a temporary boot only. Do not use `fastboot flash`, repartition the
device, or change the active slot. The unsigned v4 `fastboot boot` path is
unverified on bluejay; stop if both test images are rejected.

## Before entering the bootloader

1. Save personal files and anything needed from Android somewhere off-device.
   `fastboot flashing unlock` factory-resets userdata.
2. Record the installed build ID while Android is running:

   ```sh
   adb shell getprop ro.build.id
   ```

3. On the phone, enable Developer options and **OEM unlocking**. Do not unlock
   yet if per-device calibration data is needed.
4. Download the [Google factory image](https://developers.google.com/android/images)
   matching that exact build ID. Keep the
   archive and extracted `bootloader-bluejay-*.img` and `radio-bluejay-*.img`
   files off-device as stock recovery material; Google factory images include
   these firmware images. Record the bootloader version too once in fastboot.
5. Per-device calibration is different from the stock factory images. Pixel
   calibration lives in `persist` / `efs` / `modem` data and can only be read
   with root access. If the device is not already rooted, this runbook cannot
   back those partitions up before unlock. Decide explicitly whether to proceed
   without those per-device backups; if not, stop here and arrange a root access
   backup path first.

## Unlock and boot

Power the phone off, then hold **Power + Volume Down** to enter the bootloader.
From the repository root, record the reported identifiers:

```sh
fastboot devices
fastboot getvar product
fastboot getvar version-bootloader
fastboot getvar version-baseband
fastboot getvar current-slot
fastboot getvar unlocked
```

Check that `product` is `bluejay` and the bootloader/build information matches
the factory image you saved. Then unlock and confirm the on-device prompt. This
wipes userdata and reboots the phone:

```sh
fastboot flashing unlock
```

Return to the bootloader with **Power + Volume Down** if needed. Enable the
Pixel debug UART, then send the unsigned temporary image:

```sh
fastboot oem uart enable
fastboot boot shim/out/shim-boot.img
```

If the bootloader refuses the unsigned image and returns to fastboot, try the
AVB hash-footer variant:

```sh
fastboot boot shim/out/shim-boot-avb.img
```

If both uncompressed variants are rejected by ABL (since standard GKI kernels use LZ4 compression), try the LZ4-compressed variants:

```sh
fastboot boot shim/out/shim-boot-lz4.img
fastboot boot shim/out/shim-boot-lz4-avb.img
```

The AVB footer uses a generated test key and does not establish that bluejay's
bootloader accepts arbitrary keys for `fastboot boot`. Do not flash either
image. The probe waits about 120 seconds and requests PSCI SYSTEM_RESET. To
return to fastboot after that reset, hold **Power + Volume Down** during
startup. If the probe hangs, hold Power until the phone shuts down, then hold
**Power + Volume Down** to enter the bootloader again.

## After the run: photograph every page

Since the v2 shim, the whole report is also captured in a RAM log and
replayed after the live run: one screen-sized page at a time, each held
about 12 seconds with a `PAGE n/m  PHOTO EACH PAGE` footer, looping the
full log for about 10 minutes before PSCI SYSTEM_RESET. **Photograph
every page, in order, including the footer number** - the earlier
sections (entry EL, ID registers, CNTFRQ, GIC, PSCI, FDT memory map,
DECON, watchdog, CPUs) that scrolled by unread in the first run are all
in the replay. The panic path replays the same way, so if anything
faults, keep photographing: the blue panic screen comes first (~40 s),
then the pager. If the UART is captured, the replay also emits one
`PAGER page n/m` line per page over UART for correlating photos.

## Pulling the log over USB after the reset (pstore)

The shim also writes its whole RAM log into the Android `ramoops`
console zone (reserved RAM that survives a warm reset) right before PSCI
SYSTEM_RESET; Android then exposes it as
`/sys/fs/pstore/console-ramoops-0`. Whether bluejay's ABL preserves that
region across the shim-to-Android transition is **the unknown this run
tests** - the log begins with a `PIXELIOS PSTORE v1 build=<hash>` marker
so survival is observable.

One-time Android setup (first boot after unlocking, while the phone sits
at the setup wizard):

1. Finish the setup wizard (skipping Google sign-in is fine; Wi-Fi is
   not required).
2. Settings > About phone > tap `Build number` seven times to enable
   Developer options.
3. Settings > System > Developer options > enable **USB debugging**.
4. Connect the USB cable to the PC; accept the "Allow USB debugging?"
   prompt and check **Always allow from this computer**.

Then run the automation from the repository root (it checks
`fastboot getvar product` and refuses anything but `bluejay`):

```sh
python3 tools/pstore-pull.py run shim/out/shim-boot-avb.img
```

It runs `fastboot boot`, waits up to 240 s for the phone to come back as
an adb `device` (the shim resets itself into Android), reads
`/sys/fs/pstore/console-ramoops-0` directly (falling back to
`adb bugreport` when permissions block the read), saves the log to
`logs/probe-<UTC timestamp>.txt`, then reboots back to the bootloader.
Manual equivalent: `adb shell cat /sys/fs/pstore/console-ramoops-0` and
`python3 tools/pstore-pull.py extract <file>`.

If the marker is missing, ABL scrubbed or reused the region (or a second
reboot replaced it with the Android console log - pull right after the
first reboot). The photo pager in the default build remains the fallback
for the same run.

For a faster loop that skips the ~10-minute photo pager and resets ~5 s
after the final log save, build with
`shim/build.sh --board bluejay --avb --pstore-fast`. The default build
keeps the pager.

## UART

`fastboot oem uart enable` enables the debug UART. The bluejay console is
115200 8N1 on a 1.8 V USB-C SBU UART adapter. SBU TX/RX orientation depends on
the cable; do not connect a 3.3 V or 5 V UART. USB-C power delivery may be
disabled while the debug UART is enabled. These wiring and firmware behaviors
are not verified on the target device; see `docs/research/05-fact-check.md`
sections 8 and 9.

Known code risk (review 2, H2 residual): the console's very first UART
register read (UFCON, before any output) is unguarded. If the bootloader
left the UART block clock-gated, that APB read can stall the bus in a way no
exception handler can recover. This is an accepted Gate 0 risk rather than a
code change: guarding it means rerouting the console init behind the probe
fault machinery. If the first boot stops with no screen output at all,
suspect this read first and move it behind the guarded `try_read32` path in
`shim/shim.c` before retrying.

## USB gadget run (host side)

Stage 2 replaces the photo pager with a live serial log: the shim
enumerates as a CDC-ACM gadget (1209:0001) and the host reads it as
`/dev/ttyACM0` - no IP setup, no root.

Once available, build the image with the `--usb-gadget` flag (the flag
does not exist yet):

```sh
(cd shim && ./build.sh --board bluejay --avb --usb-gadget)
```

Start the host-side capture BEFORE `fastboot boot`. It waits (default
10 minutes) for the gadget to enumerate, raises DTR, and tees every
line to stdout and to `logs/usb-<UTC timestamp>.txt` (gitignored). It
stops cleanly when the device disappears and prints a final
`captured N bytes to PATH` line:

```sh
python3 tools/usb-log.py capture
# second terminal, phone in fastboot:
fastboot boot shim/out/shim-boot.img
```

`python3 tools/usb-log.py wait` only prints the tty node once it
appears; `python3 tools/usb-log.py extract <file>` copies a file
through unchanged (used by the tests).

Expected on-screen lines: TBD.

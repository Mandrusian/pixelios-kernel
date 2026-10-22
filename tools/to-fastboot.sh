#!/bin/bash
# Get the Pixel back into fastboot without a button press after a test reset.
# A software reset (shim REBOOT_BL / XNU sweetrestart=) lands in recovery because Android no longer boots from the iOS userdata;
# adb works there, so: wait for fastboot, or for adb (recovery/device), verify bluejay, `adb reboot bootloader`.
#   tools/to-fastboot.sh [timeout_s=240]
here=$(dirname "$(readlink -f "$0")")
exec "$here/phone" goto fastboot --timeout "${1:-240}"

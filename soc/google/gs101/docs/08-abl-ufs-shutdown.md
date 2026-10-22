# 08 - What ABL's UFS shutdown does (decompiled with REA + Ghidra)

Source: `abl.img` from the factory image `bluejay-cp3a.260905.009` (bootloader 17.0; the phone runs 16.3, whose log shows the identical messages), header stripped
(16 KiB, code from file offset 0x4000), analysed as raw AArch64 at base 0 with Ghidra (headless). Addresses below are in that stripped image.
Observed = read from the disassembly; Guess = marked.

`FUN @0x1f2a4` is `ufs_shutdown(hba)` (the only reference to the string "shutdown complete" at 0xea8a1, adrp+add at 0x1f5e0):

1. `if hba->state(+0x8b4) == 2 (already shut down) return 0`.
2. Releases two pending resources (clears flags at +0x9a0/+0x9a1, calls 0x29dc4 / 0x2a5d8 with timeout 1000).
3. `if state == 1` (device suspended): call `0x1b220` (resume); on failure print "failed to resume ufs for shutdown" (string 0xf99af) and abort.
4. Calls `0x1dc44` (flush/prepare), then up to **3 tries** of `0x1c93c(hba, 0xD0, 0x1B, ...)` with a parameter word = 3:
   **SCSI START STOP UNIT (opcode 0x1B) to the UFS Device well-known LUN 0xD0 with power condition 3 = POWER DOWN** (guess for the power-condition field: the
   word 3 is stored just before each call; error string at 0xec92a).
5. Calls `0x1b5b8(hba, 1, 3, 2, ...)` (a UFS query; guess: READ ATTRIBUTE bCurrentPowerMode) and requires the result **0x33** = UFS PowerDown; otherwise prints an
   error and fails. Stores dev power mode = 3 in the state.
6. Calls `0x1b388(hba, 0x17, 0, 0, 0)`: **UIC command 0x17 = DME_HIBERNATE_ENTER** (the link goes to Hibern8).
7. Calls the vendor hook `hba->vops[+0x68](hba, 10, 0)` (guess: Exynos "pre shutdown" notify), sets `state = 2`, prints "shutdown complete".

**Consequence (observed on the phone, now explained):** after ABL the UFS DEVICE is in PowerDown (bCurrentPowerMode 0x33) and the UniPro link is in Hibern8, while the host
registers, CMU gates and PHY isolation stay on (survey: HCS 0x10F, local DME_GETs fine, DME_PEER_GET fails, GPIO_OUT=1). Commands cannot work until the device is brought
back: our `ufsinit.c` does a device hardware reset (HCI_GPIO_OUT 0->1) + host SW reset + HCE re-enable + DME_LINKSTARTUP, which is exactly the Linux resume-from-PowerDown
path. A softer alternative (not needed): H8 exit (UIC 0x18) + START STOP UNIT active (pwr cond 1) without a device reset: UNTESTED.

Method note: REA/Ghidra defined 2,673 functions from the entry but not this one; it was found by scanning for the `adrp+add` pair that loads the message string and
disassembling the gap with `aarch64-none-elf-objdump -b binary -m aarch64`. Useful next ABL questions: where ABL arms the two watchdogs, what it programs into the
S2MPU before handing over (string "S2MPU"), and the EL2 handoff state.

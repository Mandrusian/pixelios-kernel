# Local shim build setup

The shim needs an AArch64 bare-metal compiler, GNU binutils, Python 3, and the
Android boot-image tools. No root access is required.

## Toolchain

Download Arm GNU Toolchain's `aarch64-none-elf` Linux x86_64 archive from
<https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads>, then
extract it under `~/.local/opt`. `shim/build.sh` uses
`aarch64-linux-gnu-` from `PATH` when available; otherwise it recognizes the
Arm toolchain path used in this setup. To select another installation, set
`CROSS_PREFIX`, or set `CROSS_CC`, `CROSS_OBJCOPY`, `CROSS_READELF`, and
`CROSS_NM` individually.

Fetch the AOSP scripts into the ignored `tools/third_party/` directory:

```sh
tools/fetch-android-tools.sh
```

The fetch script downloads `mkbootimg.py`, its GKI certificate helper, and
`avbtool.py` from AOSP Gitiles. `MKBOOTIMG`, `AVBTOOL`, and `PYTHON` can
override their defaults.

## Build

```sh
shim/build.sh --board bluejay
shim/build.sh --board bluejay --avb
shim/build.sh --board sweet
```

Bluejay outputs are `shim/out/shim-boot.img` and
`shim/out/shim-boot-avb.img`. The AVB test key is generated at
`shim/out/bluejay-testkey.pem`, which is ignored by git. Increase
`AVB_PARTITION_SIZE` if the target boot partition is larger than the 64 MiB
default.

The sweet v2 image uses the ignored stock DTB at
`dtb/boot_unpacked/dtb`; set `SWEET_DTB` to another copy. The board table,
generated C header, tool downloads, images, and private test key are not
committed.

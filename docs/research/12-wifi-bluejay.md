# 12 - Wi-Fi on the Pixel 6a (bluejay): chip, bus, what iOS needs, options

Desk research only. No `adb`/`fastboot`, nothing was run on a phone, nothing was extracted from a phone. Gate 5 (docs/PLAN.md:57-61) is:
iOS Settings shows Wi-Fi and the phone connects to a network.

Tags (AGENTS.md rule 5, same legend as 09): **[src]** read from source (URL/commit/file:line, fetched 2026-10-21), **[obs]** observed on
hardware (repo log), **[guess]**. `TODO(verify)` = a constant or fact I could not source; it must not be used before it is checked.

Source trees (Android Open Source Project, branch `android-gs-bluejay-6.1-android16`; the bootloader on the phone is
`bluejay-16.3-13642543` [obs, device/google/bluejay/board.json], so DT details may differ in a different Android release):

| Short name | Repo | Commit |
|---|---|---|
| `gs101-dts` | https://android.googlesource.com/kernel/devices/google/gs101 | `a9b5edb4a1af922091683feff8f7804ec6986aea` |
| `bluejay-dts` | https://android.googlesource.com/kernel/devices/google/bluejay | `eaeadd8bebb00615ddf305d4423d9c459cff36bf` |
| `soc-gs` | https://android.googlesource.com/kernel/google-modules/soc/gs | `204078758737776959ecc85c527a00fd2cabb62f` |
| `bcmdhd4389` | https://android.googlesource.com/kernel/google-modules/wlan/bcmdhd/bcm4389 | `de86f6ec77fd3ea86aba02b8c045a4f9fac69732` |

File URL form: `<repo>/+/<branch>/<path>` (add `?format=TEXT` for base64 raw).

## Summary / recommendation

- Chip: **Broadcom BCM4389** (Wi-Fi 6E combo; BT on UART HCI), PCIe endpoint on the gs101 **HSI2 "GEN4_1" root complex (`pcie_1`, ELBI
  `0x14520000`)**, 1 lane, Gen2. WLAN_EN is **gpp14-2**, PERST gph2-0. The kernel (not ABL) drives power-up; nothing says ABL enumerates it.
- iOS needs an **`en0` that looks like an IO80211 controller** (IOKit class conformance for configd, `apple80211` ioctls for wifid). None of
  IO80211Family, IONetworkingFamily or AppleBCMWLAN* exist in xnu-8796 OSS or in our tree.
- Real radio (option B) = a gs101 PCIe RC driver + Broadcom msgbuf dongle driver + an apple80211 shim. Months, high risk, and Google's
  BCM4389 firmware is not Apple's. Not the next step.
- **Recommended path: A1 -> A2 -> probe -> decide B.** A1: IOKit stand-in `IONetworkInterface` for the existing `ue0` so configd applies the
  USB service (already the open item in NOTES.md:300). A2: make that interface answer a minimal `apple80211` ioctl set as a *virtual* Wi-Fi
  ("associated" to a fake SSID, data over USB-ECM) - the cheapest way to reach the literal Gate 5. In parallel run the read-only WLAN
  probe in section 4 (PMU/GPIO reads first) so the real-radio question has numbers.
- Biggest unknowns: which `apple80211` request types iOS 16 wifid needs before it considers an interface usable (needs disassembly of wifid
  from the root image, desk-doable); whether iOS 16 insists on a Skywalk-native Wi-Fi interface (xnu-8796 has Skywalk; is it compiled into our
  kernel config?); whether PCIe1/HSI2 PCIe blocks answer reads after ABL (probe level 2).

---

## 1. Hardware

### 1.1 Chip identity

- **[src] BCM4389.** bluejay's insmod config loads `bcmdhd4389.ko`
  (`gs101-dts: insmod_cfg/init.insmod.bluejay.cfg`: `modprobe|bcmdhd4389.ko`; oriole and raven configs also do). The module is built from
  `//private/google-modules/wlan/bcm4389:bcmdhd4389` (`gs101-dts: BUILD.bazel`, kernel module list). `bcmdhd4389: Kbuild:53-55` sets
  `CONFIG_BCMDHD_PCIE=y`, `CONFIG_BCM4389=y`.
- **[src] IDs.** PCI vendor Broadcom `0x14E4` [guess: standard Broadcom ID; the driver takes it from a header I did not open, TODO(verify) in
  `include/pcicfg.h`]. PCI device `0x4441` (`BCM4389_D11AX_ID`, `bcmdevs.h:138`, `Kbuild:944 -DBCMPCI_DEV_ID=0x4441`) when OTP is
  programmed, `0x4389` when not (`Kbuild:945 -DBCMPCI_NOOTP_DEV_ID`, `dhd_pcie.c:1616-1631`). ChipCommon chip id `0x4389`
  (`bcmdevs.h:193`, mask `CID_ID_MASK 0xffff`, `sbchipc.h:830`).
- **[guess] Module vendor is Murata** (or another SiP maker); the driver has Murata FEM/CID handling (`dhd_custom_cis.c`, `CID_FEM_MURATA`)
  but nothing I read names the Pixel 6a module. A web search for a 6a teardown naming it found nothing. Irrelevant for bring-up; it only
  affects which NVRAM variant file loads (1.3).
- Same combo chip on all gs101 phones: the WLAN DT is common (`gs101-dts: dts/gs101-common-wlan.dtsi`, included from
  `dts/gs101-common.dtsi:12`), so oriole/raven/bluejay share it. [src]

### 1.2 Bus, pins, regulators

**PCIe root complex `pcie_1`, comment "HSI2 GEN4_1"** (`gs101-dts: dts/gs101-pcie.dtsi`, node `pcie@14520000`; enabled and tuned for Wi-Fi in
`dts/gs101-common.dtsi:366-377`). Mainline Linux has **no** gs101 PCIe node (`gs101.dtsi` fetched from torvalds/linux master 2026-10-21: no
`pcie`), so the Android vendor driver is the only reference. [src]

| Item | Value | Source |
|---|---|---|
| ELBI (app/wrapper regs) | `0x14520000` size `0x2000` | gs101-pcie.dtsi `reg` "elbi" |
| PHY (PMA) | `0x14550000` size `0x2000` | "phy" |
| PHY PCS | `0x14540000` size `0x1000` | "pcs" |
| sysreg HSI2 | `0x14420000` size `0x2000` | "sysreg" |
| DBI (DWC core, RC config space) | `0x14800000` size `0x301000` | "dbi" |
| Config window (outbound CFG) | `0x60FFE000` size `0x2000` | "config" |
| I/A (internal address translation block) | `0x14500000` size `0x1000` | "ia" |
| Outbound non-prefetchable MEM window | CPU `0x60000000` size `0xFF0000` | `ranges = <0x82000000 0 0x60000000 0 0x60000000 0 0xFF0000>` |
| Lanes / speed | 1 lane, Gen2 (base dtsi says 2 lanes, Gen3; common.dtsi overrides) | gs101-common.dtsi:368,373 |
| Interrupt | SPI `IRQ_PCIE_GEN4A_1_HSI2`, numeric value TODO(verify) (in `soc-gs` dt-bindings `interrupt-controller/*`; not fetched) | gs101-pcie.dtsi |
| PHY isolation control | PMU `0x17460000 + 0x3EC4` (`pmu-offset`; mask bit0, `PCIE_PHY_BYPASS = 1`) | gs101-pcie.dtsi; `soc-gs: drivers/pci/controller/dwc-whi/pcie-exynos-rc.h:120`, `pcie-exynos-common.h:52`; PMU base 0x17460000 from docs/02:142 |
| Clocks | `pcie-clk-num = <0>`, `phy-clk-num = <0>`: the driver enables **no** clk-framework clocks (`pcie-exynos-rc.c:1212-1229` loops over 0 clocks). HSI2 CMU is `0x14400000` (`pd_hsi2 cmu_id`). Which CMU gates the PCIe block gets is TODO(verify) | gs101-pcie.dtsi, gs101-pm-domains.dtsi |
| Power domain | `pd_hsi2`, PMU regs `0x17462180` (node name says `@17462080`; the file is inconsistent, read both) | gs101-pm-domains.dtsi |
| DMA path | S2MPU `s2mpu_hsi2` at `0x14480000` (+ SSMT `0x144F0000`), shared with UFS; DT comment: "ports 0 and 1 are pci-e", stream ids `0x4 0x5`, with a `TODO: double check` | gs101-pcie.dtsi `s2mpu = <&s2mpu_hsi2>`; `dts/gs101-s2mpu.dtsi` |
| Coherency / IOMMU | `use-cache-coherency "false"`, `use-sysmmu "false"` (only `pcie_0`/modem sets coherency true): non-coherent DMA, physical addresses | gs101-pcie.dtsi, common.dtsi:352-377 |

Wi-Fi GPIOs and rails (`gs101-dts: dts/gs101-common-wlan.dtsi`, `dts/gs101-pinctrl.dtsi`):

| Signal | Pin | Absolute bank base (CON at +0x00, DAT at +0x04) |
|---|---|---|
| WLAN_EN / `wl_reg_on` ("WL_REG_ON", output) | **gpp14-2** | PERIC0 pinctrl `0x10840000` (`gs101.dtsi` `pinctrl@10840000`) + bank offset `0x1C0` (`pinctrl-exynos-arm64.c` `gpp14`, 4 pins) = `0x108401C0` |
| PERST (output, func 1) | gph2-0 | HSI2 pinctrl `0x14440000` + `0x0` (`gph2`) = `0x14440000` |
| CLKREQ (func 2) | gph2-1 | same bank |
| `wl_dev_wake` (input) | gph2-5 | same bank |
| `wl_host_wake` / OOB IRQ (func F = EINT) | **gpa7-0** | FAR_ALIVE pinctrl `0x174E0000` + `0x20` (`gpa7`) = `0x174E0020`; repo already uses this controller for the volume keys (device/google/bluejay/board.json `volume_keys`, gpa7-3) [src: docs/research/05, not observed] |
| Rail 1 | S2MPG10 **LDO17M** `S2MPG10_LDO17`, 0.725-1.3 V, schematic "L17M_PCIE1", `regulator-always-on` | `dts/gs101-pmic.dtsi` (`m_ldo17_reg`); `vreg1-supply` of `pcie_1` |
| Rail 2 | S2MPG10 **LDO19M**, 1.6-1.95 V, "L19M_PCIE1", `regulator-always-on` | same (`m_ldo19_reg`); `vreg2-supply` |

All the above are [src]. Caveats: (a) the register layout "CON +0x0 (4 bit/pin), DAT +0x4 (1 bit/pin)" is the Samsung bank type used by gs101
(`pinctrl-exynos-arm64.c`, `exynos850_bank_type_off`: `fld_width {4,1,...}`, `reg_offset {0x00,0x04,...}`); that gs101's `GS101_PIN_BANK_EINTG`
macro selects it is [guess], TODO(verify) against the macro or by reading a key the shim already knows (gpa7-3). (b) `regulator-always-on` is
the Linux regulator framework's statement about the kernel's policy; it does not prove the rail is on at handoff, and the rails sit behind the
ACPM mailbox (docs/02 section 10), which we cannot read. Treat rail state as unknown.

### 1.3 Firmware, NVRAM, CLM

[src] `bcmdhd4389: Kbuild:58-66,956-959`: built with `CONFIG_SOC_GOOGLE`, paths `/vendor/firmware/fw_bcmdhd.bin`,
`/vendor/etc/wifi/bcmdhd.cal` (NVRAM/"cal"), `/vendor/etc/wifi/bcmdhd_clm.blob`, `/vendor/etc/wifi/fw_bcmdhd.map`; loaded via the standard
`request_firmware` API (`-DDHD_LINUX_STD_FW_API`, `DHD_FW_NAME "fw_bcmdhd.bin"`, `DHD_NVRAM_NAME "bcmdhd.cal"`, `DHD_CLM_NAME
"bcmdhd_clm.blob"`). With `-DSUPPORT_MULTIPLE_NVRAM -DSUPPORT_MULTIPLE_CLMBLOB -DUSE_CID_CHECK`, the NVRAM (and CLM) file name gets a
chip/module suffix: `concate_nvram_by_vid()` appends `_4389` and `_<vendor><rev>` hex digits (`dhd_pcie.c:3619-3625`,
`dhd_custom_cis.c:468-505`). So the real file names on the phone are variants such as `bcmdhd.cal_4389_<xx>` / `bcmdhd_clm.blob_<...>`.
**Exact names: TODO(verify)** by the user running `adb shell ls /vendor/firmware /vendor/etc/wifi` (read-only listing; nothing to pull).
[guess] they live on the `vendor` logical partition (inside `super`), not on a raw partition; ABL does not read them. Rule 4: firmware stays
out of git; whatever loads it must read the user's own phone image at build/boot time.

### 1.4 Is the radio powered/enumerated by ABL?

- [src] In Android the *kernel* does it, in this order: RC driver probe drives `pcie,wlan-gpio` (WLAN_EN) **low**
  (`pcie-exynos-rc.c:1989-1994`); bcmdhd probe requests `WL_REG_ON` (`dhd_custom_google.c:529-566`, comment: the request "will fail if the gpio
  is configured to output-high", i.e. high at entry is a possible state), drives it high, sleeps `WIFI_TURNON_DELAY`, then calls
  `exynos_pcie_pm_resume(ch)` (`dhd_custom_google.c:638-657`, `ch-num = <1>` from `gs101-common-wlan.dtsi`), which is
  `exynos_pcie_rc_poweron()` (`pcie-exynos-rc.c:3363`) -> link training -> Linux PCI enumeration. Hence Linux itself assumes a cold radio.
- [obs, weak] Strings of the bootloader image in this repo (`~/pixelios-artifacts/abl-in/abl.img`, `strings`): it carries `wlan_mac1`,
  `wificountrycode`, `sota.wifi` and `bt_addr` (it hands MAC/country code to Android via the command line / bootconfig [guess]) and **no
  string containing `pcie`**, while the other subsystems it does handle (modem hw/sku, bcm_rmem) show up. Strings are not a decompile; the
  absence is suggestive only.
- [guess] At XNU entry: WLAN_EN low, RC unpowered, PHY isolated. The probe in section 4 reads exactly these bits to replace the guess.

---

## 2. What iOS needs from the kernel

### 2.1 What the real iPhone X (our donor identity) has

[src: local `research/ipsw-x/d22ap.adt`, dumped with `tools/adt.py dump`]

- `/arm-io/apcie/<port 3>` is an `apcie-bridge` with a child `wlan`: `compatible = 'wlan-pcie,bcm4357|wlan-pcie,bcm'`, `device_type =
  'pcie-device'`, `iommu-parent` (DART). A second `wlan` node under a `uart` has `compatible = 'wlan-pcie-uart,bcm4357|wlan-pcie-uart,bcm'`
  plus ~25 `wlan.*` tuning properties (`wlan.autocountry.enabled`, `wlan.awdl.params`, `bcom.roam.profiles`, `wifi-calibration-msf`,
  `wifi-antenna-sku-info`, `local-mac-address`).
- `/product` has `wifi-chipset = '4357'`; `/chosen` has `mac-address-wifi0` (a `macaddr/wifiaddr,syscfg/WMac/6` placeholder that iBoot fills).
- `tools/mkadt.py` copies `/product` from this ADT (NOTES.md: "copies /product") but has no `wlan`/`apcie`/`mac-address-wifi0` handling
  (`grep -n "wlan\|wifi\|mac-address" tools/mkadt.py` is empty). So our ADT today claims `wifi-chipset 4357` and offers no Wi-Fi hardware node.
  [src]
- Apple's matching kext is therefore written for **BCM4357 behind Apple's own PCIe (apcie) + DART**; our chip is a BCM4389 behind a DWC/Samsung
  RC. Even if we had Apple's kexts the bus layer and firmware would not match. [guess, from the above; iOS 16 kernelcache not opened]

### 2.2 Userland that depends on the interface

- **Settings -> wifid.** Wi-Fi state is owned by `/usr/sbin/wifid`; clients (Settings, SpringBoard) reach it through `MobileWiFi.framework`
  with entitlement `com.apple.wifi.manager-access` (SpringBoard has it). [src: Levin, "Apple 80211 - 28 Days Later (Part II)",
  https://newosxbook.com/articles/11208ellpA-II.html, written for iOS 8-era; mechanism assumed unchanged in 16, TODO(verify)]
- **wifid -> kernel.** `Apple80211Open` is `socket(AF_INET, SOCK_DGRAM, 0)`; every operation is an `ioctl` on it with a 0x28-byte
  `struct apple80211_ioctl_str { char ifname[16]; u32 type; u32 _; u32 length; void *data; }`. Codes: `SIOCGA80211 = _IOWR('i', 201, struct
  apple80211_ioctl_str) = 0xc02869c9`, `SIOCSA80211 = _IOW('i', 200, ...) = 0x802869c8`; `type` selects the operation (power, scan,
  associate, SSID, BSSID, RSSI, channel, interface list, events, ...). [src: same article; type numbers: the OS X 10.5 SDK
  `Apple80211_*.h` headers, not fetched, TODO(verify)] `Apple80211BindToInterface` first asks for the interface list and fails if the name
  is absent. At that time IO80211Family checked the caller's wifi entitlement in-kernel via AMFI. [src: same article] Our relaxed AMFI means
  we would not check.
- **How those ioctls reach a driver in our kernel.** In `bsd/net/if.c` (`ifioctl`, default case, 3441-3448 in the patched 0082 tree) the data
  is treated as an `ifreq`, the `ifnet` is found by the first 16 bytes (`ifunit_ref`), and an unknown command falls through
  `pru_control` -> `ifnet_ioctl(ifp, ...)` (3548-3560, `pru_control` at 3555) into the `ioctl` callback given at `ifnet_allocate_extended` time (our `ud_if_ioctl`
  for `ue0`, `bsd/dev/sweet_usb.c`). `ifreq` is 32 bytes on LP64 so the 'i',200/201 words with size 0x28 do not collide with xnu's
  `SIOCSIFLOWPOWER`/`SIOCGIFCLAT46ADDR` (`sockio.h:346,348`, `ifreq`/`if_clat46req`-sized). [src for the dispatch; the size argument
  is [guess]] So **an `en0` ifnet with its own ioctl callback can answer apple80211 requests without any IOKit family**, as long as it
  copies the `data` pointer in/out itself (`copyin/copyout`).
- **configd / SystemConfiguration (is there a Wi-Fi service?).** configd builds `SCNetworkInterface` objects from the IORegistry, not from
  `getifaddrs`: an `IONetworkInterface` whose `IOInterfaceType` is `IFT_ETHER` and whose *controller* conforms to the IOKit class
  `IO80211Controller` (or `AirPortPCI`/`AirPortDriver`) becomes `kSCNetworkInterfaceTypeIEEE80211` ("airport")
  (`configd-1241.140.6: SystemConfiguration.fproj/SCNetworkInterface.c:1780-1790, 1887-1927`;
  https://github.com/apple-oss-distributions/configd). Anything else with the same type is plain Ethernet. [src]
  This is the same missing piece NOTES.md:300 already blames for `ue0` being ignored (no `IONetworkInterface`).
- **IOKit side is absent from our tree.** `iokit/Families` in the 0082 tree contains only `IONVRAM` and `IOSystemManagement`;
  `IONetworkInterface` appears only as a string in `iokit/bsddev/IOKitBSDInit.cpp:235,945,968` (BSD-name lookup for root devices).
  `IONetworkingFamily`, `IO80211Family`, `IOSkywalkFamily` and `AppleBCMWLAN*` are kexts. [src] The project already imports whole families
  into the kernel by patch (e.g. IOHIDFamily in 0058, `SweetBattery : IOPMPowerSource` in 0062), so adding *stand-in classes with the right
  names and registry properties* is the established pattern. [src]
- **iOS 16 specifics.** Apple moved Wi-Fi to the "IO80211FamilyV2" design (plug-in `AppleBCMWLANCore` + `AppleBCMWLANBusInterfacePCIe`)
  and later Skywalk-based interfaces (Black Hat USA 2020 "Dive into Apple IO80211FamilyV2",
  https://i.blackhat.com/USA-20/Thursday/us-20-Wang-Dive-into-Apple-IO80211FamilyV2.pdf; Black Hat USA 2022 vol. 2 says Skywalk is central
  and parts were open-sourced since xnu-8019). [src for the history; macOS-centred] xnu-8796 has Skywalk
  (`config/MASTER.arm64` `SKYWALK_BASE`), but whether our kernel config compiles it in is **TODO(verify)**. If iOS 16 wifid needs a Skywalk
  netif for `en0`, A2 gets harder (the nexus/netif KPI would have to be driven by our shim). [guess]
- **MobileGestalt.** Capability answers (`wifi`, wifi chipset, MAC address) come from `/product` and `/chosen` in the ADT. Exact key names
  and which of them gate the Settings row: TODO(verify) with `ipsw dyld str` on the shared cache, as done for QuartzCore (NOTES.md:446).
  [guess] `/product` `wifi-chipset` (already present) and `/chosen/mac-address-wifi0` (absent in our ADT) are involved.
- **Daemons.** `UPSTREAM_README.md:31`: Wi-Fi/BT/cellular daemons "start and fail or keep relaunching" today [src]. `WirelessRadioManagerd`
  already runs (NOTES.md:411). `wifianalyticsd` and others are needed only if wifid is up. [guess]

### 2.3 Do we have to emulate private ioctls?

Yes: wifid cannot function (not even to say "Wi-Fi is off") without answering `SIOCGA80211`/`SIOCSA80211` requests on the interface it
discovers; an unsupported ioctl returns `ENOTSUP`/`EOPNOTSUPP` (if.c fallthrough) and `Apple80211BindToInterface` fails (Levin, iOS 8+).
`SIOCGIFMEDIA` is generic xnu (`if_media`) and exists for any ifnet that provides it; `IFM_IEEE80211` constants are defined
(`bsd/net/if_media.h:262`), but nothing found says wifid *requires* them. [src / guess]
The *set* of required request types is the main desk-research gap: disassemble `wifid` and `libMobileWiFi`/`Apple80211` from the root image
(`~/pixelios-artifacts/ipsw/*.dmg`, mount needs root: stage a command for the user) and list the `type` values passed to ioctl at
startup, before first scan, and during join. Do this before writing A2.

---

## 3. Options, ranked by effort and risk

| | Effort | Risk | Gate 5 as written |
|---|---|---|---|
| A1 USB Ethernet seen by configd | low-medium | low | no Wi-Fi UI; "connection" yes |
| A2 virtual Wi-Fi (fake `en0` + apple80211 stub over USB-ECM) | medium | medium | yes, with a fake network |
| B real radio (RC driver + Broadcom msgbuf + apple80211 shim) | very high | high | yes, real |
| C ideas | see below | | |

### A. Fake `en0` backed by USB Ethernet (patches 0075-0077, 0084)

State today [obs]: `ue0` (IFNET_FAMILY_ETHERNET, `IFT_ETHER`) with 192.168.7.2/24 is created by `sweet_usb.c` `ud_if_create` and pings
at ~1 ms on the sweet phone (NOTES.md:305); on gs101 0084 is built but not yet run on hardware (09). iOS does not see it (no
`IONetworkInterface`).

- **A1 (plain Ethernet).** Add IOKit stand-ins: a controller class (`IONetworkController`-conforming, any name) and an `IOEthernetInterface`
  object with `IOInterfaceType = 6`, `IOBSDName = ue0` (or rename to `en1`), `IOMACAddress`, `IOBuiltin`, `IOLocation`, registered under
  the USB/ADT node, so configd (SCNetworkInterface.c above) creates a service and applies the `preferences.plist` "USB Ethernet" service
  already in image v15 (NOTES.md:298). UI: Wi-Fi row unchanged/off; Internet works if the host does NAT; **probably** an "Ethernet"-style
  entry in Settings once the service exists [guess]. First experiment (no hardware change): in the QEMU/sweet flow boot with trace 0080
  and watch `SWEET NET: configd route add ... ue0`; on the phone it only needs the USB gadget from 0084 + a host running
  `tools/sweet-usbnet.sh`. Unknowns: exact registry properties configd reads (`kIOInterfaceType`, `kIOBuiltin`, `kIOLocation`,
  `kIOFeatures`, parent controller/bus chain per `processNetworkInterface`, SCNetworkInterface.c:1757-1930); whether `configd` also needs
  `IONetworkController` methods through user clients.
- **A2 (virtual Wi-Fi).** On top of A1: name the interface `en0`, make the controller stand-in an `IO80211Controller` subclass named exactly
  that, and implement the apple80211 ioctl set (section 2.3) as a stub: power on, interface list, "associated" to a configurable SSID/BSSID
  with a fixed RSSI/channel/PHY mode, canned scan result (one network), join = success, link events posted so wifid publishes state. Data
  path unchanged (ECM). UI: Settings -> Wi-Fi shows the toggle on and one connected network with a checkmark; apps see a Wi-Fi-typed
  service with DNS/routes from DHCP or static config [guess]. First experiment: dump the exact ioctl sequence wifid issues (kernel
  logging in the `ifnet` ioctl callback, same style as the 0080 trace: print `type`, `length`, return `ENOTSUP`), on the current sweet
  setup with `ue0` renamed `en0` - this alone lists the required request types without any radio. Unknowns: how many request types and
  event kinds wifid needs before it stops retrying; Skywalk requirement (2.2); WiFi-state UI quirks (Settings may wait on analytics/XPC
  services that are missing); wifid's own entitlement/AMFI checks on the XPC side.
- Cost/benefit: A is the only option that can reach "Settings shows Wi-Fi and it connects" within the milestone, but the "network" is
  the USB tether through the PC, not the radio. Say so in the handoff; do not report A2 as "Wi-Fi works" (rule 5).

### B. Real radio through an IO80211-compatible shim

Minimal functions, with the reference code that would be ported (sizes are file line counts today):

1. **gs101 PCIe RC bring-up** (new, no mainline equivalent): WLAN_EN high, PHY isolation bypass (PMU `+0x3EC4`), pinctrl to active, PHY
   power-down clear, PHY/PCS register tables, soft resets, `PERST` high, wait 20 ms (`EP_BCM_WIFI`), `APP_LTSSM_ENABLE`, poll
   `RDLH_LINKUP & 0x3f == 0x11` (L0), then DBI setup, ATU windows (config `0x60FFE000`, mem `0x60000000`), BAR assignment, bus mastering.
   Reference: `soc-gs: drivers/pci/controller/dwc-whi/pcie-exynos-rc.c` (5571 lines; `exynos_pcie_rc_establish_link` 3103-3300+,
   `exynos_pcie_rc_poweron` 3363+), `pcie-exynos-gs101-rc-cal.c` (489 lines of PHY/PCS tables; this is the part that can only come from
   the Samsung/Google source, no TRM), `pcie-exynos-rc.h`/`common.h` (register and LTSSM state constants). License: GPL-2.0 (Android tree);
   the project is MIT, so porting means re-deriving constants and writing fresh code, not pasting (**decide before starting**). [src]
   Polled operation is possible (we already use polled UFS/DWC3); MSI is not required for a first version. [guess]
2. **Dongle bring-up** (Broadcom "msgbuf"/PCIe IPC): BAR0 window register (`PCI_BAR0_WIN` cfg `0x80`, `dhd` pcicfg.h:488) to reach
   backplane `SI_ENUM_BASE 0x18000000` (hndsoc.h:43) for ChipCommon/PCIe2 core/ARM CR4-CA7 control, reset/halt the ARM, download
   `fw_bcmdhd.bin` into dongle RAM through BAR1 (TCM, mapped `4 MiB` min, `dhd_pcie.h:41-42`), append NVRAM at RAM end
   (`dhdpcie_download_nvram`, `dhd_pcie.c:4147`), release ARM, read the shared-structure pointer (`dhdpcie_readshared`), set up the
   H2D/D2H rings (control submit/complete, RX post, TX flowrings, info rings). Reference: `bcmdhd4389` `dhd_pcie.c` (18121 lines),
   `dhd_msgbuf.c` (15606), `dhd_pcie_linux.c` (3582); mainline brcmfmac is the readable equivalent for the same protocol family but **has no
   4389** (`brcm_hw_ids.h`/`pcie.c` list 4377/4378/4387 only) and is ISC-licensed (`pcie.c` 2795, `msgbuf.c` 1722, `chip.c` 1487,
   `firmware.c` 868, `fwil.c` 432 lines): the better template for a licence-clean rewrite. 4389 may need newer ring/IPC revisions than
   brcmfmac's: unknown. [src for sizes/IDs, guess for porting effort]
3. **Control path:** `bcmcdc`-style iovar/ioctl over the control ring (`WLC_SET_SSID`, `wsec`, `wpa_auth`, `sup_wpa`, PMK, scan iovars,
   CLM blob download with the `clmload` iovar, country code, MAC address, event mask), plus event decode (link, scan complete, auth).
   Reference: brcmfmac `fwil.c`, `cfg80211.c` (8522 lines; only scan/connect/disconnect needed); Google's `wl_cfg80211.c` is 26343 lines.
   WPA2-PSK handshake can be done in firmware (`sup_wpa`); WPA3-SAE, 802.1X and 6 GHz add more. [guess]
4. **Data path:** FullMAC firmware delivers 802.3 frames: TX through flowrings (per-RA/TID), RX through posted buffers; DMA is non-coherent
   and S2MPU-filtered (1.2): the buffer pool must sit in a range the HSI2 S2MPU permits for the PCIe stream ids (TODO(verify); UFS DMA into
   the XNU bank `0x880100000` works [obs, 09 section (c)], but the PCIe streams are different source ports).
5. **apple80211 shim:** map ~dozens of request types and events (2.3) onto 3; hand frames to `ifnet_input`/`ifnet_output`.
6. **Firmware supply:** from the user's own vendor partition (1.3); not shipped (rule 4).

Overall: new code on the order of 5-10 kLOC C plus RE of the iOS side; none of the radio code can be tested without hardware. Also BT is
the same chip but a separate UART transport and out of scope here.
UI when done: real scan list and join; same as A2 from iOS's side. First hardware experiment: section 4 (read-only), then the stage-2
power-up and link-up (a write experiment; confirm with the user first). Unknowns: PHY tables completeness, S2MPU policy for PCIe streams,
whether Broadcom's Google-tuned `fw_bcmdhd.bin` (hw2 "customer" build) needs vendor-side services (Android HAL/`wifi_ptracker`) to run,
Murata NVRAM selection by CID (OTP read), country/CLM, thermal behaviour (docs/02 section 11).

### C. Other approaches

- **Reverse-engineered stand-in for Apple's own driver stack** is not viable for 4389; see 2.1.
- **Make the virtual Wi-Fi more realistic without the radio (A2+)**: bridge the fake "network" to a real uplink on the PC (NAT as `tools/
  sweet-usbnet.sh` host side already does). Corellium virtual iPhones do the same trick: their devices "connect to the internet over
  Wi-Fi", join a network called "Corellium" automatically, and get addresses from an internal DHCP server (support.corellium.com
  `/features/connect/wifi`). That is a hypervisor-provided virtual Wi-Fi, i.e. A2 in spirit; how Corellium implements the guest-side
  interface is not public in what I found. [src for the user-visible behaviour, guess for the mechanism]
- **Prior art for hardware radio**: Linux on Apple platforms drives Apple's Broadcom chips with `brcmfmac` plus Apple-specific firmware
  selection and PCIe quirks (Hector Martin's series "brcmfmac: Support Apple T2 and M1 platforms", https://lkml.iu.edu/hypermail/linux/kernel/
  2201.0/01459.html; BCM4378 series https://lore.kernel.org/all/87a66zaid8.fsf@kernel.org/t/). That is the inverse direction (Linux on
  Apple) and shows the msgbuf protocol is shared across Broadcom PCIe parts, supporting B's reuse of brcmfmac concepts. I found no project
  that runs iOS on a non-Apple phone with Wi-Fi; Project Sandcastle (Android on iPhone 7) reportedly enabled Wi-Fi among the first
  working peripherals (https://liliputing.com/project-sandcastle-brings-android-to-the-iphone-7-and-iphone-7/), again the inverse. [src]
- **Cheap middle path**: an Android-side Wi-Fi bridge is out (Android is erased by the userdata flash, rule 1 context).

---

## 4. Next steps for the driver

Numbered, in order. Steps 1-4 are desk or host-only.

1. **List what wifid asks for (desk, no phone).** With the user's help for the root mount, disassemble `wifid`/`MobileWiFi` from the iOS 16
   root and enumerate `apple80211` request `type` values used at startup/join; grep `libMobileGestalt` strings for the Wi-Fi keys. Output:
   `docs/research/13-apple80211-requests.md`. (Gate for A2.)
2. **A1 + trace.** Rename/duplicate the ifnet as `en0` in a sweet-only experiment and make its `ioctl` callback log every unknown command
   (`cmd`, `IOCPARM_LEN`, first 16 bytes of `data`) like patch 0080; boot on the sweet reference (or QEMU) and read which ioctls wifid
   sends. Needs only a kernel CI round trip. Check whether Skywalk is compiled into our config at the same time.
3. **IOKit stand-ins** (`IONetworkInterface`/controller classes named so `IOObjectConformsTo(controller, "IO80211Controller")` is true) and
   the `/chosen/mac-address-wifi0` + `wlan` ADT additions in `tools/mkadt.py` (generator table, not code constants - AGENTS.md workflow).
4. **A2 stub driver** answering the minimal ioctl/event set; acceptance = Settings shows Wi-Fi connected over the USB tether
   (needs the gs101 USB gadget from 0084 running on hardware first; its test order is in 09 section (f)).
5. **Read-only WLAN probe on the Pixel (first safe hardware experiment).** New `shim/wlanprobe.c` + `wlanprobe.h`, structured exactly like
   `shim/usbprobe.c`: every access through `try_read32()` (`shim/shim.c:117-135`, fault/SError-guarded), output through the same
   `con_str`/ramlog lines, host test with a fake register window (`wlanprobe_host_test.c`), and a store-spelling audit added to
   `tools/test-bluejay-boot.py` (like `check_usb_safety`). **No MMIO stores of any kind.** Two levels so a hang stays attributable:

   Level 1 (PMU and pinctrl banks only; the shim already reads PMU words and the far-alive bank, e.g. shim.c:207,291):

   | # | Register (absolute) | Meaning / decode | Constant comes from |
   |---|---|---|---|
   | 1 | `0x17462180`, `0x17462184`, and also `0x17462080`, `0x17462084` | `pd_hsi2` config/status (status is `+4`, "on" when `& 0xF == 0xF` in mainline Samsung PM-domain code; Google's `exynos-pd` may differ) | gs101-pm-domains.dtsi pd_hsi2 `reg`/node name; mainline `drivers/pmdomain/samsung/exynos-pm-domains.c:52,140` for the +4 convention; semantics TODO(verify) |
   | 2 | `0x17463EC4` (PCIe1), `0x17463EC0` (PCIe0, modem, for comparison) | bit0 = PHY isolation (`1` = bypass/PHY powered, `0` = isolated) | gs101-pcie.dtsi `pmu-offset` 0x3ec4 / 0x3ec0 + PMU base 0x17460000; pcie-exynos-rc.h:120 mask, common.h:52 value |
   | 3 | `0x108401C0` (CON), `0x108401C4` (DAT) | gpp14-2 = WLAN_EN: CON[11:8] (1 = output), DAT[2] | gs101-common-wlan.dtsi `gpp14-2`; bank base 0x10840000+0x1C0 from gs101.dtsi + pinctrl-exynos-arm64.c; DAT offset TODO(verify) via gpa7-3 cross-check |
   | 4 | `0x14440000` (CON), `0x14440004` (DAT) | gph2: pin0 PERST (CON[3:0], DAT[0]), pin1 CLKREQ (CON[7:4], expect func 2), pin5 dev_wake (CON[23:20], DAT[5]) | gs101-pcie.dtsi `gph2 0`, gs101-pinctrl.dtsi `pcie1_clkreq`/`pcie1_perst`; bank 0x14440000 from gs101.dtsi `pinctrl_hsi2` |
   | 5 | `0x174E0020`, `0x174E0024` | gpa7: pin0 = host wake (CON[3:0], DAT[0]) | gs101-common-wlan.dtsi `wlan_host_wake`; far-alive bank base 0x174E0000 [src: docs/research/05; not observed on hardware] |

   Level 1 expectation [guess]: HSI2 domain ON (UFS lives there and works [obs]); PCIe1 isolation bit 0; WLAN_EN = 0.

   Level 2 (HSI2 PCIe wrapper: only if level 1 row 1 says the domain is on; a gated block may hang the bus instead of faulting, so run it as a
   separate boot and expect that a hard reset may be needed; no writes, so nothing persists):

   | # | Register (absolute) | Meaning / decode | Constant comes from |
   |---|---|---|---|
   | 6 | `0x14520000 + 0x2C8` = `0x145202C8` | `PCIE_ELBI_RDLH_LINKUP`; `& 0x3F`: `0x11` = L0 (link up), `0x00` = detect quiet/idle | pcie-exynos-rc.h:50-51; `S_L0 = 0x11` pcie-exynos-common.h:88 |
   | 7 | `0x14520054` | `PCIE_APP_LTSSM_ENABLE` (bit0: LTSSM running) | rc.h:44 |
   | 8 | `0x145202E8` | `PCIE_PM_DSTATE` (`& 3`) | rc.h:53-54 |
   | 9 | `0x145203A4` | `PCIE_SOFT_RESET` (bits: core 0, pwr 1, sticky 2, non-sticky 3) | rc.h:58-62 (use at pcie-exynos-rc.c:3143-3173) |
   | 10 | `0x14520080` | ELBI "DEVICE_TYPE" (driver writes `0x04` here, rc.c:3162; "root complex" meaning is [guess]) | rc.c:3161-3162 |
   | 11 | `0x14800000` | DBI offset 0: root-port vendor/device. Driver treats the RC as `0x144d` / `0xecec` | `dhd_custom_google.c:53-55` (`EXYNOS_PCIE_VENDOR_ID 0x144d`, `..._DEVICE_ID 0xecec`) [src]; whether these are the real DBI values: TODO(verify) |
   | 12 | `0x14800008` | class/revision; expect class `0x0604xx` (PCI-PCI bridge) if the DBI answers | PCI spec [guess] |
   | 13 | `0x14550000 + 0x3F0`, `+0xE0C`, `+0xDE8` | PHY PLL lock (bit3), CDR lock (bit2), offset-cal done (`[3:0]==0xF`): meaningless while unpowered, informative if ABL left the PHY up | pcie-exynos-rc.c:3122-3135 |

   Stop condition: if row 6 reads `0x00` with WLAN_EN low, the radio is cold as Linux assumes and nothing about the chip id is knowable
   read-only. Expected output: the 13 register lines plus a one-line verdict "wlan: off / on-unlinked / linked".
6. **Stage-2 hardware experiment (not read-only; confirm with the user right before running).** In a separate shim build: WLAN_EN high,
   PHY isolation bypass, PHY/PCS init (ported tables, TODO), PERST release, LTSSM enable, bounded poll for L0, program the CFG ATU, read
   endpoint config dword 0 through `0x60FFE000` and expect `0x14E4` / `0x4441` (or `0x4389` without OTP; `dhd_pcie.c:1616-1631`); then,
   if wanted, write `PCI_BAR0_WIN = 0x18000000` and read BAR0+0: `& 0xFFFF == 0x4389` (`bcmdevs.h:193`, `sbchipc.h:830`). Nothing here
   touches storage, so rule 1 is unaffected, but it drives power and bus signals: reversible by power cycle. Abort criteria and bounded
   waits as in shim/AGENTS.md. This is only worth building if the decision after steps 1-5 is to pursue B.
7. **Decide B vs. stay on A** after steps 1-5, and write the licence decision (GPL Android reference vs ISC brcmfmac) into docs before any
   port code appears.

## Open items (all TODO(verify))

- Exact `/vendor/firmware` and `/vendor/etc/wifi` file names on the phone (user: `adb shell ls`).
- Numeric GIC SPI for `IRQ_PCIE_GEN4A_1_HSI2`; CMU_HSI2 gate registers for the PCIe1 block (`soc-gs` cal-if tables); S2MPU policy for the
  PCIe stream ids.
- gs101 pinctrl bank type vs `exynos850_bank_type_off`; PMU pd status semantics for Google's `exynos-pd`.
- apple80211 `type` list for iOS 16; Skywalk requirement; MobileGestalt Wi-Fi keys; whether any ADT `wlan`/`apcie` node is read by userland.
- DT differences between the fetched android16 tree and the DT the installed bootloader/kernel actually used (observe via a read-only
  `/proc/device-tree` listing by the user if needed).

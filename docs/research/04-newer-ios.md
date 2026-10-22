# Newer iOS on Pixel 6a: architecture and porting verdict

**Scope.** Desk research and local source reading only; no device access or
`adb`/`fastboot` command was used. “VERIFIED-FROM-SOURCE” means the statement
is directly supported by a cited first-party/upstream source or by a cited local
file. “INFERRED” is the engineering conclusion drawn from those facts and is
called out as such.

## Verdict

**Use the existing iOS 16/iPhone X baseline first. Do not make an arm64e
iPhone build the next on-device milestone on Tensor G1.** The Pixel 6a's
Cortex-X1, Cortex-A76, and Cortex-A55 cores are Armv8.2-class; PAC is an
Armv8.3-A feature. Apple says arm64e is supported by A12-and-later devices.
Thus every currently supported iPhone target for iOS 17, 18, or 26 is in the
PAC generation. [Arm PAC overview](https://learn.arm.com/learning-paths/servers-and-cloud-computing/pac/pac/),
[Apple arm64e documentation](https://developer.apple.com/documentation/security/preparing-your-app-to-work-with-pointer-authentication)

There *is* a useful non-PAC successor route: **iPadOS 18 on the seventh-
generation iPad (`iPad7,11` Wi-Fi or `iPad7,12` cellular, A10)**. Apple lists
the seventh-generation iPad as iPadOS 18-compatible, while its A10 predates
Apple's A12-and-later arm64e line. It is still a major port, but it avoids the
fundamental arm64e-on-v8.2 problem. This is an **INFERRED** “plain arm64
userland/cache” conclusion from the device ISA and Apple’s universal
device-specific OS builds; it has not been confirmed by mounting and inspecting
that IPSW's SystemOS cryptex in this pass. [Apple iPadOS 18 compatibility](https://support.apple.com/en-us/104986)

Strictly for the **iPhone-only iOS** line, the newest plain-arm64 userland is
therefore iOS 16 on A11 and older devices; iOS 17 is already arm64e-only.
The iPadOS 18 A10 exception is the newest useful answer when “iOS” is used for
the iOS-family userland rather than the iPhone product name.

## 1. What remains plain arm64?

| Release family | iPhone conclusion | Non-arm64e device-family exception | Practical result |
| --- | --- | --- | --- |
| iOS 17 | **VERIFIED-FROM-SOURCE:** iPhone support begins with A12-era iPhone XR/XS, which Apple identifies as arm64e-capable. | **INFERRED:** iPadOS 17 on iPad (6th gen, A10) and iPad (7th gen, A10) remains plain arm64. | A non-PAC iPadOS target exists. |
| iOS 18 | **VERIFIED-FROM-SOURCE:** iPhone support remains A12-era iPhone XR/XS and later. | **VERIFIED-FROM-SOURCE:** iPadOS 18 supports iPad 7th generation; **INFERRED:** its A10 build uses plain arm64. | Best newer userland target: `iPad7,11`/`iPad7,12`. |
| iOS 26 | **VERIFIED-FROM-SOURCE:** iPhone support begins at iPhone 11 (A13), hence arm64e. | **VERIFIED-FROM-SOURCE:** iPadOS 26 starts at iPad 8th generation, an A12 device; **INFERRED:** no iOS/iPadOS 26 non-PAC device target remains. | No viable on-device iOS/iPadOS 26 target without emulating/replacing PAC. |

Apple’s current security-release compatibility matrix corroborates the iOS 18
iPhone XR/XS and iPad 7th-gen boundary, and the iOS/iPadOS 26 iPhone 11 / iPad
8th-gen boundary. [Apple security releases](https://support.apple.com/en-us/100100)
Apple's iOS 26 guide independently lists iPhone 11 as the oldest supported
iPhone. [iOS 26 compatibility](https://support.apple.com/guide/iphone/iphone-models-compatible-with-ios-26-iphe3fa5df43/26)

### IPSW/cache qualification

**VERIFIED-FROM-SOURCE:** current IPSWs carry OS and cryptex disk images, not
an exposed cache file in the outer ZIP. ios-sweet's own root builder mounts the
OS, SystemOS and AppOS images; it locates the shared cache in the OS cryptex at
`System/Library/Caches/com.apple.dyld` and links it into the root
(`upstream/tools/mkiosroot.sh`, lines
60–108). Therefore an outer-ZIP filename listing cannot prove cache ISA.

**INFERRED:** an iPhone IPSW for 17/18/26 contains the `arm64e` cache expected
by its A12+/A13+ target, while an A10 iPadOS 17/18 IPSW contains a non-e
`arm64` cache. Do not promote either inference to a release gate: verify it by
mounting the selected SystemOS cryptex and recording `file`/Mach-O CPU subtype
and cache filenames before adopting it.

Related platforms do not rescue an iPhone port:

- **VERIFIED-FROM-SOURCE:** Apple says Apple TV 4K (2nd generation) is arm64e,
  and Apple lists tvOS 26 for Apple TV HD and all Apple TV 4K models. The HD
  and first-generation 4K are older non-PAC hardware, so **INFERRED:** tvOS
  still has an arm64 route; it is a tvOS userland/IOKit ABI, not iOS/iPadOS.
  [Apple arm64e documentation](https://developer.apple.com/documentation/security/preparing-your-app-to-work-with-pointer-authentication),
  [Apple security releases](https://support.apple.com/en-us/100100)
- **VERIFIED-FROM-SOURCE:** Apple lists Apple Watch Series 4+ as arm64e;
  older Watch software's `arm64_32` ABI is not a substitute for a 64-bit iOS
  process ABI. Apple Silicon simulator runtimes are host/simulator builds, not
  bootable device userlands. No evidence found that bridgeOS, visionOS, or a
  Simulator runtime can be substituted into ios-sweet.

## 2. arm64e on Tensor G1

### CPU fact

**VERIFIED-FROM-SOURCE:** Cortex-A76/A55 implement Armv8.2-A; Arm documents
PAC as available in Armv8.3-A and later, and its support table marks another
Armv8.2 design (Neoverse N1) as PAC-free. [Cortex-A76
documentation](https://support.arm.com/compute-ip/cortex-a76), [Arm PAC
overview](https://learn.arm.com/learning-paths/servers-and-cloud-computing/pac/pac/)

**INFERRED, high confidence:** Pixel 6a Tensor G1's X1/A76/A55 cluster has no
`FEAT_PAuth`. This agrees with the project’s existing machine description,
which records the relevant Kryo 470 cores as Armv8.2 without PAC
(`upstream/README.md`, lines 190–197; the same
constraint is explicit in `NOTES.md`, lines 403–406). A boot-time read of the
ID registers would be the final target-specific proof, but is outside this
no-device task.

### Instruction compatibility is not “PAC is just NOP”

**VERIFIED-FROM-SOURCE:** Arm deliberately placed the common SP/LR forms in
the HINT encoding space. On a core without the feature, HINT encodings NOP;
Arm specifically documents `paciasp` as HINT #25, `autiasp` #29, `pacibsp`
#27 and `autibsp` #31. `xpaclri` is likewise a HINT-space compatibility
instruction. [Arm’s PAC/BTI explanation](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/p2-enabling-pac-and-bti-on-aarch64)

**VERIFIED-FROM-SOURCE:** this does *not* apply to the general PAC
instruction set. `PACIA/PACIB/PACDA/PACDB` and authenticate forms with
register operands, authenticated branches/calls (`BRAA`, `BRAB`, `BLRAA`,
`BLRAB` and Z forms), authenticated returns (`RETAA`, `RETAB`), authenticated
loads (`LDRAA`, `LDRAB`), and authenticated exception returns (`ERETAA`,
`ERETAB`) require PAuth and are undefined when it is absent. See the Arm
instruction reference’s architectural pseudocode for
[PACIA](https://www.df.lth.se/~getz/ARM/A64/pacia.html) and the
[Armv8-A PA instruction table](https://www.usenix.org/system/files/sec19-liljestrand_0.pdf).

The operational consequence is important:

| Candidate | Assessment |
| --- | --- |
| (a) EL1 undefined-instruction trap/emulation | **INFERRED: research-only, high risk.** It must decode every non-HINT PAuth instruction, reproduce the “PAC disabled” semantic consistently, advance PC, and handle exceptions/signal state. A hot indirect-call/load site would trap twice per operation, so this is likely catastrophically slow even before correctness work. It is not enough to NOP the HINT forms. |
| (b) Static rewrite of the full dyld cache and every executable/DYLD-loaded code page | **INFERRED: more plausible than per-instruction traps, still large.** Rewrite non-HINT PAuth operations to semantically neutral equivalents, then repair code signing/page hashes and any chained fixups. It needs a measured instruction census before choosing this path. |
| (c) Use an emulator that translates PAuth | **VERIFIED-FROM-SOURCE for the existence of a path, not for Pixel hardware.** `qemu-sptm`/`darwin-vm` boots recent Apple platforms with SPTM/TXM support and explicitly says PAC is disabled in its emulation. Its stated limitation is that this works only where a correct run does not encounter PAC failures. It is a useful compatibility/profiling oracle, not evidence that a stock Armv8.2 phone can execute arm64e. [qemu-sptm](https://github.com/jprx/qemu-sptm), [darwin-vm](https://github.com/Passw/jprx-darwin-vm) |

### Frequency, ABI, and what is not yet proved

**UNRESOLVED rather than guessed:** no modern arm64e shared cache was mounted
and disassembled in this pass, so there is no defensible number for the
frequency of `BRAA`/`LDRAA`/`RETAA` versus HINT PAC. Measure that before any
rewrite/emulation decision: scan every `__TEXT_EXEC` mapping in the target
SystemOS cache and all launch-critical binaries, counting encodings by opcode,
then boot through launchd/SpringBoard with an instruction-emulation counter.

**VERIFIED-FROM-SOURCE:** Apple describes arm64e as automatically adopting
pointer authentication, with PAC in otherwise unused pointer bits; failed
authentication aborts. [Apple’s arm64e documentation](https://developer.apple.com/documentation/security/preparing-your-app-to-work-with-pointer-authentication)

**INFERRED:** arm64e does not inherently require a real PAC-capable *kernel
ABI* if a complete emulator/rewrite supplies coherent disabled-PAC semantics;
the QEMU project is evidence of that model. Conversely, on physical v8.2 the
kernel must at minimum emulate every undefined instruction reachable at EL0
and EL1 and preserve correct exception/thread state. Claims that Mach messages
or syscalls necessarily carry signed user pointers, or that a real Pixel port
must program Apple APIA/APIB keys, were **not verified** here and should not be
used as blockers without a target cache/XNU audit. The practical blocker is
the undefined instruction stream, not a demonstrated syscall ABI requirement.

## 3. Kernel generation and newer platform requirements

| OS generation | Published XNU evidence | Port significance |
| --- | --- | --- |
| iOS 17 | **VERIFIED-FROM-SOURCE:** `xnu-10002.1.13` and later `10002.*` tags are published. | Still requires rebasing the entire SWEET machine configuration and the userland-facing IOKit work. |
| iOS 18 | **VERIFIED-FROM-SOURCE:** `xnu-11215.1.10` and later `11215.*` tags are published. | Same rebase plus newer platform assumptions. The A10 iPad target avoids arm64e and SPTM, but not the port. |
| iOS 26 | **VERIFIED-FROM-SOURCE:** `xnu-12377.1.9` and later `12377.*` tags are published. | All iPhone/iPad candidates are PAC hardware; recent XNU also contains Exclaves startup/support. |

The tags above were verified against [Apple's XNU tag
repository](https://github.com/apple-oss-distributions/xnu/tags). The XNU
READMEs for these tags declare ARM64 builds; `blacktop/darwin-xnu-build` also
documents successful ARM64 builds through macOS 26. This establishes
*buildability of an OSS kernel*, not a ready-made phone kernel or complete
iOS kernel collection. [XNU build README](https://github.com/apple-oss-distributions/xnu),
[darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build)

### Security/boot stack

- **VERIFIED-FROM-SOURCE:** SPTM/TXM replaces PPL on A15-and-later and M2-and-
  later SoCs. It uses silicon primitives only available on those processors.
  It is therefore **not** a mandatory concern for an A10 iPadOS 17/18 target,
  but it is a hard emulation/porting concern for later Apple devices.
  [Apple Platform Security](https://support.apple.com/en-ca/guide/security/sec8b776536b/web)
- **VERIFIED-FROM-SOURCE:** current XNU exposes Exclaves startup and an
  exclaves Mach trap. Whether a particular iPhone/iPad userland makes it
  launch-critical is target-specific and remains **UNVERIFIED**. [XNU startup
  lifecycle](https://github.com/apple-oss-distributions/xnu/blob/main/doc/lifecycle/startup.md),
  [exclaves trap](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/mach_traps.h)
- **VERIFIED-FROM-SOURCE (local):** the current port already depends on a
  copied OS/App cryptex layout, manual cache links, a synthetic keybag, relaxed
  AMFI/sandbox, an AppleKeyStore stub, IOSurface/IOMobileFramebuffer, an exact
  IOHIDFamily user-client, MobileGestalt identity and Hactivation. See
  `upstream/README.md`, lines 75–119 and 165–188,
  and `upstream/NOTES.md`, lines 21–30. Newer
  userland will require a fresh compatibility audit of all of those contracts,
  especially AMFI/CoreTrust/trust-cache, cryptex mounting and launchd cache
  assumptions. Sealed-system/APFS-snapshot behavior has not been demonstrated
  on HFS+ and is a major probable blocker, not a fact already solved by this
  port.

## 4. Ranked next options

| Rank | Target and impersonated identity | Effort | Main blocker | Required ios-sweet changes |
| --- | --- | --- | --- | --- |
| 1 | Keep iOS 16.7.16, `iPhone10,3` / `D22AP` | Low incremental | Finish existing input, services and app stability work. | None for identity. This is the only currently demonstrated end-to-end userland. |
| 2 | iPadOS 18 on iPad 7th gen, `iPad7,11` / its exact Wi-Fi board config (or `iPad7,12` cellular) | Very high, but bounded | Rebase XNU 8796 patch stack to 11215; update all userland-facing services and iPad geometry; prove the A10 cache is plain arm64. | Replace `mkadt.py` D22 identity and copied `/product`; change 0059 identities; use the actual iPad display/canvas in 0061 and HID mapping; version all build-specific paths/preferences in `sweet-root-tweaks.py`; revalidate `mkiosroot.sh` cryptex layout/cache links and keybag. |
| 3 | iPadOS 17 on iPad 6th gen (`iPad7,5`/`iPad7,6`) or 7th gen (`iPad7,11`/`iPad7,12`) | Very high | Same rebase, fewer release changes than 18 but no reason to prefer it if 18's A10 cache is verified. | Same identity, geometry, `/product`, cryptex and tweak work as rank 2. |
| 4 | iOS 17 iPhone XR, `iPhone11,8` / `N841AP`; or iOS 18 iPhone XR | Extreme/research | arm64e non-HINT PAuth plus the rebase. | All rank-2 changes, plus an ISA census and either binary rewrite or a correct EL1 PAuth emulator. Do this first in QEMU, not on the Pixel. |
| 5 | iOS 26 iPhone 11, `iPhone12,1` / `N104AP` | Extreme, not recommended | arm64e plus newest kernel/security stack. | All prior work plus PAC, newer cryptex/CoreTrust/AMFI behavior, and potentially SPTM/TXM/exclaves-facing expectations. |

The identity fields are not cosmetic. `mkadt.py` copies the donor device’s
`/product`, publishes the root `compatible`/`model`/target values, display
scale and Hactivation security status; 0059 deliberately exposes those as
`hw.machine`, `hw.model`, `hw.target` and `hw.product`. 0061 lies about the
display because UIKit uses real-device layout metadata. Those are direct local
dependencies, not a generic device-name string change. See
`upstream/tools/mkadt.py`, lines 38–57 and
204–227; 0059,
lines 7–13 and 48–52; and
0061,
lines 9–18 and 104–121.

## Recommended gated experiment

1. Preserve iOS 16 as the known-good branch.
2. Download an iPadOS 18 `iPad7,11` IPSW, mount OS/SystemOS/AppOS, and record
   the cache filenames and CPU subtypes. Stop if the cache is arm64e.
3. Build a minimal arm64 `xnu-11215` SWEET configuration before transplanting
   the 81-patch series; make the build/rebase independently reproducible.
4. Port only the boot, storage, display/IOSurface and HID path, then adapt
   `/product`, root identity, canvas and root-image tweaks to that *exact*
   IPSW/build.
5. Treat iPhone XR+ as a separate QEMU ISA-research project until an opcode
   census proves that the non-HINT PAuth cost is tractable.

## Source and evidence notes

Local source evidence was read from `upstream/README.md`, `upstream/NOTES.md`
(including “Kernel architecture plan”), `upstream/xnu/README.md`,
`upstream/.github/workflows/xnu.yml`, all patch filenames, and the relevant
identity/root scripts. The patch series includes the explicit interfaces
called out in this report: non-PAC configuration, AMFI/trust-cache behavior,
AppleKeyStore, IOSurface/IOMobileFramebuffer, permissive sandbox, exact HID,
identity, display, activation and root/cryptex preparation.

No device was attached; no physical-Pixel CPU register read, IPSW cryptex
mount, modern cache disassembly, or iOS boot test occurred. Those omissions
are why cache-architecture and PAC-frequency conclusions are marked
**INFERRED** or **UNRESOLVED**, rather than presented as measured facts.

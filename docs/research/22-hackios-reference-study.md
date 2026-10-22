# 22 - HackiOS reference study: what another port of the same XNU does, and what we can take as ideas

Status: research only, 2026-10-22. Nothing was run, built or installed; no device was touched; no code was copied from the references
(their licence is APSL 2.0: we take ideas and re-implement with our own methods for the Pixel 6a / Tensor G1).

Tags (AGENTS.md rule 5):

- **[src K:file:line]** / **[src U:file:line]** = read from source in the reference. `K` = `~/pixelios-artifacts/refs/hackios-xnu-kernel`,
  `U` = `~/pixelios-artifacts/refs/hackios-xnu-userland`. Line numbers are from the clones as of today.
- **[ours: path]** = read from our own repo or logs (a log line is *observed on hardware* only where the log name and date are given).
- **[guess]** = my inference, not established by any source or run.
- Nothing here says that anything "works" on OUR hardware. Claims about what HackiOS achieves are their README's claims
  (`K:README.md:36-48`), not something I verified; the clones contain no run logs.

What HackiOS is: the same Apple XNU (`xnu-8796.121.2`, iOS 16.5) with the iOS 16.5 userland (iPhone10,3 shared cache 20F66), on a POCO X5 Pro 5G
("redwood", Snapdragon 778G) and in QEMU [src K:README.md:3-12]. Our tree: xnu-8796.141.3 [ours: tools/stack-apply:15], iOS 16.7.16 (20H392) root, Pixel 6a.
Differences that matter for every comparison below: (a) they boot the phone from a UEFI (Mu-Silicium port) that stays resident and serves USB, storage
and framebuffer, we boot from a bare-metal shim that hands the machine to XNU [src K:README.md:146-165; ours: shim/AGENTS.md:3]; (b) their iOS root is
a **thinned, RAM-resident APFS image** with an **empty RAM `/private/var` at every boot**, ours is a full 16 GB HFS+ root that persists on `userdata` with a
persistent `/var` [src U:userland/build.sh:55-61; K:xnu/files/bsd/dev/redwood_ufs.c:1-30; ours: tools/mkiosroot.sh:14-15].

---

## 1. Boot time / userland recipe (their ~20 s to the lock screen vs our ~35 min first boot and the poster/black-screen problems)

### 1.1 What they do (techniques)

**Process tree and init.**
- Process 1 is not launchd at first. `/sbin/launchd` of the system image is a 35-line static program (`stage0`) that mounts the small image on `/redwood`
  and execs `/redwood/sbin/init` [src U:userland/src/stage0.c:1-35]. `init` mounts a RAM filesystem on `/private/var`, recreates the directory
  template of iOS's data volume from a generated skeleton (`var.skel`), runs ~15 self-checks, writes two sysctls, then runs `etc/rc`, which is one
  line: `exec /sbin/launchd.ios` [src U:userland/src/init.c:1-14, 824 (mount_writable), 979 (restore_var), 1348 (configure_inject), 1393 (configure_vnodes);
  U:userland/rc.launchd:2]. The real launchd is renamed `launchd.ios` in the image [src U:userland/build.sh:736].
- `/System/Library/LaunchDaemons`, `NanoLaunchDaemons`, `LaunchAngels` and `xpc/launchd.plist` are removed from the image and the first becomes a symlink to
  `/redwood/LaunchDaemons`, a directory generated at build time [src U:userland/build.sh:724-726]. So launchd reads **their** job set, not Apple's 490+.
- `kern.maxvnodes` is raised at boot: the kernel config they inherit stops at 1024 vnodes ("vnode: table is full", idle daemons killed) [src U:userland/src/init.c:1380-1410].
  Ours uses the `medium` size class [ours: xnu/patches/0006-sweet-master-config-open-source-asm-pmap.patch:31], so this one probably does not apply to us [guess].

**Job selection (the biggest lever on startup load).** `mkjobs.py` copies chosen stock job plists, forces stdout/stderr to `/dev/console`, and
(`--rest`) adds every other job that has Mach services **as on-demand only**: `RunAtLoad`, `KeepAlive`, `StartInterval`, `StartCalendarInterval`, `WatchPaths`,
`QueueDirectories`, `StartOnMount`, `LaunchEvents` are deleted, so launchd starts such a daemon only when a client looks up one of its services
[src U:userland/mkjobs.py:299-330]. Core list: 21 jobs (notifyd, cfprefsd x2, logd, syslogd, distnoted, diagnosticd, fseventsd, containermanagerd x2,
runningboardd, backboardd, usermanagerd, mobilegestalt, lsd, powerd, analyticsd, logd_helper, iconservicesagent, pkd, SpringBoard) plus `wifid`
[src U:userland/build.sh:99-118]. A job that is named is loaded even if it is `_LimitLoadToDeviceTree`'d [src U:userland/mkjobs.py:289].
Denied by label substring: `watchdog, CommCenter, wifi/WiFi, bluetooth, BTServer, locationd, softwareupdate, datamigrator, backupd, thermalmonitord,
keybagd, nfcd, biometrickitd, pearld, obliterat, storage_mounter, fud, sepd, seputil, camera, ReportCrash, osanalytics, lockdown, audio, jetsamproperties,
purplebuddy, restore, usb, accessoryd, mobileassetd, bird, homed, routined, duetexpertd, dmd, syncdefaultsd, photoanalysisd, avconferenced`
[src U:userland/mkjobs.py:216-226]. Their long comment (mkjobs.py:35-215) records measured lessons worth keeping:
- A denied dependency makes a neighbour spin: `cloudd` retried `nsurlsessiond` 2351 times in one phone session because `nsurlsessiond` was denied;
  with it shipped the same pair is quiet. Rule they state: judge a denial only against a control from the same afternoon, the list is not independent per line
  [src U:userland/mkjobs.py:45-58, 88-95].
- `CommCenter` (no baseband) crash-loops (18 crashes in 7 minutes) and was denied; `syncdefaultsd` aborts in `changeTokenForStoreWithConfiguration:` and was denied;
  `apsd` and `mobileactivationd` were taken **out** of the deny list because 22 daemons ask for `apsd` (a third of all failed lookups)
  [src U:userland/mkjobs.py:69-88, 96-215].
- `fairplayd` exits 0 immediately and KeepAlive restarts it every 10 s (22 times per session) [src U:userland/mkjobs.py:20-23]; with the on-demand rewrite there is no
  KeepAlive, so there is no loop.

**No DataMigration, no migration cost.** `datamigrator` is denied, and `com.apple.migration.plist` is pre-seeded with `LastSystemVersion=20F66` and
`DMLastMigrationResults={success:true,buildVersion:20F66}` [src U:userland/var/mobile/Library/Preferences/com.apple.migration.plist; mkjobs.py:216].
Because `/private/var` is empty at every boot, LaunchServices registers all apps and extensions again on each boot, which they accept as the price:
"Right after SpringBoard appears the system is still registering apps: give it 30-40 s" [src K:README.md:82; U:userland/src/inject_sb.c:9-17]. A skipped
migration did **not** leave them without app extensions (they report 15-20 widget and poster processes) [src U:userland/mkjobs.py:94, 174 (their counts, not ours)],
which contradicts our sailfish note "LastSystemVersion skip left LaunchServices with 1 of 524 appex" [ours: docs/HANDOFF.md:384 (session 6 entry)]. The difference is in the
kernel, see 1.3 item 3 (AMFI policy syscall 0x60).

**Thin root in RAM.** `select-system.py` chooses ~2.5 GB of the 7.9 GB root: whole `usr/`, `bin`, `sbin`, `System/Library` minus a skip list (LinguisticData,
PreinstalledAssets, NanoTimeKit, Audio, Assistant, AccessibilityBundles, Photos, Carrier Bundles, Caches ...), only `en/Base` localizations, frameworks'
resources up to 4 MB, no ML models, small `.metallib` only (<= 2 MB, plus four named large ones), apps without media, staged system apps moved to
`/Applications` [src U:ios165/select-system.py:1-40, 56-69, 93-120, 145, 201-215]. The image lives in RAM, so no disk I/O sits on the boot path
[src U:userland/build.sh:55-61].

**Preferences seeded** (all in `U:userland/var/mobile/Library/Preferences/`): `com.apple.springboard` {`FBLaunchWatchdogScale`=20.0, `BKNoWatchdogs`=true,
`SBShowBatteryPercentage`}, `com.apple.frontboard` {`FBLaunchWatchdogScale`=20.0}, `com.apple.purplebuddy` {`SetupDone`, `SetupVersion`=11, `SetupState`=SetupUsingAssistant,
`ForceNoBuddy`=true}, `com.apple.keyboard.preferences` {`DidShowContinuousPathIntroduction`}, `com.apple.coremedia` {`wirelessdisplay_max_count`=0},
`com.apple.VideoProcessing` {threading auto, max-threads 7}, `com.apple.assistant.support` {Offline Dictation Status = {}}, `com.apple.migration` (above).

**Injected libraries.** The kernel gets a list "LIBRARY NAME..." from `init` via sysctl `kern.exec_inject` and adds `DYLD_INSERT_LIBRARIES` to the environment of
every exec of a matching program [src K:xnu/redwood.patch:609-690 (exec_inject_env); U:userland/src/init.c:1348-1380]. Libraries: `libredwood` into backboardd +
containermanagerd (both), `libredwood-sb` into SpringBoard, `libredwood-media` into mediaserverd, plus app/tweak ones [src U:userland/build.sh:128-131].
It relies on a kernel AMFI stand-in that answers "dyld policy: everything allowed" [src K:xnu/files/bsd/kern/kern_minimac.c:62-75, 90-117].
Hooks that bear on our problems (all by replacing Objective-C methods or vtable slots at addresses of 20F66, each guarded by an "expected IMP" check that
leaves the method alone on mismatch [src U:userland/src/inject_sb.c:150-157; inject.c:~25]):
- SpringBoard: activation state forced to "activated" (`-[SBLockdownManager _setupActivationState]`), setup assistant refused (`-[SBSetupManager _setSetupRequiredReason:]`
  forced to 0) [src U:inject_sb.c:165-190]; **app snapshot format forced to JPEG/PNG** because the ASTC (ATX) compressor on the CPU takes ~10 s per picture
  under QEMU and the 10 s scene-update watchdog then kills every app that goes to the background [src U:inject_sb.c:192-215]; **`BSUIVibrancyEffectViewDisabled=true`
  set with `CFPreferencesSetAppValue` when no GPU draws, "the lock screen's clock is white"** [src U:inject_sb.c:425-432]; the lock button long press ignored when
  the press was already over (SpringBoard busy waking the display turns every wake into Siri) [src U:inject_sb.c:436-470]; poster switcher withheld
  (`_allowsCapabilityPosterSwitcherWithExplanation:` returns NO) because the long-press poster row draws as a black screen under their window server
  [src U:inject_sb.c:515-543]; Siri-orb shader prewarm made skippable (`siri_orb=0`) [src U:inject_sb.c:612-650].
- backboardd: window-server vtable hooks, the virtual HID services (area 2), personas disabled in containermanagerd, the **Metal device array emptied** so
  CoreAnimation falls to OpenGL ES (`camtl=1` leaves it) [src U:inject.c:17-23, 530-545, 604-632].
- mediaserverd: portrait-filter prewarm skipped (a NULL GL function crashed it), and **Apple's software HEVC codec (`VCPHEVC.videocodec`) is what decodes HEIC for
  every process through VideoToolbox**; without mediaserverd ImageIO cannot decode HEIC and the iOS 16 layered wallpaper shows its gradient only
  [src U:inject_media.c:1-20].

**Debug aids with no reflash.** `applog=1` sends stdout/stderr of every process launched with `/dev/null` to `/dev/console`; `applog=2` also sets `OS_ACTIVITY_DT_MODE`
(os_log to stderr) for everything; `applog_name=A,B` does that only for programs whose file name starts with one of the names, "which is how to read what one
application extension has to say" [src K:xnu/redwood.patch:449-459]. `systrace` dumps registers/backtraces of fatal user exceptions [src K:xnu/files/osfmk/arm64/user_exception_trace.c:1-12].

### 1.2 Comparison with `tools/sweet-root-tweaks.py` and `tools/build-ios-root.sh`

What we already match [ours: tools/sweet-root-tweaks.py:1-60, 150-290]: PanicOnCrash renamed, CommCenter / fairplayd.H2 / vmd / ReportCrash / spindump / tailspind /
aggregated removed from `xpc/launchd.plist`, containers dirs, activation data ark, ExtensionKit feature flag, VirtualAudio plug-in disabled, MediaToolbox
`StartRecorderServer=false`, stock apps moved from `staged_system_apps`, HEIC->PNG wallpapers, SetupDone prefs, BKNoWatchdogs. Our kernel also has the
sandbox container-path syscall (0085) [ours: xnu/patches/0085-sweet-sandbox-container-path-appex.patch].

| Item | HackiOS | Ours | Could it explain an open issue of ours |
|---|---|---|---|
| Vibrancy for the CPU renderer | `BSUIVibrancyEffectViewDisabled`=true in SpringBoard's app prefs [src U:inject_sb.c:425-432] | not set; doc 18 proposes `EnhancedBackgroundContrastEnabled` (key mapping unverified) [ours: docs/research/18-gpu-userland.md:327-345] | Transparent lock-screen clock: same symptom they fixed. [guess] that 20H392 reads the same key |
| Display power | `RequestPowerChange` is a no-op returning success: the screen never goes dark [src K:xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp:1360-1366] | patch 0077 really blanks the panel and keeps it black until "on" [ours: xnu/patches/0077-sweet-display-power.patch:9-13] | Black screen after ~100 s / ~580 s: **observed** `SWEET FB: display off` in `logs/qr-20261022T053837Z.txt:30` and `logs/qr-20261022T063754Z.txt:1`. That auto-lock/power-key request is a candidate cause of the later black screens when nothing can wake it (touch and keys unproven) [guess] |
| AMFI policy | MAC policy named **"AMFI"** with policy syscalls 0x5a (dyld policy) and 0x60 (boot-arg state -> "not enforcing") [src K:kern_minimac.c:62-125] | policy named "SWEET AMFI", `mpo_policy_syscall` not set [ours: xnu/patches/0035-sweet-amfi-entitlements-platform-binaries.patch:261-272] | `__mac_syscall("AMFI", ...)` finds no policy on our kernel [guess: by the name lookup rule; not tested]. ExtensionKit asks request 0x60 and, without a "not enforcing" bit, wants the signature of `extensions.plist` verified; their comment says the result is "every application extension is refused ... does not correspond to a known extension point" [src K:kern_minimac.c:73-85]. We use the ExtensionKit feature flag instead [ours: tools/sweet-root-tweaks.py:feature_flags]; whether it is sufficient on 20H392 is unproven |
| Migration | skipped; var empty and rebuilt each boot | runs once, ~35 min at cpus=1 [ours: HANDOFF.md:389, 401]; one-off cost | cpus=2 and cpus=4 reach the lock screen quickly [ours: HANDOFF.md:409]; a first boot at cpus=4/8 is untried [guess: much faster] |
| Job set | 21 + on-demand rest, long deny list | stock set minus 8 jobs | Startup storm and idle load. Our crash tally for the last runs shows only `AegirPoster` (20-28x/run), `assetsd` (1-3x), `MusicWidgets` (1x) [ours: logs/runs/*/merged.log, grep CRASH] |
| FrontBoard watchdog | `FBLaunchWatchdogScale`=20 in two domains | `BKNoWatchdogs`, `FBLaunchWatchdogExceptions` only for purplebuddy | The older poster `extension check-in timeout` kills were at cpus=1 [ours: logs/qr-20261022T063754Z.txt]; at cpus=4 they are gone from the runs I grepped (log tails only), AegirPoster remains [ours: logs/runs/20261022T104928Z-spi-top/merged.log:156-159]. The check-in timeout is PlugInKit's, probably not FrontBoard's launch watchdog [guess], so low expectation |
| App snapshots | JPEG/PNG forced by hook | stock ASTC path | Will bite when apps are used (scene-update watchdog), not before |
| Debug visibility | `applog`, `applog_name` | `tools/patch-launchd-plist.py` edits `xpc/launchd.plist` per job (needs a reflash) | We cannot read why AegirPoster aborts without a reflash |
| Persistence | RAM `/var`, saved as an archive on exit | HFS+ read-write root in place | Different design, see 5 |
| Kernel printf | `-v` only | `debug=0xc serial=3 keepsyms=1 -enable_kprintf_spam -noprogress ... launchd_unsecure_cache=1` [ours: tools/build-xnu-image.sh:21] | `-enable_kprintf_spam` and `debug=0xc` cost boot time through the console [guess; unmeasured] |

### 1.3 Suggested steps, ranked by payoff/cost

1. **Root tweak: `BSUIVibrancyEffectViewDisabled`=true in `com.apple.springboard`** (one `merge_pref` call, no kernel). Re-implemented from the idea only; our own check is to
   grep the 20H392 shared cache for the string before trusting it (as docs/research/18 sec. 4.2 did for the Accessibility keys). Keep the Accessibility keys as a second
   experiment. Needs a reflash (`tools/fflash-userdata.sh`, pre-approved). Kind: userland recipe.
2. **Black screen: stop the display-off request from being fatal.** Two independent steps: (a) a boot-arg in patch 0077's framebuffer driver (e.g. `sweetfbpower=0`) that
   accepts `RequestPowerChange` but does not blank, so a run cannot go black by itself; (b) set Auto-Lock to never in the root (key name unknown; find it in the dsc,
   see section "could not determine"). Both are small. Kind: xnu patch + root tweak. Then wake-by-touch/power key is a separate item (area 2).
3. **Kernel: register a MAC policy named `AMFI` with `mpo_policy_syscall` handling 0x5a and 0x60** (our own implementation, keeping our existing platform-binary logic).
   Gains: (i) a kernel-side answer to ExtensionKit's question that does not depend on a feature-flag plist being read [guess about what 20H392 actually does], (ii) the
   prerequisite for every `DYLD_INSERT_LIBRARIES` idea below and for the sailfish `sweetenv` plan in docs/research/18. Verify first with `applog_name`-style
   logging (item 4) that `__mac_syscall` with name "AMFI" really fails today. Kind: xnu patch; check with `tools/ci-kernel --dispatch` and `tools/stack-apply --only-new`.
4. **Kernel: `applog` / `applog_name` boot-args** (stdout/stderr of `/dev/null` launches to the console; per-process `OS_ACTIVITY_DT_MODE`). Re-implement in our own words
   in `kern_exec.c`. Then one `xrun` with `applog_name=AegirPoster,PosterBoard` prints the real abort reason. No reflash needed. Kind: xnu patch + boot-arg.
5. **Identify AegirPoster before fixing it.** Symbolicate the existing `CRASH AegirPoster ... bt` lines as docs/research/13 did. I believe (not verified) that Aegir is the
   Astronomy wallpaper and uses Metal [guess]; if the abort is `_MTLReportFailure`, the cheap options are removing/disabling that poster extension in the root or
   choosing a non-Metal default poster; no injection needed. Kind: userland recipe.
6. **Measure job pruning instead of copying their list.** Add an on-demand rewrite for non-core jobs inside our `xpc/launchd.plist` edit (strip RunAtLoad/KeepAlive etc. for
   jobs outside an explicit core list), keep their rule of a same-afternoon control. Start with the deny entries they justify with a crash loop (CommCenter - ours already,
   syncdefaultsd, homed, routined, duetexpertd) only if our own logs show the loop. Kind: root recipe (`tools/sweet-root-tweaks.py`), plus a count of processes/CPU at idle.
7. **Seed the small prefs** (`FBLaunchWatchdogScale`=20 in springboard + frontboard, `ForceNoBuddy`, keyboard intro flag, `wirelessdisplay_max_count`=0). Cheap, low expected effect [guess].
8. **First boot at cpus=4 or 8** with a fresh root and `--qr off`; record the DataMigration wall time. This answers whether 35 min was only cpus=1 [guess]. Kind: boot-arg, no code.
9. Later, when injection exists: snapshot format hook (apps), lock-button long-press hook, `siri_orb` skip, software-HEVC wallpapers through mediaserverd (we currently convert HEIC to PNG on the
   host, which needs no mediaserverd [ours: tools/sweet-root-tweaks.py:wallpapers]). Addresses must be re-derived for 20H392 (theirs are 20F66) [src U:inject_sb.c:150-157].

Transferable vs Qualcomm-specific: everything in this section is hardware-independent (launchd, prefs, AMFI/sandbox syscalls, injection, debug boot-args). Not transferable:
the RAM-image/empty-/var design (our root is persistent by design), and their iOS version's hard-coded addresses.

---

## 2. Input: how touch and keys reach SpringBoard

### 2.1 What they do

- A kernel driver produces plain samples (type DOWN/MOVE/UP or BUTTON_DOWN/UP, finger index, x,y scaled 0..65535) into the character device `/dev/redwood-touch`
  [src U:userland/src/redwood_input.h:1-45 and K:xnu/files/bsd/dev/redwood_input.c]. A thread **inside backboardd** (injected library) reads it and turns samples into
  HID events through the private virtual-service API: `IOHIDEventSystemClientCreateWithType`, `IOHIDVirtualServiceClientCreateWithCallbacks`,
  `IOHIDVirtualServiceClientDispatchEvent` [src U:inject.c:710-735, 883-933]. No IOHIDFamily kext objects are needed in the kernel. (We instead compile IOHIDFamily's
  event service into the kernel and publish `SweetHIDTouch`/`SweetHIDButtons` for backboardd's `IOHIDEventServiceUserClient` [ours: xnu/patches/0058, SweetHID.cpp]. Both feed the same
  backboardd event system.)
- Two services are created: a digitizer (usage page 0x0d/0x04, properties `Built-In`, `DisplayIntegrated`, `Transport=SPI`, `DeviceUsagePairs`) and a consumer-control "buttons" service
  (page 0x0c, usage 0x01) [src U:inject.c:884-933]. Because the event server is the very process still starting, **a virtual service created at the wrong moment is never opened**
  ("still waiting for a main screen digitizer"); they wait for the property `DeviceOpenedByEventSystem` and recreate the service up to 20 times [src U:inject.c:870-933].
- Event shape per frame: one **hand** digitizer event (`DIGITIZER_HAND`) with `any finger down` for range/touch, one **finger child** per finger that is down or just lifted
  (index i+1, identity i+2, pressure 1.0 or 0, event mask `RANGE|TOUCH` on the changing finger or `POSITION` for a move), and on **both** the integer field
  `DISPLAY_INTEGRATED`=1 and `IS_BUILT_IN`=1 set on the event itself [src U:inject.c:937-973]. Coordinates are normalized 0..1.
- **Edge flag**: a finger that lands within 16/375 of the sides, 16/812 of the top or 34/812 of the bottom gets `kIOHIDDigitizerEventFromEdgeTip` for its whole life; UIKit's screen-edge
  recognizers (swipe home, back) take no touch without it [src U:inject.c:757-775, 967]. (We have this since 0069 [ours: xnu/patches/0069-sweet-touch-edge.patch].)
- **Display-state handshake**: backboardd sets the property `TouchDetectionMode` on the digitizer service. Values seen: 0xff and 0xfe during boot, 0x1 display on, 0x1fe after the
  lock button; 0xfe = "display off, a tap wakes it". When a finger lands in mode 0xfe they **synthesize a lock-button press and release** on the buttons service and swallow the
  rest of that touch; this is what "wake on touch" is [src U:inject.c:812-860, 1086-1096].
- Buttons: consumer-page keyboard events `Power 0x30`, `Menu 0x40`, `VolumeUp 0xe9`, `VolumeDown 0xea`, `IS_BUILT_IN`=1 [src U:inject.c:995-1011]. The state of the lock button is also published as
  Darwin notification state (`com.redwood.button.lock`) so SpringBoard can drop a long press that was already over: waking the display inside the press handler can take longer than a long press
  and every wake otherwise opened Siri [src U:inject.c:978-993; inject_sb.c:436-470].
- backboardd's `-[BLControl waitForALSArrival]` waits 5 s for an ambient light HID service that will never exist; they use that wait to synchronise with their own service, and note that
  removing the wait changed whether backboardd took their touch service [src U:inject.c:670-706, 1256-1262].
- The producer side (Goodix over SPI) polls the interrupt line every 8 ms in a thread (no IRQ) [src K:xnu/files/bsd/dev/redwood_goodix.c:19-24]; the same design as our polling FTM5 driver [ours: docs/research/15-touch-gs101.md:36].

### 2.2 Differences against our `SweetHID` that could matter later [ours: xnu/patches/0058-sweet-hid-event-services.patch, SweetHID.cpp]

1. **`TouchDetectionMode` is ignored.** Our `IOHIDEventService::setProperties` accepts any dictionary and does nothing with it. [src: SweetHID.cpp `setProperties`]. So (a) we cannot
   know the display/touch state backboardd wants, (b) there is no wake-on-touch, (c) after a display-off request (0077) the only wake path is a hardware key whose press has never been tested
   [ours: HANDOFF.md:449 (session 9 open risks)]. Candidate step: record the value in `SweetHIDTouch::setProperties`, log it once per change, and in mode 0xfe turn a touch-down into
   a Power key press/release through `sweet_hid_key(0, ...)`. Kind: xnu patch (`SweetHID.cpp` only). Hardware-independent.
2. **Events carry no per-event `IsBuiltIn` / `DisplayIntegrated` fields**; only the service has the property `DisplayIntegrated` [ours: SweetHID.cpp `SweetHIDTouch::start`]. Whether IOHIDEventService stamps
   them for a real driver in 1915.140 is not something I checked [guess]. If the first real touches arrive and SpringBoard ignores them, this is the first thing to compare. The field numbers
   they use are `0xb0019` (display integrated) and `0x4` (built in) [src U:inject.c:758-759].
3. **Index/identity numbering**: ours passes transducer index `i+2` and identity `i+2` on finger children and `0` on the hand; theirs index `i+1`, identity `i+2`, hand identity 0. [src U:inject.c:966-967; ours: SweetHID.cpp]. Unlikely to matter [guess].
4. **Late service creation** is not a risk for us in the same way (our services are IOKit objects matched at boot). The `waitForALSArrival` delay still exists on our side [guess: same iOS code]. It costs ~5 s of boot.
5. **Long-press vs wake race** (Siri opening after a wake by button): our display-off path (0077) fades over several vsyncs and a wake on a slow renderer may hit the same race [guess]. Park until keys and touch are proven.

### 2.3 Qualcomm-specific vs transferable

Transferable: everything in 2.1 above the producer driver (event shape, edge flag, `TouchDetectionMode`, wake emulation, long-press race).
Qualcomm-specific: the GENI SPI engine, RPMh regulators, SPMI PMIC keys [src K:redwood_goodix.c:1-45; redwood_buttons.c:1-45].

**Cross-over for our open touch problem (state as of the session-10 handoff, which post-dates docs/research/21):** the engine is no longer the prime suspect on our side. With the ABL-exact manual chip select
("variant F") the shim probe completed a burst at ~1 MHz, but every RX byte was `00` (`ff ff ff ff` with the MISO pull-up), no controller-ready event followed a reset pulse, and the working guess is "rails off /
chip in reset / bus not ours" (LDO25M/LDO26M behind ACPM) [ours: HANDOFF.md:466-470, 484-486; docs/research/23]. Two things in their driver bear on that:
1. **Power comes first, and is written by hand.** Before any SPI traffic their driver switches the chip's I/O supply (an LDO of the PM8350C, one request into RPMh's command slots) and the analog supply (a GPIO), then pulses reset;
   every step prints what it is about to touch and what it found, and "no answer" prints the pad and regulator state [src K:xnu/files/bsd/dev/redwood_goodix.c:6-18, 676-678, 1082-1084, 1415-1440]. Same order we need once the ACPM client exists (Samsung S2MPG10 behind ACPM, not RPMh: only the pattern transfers).
2. **Independent cross-check by pad-level SPI.** Where the engine "cannot be used" or the chip does not answer, they drive CS/CLK/MOSI as GPIO outputs and read MISO from the pad (mode 0, MSB first, one GPIO write per edge), and they try
   both assignments of the two data pins because "which of the two data pins is the input is not written down anywhere" [src K:redwood_goodix.c:558-592, 1385-1425]. For us this is a cheap way to separate "engine/clock" from "chip not powered or bus not ours": if a GPIO-driven read of
   the chip-id register still returns `ff`/`00` with the rails up, the engine is cleared. The pads it needs are the same CON/DAT registers the probe already uses [ours: HANDOFF.md:440, 466-470]. FTM5's limit is 10 MHz [ours: docs/research/21]; GPIO toggling from EL1 is far below that [guess].

### 2.4 Suggested steps

1. **`TouchDetectionMode` logging + wake-on-touch via a power-key event in `SweetHID.cpp`** (2.2.1). Hardware-independent, small, and needed as soon as any touch or key works. Kind: xnu patch.
2. **Pad-level (GPIO) SPI as an extra variant of the shim `--touch-probe`**, run after the rails step (ours: HANDOFF next-step 2) rather than before it: CS/CLK/MOSI outputs, MISO input, one FTM5 chip-id read, optionally with MOSI/MISO roles swapped as the reference does. Writes only touch-bank pad registers. Kind: shim probe. Rank is below the rails work because the engine now completes bursts.
3. If touch events are ignored on first contact: add `IsBuiltIn` and `DisplayIntegrated` to the events (2.2.2).
4. Keep the virtual-service route (`/dev/sweet-touch` read by an injected backboardd thread) in mind only as a fallback if backboardd never opens our kernel HID service; it is proven on their side [src U:inject.c] but needs injection (1.3.3).

---

## 3. Display / GPU path

### 3.1 What they do

- **Display**: the boot-loader framebuffer; a stand-in `IOMobileFramebuffer` user client copies the pixels of each swap's layers into it, vsync is a timer, `RequestPowerChange` is a no-op
  [src K:xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp:1-12, 1360-1366]. (Same design as our `SweetFramebuffer`.)
- **GL for CoreAnimation**: CoreAnimation's render server draws with Metal, else OpenGL ES, else its CPU rasterizer. They give it GLES by replacing `EAGLContext` methods inside
  backboardd (`libredwood-gl.dylib`, 4205 lines): `initWithAPI:properties:` makes a context of a Mesa library, `getMacroContextPrivate` returns a table of functions called as `entry(table[0], ...)` at
  the offsets of Apple's own OpenGLES library (generated by `mkeagl.py` from a slot list and Mesa's `gl.xml`), `texImageIOSurface:` and `attachImage:toCoreSurface:` make a texture **be** an IOSurface
  where the driver can share memory and fall back to copy-in/read-back otherwise [src U:userland/src/eagl.c:1-40, 90-95]. Shader source is rewritten for CoreAnimation's habits (pixel-unit texture coordinates,
  blue-first pixels, draws with no program) [src U:eagl.c:41-48]. The Metal device array in backboardd is emptied so CA does not pick Metal [src U:inject.c:530-545, 604-632].
- **Mesa**: release 25.0.7 + patches, built as **one library** `libOSMesa.8.dylib` (gallium drivers `softpipe,freedreno`, `-Dllvm=disabled`, OSMesa frontend given an OpenGL ES profile by a 33-line patch)
  [src U:tools/mk-mesa.sh:55-110; tools/mesa/osmesa-es-profile.patch]. Their `redwood_gl_create()` picks the GPU if the kernel service exists, else the software rasterizer, and the library "is not even loaded"
  without `gpu=1` [src U:userland/src/gl_osmesa.c:1-50; userland/build.sh:41-52 (GL=1 comment)].
- **Kernel seam**: an IOKit service `RedwoodGPU` whose user client speaks a **subset of Linux's msm DRM ioctls** (selector = ioctl number, structure passed in/out; buffers = client memory);
  a ~100-line libdrm stand-in (`redwood_drm.c`) is compiled into Mesa's freedreno winsys; one non-Linux request (0x7e) makes a buffer out of an IOSurface's memory
  [src K:xnu/files/iokit/Kernel/RedwoodGPU.cpp:1-60; U:tools/mesa/redwood-drm/redwood_drm.c:1-60].
- **What the Mesa-on-Darwin patch had to change** (365 lines, only the categories matter): meson gating for "no libdrm", buffer mapping through the kernel instead of `mmap`, **the fence clock**
  (`CLOCK_MONOTONIC` on Darwin is not the clock the kernel's fence uses; they use `CLOCK_MONOTONIC_RAW`/`mach_continuous_time`, otherwise waits expire at once), shader disk cache disabled (no build-id in a
  Mach-O), screen RAM size from `os_get_total_physical_memory`, an `EGLImage` hook in OSMesa that imports an IOSurface, and one hardware workaround (framebuffer-read barrier) found with CoreAnimation
  [src U:tools/mesa/freedreno-darwin.patch].
- **Cross-build recipe**: Mesa is built on Linux with the kernel's `xnu-clang` wrapper (target `arm64-apple-ios16.5`, ld64 from theos' cctools, theos' iPhoneOS16.5.sdk) via a meson cross file
  `c = ['xnu-clang', '-arch', 'arm64', '-isysroot', SDK]`, `system = 'darwin'`, `subsystem = 'ios'` [src U:tools/mk-mesa.sh:80-100; K:xnu-deps/host/bin/xnu-clang]. Needs meson, ninja, bison, flex, python venv (mako),
  llvm-ar; no LLVM [src U:tools/mk-mesa.sh:26-27].
- **Metal / AIR** (`U:docs/air.md`, 4990 lines): a translator from Apple's shader bitcode (AIR, in `.metallib`) to NIR so that the existing backend (ir3) compiles it; status in their own words: 456 system
  modules read, 321 translated and compiled, 30 run on the Adreno with a finished result; "the other two thirds are not here and are not started": the `_MTLDevice`/`IOGPUMetalDevice` object graph
  (AGXMetalA11 alone is 53 classes / 1158 methods, IOGPU 711 more) and the kernel submit ABI [src U:docs/air.md:1-20, 24-40]. A Metal driver of their own (`redwoodmetal.m`) exists as an experiment; GLES stays the production path for CoreAnimation [src U:userland/src/inject.c:530-545].

### 3.2 What is transferable to gs101 / Mali-G78 and what is Qualcomm-specific

Transferable (hardware-independent): the EAGL shim and its `getMacroContextPrivate`/slot approach (this is the same structure as sailfish's `sweetgl` that docs/research/18 already ported on paper), the OSMesa-ES
trick, the "one library" packaging, the IOSurface-as-texture contract and its read-back fallback, the gl trace boot-args (`gltrace`, `glshaders`, `glcensus`) as a debugging model, the fence-clock gotcha, the shader-cache gotcha,
the cross-build recipe, and the **software path as a first milestone**.
Qualcomm-specific: freedreno/ir3, Adreno power/GMU/zap shader/SMMU (`redwood_adreno.c`), msm ioctl semantics. A Mali-G78 equivalent needs the Panfrost `pan_kmod` seam instead (docs/research/18 sec. 2.3) and a
kernel JM driver [ours: docs/research/17]; the AIR frontend is NIR-based and so is Panfrost's compiler, but Panfrost has no ir3 patch equivalent in their tree [guess].

### 3.3 Suggested steps

1. **A software GLES renderer for CoreAnimation before any GPU exists.** Their `softpipe`-only build needs no kernel driver (`redwood_gl_create()` falls back to the software rasterizer when
   `RedwoodGPU` is absent) [src U:gl_osmesa.c:1-50]. For us this would put CoreAnimation on its OpenGL ES renderer (instead of the CPU `CA::OGL::SW` path we believe is in use [ours: docs/research/18:`0.1`]), which is exactly the path a Mali
   port will use later, and may also remove the vibrancy artefact [guess]. It needs: AMFI injection (1.3.3), the EAGL shim (we can design our own from docs/research/18 sec. 1.2, the sailfish layout), a Mach-O cross build of Mesa
   (item 2). Large (the shim is ~2000-4000 lines in both references) but entirely CPU-side and testable without kernel work. Kind: userland recipe + injected library.
2. **Make the Mesa cross-build reproducible on this Linux host with theos' toolchain**, which answers docs/research/18 sec. 3.2 ("no linker for Mach-O", cross-build needing macOS CI): their `xnu-clang` wrapper + theos ld64 +
   `-Dllvm=disabled` + softpipe builds with only the packages above [src U:tools/mk-mesa.sh]. Unverified for Panfrost and for Mesa 26.x [guess]. Another agent is installing theos; do not install here.
3. For the Mali path keep docs/research/18's option A (a fake `xf86drm.h` over an IOKit user client): HackiOS demonstrates the same layering with freedreno (kernel service + libdrm stand-in + a small Darwin patch for mapping and the fence clock)
   [src K:RedwoodGPU.cpp:1-30; U:redwood_drm.c:1-60]. Add their two gotchas to the plan: fence clock domain, and a per-process shader cache disabled until a build-id exists.
4. Do not attempt Metal/AIR now: their own status after a large effort is shaders translated but no device object graph [src U:docs/air.md:1-20].

---

## 4. Wi-Fi

### 4.1 What they do (layers, bottom to top)

1. **Radio bring-up (Qualcomm-specific).** The WCN6750's firmware runs on a Hexagon DSP ("WPSS") that the AP must load and start: SMEM, SMP2P, IPCC doorbells, the RPMh/AOP mailbox, TrustZone calls
   to authenticate and start the image, then GLINK, QRTR and QMI services to talk to the Wi-Fi firmware, in steps `wifi_step=1..16` where each step says what it will touch first
   [src K:xnu/files/bsd/dev/redwood_wifi.c:1-70; redwood_qmi.c; redwood_wlfw.c]. None of this exists on a Broadcom PCIe dongle.
2. **Linux driver inside XNU (partly transferable technique).** The unmodified Linux `ath11k` is compiled outside the XNU tree against **FreeBSD LinuxKPI headers** (patched), plus ~4,700 lines of their own
   (`wifi/lx`: locks, sleeping, work queues, timers, memory, DMA memory, skbuff, firmware files, and the "bus" for the chip; a stand-in for mac80211/cfg80211 in `lx_80211.c`, 2097 lines, with scan, WPA2-PSK join, 4-way handshake in
   `lx_wpa.c`), tied into **one relocatable object** that the kernel links when present; the entry point is `lx_start()`; XNU gives the object only the handful of calls of `sys/redwood_lx.h`
   (memory, time, threads, one sleep lock, options). The XNU side file `redwood_lx.c` (582 lines) implements them [src K:tools/mk-ath11k.sh:1-45; K:xnu/files/bsd/dev/redwood_lx.c:1-50; wifi/lx/src/lx_base.c:1-30, lx_bus.c:1-40, lx_80211.c:1-30].
   Sources are fetched at build time (Linux 7.0.14 tarball, FreeBSD 15.1) and not kept in git; each file's licence header is checked by the build [src K:tools/mk-ath11k.sh:40-60].
3. **Station boundary.** A neutral C interface `redwood_sta.h` (power, channels, scan, heard networks, join, leave, state, events) between the radio logic and everything iOS-facing; one kernel thread serialises all calls
   [src K:xnu/files/bsd/sys/redwood_sta.h:1-70].
4. **`en0` without IONetworkingFamily.** `redwood_net.c` makes the interface with the BSD interface KPI (like `if_fake`), Ethernet framing; a "link driver" fills `struct redwood_net_link {name, address, wireless, up, transmit, poll}`
   and calls `redwood_net_attach()`; frames in via `redwood_net_input()`, link state via `redwood_net_link_state()` (the cable/associated signal that starts DHCP). The first link was QEMU's virtio-net, the second the radio
   [src K:xnu/files/bsd/sys/redwood_net.h:1-60; bsd/dev/redwood_net.c:1-60]. A radio with Ethernet decap in firmware passes Ethernet frames directly.
5. **The apple80211 bridge (the layer docs/research/20 sec. 1.3 calls "topaz gap, unread").** iOS's `IO80211.framework` falls back to ioctls when the service has no user client. They register an
   `SIOCSA80211`/`SIOCGA80211` handler (0x802869c8 / 0xc02869c9; struct `{char ifname[16]; int32 type; int32 val; uint32 len; user_addr_t data}`) reached through the interface's ioctl, and answer from the station boundary
   [src K:xnu/files/bsd/dev/redwood_apple80211.c:1-48, 60-66, 714-739; redwood_net.c:228-236]. Request types they implement (numbers are Apple80211's, they read the layouts out of the 16.5 `IO80211.framework`):
   GET: 1 SSID, 2 AUTH_TYPE, 4 CHANNEL, 9 BSSID, 11 SCAN_RESULT (one BSS per call, EIO when exhausted), 12 CARD_CAPABILITIES (bits: AES, AES-CCM, PM, short slot/preamble, WPA2, WME, short GI; wrong bits mean
   "no WPA2, Unable to join"), 13 STATE, 15 OP_MODE (1 = STA), 16 RSSI, 19 POWER, 21 ASSOCIATE_RESULT, 27/254 (HW_)SUPPORTED_CHANNELS, 43/44 driver/hardware version strings, 103 CURRENT_NETWORK, 207 CHANNELS_INFO;
   SET: 10 SCAN_REQ, 19 POWER, 20 ASSOCIATE (passphrase "as typed" = key cipher 10, or the 32-byte PMK = cipher 6), 22 DISASSOCIATE
   [src K:redwood_apple80211.c:68-80, 289-455, 458-600]. Struct sizes are asserted (`_Static_assert`): assoc 592 bytes (key at 0x48), scan request 5456, scan result 1164, channels 4824, channels-info 10016, link event 32
   [src K:redwood_apple80211.c:152-176]. Asynchronous results go back as **kernel events**: `kev_post_msg` with vendor Apple, class `KEV_IEEE80211_CLASS` (6), subclass 1, event code = Apple80211 event number, data starting
   with the 16-byte interface name; events used: POWER_CHANGED 1, SSID_CHANGED 2, BSSID_CHANGED 3, LINK_CHANGED 4 (32-byte payload, or the framework drops it), ASSOC_DONE 9, SCAN_DONE 10
   [src K:redwood_apple80211.c:178-182, 598-668]. Join result numbers: 1 success, 21 invalid PMK, 9 timeout, 2 other [src K:redwood_apple80211.c:36-41, 640-650]. Every request's type is logged and a per-type summary is printed at
   the end of a run, which is how they found which requests wifid needs [src K:redwood_apple80211.c:30-34, 672-739].
6. **IOKit registry stand-ins for `wifid` and configd.** `RedwoodWLAN.cpp` (137 lines) publishes two things: a service of class `IO80211Controller` under the platform, and below it one object that is both an
   `IONetworkInterface` and carries `IO80211InterfaceRole=Infrastructure` and `IOInterfaceName=en0`, with **no user client on purpose** ("Falling back to IOCTL path"). configd lists a Wi-Fi interface only
   when its parent is an `IO80211Controller`; without a network service the system never routes through it and `wifid` reports "failed to become routable interface" [src K:xnu/files/iokit/Kernel/RedwoodWLAN.cpp:1-30].
7. **Daemons.** `wifid` is in the launchd job list; its `_LimitLoadToDeviceTree` (needs a `wlan` node) is simply removed from the plist copy [src U:userland/mkjobs.py:289; build.sh:104-118]. A tiny job `com.redwood.wifi` runs
   `net_test wifi`: with `wifi=1`, wait for `en0` and ask configd's IPConfiguration plugin to run DHCP only if configd made no service of its own [src U:userland/com.redwood.wifi.plist; net_test.m:1-40].
8. **Credentials.** A network joined at boot is read from a file in the image (`fw/network`: name + passphrase) and the 4-way handshake runs in the kernel; joining from Settings goes through request 20 [src U:userland/build.sh:564; K:README.md:44].

### 4.2 Map onto docs/research/20

| docs/research/20 | HackiOS equivalent | Hardware-independent? |
|---|---|---|
| Stage 3 item 1: ADT `wlan` node so `wifid` starts | They pop `_LimitLoadToDeviceTree` from the job instead [src U:mkjobs.py:289] | Yes. Either works; the plist edit is a root-recipe change, no kernel/ADT |
| Stage 3 item 2: `IO80211Controller` stand-in + `IOBSDName=en0` | `RedwoodWLAN.cpp`: two property-only classes, interface object with `IO80211InterfaceRole`, `IOInterfaceName`, no user client [src K:RedwoodWLAN.cpp:1-30] | Yes |
| Stage 3 item 3: `ifnet` en0 answering `SIOCSA80211/SIOCGA80211`, first version logs every type and returns EOPNOTSUPP | `redwood_apple80211.c`: the full list above, event posting, per-type statistics | Yes (layouts are iOS 16.5; 16.7 is the same major, check with the struct asserts) |
| Stage 3 item 4: radio core exposing scan/join/link/tx/rx/events | `redwood_sta.h` boundary + `redwood_net.h` link | Yes |
| Stage 2: Broadcom msgbuf PCIe driver, firmware download | `ath11k` over `lx_*` layer, QMI firmware handshake | **No** (ath11k/QMI/GLINK are Qualcomm). The compat-layer method applies if we ever wrap a Linux driver, but doc 20 chose an own msgbuf driver |
| "WPA2 PSK: firmware supplicant or our own 4-way" | own SHA-1/HMAC/PBKDF2/AES-unwrap and 4-way in `lx_wpa.c`, tested against RFC vectors (`tools/wpa-test`) [src K:wifi/lx/src/lx_wpa.c:1-12] | Only needed if the Broadcom firmware does not do the handshake itself (brcmfmac has a firmware-supplicant mode [guess]) |

### 4.3 Suggested steps (ranked)

1. **Start Stage 3 now, without a radio, using the USB-ECM `en0` we already have as the link.** Their link abstraction (`redwood_net_link`, first link = a virtual NIC) is exactly this order. Concretely, as one xnu patch family: (a) `SweetWLAN` property-only classes as in 4.1.6;
   (b) mark the existing USB `en0` as wireless (the "subfamily" flag) behind a boot-arg; (c) an `apple80211` ioctl handler that **only logs request types and answers EOPNOTSUPP** first, then the minimal set above
   with a fake station (power on, one canned channel list, a scan that returns a fixed BSS); (d) remove `_LimitLoadToDeviceTree` for `com.apple.wifid` in the root recipe; (e) the kernel event poster. Success lines: `wifid` appears in the
   process list, Settings shows a Wi-Fi row, the request-type histogram is printed. This closes docs/research/20's "request set wifid needs" unknown (D12 2.3) and takes ~8-15 days off the critical path while the PCIe/PHY blockers are open [ours: docs/research/20:204-275].
   Kind: xnu patch + root tweak. Hardware: USB gadget (never run under XNU yet [ours: HANDOFF.md:402]) or QEMU.
2. Take their per-request log and end-of-run summary as a design rule for ours.
3. Keep the "link driver" interface narrow: `{address, wireless, up, transmit, poll}` + `attach/input/link_state` is enough for both the USB debug link and the BCM4389 link.
4. Do not adopt the FreeBSD-LinuxKPI route for the radio unless we decide to wrap Google's `bcmdhd` (rejected in doc 20 because of GPL and size) or mainline `brcmfmac` (does not list 4389 [ours: docs/research/20:29-32]). Their method shows it is feasible for a Linux driver of this size; the cost
   they paid is the 4,700-line compat layer plus a stand-in for mac80211/cfg80211 [src K:wifi/lx/src/*].
5. Firmware supply: they take it from the user's own phone at image-build time and keep it out of git, the same rule we chose [src K:README.md:86-99; ours: docs/research/20 sec. 1.3].

---

## 5. Storage: persistent writes

### 5.1 What they do

- **They do not drive the UFS controller from their own code for normal I/O.** UEFI never exits; its UFS driver keeps the controller initialised and the loader serves read/write requests from core 0 while the
  kernel runs on another core, through a shared page (`ufs_shared_pa=`): partition table, request, answer, buffer, one request at a time [src K:xnu/files/bsd/dev/redwood_ufs.c:1-30]. A later driver (`redwood_ufshc.c`) takes the controller's transfer list for the run so 32 requests can be
  in flight, with the read-only guarantee enforced by a **whitelist on every command before the doorbell**: SCSI opcodes (READ(10)/(16), TEST UNIT READY, REQUEST SENSE, INQUIRY, READ CAPACITY), data direction from-device-only, UPIU never-write flag, LUN restricted, descriptors read back and re-checked
  after being written into the slot [src K:redwood_ufshc.c:1-45]; it restores the firmware's list on the way out [src K:redwood_ufshc.c:1-25].
- **Writes**: the iOS volume is APFS and **read-only** in their kernel (their kernel APFS driver reads; the build writes it from the computer). Persistence is an **archive of `/private/var`** (own tar-like format with xattrs, hard links,
  flags, checksums) written into a **partition of their own** with two alternating 2 GiB slots, header written last over a zeroed one; `var_store watch` registers a sysctl and the kernel asks it to save on the orderly way back to the loader; boot-time `load` verifies the checksum before unpacking
  [src U:userland/src/var_store.c:1-60; K:redwood_ufs.c:1-30]. Writes are accepted only on the character device of that partition, whole blocks past `UFS_WRITE_FIRST` [src K:redwood_ufs.c:17-24, 78].
- That partition was **carved out of Android's userdata**: they shrink the f2fs filesystem with Android's own kernel, then write a new GPT [src K:tools/userdata-carve.py:1-30].

### 5.2 Compared with ours

- Ours: our own UFS host driver (cold init, DMA, HS) and a **read-write HFS+ root in place on userdata** [ours: xnu/patches/0028, 0030, 0031, 0080]; writes are limited to the userdata LBA range by a re-check of the partition bounds before each command [ours: xnu/patches/0030-sweet-ufs-writes.patch:9-16].
  The iOS volume itself is therefore writable and `/var` persists without a save step; DataMigration state survives between runs [ours: HANDOFF.md:408].
- Their carve-out of a partition needs a GPT write, which is forbidden for us by AGENTS.md rule 1; we have no use for it because userdata is already ours.
- UEFI-resident services (loader serving storage) have no analogue on the Pixel: no UEFI is known for it (see section 6).

### 5.3 Suggested steps

1. **Sync before the self-reset.** Our `--restart` path resets the SoC through the PMU [ours: HANDOFF.md:365, 408]; I found no filesystem sync or remount-read-only before the reset in patches 0083/0086 (only a log flush) [ours: grep of xnu/patches/0083, 0086].
   The volume is journaled [ours: tools/mkiosroot.sh:14-15], so a replay should cover it [guess], but a bounded `sync` a few seconds before the reset is cheap, and their `redwood_var_save_on_return` shows the pattern of
   "kernel asks the writer to finish, waits bounded, then returns" [src K:redwood_ufs.c (redwood_var_save_on_return), loader_return.c:60]. Kind: xnu patch.
2. Adopt their **command whitelist + descriptor read-back before the doorbell** as an audit item for `sweet_ufs.c` only if we ever expose SCSI passthrough; today `ufs_scsi()` has fixed callers (read, write, sync-cache, probes) [ours: xnu/patches/0030-sweet-ufs-writes.patch:87-97]. Low priority.
3. Do not adopt the archive design: it exists because their APFS root is read-only; ours is not. Revisit only if we move to an APFS root.

---

## 6. Loader / boot flow and the test cycle

### 6.1 What they do

- **Chain**: `fastboot boot` of an Android boot image whose "kernel" is gzip(BootShim + a UEFI image (Mu-Silicium port) + an "XNUP" container) and a DTB; the shim copies the UEFI to its base and the container to `PAYLOAD_BASE`; a patched `BootManagerLib`
  starts the XNU loader (an EFI application built with clang + lld-link, no EDK2) from the container [src K:mkxnuimg.py:1-45; K:uefi/README.md:1-20; K:xnuloader/loader.c:1-14].
- **The loader stays resident** (`ExitBootServices` is never called) and **publishes a return stub** in `/chosen/loader-return`; when the kernel is done (timer, panic, `halt`) it collects the message buffer, the panic buffer and everything printed, maps the stub through TTBR0 and
  jumps back with the log; the loader restores the UEFI environment and writes the log to a storage partition (`logdump`) [src K:xnu/files/osfmk/arm64/loader_return.c:1-20; K:xnuloader/loader.c:37, 262-375].
- **Co-residency**: the kernel can run on a different core (`bootcpu=7`, the fastest) while the loader keeps core 0, serving the **fastboot protocol over its own USB link while the kernel runs**: `xl log` shows the kernel's log so far during a run,
  `xl screenshot` reads the framebuffer, `xl stop` ends the run now, a **heartbeat** counter the kernel bumps every 10 s tells the loader the kernel is alive, and the loader feeds the (Qualcomm hypervisor) watchdog until the heartbeat stops
  [src K:xl:1-25; xnuloader/loader.c:751, 869, 2220-2350; xnu/files/osfmk/arm64/loader_return.c:60-110].
- **Log design**: 1.5 MB buffer = first 256 KB kept + a 1 MB ring of the most recent output + room for the final message buffer; the dropped middle is marked with an exact line [src K:loader_return.c:30-40; docs/checks.md "Inference from absence"].
- **RAM-only session**: nothing is written to the phone; the big system image is uploaded **once per loader session** (`xl ramdisk`, 2.5 GB, ~80 s) into a fixed place inside the kernel's memory window and survives many kernel runs
  (`RESIDENT_RD_OFFSET`) [src K:xnuloader/loader.c:44-50; xl:1-20]. Kernel runs of a few seconds to minutes (`XL_SECS`) return to the loader and the next `xl run` re-uploads only the kernel.
- **QEMU**: the same kernel and images run on QEMU `virt` with a mouse as finger and `qemu/touch.py` for scripted taps and screenshots; they report the lock screen after 2-3 minutes there [src K:README.md:50-82; qemu/].

### 6.2 What is transferable to our test cycle

Our cycle: build the image, `fboot`, observe through the camera/QR (tail of ~12 kB) or USB (never run), wait for the **self-reset** (`restart` + ~140 s observed: ~11 min with `restart=520`), phone lands in recovery, `to-fastboot.sh` [ours: HANDOFF.md:408-409, 446-447; docs/AUTOMATION.md:27].
- We have no UEFI and no resident service layer after the shim hands over; their "kernel returns" and "live USB log" depend on a firmware that stays alive. A Tensor G1 UEFI port is not known to me (section 8).
- Transferable ideas that do not need UEFI:
  1. **Short, pattern-driven runs.** The reset timer is the only exit. A boot-arg that arms the reset N seconds after a chosen log line appears (the log-poll thread exists since 0086) would end a run when its purpose is served, instead of after a fixed 520 s. Kind: xnu patch + `xrun` option. Expected gain: most
     of the ~11 min per cycle [guess].
  2. **Two-part log ring (head + recent tail) with an exact dropped-marker**, and a small **priority ring** for `SWEET ...` summary lines that is repeated on every QR frame, so a 12 kB tail never hides the verdict lines. Kind: xnu patch (`sweet-qr-log`) + `qrmerge.py`.
  3. **Heartbeat in the log** every N seconds (`xrun` could tell "kernel alive but silent" from "hung") [src K:xnuloader/loader.c:869].
  4. **Run the userland in QEMU for recipe work.** Their lock screen is reached in QEMU in 2-3 minutes with the thin image; our `tools/qemu-sweet.py` only runs a payload with a mock root [ours: tools/qemu-sweet.py:1-20]. Using QEMU for root-recipe iteration (the poster crash, preference keys, launchd pruning) would remove the phone from that loop;
     it needs a root device our kernel can read in QEMU (virtio-blk, which they have as a driver [src K:xnu/files/bsd/dev/redwood_virtio_blk.c]; we have none [ours: grep virtio xnu/patches]) and a thin root (their `select-system.py` idea) or a RAM disk big enough. Cost: medium-high; payoff: every userland experiment stops needing a reflash plus a 12-minute hardware run [guess].
  5. **Per-core speed probe.** They run the kernel on the biggest core and raise its frequency because the boot loader leaves cores at a modest frequency ("about 2.5 times as fast") [src K:redwood_cpufreq.c:1-12; xnuloader/loader.c:751]. On gs101 our boot core is an A55 and `cpus=4/8`
     are untested for speed [ours: HANDOFF.md:408]. A shim probe that spins N iterations on each core and prints CNTVCT deltas would tell whether clusters sit at low frequency (read-only, no register writes). Kind: shim probe.

---

## 7. Build: the local Linux XNU build

### 7.1 What they do (exact recipe, read from source)

Packages: `git curl tar xz make cc clang clang++ flex bison m4 perl python3 autoreconf automake` (the check list of `xnu-deps/setup.sh`), plus `lld` per the README (Debian: `build-essential git curl clang lld python3 perl xz-utils`);
the wrapper calls `/usr/bin/clang`; "tested with clang 22.1" [src K:xnu-deps/setup.sh:1-18, 61-66; K:xnu/README.md:11-20, 60]. **theos** in `~/theos` (or `$THEOS`) providing
`toolchain/linux/iphone/bin/{ld,nm,otool,strip,lipo,libtool,nmedit,ctf_insert,codesign_allocate,install_name_tool}` (cctools / ld64) and `sdks/iPhoneOS16.5.sdk` [src K:xnu-deps/setup.sh:19, 42-46, 107-112, 121-135].
(Observed today: `~/theos` does not exist on this host; another agent installs it. [ours: `ls ~/theos` -> no such file].)

Steps [src K:xnu/README.md:18-32; xnu/apply.sh; xnu-deps/setup.sh; xnu-build.sh]:
1. `xnu/apply.sh`: shallow fetch of tag `xnu-8796.121.2` (commit c54f35ca...), branch `redwood`, then `git apply redwood.patch` and `cp -a files/.` (~6 s).
2. `xnu-deps/setup.sh` stage 1 (~30 s): fetch pinned sources of AvailabilityVersions-115.4, libdispatch-1415.100.11, libpthread-514.100.2, a port of `bootstrap_cmds` (mig), tcsh, unifdef; download the macOS 13.3 SDK tarball (only
   `Kernel.framework`, for the DriverKit headers); build `mig`, `migcom`, `unifdef`, `tcsh` (symlinked as `csh`), `unzbm`; link theos' cctools into `xnu-deps/host/bin`; make the SDK as a tree of symlinks to theos' iPhoneOS16.5.sdk plus `usr/local`
   (availability.pl, firehose headers). Stage 2: `xnu-build.sh exporthdrs`, then build `libfirehose_kernel.a` (from libdispatch) and `libpthread_kext.a` (from libpthread) as static libraries linked into the kernel (there is no kext loader).
3. `./xnu-build.sh -j8 -k` (~45 s on 12 threads): `make SDKROOT=iphoneos ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=BCM2837 KERNEL_CONFIGS=DEVELOPMENT ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 BUILD_WERROR=0 BUILD_LTO=0 DO_CTFMERGE=0
   HOST_CC="clang -include hostcompat.h ..." HOST_FLEX=flex HOST_BISON=bison HOST_GM4=m4 CC=xnu-clang CXX=xnu-clang++`. **`-k` is required**: make exits 2 because `check_all_exports` and three symbolset steps fail **after** the kernel is linked
   (217 "exported name not in import list" complaints from DriverKit RPC names of the `iig` stubs); look at the kernel file's date, not make's status [src K:xnu/README.md:36-46].
4. Host wrappers in `xnu-deps/host/bin` [src K:xnu-deps/host/bin/*]: `xnu-clang` (`/usr/bin/clang -target arm64-apple-ios16.5 --ld-path=ld64 -w`, a force-included compat header, drops `-enable-trivial-auto-var-init-zero-knowing-it-will-be-removed-from-clang`, `-fapple-kext-vtable-verify`,
   `-Wno-vla-cxx-extension`, `-Werror*`), `xcrun` (answers `-show-sdk-path/-version/-platform-path/-find`), `iig` (copies the pre-generated DriverKit header from the macOS 13.3 SDK and the matching `iig-impl/*.cpp` stub), `codesign`/`plutil` (no-ops),
   `sw_vers` (13.4), `install` (drops `-S`). `xnu-env.sh` exports `THEOS`, `XNU_SRC`, `XNUDEPS`, `XNU_SDK_PATH`, `XNU_SDK_VERSION=16.5` and prepends `host/bin` to `PATH`.
5. Traps they record: the repo path must not contain spaces; `xnu/export.sh` regenerates the patch from the whole tree (roll-back risk); the symptom of a stale tree is a kernel that builds and crashes at boot right after "Stackshot Succeeded"
   [src K:xnu/README.md:50-80]. Disk: ~600 MB plus theos. `iig` stubs are generated from the linker's undefined-symbol list by `xnu-deps/gen-iig-stubs.py` and must be regenerated if that set changes [src K:xnu/README.md:48-49].

### 7.2 Which steps apply to OUR patch stack (`xnu/patches`) and which do not

Our CI: macOS-14 runner, blacktop's `darwin-xnu-build` (pinned) with the real Xcode 15 `iig`, tag `xnu-8796.141.3` for the pristine check, machine config `SWEET` [ours: .github/workflows/xnu.yml:25-60; tools/stack-apply:15].

| Their step | For us |
|---|---|
| tag `xnu-8796.121.2` | **Different tag.** Our stack applies on `xnu-8796.141.3`; `tools/stack-apply` already knows how to apply it on a pristine clone. Do not use their `redwood.patch` or tag |
| `redwood.patch` Linux-host fixes (Makefile, `SETUP/config/doconf` csh, `makedefs/MakeInc.*`, `tools/lldbmacros/core/syntax_checker.py`, small source fixes for new clang) | Needed in some form for a local Linux build and **absent from our stack** (the CI on macOS did not need them). Write our own small `linux-host` patch (outside `xnu/patches/` so CI is unaffected, e.g. `tools/linux-build/`), derived by trying the build, not by copying theirs. Our `0001`/`0002` already cover the new-clang flag and `UIO_STACKBUF` |
| `MACHINE_CONFIGS=BCM2837` | `MACHINE_CONFIGS=SWEET`; our patch 0004 already sets `ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG` to arm64 for SWEET, and 0006 adds `config/MASTER.arm64.sweet` [ours: xnu/patches/0004, 0006] |
| `iig` stand-in + generated stubs | Needed (no Xcode on Linux). Our kernel is a different object set than theirs, so `undefined-all.txt` must be regenerated for SWEET; the `-k` + `check_all_exports` tolerance is the same |
| libpthread kext static library | Ours: stager `0019-stage-libpthread.sh` + `0019-sweet-builtin-pthread.patch` fetch and build pthread into the kernel (the same idea, different mechanism); `libfirehose_kernel.a`: CI links Apple's from the SDK; locally it has to be built from libdispatch as in setup.sh stage 2 |
| minicrypto instead of corecrypto | Ours: BearSSL staging (`0020-stage-bearssl.sh`, `0027/0032/0033`) and `0013/0014`; unrelated to their files |
| HFS | Ours: `0026-stage-hfs.sh` + `0026-sweet-builtin-hfs.patch` (fetched from GitHub at stage time; needs network) |
| `-k`, 45 s | Applicable: add `-k` equivalent and judge success by the kernel file's mtime and size, as `tools/ci-kernel --expect-string` already does for CI artifacts |

Build-order sketch for the other agent (do not run here): theos installed -> `tools/stack-apply --keep` style pristine copy of `xnu-8796.141.3` -> run the three stagers -> `git apply` the 93 patches in order -> apply the local `linux-host` fixes -> `xnu-deps`-style setup
(sources, host tools, SDK overlay, iig stubs) -> `make ... MACHINE_CONFIGS=SWEET -k`. Expect a long tail of first-build failures (new clang vs Apple clang beyond 0001/0002; the stubs for headers CI gets from Xcode, which `tools/stack-apply` stubs with an empty `TargetConditionals.h`
[ours: tools/stack-apply:30-34]). Whether theos' ld64 accepts our kernel link (the SWEET kernel is plain arm64, not arm64e) is unknown [guess: yes, since theirs is also plain arm64 BCM2837].

---

## 8. Prioritised top 10 for us

Ranked by expected payoff per unit cost toward milestone (1) GUI with working input, then (2) Wi-Fi. All are ideas to re-implement; none is verified on our hardware.

1. **`BSUIVibrancyEffectViewDisabled`=true (and optionally the Accessibility keys) in the root recipe.** One `merge_pref`, targets the transparent lock-screen clock. Needs a reflash. [src U:inject_sb.c:425-432]
2. **Stop the display-off request from blacking out a run** (boot-arg in `SweetFramebuffer`'s `RequestPowerChange`; auto-lock off in the root). `SWEET FB: display off` is observed in two QR logs; hackios makes that request harmless. [ours: logs/qr-20261022T053837Z.txt:30; src K:MiniIOMobileFramebuffer.cpp:1360-1366]
3. **Touch bring-up order from the reference: power first, then reset, then SPI, each step logged**, with a **GPIO-driven SPI read as an independent cross-check** (and a swapped MOSI/MISO variant). Fits the rails-off hypothesis in the latest handoff; the engine is no longer the main suspect. [src K:redwood_goodix.c:6-18, 558-592, 1385-1440]
4. **`applog` / `applog_name` boot-args** so one run prints why AegirPoster (and any extension) aborts, without a reflash. [src K:xnu/redwood.patch:449-459]
5. **AMFI MAC policy named "AMFI" with syscalls 0x5a and 0x60** (our own implementation). Kernel-side answer for ExtensionKit, prerequisite for `DYLD_INSERT_LIBRARIES`, `sweetenv` and every injected hook. [src K:kern_minimac.c:62-125; ours: patch 0035 policy is "SWEET AMFI" without `mpo_policy_syscall`]
6. **Wi-Fi Stage 3 without a radio on the USB `en0`**: `IO80211Controller`/interface registry stand-ins, `SIOCSA80211/SIOCGA80211` handler with request logging, kernel events class 6/1, `com.apple.wifid` loaded without `_LimitLoadToDeviceTree`. Closes the "request set wifid needs" unknown. [src K:RedwoodWLAN.cpp, redwood_apple80211.c]
7. **`TouchDetectionMode` handling and wake-on-touch via a Power key event in `SweetHID`** (plus `IsBuiltIn`/`DisplayIntegrated` per-event fields if first touches are ignored). [src U:inject.c:812-860, 1086-1096, 937-973]
8. **Pattern-driven early reset + sync-before-reset + head/tail/priority log rings** to shorten the ~11-minute hardware cycle and protect the HFS+ root. [src K:loader_return.c:30-40; ours: patches 0083/0086]
9. **Software GLES for CoreAnimation (OSMesa-ES softpipe through an EAGL shim), built with theos' toolchain via an `xnu-clang`-style wrapper.** Large, but unblocks the GL path and the Mesa cross-build before any Mali kernel work, and tests the shim design that Panfrost will reuse. [src U:eagl.c, gl_osmesa.c, tools/mk-mesa.sh]
10. **Measure-first job pruning and a first boot at cpus=4/8**: strip RunAtLoad/KeepAlive from non-core jobs in our `xpc/launchd.plist` edit, keep a same-afternoon control, record DataMigration wall time at higher cpus. [src U:mkjobs.py:35-45, 299-330]

Honorable mentions: per-core speed shim probe (6.2.5); QEMU as a root-recipe test bed (6.2.4); local Linux XNU build (7), which saves CI latency (~4-8 min) but needs a `linux-host` patch and iig stubs of our own; AegirPoster identification (1.3.5).

---

## 9. What I could not determine

- **Whether 20H392 honours `BSUIVibrancyEffectViewDisabled`**, and what the key for Auto-Lock is in 20H392's SpringBoard/Preferences (find it with the same dsc string greps as docs/research/18 sec. 4.2).
- **Whether the ExtensionKit feature-flag workaround is enough on 20H392**, i.e. whether `__mac_syscall("AMFI", 0x60)` is still the question asked and whether our missing "AMFI" policy name matters. Their comment is for 16.5 [src K:kern_minimac.c:73-85].
- **The cause of the AegirPoster abort** (I suspect Astronomy/Metal; unverified). Symbolicating the `bt` addresses in `logs/runs/20261022T104928Z-spi-top/merged.log:157-159` is the first step.
- **Their real time to the lock screen.** "~20 s" is a README claim [src K:README.md:42]; there are no run logs in the clones, so I could not break it down into kernel, job count, RAM image and CPU frequency effects. It also uses 8 cores at up to 2.4 GHz with a RAM-resident thin root, none of which we have.
- **Whether DataMigration at cpus=4/8 is fast**, and whether the 35 min was CPU-bound.
- **The CPU frequency state ABL leaves on gs101** and whether all clusters are usable at speed; HackiOS had to raise it on their SoC [src K:redwood_cpufreq.c:1-12].
- **Whether any UEFI exists for Tensor G1** (Mu-Silicium target list was not checked offline). Without one, the resident-loader ideas (live USB log, kernel returning to the loader) have no host; the shim-side substitute is unknown.
- **Whether theos' ld64 and their wrapper can link our SWEET kernel**, and how many new-clang errors our 93 patches meet on a non-Apple toolchain beyond 0001/0002.
- **Whether Mesa 25.0.x Panfrost builds for Darwin/iOS and needs `mesa-clc`/LLVM** for Valhall v9. Their build disables LLVM for freedreno only [src U:tools/mk-mesa.sh:109]; I did not check Panfrost's build-time needs.
- **Whether Apple80211 request layouts and event numbers of iOS 16.5 equal 20H392's.** Their `_Static_assert` sizes are for 16.5 [src K:redwood_apple80211.c:164-176]; compare against our dsc before relying on them.
- **Whether the PCIe/Broadcom side needs a Linux-driver wrapper at all** (decided against in docs/research/20; their ath11k wrapper shows the cost if that changes).
- **EL3 / ACPM gating** for GPU and Wi-Fi power: nothing in the reference helps (their Qualcomm path uses TrustZone SCM calls and RPMh, different mechanisms) [ours: docs/research/17, 20].

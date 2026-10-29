# Raven continuation: offline preparation, 2026-10-30

Status: **read from source / verified offline; no new hardware test**.
No phone commands were executed. No signing checks were disabled.

## State verified

- Main checkout `/home/xavi/pixelios-raven` is dirty on `raven-port`, missing 0201–0206. Do not reset, switch, or overwrite it.
- This isolated checkout descends from Phase 7D.6 commit `5a8bbf56d787dbb10b80f7596f30db5cae61e96a`; the remote branch was checked through the GitHub connector.
- 7D.7 root SHA256 matches `68384b4a603498d57876fd365ec0f4fb3753545cc0169f12b5fe35e5931c7a96`; `fsck.hfsplus -fn` reports OK.
- 7D.6 boot image SHA256 matches `c607a39b3a5594df0659b08dd33cadf4a70c9312e5219a2e02473c3b5db33ca2`.
- Existing kernel strings identify `xnu-8796.141.3`. Earlier `check/bsd/kern/kern_exec.c` files are snippets, not complete XNU source trees.

## 7D.8 change and tests

0207 adds only the exact `launchd_cache_loader` name to the existing helper predicate.
Parent, CS_VALID|CS_SIGNED, root mount / read-only-or-COW, spawn platform, and enclosing platform conditions remain unchanged.
Source: [set_proc_name](https://github.com/apple-oss-distributions/xnu/blob/xnu-8796.141.3/bsd/kern/kern_exec.c),
[proc_name_t](https://github.com/apple-oss-distributions/xnu/blob/xnu-8796.141.3/osfmk/mach/resource_monitors.h),
[MAXCOMLEN](https://github.com/apple-oss-distributions/xnu/blob/xnu-8796.141.3/bsd/sys/param.h).
`p_name` holds 32 characters plus NUL; `p_comm` holds 16 plus NUL. The 20-character loader name fits `p_name`.
No truncated-name aliases are necessary or admitted.

Fetched the complete Apple kern_exec.c and applied every existing patch touching that file through 0206.
0207 applies, reverses exactly, and rejects duplicate application. The compiled extracted name predicate and acceptance gate pass 168 cases, including failed constraints and truncated/suffixed names.
Run `python3 tools/raven-test-cache-loader.py ../source` from this checkout.
This verifies the affected file, not application of the complete patch series to a full XNU tree, compilation, cryptographic validity, or hardware behavior.

## 7D.9 preparation

`../root-preparation/usbroot-phase7d9-logging.hfs.pending` is a distinct reflink clone, hash-verified identical to 7D.7. **It is not modified or ready to boot.**
`logging-overlay.tar` contains exactly six metadata entries: diagnostics 0751, uuidtext and diagnostics/timesync 0755, empty .migrated files 0644, all uid/gid 272.
Modes and ownership come from the locally available Pixel 6a `tools/mkiosroot.sh` lines 142–150, rather than assumptions. Overlay generation validates archive metadata and refuses existing output.

Next, inspect actual mounted Apple source and cloned root with `tools/raven-review-services.py SOURCE ROOT` and save its JSON outside the images. This tool never installs services; it reports full definitions, executable presence/hashes/modes, and symlinks needing review. Synthetic tests cover a missing executable, preserved _PanicOnCrash, symlink refusal, and path escape refusal. Actual images have not been inventoried with this new tool.

Review service batches in this order: logging/notify; preferences/config/security; container and RunningBoard; BackBoard; SpringBoard. Check MachServices, sockets, user/group, disabled/KeepAlive conditions, environment, dependencies and central launchd cache entries before installing any plist. Merely copying a LaunchDaemon plist may not affect a cached launchd configuration. Preserve Apple binary hashes. Do not add optional early helpers without evidence.

Reference `sweet-root-tweaks.py` renames _PanicOnCrash and sets BackBoard CA_BACKDROP_SCALE=1. Treat these as reference behavior only: no blanket changes have been made. Inspect actual Raven jobs and identify crash loops first. Watchdog boot flags already present are not proof that userland watchdog behavior is handled. Keep AMFI and secure dyld cache handling enabled; log a concrete failure before proposing any security change.

After root access is available: verify parent hash again, mount only the pending clone writable, check all parent paths for symlinks, provision the six entries, verify uid/gid/modes, unmount, run read-only fsck, hash both images, and publish a new manifest with parent lineage and hardware_verified=false. Do not rename pending to final before checks pass.

Additional safety audit needed: patch 0202 caps COW **payload** at 32768 * 4096 = 128 MiB, but per-page structs and the bucket table add memory overhead. If the 128 MiB requirement applies to total allocation, the existing implementation exceeds it. Resolve explicitly in a separate numbered patch before claiming strict total-memory compliance.

## 7E ordered Raven plan

1. Obtain a green 7D.8 CI build; verify run head SHA, nonempty ARM64 kernel, SHA256SUMS and expected loader diagnostic strings. Package using the known 7D.6 workspace in a new output directory; retain the no-UFS ADT hash and all required flags. Audit packaging scripts for implicit phone operations before running. No new boot image exists yet.
2. Explicitly approved temporary hardware test of 7D.8 + unchanged verified 7D.7 root: prove loader acceptance and capture the next failure. Keep the dependency image separate so kernel/root effects remain attributable. Use `bash tools/raven-phase7-log.sh live LOG` and `bash tools/raven-phase7-log.sh history LOG` for every test. No boot is authorized by this document.
3. Test logging/service batches incrementally under RAM-COW with host image read-only. Observe normal launchd job submission and execution, then BackBoard IPC, then SpringBoard. Track COW exhaustion and actual crash/signing/dyld failures.
4. Inspect the existing no-UFS ADT, shim boot_args video, and XNU IOFramebuffer publication as one chain. Raven workspace board.json says 1440x3120, stride 5760, framebuffer size 0x1123800, candidate base 0xfac00000. These are source values, not newly measured framebuffer proof; board status text is stale. Reserve and validate the exact physical span against Raven captures, payload, USB DMA, and kernel memory before any change. Never substitute Bluejay addresses.
5. Establish display continuity from ABL through shim and XNU: retain scanout/power state, verify pixel format, mapping/cache coherency and IOFramebuffer dimensions, and ensure neither payload allocation nor console clears corrupt the surface. Identify how the reference framebuffer driver exposes the software surface to BackBoard. Gate any new hardware/register write behind independent review and Raven evidence.
6. Once BackBoard publishes a display, attempt the reference software compositor path, inspecting framework dependencies and CA_BACKDROP_SCALE. Capture first visible frame with build/root hashes and logs. GPU acceleration, touch, and other peripherals follow. Do not reuse Bluejay touch initialization without Raven controller/pin/address evidence.

## Exact blockers

- Session sandbox permits writes only to Downloads and /tmp, so the requested main repo cannot be edited directly. Changes are staged here instead; no permission bypass was attempted.
- Shell DNS cannot resolve api.github.com and gh reports an invalid credential. GitHub connector supports reads but exposes no workflow dispatch tool. CI has not been triggered. No kernel artifact was fabricated or packaged.
- Mounted image access requires host privileges; repository rules require staging root commands for the human. This session cannot perform those mounts. No actual service selection or HFS+ metadata modification has been completed.
- Only ~3 GiB host space remains; reflink cloning succeeded without a full copy. Do not fall back silently to a full 4 GiB copy.
- No tool is available for searching prior ChatGPT chats; PIOS4 was not retrieved.

The repository-wide gate rebuilds and overwrites shim/out images. It was not run; no push has been made. The new branch is local wip only, with no claim of completed 7D.8/7D.9 hardware milestones.

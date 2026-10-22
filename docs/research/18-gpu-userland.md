# 18 - GPU userland for bluejay (Mali-G78): the sailfish structure, Mesa Panfrost, cross-build, and what to do before there is a GPU

Desk research only. Nothing was run on a phone, `adb`/`fastboot` were not used, no Apple binary or cache content is quoted beyond symbol and
string names. Scope: the **userland half** (QuartzCore/backboardd -> GL -> Mesa -> a thin kernel interface). The kernel half (Mali power,
clocks, MMU, job submit) is a separate note; it is only specified here as the interface the userland needs. `docs/PLAN.md:66-70` says GPU
research goes in `13-gpu-bluejay.md`; 13 is already the stall note, so this is 18 and PLAN.md should be pointed here.

Tags (AGENTS.md rule 5): **[obs]** observed on hardware or in a repo log/run, **[src]** read from source (file:line or URL + commit),
**[guess]**. `TODO(verify)` = not sourced, do not use as a constant.

Sources and pins:

| Short | What | Pin |
|---|---|---|
| `sailfish` | ios-sailfish (joranv1), `~/pixelios-artifacts/refs/ios-sailfish`, README says iOS 16.7.16, Pixel 1, Adreno 530 | local clone, 256 files; cited as `tools/sweetgl/<file>:line`, `xnu/patches/NNNN` |
| `topaz` | ios-topaz (MakrSas), `~/pixelios-artifacts/refs/ios-topaz` | local clone |
| `mesa` | Mesa main on 2026-10-22 | `f1fbde1c931802aa42b95df19a2e5f4a3e280c6b` (gitlab.freedesktop.org/mesa/mesa); release `mesa-26.2.5` tarball used for the dry run |
| `nvmac` | nullmoth/nvidia-macos-driver | `f2114ab2aa39392d4cb2b0582a0ff0157841d30e` |
| `dsc` | our iOS 16.7 (20H392) shared cache, `~/pixelios-artifacts/work/dsc/cache` (string greps; `ipsw` symbolication) | local |

## 0. Summary

1. **How iOS renders on our port today [obs + guess].** backboardd finds no accelerator (`matching1 IOAcceleratorES -> ...`, log
   `qr-20261022T080107Z.txt`), CA draws into IOSurfaces we back with kernel pages and our `AppleCLCD::present` copies them to the bootloader
   framebuffer (NOTES.md:406-409; docs/research/11:134). The renderer in use was **never confirmed**. QuartzCore has four renderers
   (`sw_new_context`, `gles_new_context`, `metal_new_context`, `new_null_context`, NOTES.md:407); with no Metal device and Apple's EAGL unable to
   create a GLEngine context the only one left is the CPU rasteriser `CA::OGL::SW` [guess, high]. Confirm with one backboardd stack sample
   (frames named `CA::OGL::SW::draw_polygon`; sailfish hooks that very function, `tools/sweetgpu-ca/sweetgpu-ca.c:3-6`).
2. **The sailfish structure is portable and small on the userland side.** Mesa gallium + a ~2200-line EAGL shim (`eagl.c`) + a ~530-line DRM
   emulation (`sweet_drm.c`) + 315 lines of gallium frontend (`sweetgl.c`), linked into one `sweetgl.dylib`, injected into backboardd only
   with `DYLD_INSERT_LIBRARIES`. CA does not know it is not Apple's GL: it asks for `EAGLContext initWithAPI:3`, and when that succeeds it
   chooses its own GLES renderer (`eagl.c:2205`, comment on `sweetgl_eagl_install`). There is no Metal anywhere.
3. **G78 is Valhall v9 (arch 9.2, "G68/G78" in Mesa's table), job-manager (not CSF), OpenGL ES 3.1 in Mesa's Panfrost** (`pan_model.c:93`,
   `docs/drivers/panfrost.rst` row "G57, G68 / Valhall (v9) / ES 3.1"). That is above what CA needs (API 3 = ES 3.0 plus what sailfish's
   shims fill in). **PanVK does not build for v9** (`src/panfrost/vulkan/meson.build:145` loops `[6, 7, 10, 11, 12, 13, 14]`), so any
   Vulkan/Metal-translation route (section 6) is closed for the G78. GLES via Panfrost gallium is the only path.
4. **Panfrost's kernel seam is `pan_kmod`**, with two backends selected by `drmGetVersion()->name` (`pan_kmod.c:23-45`); a JM GPU uses the
   `panfrost` backend (11 ioctls, no VM_BIND, no user VA management). Two options: (A, first) copy sailfish: fake `xf86drm.h` +
   emulate the `DRM_IOCTL_PANFROST_*` set over an IOKit user client, **zero Mesa patches beyond meson gating**; (B, later) a third
   `pan_kmod_ops` backend `xnu` that calls the user client directly. Section 2 lists the operations.
5. **Biggest blocker: the kernel GPU driver for Mali-G78 on Tensor G1** (power domain/clocks via Google's firmware stack, the Mali MMU, job
   slots, cache coherency). Sailfish needed 32 GPU patches (248 KB) for the Adreno 530 (including CP microcode, zap shader, SMMU). The
   G78 JM needs no microcode (the CSF GPUs G310+ do), which removes the worst Adreno item, but Tensor power/clock bring-up is unknown here
   (`TODO(verify)`: google-modules/gpu kbase for gs101). Second blocker, a build one: Mesa 26.2 panfrost needs host LLVM + SPIRV-LLVM-Translator
   (mesa-clc, section 3) and the host has no linker for Mach-O.
6. **Cheapest experiment, no GPU:** turn on Reduce Transparency (+ Reduce Motion) for `mobile` in `tools/sweet-root-tweaks.py`
   (`com.apple.Accessibility`: `EnhancedBackgroundContrastEnabled`, `ReduceMotionEnabled`, key names read from the dsc, section 4), and port
   sailfish's `sweetenv` boot-arg (patches 0104/0163) + the AMFI dyld policy (0139) so CA knobs can be tried per boot without reflashing.

## 1. The sailfish userland, in depth

### 1.1 What runs where [src]

```
backboardd (only; SpringBoard gets a different, small hook)
  QuartzCore  CA::OGL::GLESContext  --GLI dispatch table--> sweetgl.dylib (eagl.c: 1036 slots -> Mesa gl* entry points)
  EAGLContext (Obj-C, methods swizzled by eagl.c)           |
                                                            +-> sweetgl.c  : gallium "frontend" (st_api_create_context, GLES 3.1, surfaceless)
                                                            +-> Mesa gallium freedreno, ir3 compiler, NIR, glsl  (static libs)
                                                            +-> sweet_drm.c: libdrm + msm ioctl emulation  (IOKit user client "SweetGPU")
IOSurface.framework -> IOCoreSurfaceRoot (kernel SweetSurface.cpp)   <- same memory the GPU maps (selector 12)
IOMobileFramebuffer -> AppleCLCD (kernel SweetFramebuffer.cpp): swap -> GPU scale/blit into panel buffer
```

### 1.2 The EAGL shim (`tools/sweetgl/eagl.c`, 2235 lines, 76 KB) [src]

* **Which Obj-C entry points are replaced** (`eagl.c:2205-2232`, `sweetgl_eagl_install`, by `method_setImplementation` on `EAGLContext`):
  `initWithAPI:properties:`, `dealloc`, `API`, `setParameter:to:`, `getMacroContextPrivate`,
  `texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:`, `attachImage:toCoreSurface:invertedRender:`, and the
  class methods `+setCurrentContext:` / `+currentContext`. Contexts the shim did not create fall through to Apple's original (`eagl.c:1786-1850`).
  The header comment (`eagl.c:5-11`) states this is the whole interface CA uses.
* **How GL calls reach Mesa.** `getMacroContextPrivate` returns `{rctx, disp[1036]}`, the layout of Apple's `GLEngine` GLI table
  (`eagl.h:10`, `SGL_SLOTS 1036`). The slot -> name map is `tools/sweetgl/ios-dispatch.txt` (8.9 KB, reverse engineered from
  GLEngine's `_gliInitDispatchTable`; `tools/gles-dispatch-scan.py`, `gles-dispatch-layout.py`). `gen-dispatch.py` (104 lines) generates
  `out/dispatch.c`: each slot becomes `wrapper(rctx, args...) { make current; call Mesa's gl<Name> }`, resolving names against
  `nm -g libglapi_bridge.a`; ~30 slots are hand-written (`gen-dispatch.py:19-26`, prototypes in `eagl.h:40-87`).
* **What CA's GL renderer needs that Mesa ES lacks, and the shim adds** (all in `eagl.c`):
  `GL_TEXTURE_RECTANGLE` emulated as 2D + `sampler2DRect` rewrite with per-draw `1/size` scale (265-300, 539-580);
  `GL_EXT_shader_framebuffer_fetch` emulated by sampling the destination texture after a flush + texture barrier (295-350, 658-710);
  GLSL source rewriting (`sgl_ShaderSourceARB`, `texfn_rewrite`, 421-540); ARB handle-object calls mapped to ES 3 (143-200);
  Apple's `internalformat GL_RGBA + format GL_BGRA` accepted (1276-1290); `glFlush == glFinish` ("flush = finish", 1296-1340, because CA
  hands surfaces to the display without a fence); "variable masked blur" approximated in one 24-tap pass (382-420);
  a binary patch of QuartzCore's own tables for four texture functions CA ships without GL shader text (`sgl_install_texfns`, 2150-2203; it
  writes constants inside the shared cache at hard-coded 20H cache addresses `0x1d5b44a10`, `0x18855a1e4`, and **refuses when the bytes
  do not match**, 2188); optional entry hooks on QuartzCore functions for research (`SGL_HOOK_STUB`, 1920-2150).
* **Mesa quirks set from the shim** (`sweetgl.c:106-113`): `FD_MESA_DEBUG=sysmem,nofp16`, `IR3_SHADER_DEBUG=nofp16` (CA's shaders break with fp16 mediump).
* **Context** (`sweetgl.c:131-140`): `st_api_create_context` with `API_OPENGLES2`, major 3 minor 1 for API>=3 ("3.1: glGetTexLevelParameteriv",
  needed by the rect-texture code); contexts are **surfaceless** (`st_api_make_current(st, NULL, NULL)`), CA only draws to FBOs.
* **SpringBoard-only hook** (`layerdump.c:120-216`, `sweetgl_vibrant_fill_install`, selected in `sweetgl.c:299-305`): a `CALayer` with a single
  `vibrantColorMatrix` filter and no contents is turned into a plain translucent background fill (the GL renderer builds no source
  surface for it). Relevant to section 4: this is the same family of filters the lock-screen clock uses.

### 1.3 How the rendered surface reaches CoreAnimation / the panel [src]

CA renders into IOSurfaces. The shim turns each IOSurface into GPU-visible storage the way EGL images work, **without a prime fd**:

1. `bind_surface()` (`eagl.c:1619-1716`) is called for both `attachImage:toCoreSurface:` (render target) and `texImageIOSurface:` (sampled).
2. `sweet_drm_import_iosurface(id, base_address, ...)` (`sweet_drm.c:~480`) calls SweetGPU selector 12 with the **IOSurface id**; the kernel
   looks up the surface's wired pages, maps them in the GPU VA space, and returns a GEM handle + iova + size.
3. A `pipe_resource` is made with `screen->resource_from_handle(WINSYS_HANDLE_TYPE_KMS, handle, stride=bytes-per-row, offset=page offset,
   modifier=DRM_FORMAT_MOD_LINEAR)` (`eagl.c:1668-1690`), cached in a 32-entry LRU keyed by (surface id, format, size, pitch, base)
   (`eagl.c:1457-1517`) so the writer and the reader of one surface share one Mesa resource (otherwise Mesa could reorder the batches).
4. `_mesa_EGLImageTargetTexture2DOES(target, image)` binds it; the frontend's `get_egl_image` returns the resource (`eagl.c:1364-1374`).
5. If the row pitch is not 64-byte aligned (an Adreno A5xx linear requirement) the surface is **copied** into an ordinary texture instead
   (`copy_surface`, `eagl.c:1559-1615`; re-copied when the surface seed moves). The kernel half tunes IOSurface row alignment to avoid this
   (patches 0153, 0160, 0162).
6. Present: CA calls IOMFB `SwapEnd`; the kernel copies/scales the finished surface into the panel buffer (patch 0092 `fb-present-in-swap`,
   0164 `gpu-present-600mhz`: GPU bilinear scale 750x1334 -> 1080x1920, double-buffered). Fencing is `glFinish` per flush (`eagl.c:1296`).

### 1.4 How `sweetgl.dylib` is built [src]

* Mesa pinned by `tools/sweetgl/mesa-setup.sh` (a checkout at `research/mesa`, version not recorded in the script; the `.patch` hunks
  match Mesa ~25.x/26.0: `freedreno_bo.c` with `os_mmap`, `fd_layout_resource_for_handle`; `TODO(verify)` exact commit).
* Meson cross build on a Mac, **static**: `-Dplatforms= -Degl=disabled -Dglx=disabled -Dgbm=disabled -Dgallium-drivers=freedreno
  -Dvulkan-drivers= -Dfreedreno-kmds=msm -Dllvm=disabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dzstd/zlib/expat=disabled
  -Dxmlconfig=disabled -Dshader-cache=disabled -Dbuild-tests=false -Dgallium-rusticl=false -Dtools= -Dteflon=false` (`mesa-setup.sh:11-20`).
  Cross file `ios-cross.txt`: Xcode 15 clang, `iPhoneOS17.5.sdk`, `-arch arm64 -miphoneos-version-min=16.0`, `[host_machine] system='darwin',
  subsystem='ios'`, `pkg-config = 'false'`.
* **The only Mesa patch is `mesa.patch` (1.7 KB, 3 hunks):** (1) `freedreno_bo.c`: `os_mmap` -> `sweet_drm_bo_map(fd, offset)` (no real mmap
  of a DRM node), (2) `src/freedreno/meson.build`: build `subdir('drm')` also when `host_machine.system()=='darwin'`, (3) `fd_layout_resource_for_handle`: A5xx
  linear stride multiple of 64 -> `pitchalign = 6`.
* **libdrm is not built**: `include/xf86drm.h` (2 KB, the 20 functions freedreno uses) + `sweet_drm.c` implement them. Syncobj/prime calls return
  `-ENOSYS` (`sweet_drm.c:~500-520`). **dlopen/threads**: no dlopen at all (everything static; `-dead_strip`, `-exported_symbols_list`
  with 5 symbols); Mesa's `HAVE_PTHREAD` and friends come from the compile database (`build.sh:30-36` copies `-D` flags of `st_manager.c`).
  No winsys, no EGL, no GBM, no LLVM (freedreno's ir3 compiler is self-contained).
* `build.sh` links 24 static libs (`libmesa.a`, `libgallium.a`, `libfreedreno.a`, `libfreedrenowinsys.a`, ir3, nir, glsl, glcpp, glapi bridge,
  util ...) + `-framework IOKit CoreFoundation IOSurface OpenGLES CoreGraphics -lobjc -lc++ -lz`, **ad-hoc `codesign -s -`**, then **zero-pads
  the dylib to exactly 64 MiB** so the file keeps one extent and can be rewritten in place on the phone (`build.sh:49-53`, `push.sh`). The
  pad trick exists only because sailfish pushes from TWRP; we flash `userdata` wholesale and do not need it.
* `OpenGLES` framework is linked for the headers/types only; the dylib exports 5 symbols (`_sweetgl_init/create/make_current/destroy/selftest`).

### 1.5 Injection [src]

* `sweetenv=NAME:VAR=VAL[,VAR=VAL][;NAME2:...]` boot-arg: `kern_exec.c` appends the variables to the environment of every exec whose
  basename is NAME (patches **0104** 2.1 KB, **0163** 1.6 KB: several names). Used as
  `sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib` (README boot-args).
* dyld ignores `DYLD_*` unless the process is allowed; dyld asks AMFI (`__mac_syscall("AMFI", 0x5a, ...)`). Patch **0139** (1.4 KB) renames
  the built-in MAC policy to exactly `"AMFI"` and answers op 0x5a with flags `0x7b` (allow @paths, path vars, fallback paths, print vars,
  failed insertions, interposing). Without it the variable is silently dropped. `TODO(verify)`: our policy name today (`xnu/patches/0035`,
  `0036`).
* Constructor (`sweetgl.c:297-315`): in SpringBoard install the vibrant hook; in every other process (backboardd) install the EAGL shim.
  The dylib sits in `/usr/lib` on the root volume; the shared cache cannot hold it.
* `0166` (3.3 KB): an **empty `IOAcceleratorES` IOService** is published. "CAMetalContextCreate polls `IOServiceMatching("IOAcceleratorES")`
  300 x 100 ms in every process that sets up CA rendering ... This one has no MetalPluginName, so Metal passes over it and CA still picks
  GLES" (`0166` comment). We already carry this (our 0086, from topaz 0138 / sailfish 0166).

### 1.6 Kernel patches the userland half depends on (sizes; `xnu/patches/`) [src]

| Group | Patches | Total | Role |
|---|---|---|---|
| IOSurface / IOMFB base | 0046 (46 KB, we have it), 0066 | | `IOCoreSurfaceRoot` + `AppleCLCD`; ours is the shared ancestor |
| IOSurface fixes for GPU sharing | 0152, 0153, 0157, 0158, 0159, 0160, 0162 | 7 patches, 15 KB | refuse wrap, explicit row alignment, client-address publication, read-only client memory, exact/as-asked rows (CoreUI only) |
| Env + dyld | 0104, 0163, 0139 | 5 KB | injection |
| Accelerator stub / IOKit quiet | 0166 | 3.3 KB | no 30 s CA poll; we have it as 0086 |
| GPU kernel driver | 0119-0151, 0155, 0156, 0161, 0164 (32 files with "gpu") | 248 KB | Adreno 530 bring-up, SMMU, VA space (0137, 10.7 KB), user client (0138, 20.9 KB), bo + submit (0150, 32 KB), cached bo (0151), present (0164, 18.7 KB); the ~100 KB of "hand-written Adreno shader" patches (0126, 0131, 0133, 0136, 0141-0143, 0149) are the first display blit before Mesa and are **not needed** for userland GL |
| Tools | `tools/gpu-shaders/*.asm` (5 files, 17 KB), `tools/ir3asm/` | | assembler for those blits; not needed |

### 1.7 Table: sailfish component -> what changes for Mali-G78

| Sailfish component | Role | For G78 / bluejay | Effort |
|---|---|---|---|
| `eagl.c` EAGL shim + `ios-dispatch.txt` + `gen-dispatch.py` | CA <-> Mesa GL | **Reuse unchanged** (GPU independent). Re-derive hard-coded cache addresses for our cache build if different from sailfish's 20H (`eagl.c:1931-1932, 2188`, `sweetgpu-ca.c DRAW_POLYGON`). Fix-ups for Panfrost differences (rect-texture rewrite, fbfetch emulation may be dropped if Panfrost exposes `GL_EXT_shader_framebuffer_fetch`: `TODO(verify)`) | low |
| `sweetgl.c` frontend | `pipe_screen` + `st_context` | Replace `fd_drm_screen_create_renderonly` with `panfrost_drm_screen_create(fd, config, NULL)` (`panfrost_drm_winsys.c`), drop the freedreno env knobs, keep GLES 3.1 context | low |
| `sweet_drm.c` + `xf86drm.h` | msm ioctl emulation | Rewrite as `DRM_IOCTL_PANFROST_*` emulation or as a `pan_kmod` backend (section 2); ~500 lines; much simpler (no fence-fd, no submitqueues; syncobjs needed though, section 2.3) | medium |
| `mesa.patch` | 3 hunks | Rebase for panfrost: gate `src/gallium/drivers/panfrost`, `winsys/panfrost/drm`, `src/panfrost/lib/kmod` on `host_machine.system()=='darwin'` (today `with_gallium_drm = system_has_kms_drm and [...with_gallium_panfrost...]`, `meson.build:259-266`), no `os_mmap` hook needed if `bo_mmap` is overridden | low-medium |
| `mesa-setup.sh`, `ios-cross.txt`, `build.sh` | Mesa cross build | `-Dgallium-drivers=panfrost`; **LLVM is required at build time by Panfrost** (mesa-clc/libclc: `panfrost.rst` "Building"); do the host-tools two-stage build (`-Dtools=panfrost -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true` on host, then `-Dmesa-clc=system -Dprecomp-compiler=system`); `-Dpanfrost-rust=false` (default is true, `meson.options:915`); `-Dllvm=disabled` stays for the runtime | medium (section 3) |
| 64 MiB pad + `push.sh` | in-place file replacement from TWRP | drop; ship via the root image (`tools/sweet-root-tweaks.py`) | none |
| kernel `SweetGPU` user client (sel 10-16: bo new/close, import surface, submit, wait, cache, params) | msm-style bo/submit | New `SweetMali` user client with the same shape: bo new/close/mmap, import IOSurface, submit job chain (`jc` + bo list + in/out sync), wait bo/sync, cache sync, get_param | **high (the blocker)** |
| kernel SMMU + GPU VA (0121-0123, 0137, 0155, 0156, 0161) | Adreno goes through an ARM SMMU the kernel programs | Mali-G78 has its own MMU (address spaces); the kernel must build Mali page tables for the bos. Panfrost JM expects the **kernel to choose the GPU VA** (`panfrost_kmod_vm_bind` rejects user VAs: `panfrost_kmod.c:719-724`) and to give it back via `GET_BO_OFFSET` | high |
| CP microcode / zap shader (0127-0130) | Adreno needs firmware | **None for G78 JM** [guess: JM GPUs run without MCU firmware; CSF G310+ load `mali_csffw.bin`] | removes work |
| `fb-present-in-swap`, `gpu-present` (GPU scale + flip) | display | Our panel path is a CPU copy to the ABL framebuffer (0046). Keep CPU copy first; later a Mali blit job for the 1125x2436 -> 1080x2400 scale | later |
| `layerdump.c` vibrant fill | GL renderer misses a vibrancy case | Test on Panfrost; may be needed identically since the missing case is CA-side | low |
| 0104/0163/0139 | injection | Port as is (none of our 0001-0088 contain them); needs a `sweetenv` entry in our boot-args builder (`tools/build-xnu-image.sh`) | low |

## 2. Mesa Panfrost for the Mali-G78

### 2.1 Which Mesa, which arch [src]

* `src/panfrost/model/pan_model.c:93`: `VALHALL_MODEL(PAN_PROD_ID(9, 2, 4), 0, "G68", "G78", ...)`. The Mali-G78 (any core count, the
  Pixel 6a has MP20) is **arch major 9, minor 2, product major 4** (`PAN_PROD_ID(arch_major, arch_minor, prod_major)`, `pan_model.h:39`);
  `pan_arch(gpu_id)` returns 9 (`pan_model.h:108-121`). G57/G77 are 9.0. G610/G310 are 10.x and are the first **CSF** parts (`panfrost.rst`).
  The Pixel's actual `GPU_ID` register value (and that it matches this row) is `TODO(verify)`: it must be read by a probe (shim, read-only
  register read of `GPU_ID`), not assumed.
* `docs/drivers/panfrost.rst` (support table at the pin): "G57, G68 / Valhall (v9) / OpenGL ES 3.1 [Khronos conformant, submission 980 for
  G57] / OpenGL 3.1 / Vulkan: -- / OpenCL 3.0". GLES 3.1 on v9 is therefore **conformant for G57** and the same code path is used for G68/G78.
* Version: any Mesa in 2024-2026 has v9 in the gallium driver; the first `pan_kmod` abstraction landed in 24.1 era (`TODO(verify)`
  exact release); we pin **`mesa-26.2.5`** (archive.mesa3d.org, 68 MB tarball, fetched 2026-10-22) or main at `f1fbde1c` because the kmod
  API is stable and the panfrost-rust/mesa-clc build story is documented for it. Do not use the pre-kmod (pre-24) layout: `panfrost_bo`
  talked to ioctls directly.

### 2.2 What the winsys layer expects from the kernel [src]

Gallium panfrost: `panfrost_drm_screen_create(fd, config, ro)` (`winsys/panfrost/drm/panfrost_drm_winsys.c`) -> `pan_kmod_dev_create(fd, ...)`
(`pan_kmod.c:76-98`) tries backends in order **by driver name**:

```
pan_kmod.c:23-45:  { "panfrost", &panfrost_kmod_ops },  { "panthor", &panthor_kmod_ops },
                   #ifdef HAVE_PANFROST_VDRM { "panfrost_virtio_gpu", ... }, { "panfrost_vpipe", ... } #endif
```

and `PAN_KMOD_RESTRICT_TO_BACKENDS` selects among them (`pan_kmod.c:47`). There is already a **non-DRM-hardware precedent**: the virtio
backends in the same table, built behind `-Dpanfrost-virtio`. A third backend is a table entry + one `.c` file.

`panfrost` (JM) backend ioctls actually used, from `panfrost_kmod.c` (the DRM uAPI `drm-uapi/panfrost_drm.h`):

| ioctl | where | purpose |
|---|---|---|
| `DRM_IOCTL_PANFROST_GET_PARAM` | 343 | `GPU_PROD_ID, GPU_REVISION, SHADER_PRESENT, TILER_FEATURES, MEM_FEATURES, MMU_FEATURES, L2_FEATURES, THREAD_FEATURES, MAX_THREADS, THREAD_MAX_WORKGROUP_SZ, THREAD_TLS_ALLOC, TEXTURE_FEATURES0, AFBC_FEATURES, SELECTED_COHERENCY, SYSTEM_TIMESTAMP(+_FREQUENCY), ALLOWED_JM_CTX_PRIORITIES` (17 params; from the Mali registers the kbase/our driver reads) |
| `CREATE_BO`, `MMAP_BO`, `GET_BO_OFFSET` | 453, 538, 137 | allocate, CPU map (offset to `mmap`), **GPU VA of the bo (kernel-chosen)** |
| `WAIT_BO`, `SYNC_BO`, `MADVISE`, `QUERY_BO_INFO`, `SET_LABEL_BO` | 573, 604, 621-632, 152, 784 | fence wait, cache maintenance, evictable hint (optional), info, label (optional) |
| `SUBMIT` | 350 (`pan_jm.c:71-140`) | `{jc, requirements, in_syncs[], out_sync, bo_handles[]}` ; one job chain per batch, in/out are **DRM syncobj handles** |
| `PERFCNT_*` | 873-1086 | optional, skip |
| libdrm: `drmPrimeFDToHandle/HandleToFD`, `drmSyncobj{Create,Destroy,Wait,ImportSyncFile,ExportSyncFile,FDToHandle,...}`, `drmGetVersion`, `drmIoctl` | `panfrost_kmod.c:182,485,523`; `pan_fence.c:28-120`; `pan_jm.c:1154` | |

### 2.3 Proposed third backend `xnu`: the exact `pan_kmod_ops` to implement [src: `pan_kmod.h:456-553`]

Backend struct `struct pan_kmod_ops xnu_kmod_ops` + a `{ "xnu", &xnu_kmod_ops }` row in `pan_kmod.c`; `drmGetVersion` (our shim) returns
name `"xnu"`. The `dev` object wraps an `io_connect_t` to the IOKit user client. Operations:

| `pan_kmod_ops` member | XNU implementation | Required? |
|---|---|---|
| `dev_create(fd, flags, allocator)` / `dev_destroy` | `IOServiceOpen("SweetMali")`; fill `props` (`gpu_id`, `shader_present`, `tiler/mem/mmu/texture features`, `thread_*`, `afbc`, `coherency`, `max_tiler_...`) from one `GET_PARAM`-style call; `supported_vm_op_flags = 0`, `pgsize_bitmap = 4K\|2M` as panfrost does (`panfrost_kmod.c:336`) | yes |
| `dev_query_user_va_range` | the Mali VA window the kernel offers (48-bit GPU VA; JM allocates, so just a range) | yes |
| `bo_alloc(dev, vm, size, flags)` / `bo_free` | selector "bo new": wired kernel pages, write-combined or cached mapping, **returns handle + GPU VA** (as sailfish sel 10/11) | yes |
| `bo_mmap(bo, prot, flags, addr)` | return the kernel-created user mapping (as `sweet_drm_bo_map`); on iOS you cannot `mmap` a DRM node, so the user client maps the bo into the task (`IOConnectMapMemory`) | yes |
| `flush_bo_map_syncs` / `PAN_KMOD_DEV_FLAG_MMAP_SYNC_THROUGH_KERNEL` | one cache-maintenance selector (`dc civac`/`cvac` over a range); set `props.is_io_coherent` truthfully (`pan_kmod.c:193`) | yes if not IO-coherent |
| `bo_wait(bo, timeout, read_only)` | wait on the bo's last-submit fence (sailfish sel 14 pattern) | yes |
| `bo_import(dev, fd)` / `bo_export(bo)` | **Panfrost imports by prime fd** (`pan_bo.c:464-494`). Pass a fake fd carrying an IOSurface id (the sailfish selector-12 trick), or add a gallium-side `resource_from_handle` branch for `WINSYS_HANDLE_TYPE_KMS` (already exists: `pan_resource.c:443-447`) | yes (IOSurface sharing) |
| `vm_create(flags, va_start, va_range)` / `vm_destroy` | JM: a single implicit VM; return a dummy object | yes |
| `vm_bind(vm, mode, ops[], n)` | JM semantics: MAP of a whole bo with `PAN_KMOD_VM_MAP_AUTO_VA` returns `va.start = bo offset` (`panfrost_kmod.c:697-745`); UNMAP a no-op; reject partial/sparse. Copy `panfrost_kmod_vm_bind` verbatim | yes |
| `vm_query_state` | `PAN_KMOD_VM_USABLE` | yes |
| `query_timestamp` | Mali `TIMESTAMP` or `mach_absolute_time()` if the counters match (sailfish did the latter, `sweet_drm.c get_param`) | yes |
| `bo_set_label`, `bo_make_(un)evictable`, `perf_*` | not implemented (optional members) | no |

Submission is **not in `pan_kmod_ops` for JM**; `pan_jm.c:jm_submit_jc` fills `struct drm_panfrost_submit` and the kmod layer only forwards it
through `panfrost_kmod_submit_job` (`panfrost_kmod.c:348`). The `xnu` backend would add its own `submit` hook the same way (one selector:
`jc`, `requirements`, `in_syncs`, `out_sync`, bo list). **Syncobjs**: gallium panfrost uses `drmSyncobj*` through `util_sync_provider`
(`pan_kmod.h:1042-1120`, `pan_fence.c`): the `xnu` kmod needs a sync provider backed by a kernel fence table (create/destroy/wait/signal/
reset). A one-fence-per-context shortcut is enough for a first light (sailfish used a per-bo sequence number and `glFinish` after every
flush).

**Option A (cheaper, recommended first):** keep `panfrost_kmod.c` untouched, give it a `xf86drm.h` whose `drmIoctl/drmCommand*/drmSyncobj*/
drmPrime*/drmGetVersion` are implemented in the dylib over the IOKit user client, exactly like `sweet_drm.c`. Cost: ~500-700 lines of C in
the dylib, **no Mesa source patch except meson gating**, and `bo_mmap` needs the one-line `os_mmap` redirect (the sailfish `freedreno_bo.c`
hunk; `panfrost_kmod.c:538-541` does `mmap(fd, offset)`). Tag [guess]: A works because `panfrost_kmod.c` only needs the 11 ioctls above
(grepped, section 2.2). **Option B** (the `xnu` backend) is cleaner and removes the fake-ioctl layer but touches two Mesa files; do it when
rebasing becomes the pain, not before.

### 2.4 Things the GLES context needs that differ from Adreno

* Valhall linear textures/render targets: stride alignment for scanout-style dumb buffers is 64 bytes in Panfrost's own helper
  (`panfrost_drm_winsys.c`, "Find the smallest width alignment that gives us a 64byte aligned stride"). For IOSurface import, check the
  pitch Panfrost accepts in `panfrost_resource_from_handle` (`pan_resource.c:313`-ish); CA's own bitmap surfaces will not always satisfy it, so
  keep sailfish's `copy_surface` fallback. `TODO(verify)` exact alignment rule for v9 linear import.
* Panfrost prefers AFBC/tiled layouts for render targets it allocates itself; imported surfaces must be `DRM_FORMAT_MOD_LINEAR`.
* No fp16 problem expected, but the `nofp16` workaround exists because CA misdeclares precision: keep an equivalent knob (`PAN_MESA_DEBUG`/NIR
  option) `TODO(verify)`.
* Tile-based deferred rendering works on both; sailfish's GMEM-restore bug (`FD_MESA_DEBUG=sysmem`, `sweetgl.c:103-108`) was Adreno-specific, but the
  same class of bug (tile writeback over the end of an unpadded IOSurface) is possible on Mali with partial tiles: pad IOSurface allocations to
  the tile size (`0153`, `0156` pattern).

## 3. Cross-build feasibility (this Linux host, no root) and CA's GL requirements

### 3.1 SDK situation [obs]

* The project builds the kernel on **GitHub `macos-14` runners with Xcode 15** (`.github/workflows/xnu.yml:37-67`), where the iPhoneOS SDK
  ships with Xcode. Sailfish does the same locally on a Mac (`ios-cross.txt`, `iPhoneOS17.5.sdk`). **No iPhoneOS SDK is on this host**
  (`~/pixelios-artifacts` has the IPSW, a dyld shared cache extraction and `ipsw`, no SDK; the KDK is also CI-only).
* For a feasibility dry run I fetched `theos/sdks` sparse (`iPhoneOS16.5.sdk/usr` only, 55 MB, into `/tmp/gpu18`, **not** in git; Apple SDK
  headers/stubs must never enter the repo, AGENTS.md rule 4).

### 3.2 Dry run result [obs, 2026-10-22, all in `/tmp/gpu18`, ~600 MB total]

1. `pip --target` meson 1.12.1 / ninja / mako / pyyaml: OK. `python3 -m venv` is unavailable (needs `python3-venv`).
2. Mesa 26.2.5 tarball unpacked, clang 19.1.7 present. `meson setup` with a darwin/ios cross file (`clang -target arm64-apple-ios16.0
   -isysroot iPhoneOS16.5.sdk`) stopped at the first step: **"Unable to detect linker ... invalid linker name '-fuse-ld=lld'"**. The host has
   `clang` and `llvm-ar` but **no `lld`/`ld64.lld`/`ld64`** (`apt install lld` = root). Xcode's linker only exists on the macOS runner.
3. A native (Linux) configure of the same option set, to find the non-linker blockers, stops at `Dependency "LLVMSPIRVLib" not found`
   (`meson.build:2084`): with `-Dgallium-drivers=panfrost` Mesa 26.2 requires **LLVM + SPIRV-LLVM-Translator** (mesa-clc). Also missing and
   needed later: `libdrm` (irrelevant for iOS, replaced by a stub header), `bison`, `flex`, `cmake`/`libclang` dev, `glslang`/`spirv-tools`
   (`TODO(verify)` which ones configure demands after LLVMSPIRVLib).
4. Not tried: running the actual iOS compile. It cannot link, and a compile-only check without `bison/flex` generated sources is not meaningful.

**Blockers (exact), in order:** (1) a Mach-O linker (`lld` with `-fuse-ld=lld`/`ld64.lld`, or Xcode) -> staged root command: `apt install lld`
(user runs it); (2) `libllvmspirvlib-dev` (+ `libclang-19-dev`, `bison`, `flex`) for the host-side `mesa-clc`; (3) the iPhoneOS SDK
(runner-provided, or a licensed copy outside git). **Conclusion:** the realistic place to build `sweetgl.dylib` is a **second CI job on
`macos-14`** (same runner family as the kernel): `brew install meson ninja bison flex llvm spirv-llvm-translator`, the two-stage Mesa build
of section 1.7, `xcrun -sdk iphoneos clang`, ad-hoc `codesign`, upload as an artifact (the same `gh run download` flow as the kernel). A Linux
build is possible only after staging the three items above.

### 3.3 What QuartzCore needs from OpenGL ES (the Metal-less path) [src + guess]

* Renderer set: `sw_new_context` (CPU), `gles_new_context`, `metal_new_context`, `new_null_context` (NOTES.md:407); env knobs include
  `CA_ENABLE_OGL`, `CA_NO_ACCEL`, `CA_METAL_IMAGE`, `CA_IOSURFACE_IMAGE`, `CA_MAX_TEXTURE_SIZE` (strings in the dsc, 20H392).
* **Minimum level:** CA creates `EAGLContext initWithAPI:` with **API 3** (`kEAGLRenderingAPIOpenGLES3`): sailfish's shim only takes over when
  `api >= 2` and creates a 3.0/3.1 context (`eagl.c:1721`, `sweetgl.c:131-135`); "CA picks its GLES renderer by itself once `initWithAPI:3`
  succeeds" (`eagl.c:2205`). So **ES 3.0 is the floor**; Panfrost v9 gives 3.1.
* **Extensions CA's GL path used on Adreno/Mesa** (everything the shim had to emulate): `GL_TEXTURE_RECTANGLE` (APPLE), `GL_EXT_shader_framebuffer_fetch`,
  ARB shader-object entry points via the GLI table, `GL_APPLE_fence`/`FinishObject`, `EXT_discard_framebuffer`, `OES_get_program_binary`
  style calls (`ShaderBinaryOES`), IOSurface-as-texture (`texImageIOSurface`, EGL-image equivalent). All of these were handled in 2.2 kLoC
  of shim; none depends on the GPU vendor except the pitch alignment and fbfetch.
* **Metal first, GLES second, SW last.** `CAMetalContextCreate` runs first and polls for `IOAcceleratorES` (sailfish 0166 comment); with no
  `MetalPluginName` it yields, and CA falls to GLES; without a working EAGL it falls to `CA::OGL::SW` [src for the first two, guess for the
  last (never observed here)].

## 4. Why the lock screen looks see-through on the CPU renderer, and what to do first

### 4.1 Evidence [obs] and analysis [guess]

* The user sees the lock-screen clock rendered with transparency. Facts from our logs: the lock screen is up (`SBCoverSheetPrimarySlidingViewController`,
  115000 swaps, `qr-20261022T053837Z.txt`); **every poster/wallpaper extension is killed with `extension check-in timeout`** (`CRASH WeatherPoster
  ... reason ns 7 code 0x3: extension check-in timeout`, also Collections/Unity/Pride/Photos/Emoji/Extragalactic posters in
  `qr-20261022T0*.txt`; HANDOFF.md session 7). SpringBoard also creates a **1125x2436 `w30r` (10-bit wide-gamut) surface** at runtime
  (`SWEET SURF: SpringBoard create id 11 1125x2436 fmt 0x77333072 ... size 11239424`, `qr-20261022T080107Z.txt`): a full-screen wide-colour
  backdrop/snapshot is being rendered on the CPU.
* So the lock screen has **no wallpaper to sit on**; the time (`CSProminentTimeView`, `SBFLockScreenDateView`, strings in the dsc) is a
  vibrant/material view whose look is defined by compositing filters against what is behind it (the filter names `plusL`, `vibrantColorMatrix`,
  `luminanceCurveMap`, `gaussianBlur`, `colorSaturate` all exist in the cache). Over an empty (black or clear) backdrop a vibrant/plus-lighter
  glyph is drawn at its **translucent fill alpha**, i.e. looks see-through, and the blur/backdrop capture is wrong in the CPU renderer:
  sailfish already needed `CA_BACKDROP_SCALE=1` for CPU backdrop copies (`tools/sweet-root-tweaks.py:90-99`, ours already sets it) and a
  GL-renderer fix for `vibrantColorMatrix` layers with no contents (`layerdump.c:120-205`: "the filtered result is empty"). `CA::OGL::SW`
  has a `tex_vibrant_color_matrix` (eagl.c:357-360 describes it as the reference), so the SW renderer is *expected* to draw it; whether it does
  correctly with a missing backdrop is a hardware observation, not a source fact. Rank of causes: (1) no wallpaper behind (poster timeouts),
  (2) the translucency is by design and only looks wrong without a backdrop, (3) a CA SW bug on the filter chain (least likely, not
  excluded). Test (1) first: solid colour wallpaper or Reduce Transparency (section 4.2) changes the picture within one boot.

### 4.2 Preference keys, read from the shared cache [src: dsc]

* Domain `com.apple.Accessibility` (string present 27x in the cache), file `/var/mobile/Library/Preferences/com.apple.Accessibility.plist`.
* The key block in cache file `.05` reads (adjacent cstrings, the AXSettings preference-key table):
  `... EnhancedTextLegibilityEnabled | EnhancedTextTrackingEnabled | EnhancedBackgroundContrastEnabled | ReduceMotionEnabled | DifferentiateWithoutColor |
  ReduceMotionAutoplayMessagesEffectsEnabled | ReduceMotionAutoplayVideoPreviewsEnabled | ReduceMotionReduceSlideTransitionsPreference ...`.
  UIKit's `UIAccessibilityIsReduceTransparencyEnabled` is backed by `AXSEnhanceBackgroundContrastEnabled` ("Reduce Transparency" in
  Settings is "enhance background contrast" in the framework; `AXSEnhanceBackgroundContrastEnabled`/`AXSSetEnhanceBackgroundContrastEnabled`
  strings and `_reduceTransparencyStatusDidChange` observers exist). **Therefore: `EnhancedBackgroundContrastEnabled = true`** is the key
  that maps to Reduce Transparency [src for names, **guess** for the exact name-to-setting mapping; confirm by flipping it and observing
  `UIAccessibilityIsReduceTransparencyEnabled` through a SpringBoard log line or visually]. `ReduceMotionEnabled` is a literal match.
  The MDM payload key list in cache `.35` (`... ReduceMotionEnabled | IncreaseContrastEnabled | ReduceTransparencyEnabled ...`) is the
  managed-config schema, not the plist keys; do not set `ReduceTransparencyEnabled` in `com.apple.Accessibility` expecting an effect.
* `tools/sweet-root-tweaks.py` already has `merge_pref(root, name, values)` (line 64) writing mobile-owned 0600 plists. Proposed addition
  (one call, in `main()` next to the SpringBoard prefs):
  `merge_pref(root, 'com.apple.Accessibility', {'EnhancedBackgroundContrastEnabled': True, 'ReduceMotionEnabled': True})`.
  Notifications: these are read through `cfprefsd` and `notify`; with the pref present at first launch no notification is needed.

### 4.3 Ranked cheapest experiments (no GPU)

| # | Experiment | Cost | Expected effect | Tag |
|---|---|---|---|---|
| 1 | **Reduce Transparency + Reduce Motion** defaults (4.2). UIKit then replaces `UIVisualEffectView` blurs/materials with opaque backing colours and SpringBoard drops backdrop filters (observer `_reduceTransparencyStatusDidChange`, `SSReduceTransparencyFlashView`, `setReduceTransparencyBackingColor` are all in the cache) -> fewer CPU blurs, no "see-through" materials, shorter animations | root image rebuild + reflash (userdata flash is pre-approved) or edit the existing image | big CPU saving, clock should become opaque white | guess (mechanism [src]) |
| 2 | **Port `sweetenv` (0104/0163) + AMFI dyld policy (0139)** so CA env knobs and later `DYLD_INSERT_LIBRARIES` can be tried per boot via `build-xnu-image.sh` boot-args, no reflash | kernel patch, CI ~4 min | enables 3, 4 and the whole GPU plan | src (patches read) |
| 3 | CA knobs via sweetenv on backboardd (strings exist in the 20H392 cache): `CA_DISABLE_COLOR_MATCHING=1` and `CA_COLOR_P3_CLAMP`/`CA_BLEND_COLOR_SPACE` (the 10-bit wide-gamut surface suggests P3/wide blending in the SW path), `CA_DISABLE_DIRTY_REGIONS` **unset** (keep), `CA_MAX_TEXTURE_SIZE`, `CA_NO_ACCEL`, `CA_PRINT_PERF=1`/`CA_PERF_HUD` for numbers, `CA_BACKDROP_SCALE=2..4` to trade blur quality for speed (currently 1 = full-resolution capture, the slowest CPU setting) | boot only | measure which one moves the frame time | guess (names [src]); none has a documented semantic, each needs one observed boot |
| 4 | Fix the missing wallpaper: SMP (`--cpus 4`) so poster extensions meet their check-in deadline (HANDOFF "Open: (2)(3)"), or temporarily disable `PosterBoard` wallpaper extensions and set a solid colour wallpaper | boot | lock screen gets a backdrop; see-through look explained or not | guess |
| 5 | Confirm the renderer: one backboardd sample showing `CA::OGL::SW::*` frames, and count `SWEET FB: swap` per second (log already prints swaps) = the baseline for later GPU numbers | boot | baseline | obs-able |
| 6 | Disable remaining SpringBoard vibrancy via `reduceTransparency` only if (1) leaves visible artifacts; do **not** add the sailfish `vibrant_fill` hook for the SW renderer (it fixes a GL-renderer gap) | | | guess |

## 5. Staged plan, risks, what only hardware can answer

| Stage | Deliverable | Gate (observable) | Risk |
|---|---|---|---|
| G0 | CPU path tuned: experiments 1-5 | measured swaps/s, clock opaque | low |
| G1 | `sweetenv` + AMFI dyld policy (kernel patch), `sweetgl.dylib` built as a **stub** (selftest only, no GPU) injected into backboardd; `EAGLContext` shim returns NULL context so CA keeps SW | log `sweetgl: EAGL shim installed` from backboardd | dyld policy, code signing of an ad-hoc dylib (our AMFI allows, `sweet_amfi`) |
| G2 | Mesa Panfrost cross-built on macos-14 CI (two-stage, no LLVM at runtime), linked with a **software fake kmod** (returns `GET_PARAM` of a G78, bo = malloc, submit = fail) | `screen->get_name` prints "Mali-G78"-like in the log; shader compile of CA's GLSL works host-side (`sweetgl_selftest` minus the draw) | mesa-clc dependencies, Panfrost compile-time size (64 MiB pad is not needed) |
| G3 | Kernel probe in the shim: read `GPU_ID`, `SHADER_PRESENT`, power the Mali (power domain, clocks, `TODO(verify)` ACPM), read-only first | `GPU_ID` matches `PAN_PROD_ID(9,2,4)` family | **Tensor G1 GPU power sequence unknown**; bootloader state of the GPU unknown; any write beyond register reads in the shim needs SAFETY.md review |
| G4 | `SweetMali` IOKit user client: bo alloc/map/import, Mali MMU tables, a no-op job chain (`WRITE_VALUE`), `SUBMIT` + fence interrupt | `sweetgl_selftest` renders the 256x256 triangle and reads back red/blue (the exact test in `sweetgl.c:150-210`) | job slots/IRQ handling, cache coherency (ACE-lite vs IO coherent), GPU faults hang the SoC |
| G5 | CA on Mesa: enable the EAGL shim for real; IOSurface import; `glFinish`-per-flush | lock screen drawn by `CA::OGL::GLESContext` (`SWEET FB: swap` rate up, CPU idle) | CA shader gaps (sailfish needed 4 texture-function patches and a blur approximation), 20H cache address mismatch for the binary patches (refuse-on-mismatch is built in) |
| G6 | GPU scale/flip to the panel instead of CPU copy | optional | the display path is the ABL framebuffer (no DPU driver) |

**Only hardware can answer:** the real `GPU_ID` and whether the G78 is IO-coherent on this SoC; whether the Mali power domain/clock/reset can
be brought up from XNU without Google's ACPM stack; the Mali interrupt number and whether our GIC setup reaches it; Valhall linear-import
pitch rule with CA's actual surfaces; whether CA's shaders compile and render correctly on Panfrost (fbfetch, rect textures, blur); which
CA renderer runs today; and whether Reduce Transparency fixes the clock.

**Rank of risks:** (1) Tensor GPU power/clock bring-up (unknown, kernel), (2) Mali MMU + job submit correctness (unknown, kernel, no firmware
helps), (3) the host build toolchain for Panfrost (known blockers, section 3), (4) CA/Mesa behavioural gaps (sailfish shows they are
solvable with a shim of ~2 kLoC, all GPU independent), (5) dyld/AMFI injection (small).

## 6. Reference: nullmoth/nvidia-macos-driver (`nvmac`, pin `f2114ab2`)

An NVIDIA PCIe driver for **x86_64 macOS 15** [src README.md, docs/HOW-IT-WORKS.md]. It is **not usable** for the Mali-G78 / arm64 / iOS
and is not a plan alternative; one structural idea is worth recording.

* **Structure** [src HOW-IT-WORKS.md section 1]: Metal app -> `NVMTLDriver.bundle` (a Metal *driver plugin* in `/Library/GPUBundles`, 32 files in
  `plugin/`) -> `libnvmtl_translate.dylib` (Apple AIR -> SPIR-V) -> NVK (Mesa Vulkan, patched `nvk-macos.patch`, 12.3 k lines, on Mesa `17ca6174`) ->
  4 kexts (`NVRM` = NVIDIA open GPU kernel modules on XNU, `NVRMFB` IOFramebuffer, `NVAccel` IOAccelerator, `NVRMAGDC`). Mesa Vulkan sits
  behind a **user-space Metal implementation**, not behind Apple's Metal.
* **(a) What the translator would let iOS Metal clients do.** `translator/` is metal2vulkan (Rust, **295 k lines** under `translator/translator/src`,
  Vulkan 1.2 SPIR-V out of AIR bitcode, needs `llvm-dis` and `spirv-val` at run time, alpha). On iOS the Metal clients are QuartzCore's
  `metal_new_context` renderer, SpringBoard/UIKit apps and Metal-using apps. For them to run, a **Metal device plugin** (what
  `IOAcceleratorES`'s `MetalPluginName` property selects; sailfish 0166 comment) would have to implement `MTLDevice`, queues, encoders and
  resources on top of Vulkan, and ship AIR -> SPIR-V for QuartzCore's *precompiled* Metal libraries (`CA_DISABLE_PRECOMPILED_PIPELINES` /
  `CA_DISABLE_GENERIC_SHADERS` strings exist in the cache). That is the "large Metal layer" PLAN.md:69 warns about, and it is the only way
  apps that require Metal would run. QuartzCore itself does **not** need it: it falls back to GLES (section 3.3). On iOS the plugin
  loading, the AGX-specific compiler service and the closed `MTL*` driver interface are unexamined (`TODO(verify)`), and Apple's AIR on iOS can differ
  from macOS's.
* **(b) Is PanVK available for the G78?** **No.** PanVK builds archs `[6, 7, 10, 11, 12, 13, 14]` (`src/panfrost/vulkan/meson.build:145`;
  `valhall_archs = [9, 10, 11]` is declared at line 81 but 9 is not in the loop); the support table lists no Vulkan for "G57, G68 (Valhall v9)"
  (`docs/drivers/panfrost.rst`). v6/v7 (Bifrost JM) have Vulkan 1.3 (experimental, needs `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`); v10+ (CSF) are
  the real PanVK targets. So **NVMTL-style Metal-over-Vulkan cannot be built for the Pixel 6a's GPU today**; it would need a v9 PanVK port first.
* **(c) Licensing** [src LICENSE, NOTICE]: repository top-level license is **PolyForm Noncommercial 1.0.0** (no commercial use; not
  compatible with copying into the MIT PixOS tree); `translator/` is **LGPL-3.0-or-later** (metal2vulkan, Anees Iqbal; modifications by
  NullMoth also LGPL); NVK/Mesa MIT; kexts build against NVIDIA open modules (MIT/GPL-2.0). We may *read* it for design; we may not copy
  `plugin/` or `kexts/`; linking the LGPL translator dynamically would be allowed but it is irrelevant until PanVK v9 exists.
* **Verdict:** keep the GLES/EAGL sailfish-style plan as the first target. Revisit Metal-over-Vulkan only after (i) G78 GLES is accelerated and
  (ii) someone ports PanVK to v9 (JM) or the hardware changes (G710+/CSF).

## 7. Commands used (reproducible, nothing here touched a phone)

```
# Mesa pins and files: gitlab.freedesktop.org/mesa/mesa, commit f1fbde1c931802aa42b95df19a2e5f4a3e280c6b
#   src/panfrost/lib/kmod/{pan_kmod.h,pan_kmod.c,panfrost_kmod.c,panthor_kmod.c,pan_kmod_backend.h}  src/panfrost/model/pan_model.{c,h}
#   src/gallium/drivers/panfrost/{pan_jm.c,pan_fence.c,pan_bo.c,pan_resource.c}  src/gallium/winsys/panfrost/drm/panfrost_drm_winsys.c
#   docs/drivers/panfrost.rst  meson.build:259-266  meson.options:915  src/panfrost/vulkan/meson.build:145
# dry run (in /tmp/gpu18, not in git):
pip3 install --break-system-packages --target /tmp/gpu18/py meson ninja mako pyyaml packaging
curl -sL https://archive.mesa3d.org/mesa-26.2.5.tar.xz -o mesa.tar.xz
git clone --depth 1 --filter=blob:none --sparse https://github.com/theos/sdks.git ; git sparse-checkout set iPhoneOS16.5.sdk/usr
python3 py/bin/meson setup build-ios mesa-src --cross-file cross/ios.txt -Dgallium-drivers=panfrost -Dllvm=disabled -Dpanfrost-rust=false ...
#   -> "Unable to detect linker ... -fuse-ld=lld"   (no lld on host)
# dsc string checks: grep -a -o -P "\x00EnhancedBackgroundContrastEnabled\x00" ~/pixelios-artifacts/work/dsc/cache/dyld_shared_cache_arm64.05
```

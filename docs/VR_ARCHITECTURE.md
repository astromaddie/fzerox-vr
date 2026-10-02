# VR (OpenXR / Steam Frame) — architecture notes

Status: **M0 gates passed; flat-screen and stereo-race presentation implemented and running on a
Steam Frame** (unattended, headset in standby; not yet verified by a person wearing it). Sections 1-3
record what the stock renderer does and what was measured; section 4 describes what was built.

Not built yet (planned next wave): an in-headset **VR settings UI** (the `gEnhancements.VR.*` CVars
are only reachable by editing the config file) and a **Quit to Steam** item. Until then the only exit
is the runtime's own (the System button / Steam menu). The system layer on the controller (section 4,
"System layer") is implemented but, like everything else here, not yet tried by a person in the headset.

Target: optional OpenXR output mode (`GDX_OPENXR=ON` at build time, `--vr` / `GDX_VR=1` at launch)
running natively on Steam Frame (Linux ARM64). Desktop behaviour with VR off must stay
bit-identical, and `decomp/` changes must keep the retail build matching (see CONTRIBUTING.md).

---

## 1. Current task/present semantics

Source-read flow (line numbers at `main` 5d6d184):

```text
port/main.cpp host loop (one iteration = one 60 Hz logic tick)
  w->HandleEvents(); gdx_controller_poll(); gdx_fixed_aspect_tick();
  compute this tick's sub-frame count (remainder accumulator, main.cpp ~1403-1515)
  gdx_gfx_interp_tick_config(interpOn, tickStart, kGdxInterpTickSeconds, count, ...)
  gdx_vi_tick()                         <- the game frame actually runs here
      synchronous Main fiber dispatch
        game logic (Race_Update ...)
        Race_Draw builds the N64 DL into the current GfxPool
        osSpTaskStart -> gdx_gfx_run(task)        (2-6 gfx tasks per tick)
            ConvertRoot: N64 DL -> wide "converted" DL; matrices rerouted to scratch slots
            if P2 (FrameInterpolation) active:
                for k in 0..count-1:
                    t = (k+1)/count
                    GdxP0RefillScratch(t); GdxVpRefillScratch(t); GdxVtxRefillScratch(t)
                    fw->DrawAndRunGraphicsCommands(converted)   StartDraw..Run..EndDraw..EndFrame(swap)
                gGdxInterpPresentedLastTick = 1
            post-Run frees (gPersistentAllocations.clear, RGBA16 range retirement)
  if !interpOn: host w->EndFrame() (swap) then gdx_frame_pacer_tick()
  else:         skip EndFrame if the bridge presented; gdx_host_pace_logic_until(deadline)
```

Facts established from source:

- **Presentation is nested inside the game tick.** The sub-frame loop runs inside `gdx_gfx_run`
  (`n64_gfx_bridge.cpp` ~8949-9330), i.e. inside the game fiber's gfx-task submission. The converted
  DL and both GfxPools are only guaranteed alive there: the next tick's `Gfx_InitBuffer` flips pool
  parity (`decomp/src/sys/sys_gfx.c:115-125`), and the post-Run frees follow the loop.
- **Two GfxPools** (`D_8024DCE0[2]`, parity `D_800DCCFC`) hold this tick's and the previous tick's
  matrices at the same offsets; interpolation lerps between them (`gdx_interp::PrevPoolBase`).
- **Camera matrix:** `Race_Draw` (`decomp/src/overlays/ovl_i2/race.c:250`) and `Background_Draw`
  (`background.c:1147`) load the combined projection·view `GfxPool::unk_20208[cameraIndex]` with
  `G_MTX_PROJECTION`. Course geometry is world-space vertices with no modelview. The bridge already
  rebuilds this matrix from an interpolated camera pose (`gdx_camera_build_projview`,
  `port/gdx_camera_pose.h`; bridge ~3376/3431). **This is the per-eye injection point.**
- **Sub-frame t** is `(k+1)/count` with `count` from a rational remainder accumulator. History in
  the bridge comment (~8960): an earlier wall-clock `t` coupled to VSync jitter and destabilised the
  frame rate. For VR, `t` will be derived from OpenXR's `predictedDisplayTime`, which is the
  runtime's smoothed display timeline rather than a jittery host clock read, so that failure mode
  does not carry over. Fixed `(k+1)/count` spacing at a non-integer ratio (90/59.94) shows game
  time in uneven steps, which reads as judder in a head-tracked view.
- Each sub-frame is a **full present** (`DrawAndRunGraphicsCommands` ends in `EndFrame` → swap).
  VR needs that split into render-only and swap.

Open, **[device]**: how many of the 2-6 tasks per tick carry world geometry vs. HUD/other; whether
the P2 loop runs once per tick or once per qualifying task; replay ms per pass on the Frame CPU.
Collect with `GDX_PERF=1` and the existing `[interp-p2]` lines.

---

## 2. Camera-relative and screen-space rendering inventory (race, 1 player)

Scope: everything whose output depends on the **game camera** in a way that breaks when the viewer
rotates their head away from it. Non-race screens (title, menus, machine select, results, Course
Edit editors, ending, records) are planned to be shown on a flat OpenXR quad layer and are out of
scope here.

Draw order in `Race_Draw` (1P): `Background_Draw` → `Course_Draw` → `Course_GadgetsDraw` →
`Camera_Draw` (viewport/scissor) → `Racer_Draw` → `Menus_Player1SpecialDraw` → `Menus_Draw` (HUD).
`Menus_Draw` is the single tail call that draws the HUD, so it is the natural world/HUD boundary.

| System | file:function | Current criterion / space | Required VR behaviour |
| --- | --- | --- | --- |
| Track chunks | `game/course.c` `Course_Draw` (~4597-4760) | Depth `< 0` or `> sCourseFarRenderDistance×DrawDistance` rejects; then NDC x (`±gdxCullX`) and NDC y (`±1`) on chunk centres. Cull origin is pulled back by `sCourseRenderOriginDistance` | VR: replace depth sign test with distance; skip NDC x/y. Chunk groups are ordered by depth relative to the game camera (`drawState ±1` from depth gradient). Check translucent ordering behind the camera. **Check GfxPool/`courseVtxBuffer` capacity**: un-culling more chunks may overflow (`course.c:4555` already guards against the effects buffer) |
| Racer models | `game/racer.c` `Racer_Draw` (~5900-5990) | Depth `< 0` or `> 2500` rejects; NDC x/y reject; LOD and shadow/boost LOD by depth | VR: distance-based reject (keep 2500); no NDC test; LOD from distance |
| Rank markers over top-3 racers | `game/racer.c` `Racer_Draw` (~7180-7240) | CPU-projected through `projectionViewMtx` to screen position | Per-eye reprojection, or world-space billboard; MVP: move to HUD layer or hide |
| Effect quads (boost, sparks, etc.) | `game/racer.c` (~6460-6500, effects vertex buffer) | Quads built from camera basis (billboards facing the **game** camera) | Acceptable at first (they face roughly the right way); revisit if they look flat |
| Skybox | `ovl_i3/background.c` `Background_UpdateSkyboxVtx` (:751), drawn in `Background_Draw` (:1120) | One quad at `eye + forward×skyboxDepth`, sized to the camera FOV and frustum offsets | **Head turn sees past its edge.** Rebuild per eye from the eye pose and eye FOV (+margin) via the vertex scratch path |
| Venue floor | `background.c` `Background_UpdateVenueFloorVtx` (:867) | Camera-relative quad | Same as skybox |
| Clouds | `background.c` `Background_UpdateCloudVtx` (:984) | Camera-relative, scrolling | Same as skybox. Check UV scroll stays world-consistent |
| Background sprites | `background.c` `Background_UpdateBackgroundSprites` (:1388), draw (:1470) with per-sprite matrices `unk_2C368` | To verify: camera-relative placement | To verify |
| Widescreen background tweaks | `background.c` `Background_Update` (:649-690) | Uses `gdx_get_widescreen_geometry_xscale` | VR target must report xscale 1.0 |
| Stars | `background.c` `Background_UpdateStars` (:1548), `Background_DrawStars` (:1616) | Forward-depth reject, CPU projection, scissor test (`gdxCx`), drawn as `gSPTextureRectangle` | **Head-locked in VR.** Per-eye reprojection, or hide in VR MVP (logged) |
| 1P scissor | `game/camera.c` `Camera_Draw` (:1417) | `gDPSetScissor(12,16,308,224)` crops the frame border | Full eye target in VR |
| Hor+ vertex squash | `libultraship/src/fast/interpreter.cpp` `GfxSpVertex` → `AdjXForAspectRatio` (:2532, :2652) | Scales clip-space x of every 3D vertex by `(4/3)/aspect` | Identity for VR eye targets. Also `G_EX_WIDESCREEN_STRETCH` |
| HUD | `ovl_i3/menus.c` `Menus_Draw`, `Menus_Player1SpecialDraw` (:5381) | Screen-space rects and ortho draws | Head-locked quad layer, split from the world by a PORT-gated marker before `Menus_Draw` |
| Course gadgets (mines, jump plates, ...) | `game/course_gadgets.c` `Course_GadgetsDraw` (:255) | To verify: appears to follow course/segment visibility | To verify against the chunk change |

Not race-relevant (shown on the flat quad layer): `overlays/ending/ending_effects.c` (fireworks,
ultrawide scissor macros), `overlays/course_edit/1A0B50.c` and `1A2D70.c` (editor picking and
projection), `ovl_i3/records_entry.c`, `overlays/records/records.c:943` (ghost marker projection).
Course Edit **test runs** use the race pipeline and are in scope.

---

## 3. On-device gates

### M0-0 Deploy & launch
- [x] Shell and file copy: the Frame enumerates on macOS as a USB network interface ("Steam Frame",
  host side `10.86.200.234/29`, device `10.86.200.233`). SSH as `steamos` with a key installed via
  `ssh-copy-id`; `scp`/`rsync` for files. Port 22 open (also 5555, 8081, 27036).
- [x] Build: `tools/docker/build-linux.sh` (pinned CI environment in an arm64 container on an Apple
  Silicon Mac via Colima), then `tools/package_linux.sh <build> <ver> <out> linux-arm64` in the same
  container. ABI check passes (GLIBC_2.34, GLIBCXX_3.4.30; device has glibc 2.39).
- [x] The packaged build launches natively on the device from SSH with `DISPLAY=:0` (gamescope
  Xwayland): window, GL renderer (OpenGL shader cache written), audio, controller DB, ROM SHA-1
  verified, and it reaches the in-window first-time setup.
- [ ] First-boot extraction: blocked on the Expansion Kit inputs. Setup requires
  `baserom.translated.ek.ndd` (or `baserom.jp.ek.ndd`) **and** a 64DD IPL ROM; the user-supplied
  `F-Zero X Expansion Kit.n64` (66,551,504 bytes, `80 37 12 40` cartridge header) is not a 64DD
  disk image. Setup also logs a repeating `[content] prep-wd: initial mount failed` while the disk
  is absent — upstream noise, not device-specific.
- [x] First-boot extraction completed on device with the user's US rev0 ROM, translated EK disk
  and US-prototype 64DD IPL (all SHA-1 recognised by setup).
- [x] Launch through Steam as a **devkit title**, which is also what gives the game Steam Input: the
  Frame controllers are owned by SteamVR/Steam and no gamepad device exists under `/dev/input`, so
  a binary started from SSH sees no controller at all. Install lives in `~/devkit-game/GDiffuser`
  with `LAUNCH-STEAM-FRAME.sh` (cd + exec, log to `run-stdout.txt`), registered via
  `~/devkit-utils/steam-client-create-shortcut` with `compat_tool: SteamLinuxRuntime_4-arm64`.
  The game ID must be alphanumeric: `G-Diffuser` is rejected by Steam ("missing/invalid
  arguments"), `GDiffuser` registers.
- [ ] Confirm Steam Input exposes a virtual gamepad to the game when launched this way.
- Risk for VR mode: once G-Diffuser is an OpenXR scene app, SteamVR may route controllers to the
  OpenXR action system instead of the Steam Input gamepad. If so, M10 needs OpenXR actions mapped
  to the N64 pad, not just a recenter binding.

### M0-A OpenXR GL probe — PASS
Probe: `tools/vr_probe/gdx_openxr_probe.c` (SDL2 + GL 4.1 core request, exactly as
`gfx_sdl2.cpp`; Xlib/GLX binding; clears left eye dim red, right eye dim blue).

Device (2026-10-01): SteamOS 0.3.0 `vr` variant, build 20260922.6101926, kernel 6.18 aarch64,
SM8650 (Snapdragon 8 Gen 3, 8 cores), 15 GiB RAM, glibc 2.39 (release floor is 2.35 — OK).

| Item | Result |
| --- | --- |
| Session/display | gamescope `--backend openvr`, Xwayland `:0`, `XDG_SESSION_TYPE=x11` |
| SDL video driver | `x11` |
| GL | **Zink** (GL on Vulkan) over Turnip, Adreno 750, Mesa 26.3.0-devel: 4.6 core, GLX context |
| GL interop exts | `GL_EXT_memory_object(_fd)`, `GL_EXT_semaphore(_fd)` present (fallback pre-qualified) |
| Vulkan | Turnip, Vulkan 1.4.359 |
| OpenXR runtime | `SteamVR/OpenXR` 2.17.10, `~/.config/openxr/1/active_runtime.json` → `/opt/steamvr/steamxr_linuxarm64.json` |
| Instance exts (48) | incl. `XR_KHR_opengl_enable`, `XR_KHR_opengl_es_enable`, `XR_KHR_vulkan_enable2`, `XR_MNDX_egl_enable`, `XR_FB_display_refresh_rate`, `XR_KHR_convert_timespec_time`, `XR_EXT_frame_synthesis`, `XR_FB_space_warp`, `XR_KHR_composition_layer_depth`, `XR_KHR_visibility_mask`, `XR_EXT_user_presence`, `XR_META_recommended_layer_resolution` |
| Implicit API layer | `XrApiLayer_VALVE_fdm_injection` (always loaded) |
| `xrGetOpenGLGraphicsRequirementsKHR` | min 4.3, max 4.6 |
| `xrCreateSession` (Xlib/GLX binding) | **success** |
| View config | `PRIMARY_STEREO`; recommended **1728×1728** per eye, 1 sample; max 8192 |
| Eye FOV (rad) | L eye: L −1.028 R 0.885 U 0.864 D −1.048; R eye mirrored; IPD 69.8 mm at test time |
| Swapchain formats | `GL_SRGB8`, `GL_SRGB8_ALPHA8`, depth 16/24/32F — **no linear RGBA8** |
| Reference spaces | VIEW, LOCAL, STAGE |
| Blend modes | OPAQUE, ALPHA_BLEND |
| Default display period | 13.889 ms (**72 Hz**) |
| Frame loop | `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame` run at 71.9 Hz; first frame rendered |

Consequences:
- **GL is go.** No Vulkan backend needed. Interop fallback not needed but available.
- **Request GL ≥ 4.3 in VR mode.** libultraship asks for 4.1 core; Zink returns 4.6 anyway, but the
  runtime's stated minimum is 4.3, so the VR path should request it explicitly.
- **sRGB-only swapchains.** The game's output is already gamma-encoded. Copy into the swapchain with
  `GL_FRAMEBUFFER_SRGB` disabled so values land unconverted (the compositor then reads them as
  sRGB-encoded, which is what they are). Verify visually against the flat build.
- **Zink overhead** adds CPU cost per GL call; M0-D must measure on this stack.
- The loader is available as SteamVR's `/opt/steamvr/bin/linuxarm64/libopenxr_loader.so` and a
  system static `libopenxr_loader.a`. We still ship a pinned Khronos loader in `lib/`.
- 72 Hz is the default rate; `XR_FB_display_refresh_rate` can request others. 72 Hz/59.94 is a
  non-integer ratio, so the display-time scheduler (M2) is needed from day one.

Worn-headset run (2026-10-01 23:03): `IDLE → READY → SYNCHRONIZED → VISIBLE → FOCUSED` within
30 ms of `xrBeginSession`; 706/706 frames rendered with valid poses; 70.4 Hz average over 10 s on
the 13.889 ms period; ~0.44 ms begin→end per frame for two eye clears. An earlier run with the
headset off-head stayed `SYNCHRONIZED` (`shouldRender` false) because the HMD driver was in
standby (`[CCVTrackedHmdDriver] in StateStandby` in `~/.local/share/Steam/logs/vrserver.txt`) —
the VR layer must treat that as a normal not-renderable state (freeze sim, keep polling).

### Flat-mode issues seen on device (stock build, before any VR change)
- **Small text sprites corrupted** (user report, first flat session). Candidates: Zink handling of
  small texture rectangles / TLUT formats; the 64DD IPL font path (US-prototype IPL in use).
  A/B against native freedreno GL and against the frame captures.
- **Crackly audio** (user report). Candidates: audio underruns from CPU scheduling on the A520/A720
  cluster, audio-thread starvation, or PipeWire/Pulse latency inside the Steam Linux Runtime
  container. Check the audio thread's underrun counters and buffer sizing.

### M0-D Performance — GO (CPU budget), measured flat on device 2026-10-01

Run: stock build, Steam-launched devkit title, `GDX_PERF=1`, Frame Interpolation on, capped at
180 fps (3 planned passes/tick), user-played GP race plus menus, ~9 minutes, 45 ten-second
`[GDX perf] summary` windows (`build/device-logs/m0d-race1.txt`, not committed). Flat window was
gamescope at 90 Hz (11.11 ms refresh cycle).

Steady-state means per 60 Hz tick across the race windows:

| Phase | ms | Notes |
| --- | --- | --- |
| decomp game logic (`logic`) | 0.50–0.59 | |
| DL conversion (`xlate`) | 1.0–1.5 | once per tick |
| `Interpreter::Run` per replay (`irun`) | 1.0–1.6 | CPU side, through Zink; this is the per-eye cost |
| ImGui frame per pass (`gui`) | 0.4–0.75 | not needed per eye in VR |
| present (`eframe`) | ~3.0 | gamescope swap/vsync block — **absent in VR** (no desktop swap per eye) |
| `sim_hz` | 59.9 | held throughout (one window read 59.2, recovered) |
| audio mix p95 / max | 0.16 / <1 ms | audio work is cheap — crackle is not CPU starvation of the mixer |

Desktop interpolation only achieved ~63–70 presents/s: `[interp-pace] planned=3 passes=1
budget=2` — the budget guard sheds passes because each flat present blocks on gamescope's swap.
That limit belongs to the desktop present path, not to rendering cost, and VR does not use it.

VR estimate per XR frame (stereo, no desktop swap, + ~0.3 ms GPU blit per eye assumed):
- frame that also runs a tick: logic 0.6 + xlate 1.5 + 2 × irun 1.6 + copies 0.6 ≈ **5.9 ms**
- frame without a tick: 2 × 1.6 + 0.6 ≈ **3.8 ms**
- budgets: 72 Hz 13.9 ms, 90 Hz 11.1 ms, 120 Hz 8.3 ms → **fits 90 Hz with ~2× headroom**; 120 Hz
  is plausible.

Not yet measured: GPU time at 1728² per eye through Zink (N64-era fill, expected small), and
`irun` at eye resolution rather than window size (CPU-bound work, expected to be flat). Both get
re-measured with the first stereo build via `[vr-perf]`.

### linux-arm64 archive mismatch (found and fixed during M0)

On ARM64 the extracted `fzerox.o2r` never matched the build's golden SHA-256
(`port/gen/gdx_o2r_expected.h`), so first boot discarded it, booted from the raw ROM and re-entered
setup on every launch. With identical ROM and recipes, x86-64 `gdx-extract` produced the golden
archive and ARM64 differed in 23 `course_data/*` records only:
- `CourseData::mFileName` is a `std::vector<char>` written through `BinaryWriter::Write(int32_t)`
  (there is no `char` overload); `char` is unsigned on ARM64, so a `0xFF` name byte became
  `ff 00 00 00` instead of `ff ff ff ff`;
- the course checksum (`CourseData::CalculateChecksum`) differed by a few units because GCC fuses
  multiply-adds on ARM64 by default (`-ffp-contract=fast`), changing rounding before the float→int
  truncations. The game build already disables contraction (top-level `CMakeLists.txt`), but Torch
  is an isolated ExternalProject and inherited nothing.

Fix: `port/cmake/gdx_extract_portable.cmake`, injected via `CMAKE_PROJECT_INCLUDE`, adds
`-fsigned-char -ffp-contract=off` to every Torch target (Torch overwrites `CMAKE_CXX_FLAGS`, so
command-line flags never reached its sources). ARM64 extraction then reproduces the golden hash
`f2983f2f…26311` exactly, on the build container and on the Frame; the x86-64 extractor built
with the same flags still produces the golden archive (no change there, as expected: signed `char`
is the x86 default and x86-64 GCC does not contract without `-mfma`). This affects every linux-arm64
release user, independent of VR, and is worth upstreaming on its own.

---

## 4. Implementation (GDX_OPENXR)

### Build and launch
- `-DGDX_OPENXR=ON` (Linux only) fetches OpenXR-SDK `release-1.1.63` (archive SHA-256 pinned) and
  links the loader **statically**: the executable's NEEDED list is unchanged and the package ships
  no new library. OFF builds are untouched.
- VR is a launch-time choice: `--vr` or `GDX_VR=1`. Without it the only effect is one log line,
  `[openxr] VR mode off`; desktop rendering, pacing and display lists are unchanged.
- On the Frame: two Steam devkit titles share one install. **GDiffuser** launches flat;
  **GDiffuserVR** runs `LAUNCH-STEAM-FRAME-VR.sh` (`--vr`, `GDX_PERF=1`, log in
  `~/devkit-game/GDiffuser/run-vr-stdout.txt`).

### Modules
| File | Role |
| --- | --- |
| `port/vr/gdx_vr_scheduler.*` | Display-time scheduler: places each XR frame on the 59.94 Hz tick timeline by `predictedDisplayTime`, says how many ticks must run first and at which interpolation t to render. Pure; `gdx_vr_scheduler_tests` (45 checks, incl. 72/90/120/144/45 Hz uniform steps and an hour without drift). |
| `port/vr/gdx_vr_system.*` | System layer as pure policy: left View button hold/short-press timing, recentre-request merging (Level vs Height), the system haptic pulse, and the `gEnhancements.VR.*` settings table (names, defaults, clamp ranges). `gdx_vr_system_tests` (33 checks). |
| `port/vr/gdx_vr_math.*` | Per-eye projection·view in the game's conventions; asymmetric XR FOV; s15.16 fit (uniform scale, NDC-invariant). `gdx_vr_math_tests` (34 checks) compares against a transcription of `Matrix_SetLookAt`/`Matrix_SetFrustrum`/`Camera_CalculateProjectionViewMtx`. |
| `port/vr/gdx_openxr.*` | Instance/session/swapchains (GL, Xlib/GLX binding), session states, the shared frame protocol, flat quad, stereo projection layer, head-locked HUD quad, OpenXR controller actions, diagnostics. |
| `libultraship` `Interpreter` | `gfx_pre_present_hook`; `SetVrEyeTarget` (offscreen eye-sized target, no MSAA/post shader/hor+ squash, overscan border scissors removed); `VrPass` world/HUD split at the HUD marker; texture rectangles dropped from world passes. |
| `port/n64_gfx_bridge.cpp` | VR branch of the replay: per XR frame, per eye, refill scratch at t, replace camera 0's projection·view with the eye's (`GdxVrEyeRebuildSlot`), rebuild the backdrop for the eye, replay world-only; HUD-only replay once per tick. |
| decomp (PORT-gated) | `race.c`: HUD split marker before `Menus_Draw` (emitted only in VR). `background.c`: per-eye backdrop vertex rebuild on copies of camera/background, with save/restore of the pool vertices. `course.c` / `racer.c`: distance-only culling while `gGdxVrWideCull`. |
| `port/input_bridge.c` | `gGdxVrInputMerge` hook: OpenXR controller actions OR-merged into pad 1. |

### Frame protocol
One XR frame is waited on at a time. `PlanFrame(predictedDisplayTime)` gives the ticks it needs;
each completed tick pays one off; whichever path runs after the last tick shows it — the
pre-present hook in flat mode (copies the composited back buffer into the quad swapchain), the gfx
bridge in stereo mode (eye replays inside the tick's final task, where the retained display list
and both GfxPools are alive). Frames that need no tick are resubmitted with the previous image.
Stereo is chosen per tick: 1 player and game mode GP race / practice / time attack / death race
(the title attract demo is a GP race, so it is stereo too). Every switch marks an interpolation cut.

Measured on the Frame (72 Hz runtime default, headset in standby, forced rendering):
- sim rate 59.9–60.0 ticks/s in flat and stereo (e.g. 2,400 XR frames in 33.3 s → 2,001 ticks);
- world-only eye replay 0.8–1.4 ms per eye, eye copy 0.05–0.16 ms; HUD pass once per tick.

### Diagnostics (environment)
| Variable | Effect |
| --- | --- |
| `GDX_VR=1` | Same as `--vr`. |
| `GDX_VR_FORCE_RENDER=1` | Render eyes even when the runtime says not to (headset in standby); synthetic straight-ahead pose if none is tracked. |
| `GDX_VR_DUMP_FRAME=<n>` | Write both eyes (`vr-eye<e>-<n>.bmp`) and the HUD rendered over black and over white (`vr-hud-black.bmp`, `vr-hud-white.bmp`; the two inputs of the alpha-recovery shader) of stereo frame n; each dump logs its % fully transparent pixels. |
| `GDX_VR_DIAG_YAW=<deg>` | Turn the tracked head about LOCAL +y, to capture head turns unattended. |
| `GDX_VR_DUMP_ON_SPRITE=1` | Dump the eyes of the first stereo frame that places a world sprite (`vr-sprite-eye<e>.bmp`) and the HUD of the first tick that draws a HUD sprite (`vr-sprite-hud.bmp`). |
| `GDX_PERF=1` | Adds `[vr-perf]` every 600 XR frames: frames, tick-less frames, stereo frames, rebases, sim ticks, display period, per-eye render and copy ms. |

CVars: `gEnhancements.VR.Stereo` (1; 0 keeps races on the flat screen), `gEnhancements.VR.WorldScale`
(10 world units per metre — the game's own scale: the HUD shows km/h as speed × 21.6 with speed in
units per 60 Hz tick, i.e. 10 units = 1 m), `gEnhancements.VR.WideCull` (1),
`gEnhancements.VR.HorizonLock` (0, default off: the viewer rolls with the chase camera; 1 removes that roll).

Persistence and ranges: the first VR launch registers all ten `gEnhancements.VR.*` keys with their
defaults and saves the libultraship config (`CVarSave`), so they appear in the config file and persist
from then on. On load, out-of-range or non-finite values are clamped and written back (and a value of
the wrong CVar type, e.g. `"HudHeight": 2`, is converted); every read clamps again. The log prints a
`[vr] settings:` line with the effective values. Ranges: `Stereo`, `WideCull`, `CameraMode`,
`HorizonLock`, `LevelPitch` 0..1; `HudAspect` 4/3..4 (default 1.5); `HudHeight` 0.3..3 m (1.2);
`NoseForward`, `NoseUp` -40..40 world units (8, 6); `WorldScale` 2..50 (10). Chase/cockpit toggles also
save `CameraMode`. There is no UI for any of them yet.

### Verified unattended (eye dumps at 0°, 90° and 180° head yaw)
- Correct stereo: near objects shift the right way between eyes; the asymmetric eye frustums offset
  distant scenery as expected; images upright and unmirrored.
- No overscan border; HUD absent from the eyes and rendered on its own layer, whose image is 99.1%
  fully transparent (only the HUD elements are opaque), so the world shows through it.
- Sky, venue floor, clouds and distant scenery present at every head yaw, including straight back.
- Track and machines behind the game camera present (distance culling); no vertex-buffer overflow.

### Known limitations / next
- Not yet seen through the headset: world scale, comfort (the chase camera's roll in pipes and loops
  is applied to the viewer's reference frame), HUD size/distance, brightness.
- Screen-space sprites the game draws in the world part of the list are now handled per kind
  (G_NOOP sub-codes 0x57-0x5A, emitted by PORT-gated helpers in `port/decomp_port.c`, VR only):
  - **HUD sprites** — the "CHECK" racer-behind warning (`racer.c` Racer_Draw) and the FINISH /
    result text (`Menus_Player1SpecialDraw`, `race.c`) are bracketed and drawn by the HUD pass onto
    the head-locked HUD layer, where they behave as on a flat screen.
  - **World sprites** — the top-3 position badges and the GP rival marker are anchored to their
    world point (20 units above the machine). Each eye replay re-projects the anchor with the eye's
    projection·view, rebuilds the rectangle around it (the game's own rectangle was clipped to the
    game camera's screen), moves the primitive depth to the eye depth, and scales the sprite by
    gameTanHalfY / eyeTanHalfY so it keeps its flat-screen angular size. Verified on the Frame: the
    "1ST" badge renders over the leader in the eye that sees it.
  - **Stars** (`Background_DrawStars`) are still left out in VR; a per-eye re-projection like the
    world sprites would bring them back.
- Effect billboards (boost flames, sparks) still face the game camera, not each eye.
- Controller input: OpenXR actions merged into pad 1. Native Steam Frame profile
  (`/interaction_profiles/valve/frame_controller_valve`, extension
  `XR_VALVE_frame_controller_interaction`, paths per Steamworks "Steam Frame Input"): left stick →
  stick, right stick → C buttons, right A → A, right X → B (boost), right Y → C-right (change
  view), left trigger → Z, right trigger → R, left bumper → L, right bumper → R (the right trigger
  and right bumper both send R), right menu → START, left D-pad → D-pad, left View → system layer
  (below). Stick clicks are unbound. Index/Touch bindings remain as fallbacks. Triggers, bumpers and
  grips are not final: gameplay remapping is a pending decision and was deliberately not touched.
- Race HUD layer: rendered at `gEnhancements.VR.HudAspect` (default 1.5:1) with the widescreen HUD
  layout forced on, so edge-anchored elements sit at the panel sides; shown head-locked 1.5 m ahead,
  `gEnhancements.VR.HudHeight` metres tall (default 1.2). Coverage is recovered by rendering the HUD
  over black and over white and combining on the GPU.
- Recentre: see "System layer" below. Chase view is levelled against the track's up
  (`Camera.racerBasis.y`; `gEnhancements.VR.LevelPitch`, default 1) while still banking with it.
- Cockpit view (the plan's "Nose VR"): right B on the Frame controllers toggles chase ↔ cockpit
  (start mode `gEnhancements.VR.CameraMode`, 0 chase / 1 cockpit; `GDX_VR_COCKPIT=1` for tests).
  The eye is anchored at the followed racer's rendered position (`modelPos`) plus
  `gEnhancements.VR.NoseForward` (8) world units along its forward and `gEnhancements.VR.NoseUp`
  (6) along its up, oriented by its travel basis (`trueBasis`, so spin attacks and the body's lean
  animation do not turn the view). The pose is snapshotted per tick with the camera poses and
  interpolated at the frame's t, so it stays phase-locked to the rendered machine. Near plane is
  capped at 2 units (0.2 m) so the machine's own nose is not clipped. Offsets are first guesses,
  not yet calibrated against machine geometry or tried in the headset.
- No desktop mirror while in stereo, HUD always head-locked.
- The flat quad is world-locked 2 m ahead of the last recentre, at the height anchor (see below).

### System layer
The controls that are the same in every Steam Frame port (cross-port standard, section 1). Nothing
else does system work: stick clicks and chords stay game actions.

| Input | Action |
| --- | --- |
| Right Menu | N64 START (the game's pause) |
| Left View, short press | N64 B, **only while no race is on screen** (flat menus), so it is Back there. In a race the short press does nothing, so it never collides with boost. B is sent on release and held for 100 ms (the game polls pads once per tick) |
| Left View, hold 1 s | Recentre (yaw and horizontal position; height anchor kept), with a 0.6 amplitude / 80 ms buzz on both hands |
| Left View, hold 3 s | Recentre and re-anchor the height (eye height and flat menu quad height), buzz again. The 1 s recentre has already fired by then |

Index has no View button, so left A stands in for it; Touch uses left Y. Haptics are two vibration
output actions (left and right) bound to `/output/haptic` on the Frame, Index and Touch profiles.

Recentre details:
- A recentre applies at an XR frame boundary (`xrWaitFrame`). It captures the head's yaw about LOCAL
  +y and its horizontal position; eye poses and the flat quad are expressed relative to them. The
  poses submitted to the compositor stay in raw LOCAL space.
- The **height anchor** is the LOCAL height treated as the game camera's height (and the flat quad's
  centre height). It changes only on Height recentres: View hold 3 s, gaining focus (possibly a
  different wearer), the runtime's `XrEventDataReferenceSpaceChangePending` (a system recentre), and
  the very first recentre.
- Level recentres (yaw and position only): View hold 1 s, every flat↔stereo switch (each race and each
  return to the menus starts facing forward), and the R-B chase/cockpit camera toggle. The camera
  toggle recentre is kept deliberately (the two views are anchored differently); it is the one
  recentre that is not on the system layer.
- Requests that land before the next frame boundary merge into the stronger kind. A View press in
  progress when focus is lost is dropped (it neither recentres nor sends B).
- Log lines: `[vr] recentre requested (<why>, <what>)` and `[openxr] recentred: ... height anchor
  <y> (updated|kept)`.

Not verified on a headset: the View binding on the Frame profile, that the Frame profile accepts
`/output/haptic`, the hold feel, and the buzz.

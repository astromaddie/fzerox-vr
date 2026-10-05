# VR (OpenXR / Steam Frame) — architecture notes

Status: **M0 gates passed; flat-screen and stereo-race presentation implemented and running on a
Steam Frame** (unattended, headset in standby; not yet verified by a person wearing it). Sections 1-3
record what the stock renderer does and what was measured; section 4 describes what was built.

A minimal **in-headset VR settings page with Quit to Steam** exists (section 4, "In-headset menu"): the
G-Diffuser menu's VR tab on the flat quad, opened by holding Menu + View for 0.5 s. It is not a full
settings page (no tab set, no pointer, no gameplay or comfort-turn settings yet). The system layer, the
menu, haptics, the one-shot refresh-rate request and held-rate report, the resolution scale and the `[vr-perf]` line are
implemented but, like everything else here, none of it has been tried by a person in the headset.

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
| View config | `PRIMARY_STEREO`; recommended **1728×1728** per eye, 1 sample; max 8192 (the port renders at 1728 x `ResolutionScale`, 2160² by default) |
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
- 72 Hz is what the runtime starts at; `XR_FB_display_refresh_rate` can request others (the rate is really SteamVR's per-app setting, see "Display"). 72 Hz/59.94 is a
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
| `port/vr/gdx_vr_system.*` | Pure policy, no OpenXR: an adapter over sfvr's View-button state machine, recentre-request merging (Level vs Height), the Menu + View chord, stick-as-D-pad for menu navigation, refresh-rate picking, the held-display-rate watch (`RefreshWatch`: report once, recommend a per-app rate), the display rate from `predictedDisplayPeriod`, the eye image size calculation, the display-standard settings migration, and the `gEnhancements.VR.*` settings table (names, defaults, clamp ranges). `gdx_vr_system_tests` (134 checks, including the cockpit roof lookup, the held-rate watch, the eye size and the migration). |
| `port/third_party/sfvr/` | The shared cross-port VR library (C99), vendored by copying from `~/code/sfvr`; `VERSION` holds the pinned commit. Never edited here. Built as `gdx_sfvr`. Used for the View button, the haptic queue and the `[vr-perf]` line. |
| `port/vr/gdx_vr_math.*` | Per-eye projection·view in the game's conventions; asymmetric XR FOV; s15.16 fit (uniform scale, NDC-invariant). `gdx_vr_math_tests` (74 checks) compares against a transcription of `Matrix_SetLookAt`/`Matrix_SetFrustrum`/`Camera_CalculateProjectionViewMtx`, and checks cabin-point precision of the camera-relative eye projection. |
| `port/vr/gdx_openxr.*` | Instance/session/swapchains (GL, Xlib/GLX binding), session states, the shared frame protocol, flat quad, stereo projection layer, head-locked HUD quad, OpenXR controller actions, diagnostics. |
| `port/vr/gdx_vr_settings.*` | Loads, repairs and saves the `gEnhancements.VR.*` CVars at session start (missing keys get defaults, wrong-typed values are converted, strings are reset). `gdx_vr_settings_tests` runs it against libultraship's real config code, then checks the SIGTERM exit path. |
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
| `GDX_VR_FORCE_RENDER=1` | Render eyes and flat frames even when the runtime says not to (headset in standby); synthetic straight-ahead pose if none is tracked. |
| `GDX_VR_DUMP_FLAT_EVERY=<k>`, `GDX_VR_DUMP_FLAT_UNTIL=<n>` (default 600) | Write the window back buffer the flat quad shows (`vr-flat-<i>.bmp`) every k-th flat tick up to tick n, shown or not. |
| `GDX_VR_DUMP_FRAME=<n>` | Write both eyes (`vr-eye<e>-<n>.bmp`) and the HUD rendered over black and over white (`vr-hud-black.bmp`, `vr-hud-white.bmp`; the two inputs of the alpha-recovery shader) of stereo frame n; each dump logs its % fully transparent pixels. |
| `GDX_VR_DIAG_YAW=<deg>` | Turn the tracked head about LOCAL +y, to capture head turns unattended. |
| `GDX_VR_DUMP_ON_SPRITE=1` | Dump the eyes of the first stereo frame that places a world sprite (`vr-sprite-eye<e>.bmp`) and the HUD of the first tick that draws a HUD sprite (`vr-sprite-hud.bmp`). |
| `GDX_VR_DUMP_COUNT=<k>` | With `GDX_VR_DUMP_FRAME`, dump k consecutive stereo frames (left eye only when k > 1). Each dump is a synchronous readback, so dumped frames are several ticks apart. |
| `GDX_VR_DIAG_GEOM=<n>` | Log the first n cockpit eye renders' far-plane vertex count, clip-rejected and emitted triangles (`[vr-geom]`). |
| `GDX_VR_DIAG_PITCH=<deg>` | Tilt the located head about its own x axis (negative looks down), e.g. to capture the cabin dash. |
| `GDX_VR_CABIN_IN_CHASE=1` | Also draw the cabin (without the hull cut) in the chase view, to inspect it from outside. |
| `GDX_VR_COCKPIT_NEAR=<units>`, `GDX_VR_COCKPIT_FAR_SCALE=<x>` | Override the cockpit projection's near plane (default 0.5 in the cabin, 2 on the roof) and far multiple (default 4), for experiments. |
| `GDX_PERF=1` | Makes the `logic` and `xlate` stages of `[vr-perf]` real (they come from the port's GDX_PERF telemetry; they read 0.00 otherwise). |

`[vr-perf]` is logged every 10 s while a session runs (sfvr_perf, the cross-port format):
`[vr-perf] fps=71.9 missed=3 cpu=7.31ms logic=1.20 xlate=0.40 irun=5.02 hud=0.30 eye=2.21/2.19ms gpu=n/a`.
`irun` is the per-eye interpreter replays (also shown per eye as `eye=`), `hud` the HUD pass, `xlate`
display-list translation, `logic` the game tick minus translation, run, mirror and the time inside the
stereo present (which includes xrWaitFrame's blocking). `missed` counts XR frames whose predicted
display time jumped by more than 1.5 display periods. `gpu` is `n/a`: there is no GPU timing yet. A
second line, `[vr] pacing: ...`, carries the old extras (tick-less and stereo frame counts, rebases, sim
ticks, display period, copy ms). `logic` is a derived figure that has not been checked against a
device run.

CVars: `gEnhancements.VR.Stereo` (1; 0 keeps races on the flat screen), `gEnhancements.VR.WorldScale`
(10 world units per metre — the game's own scale: the HUD shows km/h as speed × 21.6 with speed in
units per 60 Hz tick, i.e. 10 units = 1 m), `gEnhancements.VR.WideCull` (1),
`gEnhancements.VR.HorizonLock` (0, default off: the viewer rolls with the chase camera; 1 removes that roll).

Also registered: `gEnhancements.VR.Haptics` (0..1, 0.6: scale for every haptic pulse) and
`gEnhancements.VR.RefreshRate` (72..120, 120: Hz asked of the runtime once per session; no effect on the Steam Frame), `gEnhancements.VR.ResolutionScale` (0.75..1.5, 1.25: eye image size as a multiple of the runtime's recommendation, read at VR start) and `gEnhancements.VR.DisplayStandard` (settings-migration version, currently 1; not shown in the menu).

Persistence and ranges: the first VR launch registers every `gEnhancements.VR.*` key with their
defaults and saves the libultraship config (`CVarSave`), so they appear in the config file and persist
from then on. On load, out-of-range or non-finite values are clamped and written back (and a value of
the wrong CVar type, e.g. `"HudHeight": 2`, is converted); every read clamps again. The log prints a
`[vr] settings:` line with the effective values. Ranges: `Stereo`, `WideCull`, `CameraMode` (default 1,
cockpit), `HorizonLock`, `LevelPitch` 0..1; `HudAspect` 4/3..4 (default 1.5); `HudHeight` 0.3..3 m (1.2);
`RoofForward` -12..12 world units (5.8); `RoofClearance` 0..20 world units (3.5); `CockpitTilt` 0..1 (0.5); `WorldScale` 1..50 (1; both views); `Haptics` 0..1 (0.6);
`RefreshRate` 72..120 (120); `ResolutionScale` 0.75..1.5 (1.25). Chase/cockpit toggles also save `CameraMode`. The VR tab of the menu (below)
edits all of these except `Stereo`, `WideCull` and `DisplayStandard`.

### Verified unattended (eye dumps at 0°, 90° and 180° head yaw)
- Correct stereo: near objects shift the right way between eyes; the asymmetric eye frustums offset
  distant scenery as expected; images upright and unmirrored.
- No overscan border; HUD absent from the eyes and rendered on its own layer, whose image is 99.1%
  fully transparent (only the HUD elements are opaque), so the world shows through it.
- Sky, venue floor, clouds and distant scenery present at every head yaw, including straight back.
- Track and machines behind the game camera present (distance culling); no vertex-buffer overflow.

### Known limitations / next
- Fixed (2026-10-03): heap corruption at exit after a SIGTERM. The unattended Frame runs that saved
  settings at startup aborted at exit with glibc `corrupted double-linked list` and hung. The save
  turned out not to be the cause. libultraship's CrashHandler answers SIGTERM and SIGINT with
  `exit(1)` from inside the signal handler, so static destructors ran while the dedicated audio
  thread was still producing. That thread's own statics (the sample buffers in libultraship's
  `osAiSetNextBuffer`, the `gdx_perf` state) are created after its atexit join is registered, so
  they are destroyed first and the thread goes on copying 2240-byte buffers into freed memory. A
  later `free()` trips over the damage, and the crash handler then hangs trying to show a message
  box. On the host (arm64 container, Xvfb, `--vr` with no runtime) every SIGTERM'd run of the old
  build hung like this, 8 of 8, whether it saved or not; gdb shows `exit()` freeing the buffer while
  the audio thread is mid-tick. Now `port/gdx_shutdown_signal.c` turns the first SIGTERM or SIGINT
  into a window close, so the normal teardown (VR session, audio thread join) runs before `main()`
  returns; 8 of 8 runs of the fixed build exit 0. A second signal at least 2 s after the first still
  kills the process outright; repeats inside that window are absorbed, because timeout(1) and other
  supervisors signal both the process and its group, so one stop request often arrives as two
  SIGTERMs (on the Frame this killed 1 of 4 runs of the first version of the fix outright).
  SIGQUIT still goes through libultraship. `gdx_vr_settings_tests` covers the first-launch settings
  path and this exit (set `GDX_VR_SETTINGS_TESTS_OLD_SIGTERM=1` to see the old behaviour under
  ASan). The early Frame pattern (only the two saving runs failed) was chance: a later
  Frame run with a config that needed no save (2026-10-03, skybox diagnosis) also hung after
  SIGTERM and had to be killed. Verified on the Frame (2026-10-03, cockpit, live OpenXR session, SIGTERM at
  45-60 s): the old build exited 1 or hung until SIGKILL; the fixed build exited 0 in 6 of 6 runs
  (3 with a startup save, 3 without) within a second, logging the window close, `[openxr] shut down`
  and the audio thread stopping, in that order, with no heap errors.
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
  layout forced on, so edge-anchored elements sit at the panel sides; shown 1.5 m ahead of the head,
  following its yaw and pitch but never its roll (`NoRollPose`), `gEnhancements.VR.HudHeight` metres tall (default 1.2). Coverage is recovered by rendering the HUD
  over black and over white and combining on the GPU.
- Recentre: see "System layer" below. Chase view is levelled against the track's up
  (`Camera.racerBasis.y`; `gEnhancements.VR.LevelPitch`, default 1) while still banking with it.
- Cockpit view (the plan's "Nose VR"): right B on the Frame controllers toggles chase ↔ cockpit
  (start mode `gEnhancements.VR.CameraMode`, 0 chase / 1 cockpit, default cockpit; `GDX_VR_COCKPIT=1`
  for tests).
  The eye rides on the machine's roof, so the nose is in view ahead. It is anchored at the followed
  racer's rendered position (`modelPos`, the model's underside), moved `gEnhancements.VR.RoofForward`
  (5.8) world units along its forward, then up by that machine's roof height there plus
  `gEnhancements.VR.RoofClearance` (3.5 world units). Defaults moved from
  0 and 3 after the first look (2026-10-03: "slightly too low, slightly too far back"), then to the
  wearer's own slider settings. The current defaults (cockpit view, 5.8 forward, 3.5 clearance,
  tilt 0.5, world scale 1) are the settings the wearer tested and asked to keep (2026-10-03).
  - Orientation: the view's up blends from the travel basis (`trueBasis.y`) to the body's
    (`modelBasis.y`: the steering lean and the smoothed tilt the machine is drawn with) by
    `gEnhancements.VR.CockpitTilt` (0.5 by default; 1 = tilts fully with the machine). The heading is the travel forward
    flattened against that up, so a spin attack (which turns only `modelBasis.x`) never spins the
    view. `HorizonLock` still removes roll. All three cockpit settings are on the VR menu page.
  - Roof heights differ six-fold across the roster (Twin Noritta ~3.5 units at the centreline,
    Black Bull ~21), so they are per machine: `port/vr/gdx_vr_roof_table.h`, generated by
    `tools/gen_vr_roof_table.py --o2r <fzerox.o2r>` from the stock LOD1 race meshes (the highest
    body surface within 4 units of the centreline, from just behind the eye to the nose, so the view
    clears canopies and fins ahead). `gdx_vr_read_player_vehicle` reports the stock model (super
    machines included); edited custom machines and model packs use a 15-unit fallback roof. The
    first cockpit (`NoseForward`/`NoseUp` 8, 6) put the eye inside nearly every body.
  - Near plane 2 units, with fog remapped. The roof under the eye is inside the game camera's
    near plane (16). Using 16 with `GL_DEPTH_CLAMP` (d2daa50) kept the near hull on screen, but all
    of it shared one clamped depth, so the hull's inner faces drew over the roof ("the camera clips
    inside the ship", 2026-10-03 wear). The RSP takes fog from z/w, which a different near re-maps
    (the whole scene fogged), so the bridge computes `Interpreter::SetVrFogRemap` per eye: both
    projections give z/w = A - B/distance, and fog reads `bias + k*(z/w)` with k = B_game/B_eye,
    bias = A_game - k*A_eye. That is exactly the game camera's fog at every distance.
  - Far plane 4x the game camera's. With near 2 the backdrop (7000 units, far 8192) sat within ~8e-5
    of z/w = 1, about the matrices' s15.16 rounding, so its vertices crossed the far plane from frame
    to frame and whole triangles popped ("skybox buildings flicker", 2026-10-03 wear). Measured on
    the Frame with `GDX_VR_DIAG_GEOM` in the attract demo: far-plane vertices per left-eye render
    0..112, mean 28.8, unstable in 65 of 750 frames at 1x; 0 in every frame at 4x, with the same
    triangle count. Not depth precision: the backdrop is drawn without Z. The fog remap uses each
    projection's own near and far.
  - Clip, not reject: the machines are drawn with the F3DEX2.Rej microcode, which the interpreter
    emulates by dropping any triangle with a vertex behind the eye, inside the near plane or past
    twice the viewport. From the roof, a hull face hit that as soon as one corner left the view, so
    whole faces flickered out (2026-10-03 wear). Cockpit eyes set
    `Interpreter::SetVrClipNotReject`, so those triangles go to the homogeneous clipper (added after
    the reject emulation, and the reason the "giant wedges" it guarded against no longer occur).
    Flat mode and the chase view keep the reject behaviour.
  - Scale: `WorldScale`, as in the chase view. The wearer found 2 (the old minimum) still made the
    cockpit feel a little small, so the range now goes down to 1.
  - **Cabin style** (`gEnhancements.VR.CockpitStyle` 1, the default; 0 keeps the roof seat): the
    viewer sits inside a cabin in their own machine. `tools/gen_vr_cabin_table.py --o2r` measures
    each stock LOD1 mesh into `port/vr/gdx_vr_cabin_table.h`: cabin volume, seat, cabin geometry
    (floor, walls, body-coloured sills, bulkhead, headrest, dash) and the canopy glass triangles.
    27 of 33 machines keep their own canopy as tinted glass (Twin Noritta: the left hull's; its
    other cockpit stays as drawn); the six without glass get a generic frame and glass. Custom
    machines and model packs fall back to the roof seat. In cabin ticks Racer_Draw brackets the
    followed racer's LOD1 with `G_NOOP` 0x5B/0x5C; inside them cockpit eyes skip hull triangles in
    the cabin volume or on the canopy (`Interpreter::SetVrCabinCut`), and the cabin list is drawn
    after with the machine's own lights, then the machine setup list restores state. The cabin's
    vertices and list live in emulated RDRAM, the list range registered as PORT-format
    (`gdx_register_host_wide_command_range`): in the executable's static data the bridge read the
    vertices as N64 data (scrambled axes), and unregistered RDRAM lists as 8-byte commands (nothing
    drawn). `CabinSeatUp` / `CabinSeatForward` (+-10 world units) adjust the seat.
  - **Cabin steadiness** (2026-10-05, after "the boost-ready indicator jitters, and so does the whole
    ship" and "the CHECK lights drift up and down against the mirror"; build- and test-verified,
    not yet worn):
    - Camera-relative eye projection. The eye's projection*view used to reach the interpreter as an
      s15.16 matrix with the world translation in it; rounding at thousands of units from the course
      origin moved everything by up to ~0.1 units per frame, degrees of jitter on a cabin a few units
      away, while the compositor placed the quads exactly. Each VR eye (chase, cockpit and the
      mirror replay) now hands `Interpreter::SetVrEyeProjection` a float projection*view relative to
      the eye anchor and the anchor itself in double; while that projection is loaded every MP is
      formed from the modelview translated by -anchor in double (`Interpreter::ComputeMP`). The
      s15.16 matrix still goes in the slot (and `MtxFromF` now rounds rather than truncates). The
      dash and mirror quads are placed relative to the anchor too (`SetCockpitBody` takes the body
      origin relative to the eye). `gdx_vr_math_tests` `CaseCabinPrecision`: cabin points within
      ±15000 units land within 0.01 degrees of a double reference (the old path: degrees). A log
      line `[vr] eye projection: rebuilt slot not loaded by the list` means the override missed.
    - The cabin eye is the machine's model matrix applied to the seat, taken from the replay's own
      interpolated body matrix (`GdxPlayerBodyMtx`), and the view takes the body's whole frame, so
      `CockpitTilt` applies to the roof seat only. A spin attack now turns the cabin view with the
      machine. The anchor no longer tweens on a tick where that matrix snaps (also for the roof).
    - The cabin racer is placed without its ride shake (the speed vibration along the body's up,
      ±0.3 units at 1000 km/h, five times on dirt, and the spin-out jitter): its model matrix, attack
      highlight, booster flames and the eye all use `gdx_vr_racer_model_pos`. Chase and roof views
      keep the shake.
  - **Dash** (cabin only), aiming at a real dashboard:
    - HUD crops (`imageRect`) of the race HUD swapchain as quad layers, posed from the machine's body
      frame through the cockpit eye anchor into LOCAL (`WorldToTracking`; `kDashPanels`), all on the
      dash face: energy over the timer on the left, the minimap in the centre, speed on the right. The
      face leans back 25 degrees from vertical (`DASH_LEAN_DEG` in the generator, `kDashFaceDeg`
      here), with a shelf on top, and each readout is turned to face the seat (`PlaceFacingSeat`).
      First wear (2026-10-05) found them far too large and too far out, needing a head turn: now
      about 10 degrees wide at the default `DashHudSize` (0.45), within about 17 degrees of straight
      ahead. The minimap crop is the game's whole 64x64 map box (`minimap.c`, right-anchored), since a
      course is drawn anywhere inside it; the old crop fitted one course and cut wider ones off.
      Crops assume OpenGL bottom-up rows (`HudRect`).
    - Floating HUD: the HUD swapchain holds two stacked copies of the HUD, the whole one (chase view,
      dash crops) and one with the four dash crops cut out by the combine shader, which the cabin
      shows as its floating panel. The cabin used to float a crop of the left two-thirds, which cut
      off FINISH, the lap times and the place on the right (2026-10-05 wear).
    - Annunciator pod: a block rising from the middle of the dash shelf, its face turned to the seat,
      with two lamps: "CHECK ENGINE" (red, energy under 25%) on the left and "BOOST READY" (green,
      while a boost is available and not running) on the right. Cabin geometry, unlit, coloured per
      tick in `gdx_cabin_lamps`; the lettering is part of the lamp, built from a 5x7 font as one quad
      per run of lit pixels (`gdx_cabin_emit_text`, from the generator's per-machine text frames, in
      model units x64 under a 1/64 scale since a font pixel is a fifth of a unit): dark on a lit
      lamp, pale on a dark one. Blinks are 1.5 Hz, 3 Hz when urgent (energy under 10%, a racer within
      1000). Replaced sill lamps with quad labels (2026-10-05 wear: out of view at speed, a 10 Hz
      boost flash, 3-7.5 Hz blinking, labels drifting against the cabin).
    - Rear-view mirror (`CabinMirror`, on): a third world replay per tick from the cockpit anchor
      raised 4 model units above the canopy top (the cabin's bulkhead fills it otherwise), turned
      180 degrees, 60 x 20 degree FOV, 768x256, copied flipped left to right into its own swapchain
      and shown as a quad 40 model units ahead of the seat and up to 11 above it, about 13 degrees
      wide, facing the seat (it was in the middle of the forward view, 19 degrees wide). Its frame
      is drawn into its own image: a dark bezel that blinks amber while a racer is close behind
      (`gGdxVrCabinLamp[1]`), replacing amber cabin lamps at the mirror ends that drifted against it.
      `GDX_VR_DUMP_FRAME` also writes `vr-mirror.bmp` (unflipped, before the frame).
    - Near plane 0.5 in the cabin (2 for the roof seat): the canopy is ~1.3 units over the eye and
      near 2 cut its top off ("the windshield cuts through my head", 2026-10-05).
    - Headset-unverified: the 2026-10-05 layout (readouts, pod, mirror, floating HUD, near plane) is
      build-verified only; the layout was approved from an offline mockup.
  - The pose is snapshotted per tick with the camera poses and interpolated at the frame's t, so it
    stays phase-locked to the rendered machine.
  - Verified unattended (before the tilt and near-plane change): a forced-render cockpit eye dump in
    the attract demo shows the followed machine's canopy and nose below the horizon in both eyes.
    The tilt, the fog remap and the new defaults are build-verified only.
- No desktop mirror while in stereo, HUD always head-locked.
- The flat quad is world-locked 2 m ahead of the last recentre, at the height anchor (see below).
  Flat frames also submit a black projection layer under it. With the quad alone, a corrupted,
  enlarged and mirrored copy of the 64DD splash showed behind the panel (2026-10-04 wear), while
  the image copied into the quad was clean (`GDX_VR_DUMP_FLAT_EVERY`). Most likely SteamVR kept
  drawing the Steam launch's desktop window on a theater screen until the app submitted a
  projection layer. The backdrop is accepted (full unattended run, 72 fps, no XR errors); not yet
  worn.

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

Haptics go through one sfvr queue, coalesced (strongest amplitude, longest duration per hand) and flushed
once per XR frame, scaled by `gEnhancements.VR.Haptics` (default 0.6, so the 0.6 system buzz plays at
0.36). N64 rumble is routed there: the game's rumble state machine (replicated in `port/input_bridge.c`)
switches the motor on and off on its own duty cycle; the commanded level is recorded per port
(`gGdxVrMotorOn`), and while it is on a 40 ms full-amplitude pulse is submitted to both hands every host
iteration, so the game's pattern becomes the haptic pattern. The pak has no intensity or handedness to map.

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

### Display: 120 Hz at native resolution
This follows the cross-port display standard (steam-frame-vr-port, standard.md section 9): 120 Hz at about
the panel's own 2160x2160 per eye, the way Half-Life (Lambda1VR) runs. **None of it has been measured on
the device yet**: whether F-Zero X holds 120 Hz at 2160x2160 through Zink, and whether the report below ever
fires, are still open.

**The app cannot change the refresh rate on the Frame.** SteamVR runs a title at its per-app preferred
rate, set by the user in the headset, and only for Steam-title launches (a launch outside Steam runs at 72).
The runtime offers the app only that rate, and the `XR_FB_display_refresh_rate` list can say 90 while the
compositor runs 120. So this port does not try to manage the rate:
- `xrRequestDisplayRefreshRateFB` is called at most once per OpenXR session, only for an offered rate: the
  closest offered rate at or below `gEnhancements.VR.RefreshRate` (72..120, default 120). If none is offered
  at or below it, nothing is requested. It is never retried and never stepped down (a retry cannot take
  effect; Halo's 20 s retries were never granted). Changing the setting mid-session does nothing until the
  next VR start.
- The real display rate is taken from `predictedDisplayPeriod` (`DisplayRateHz`), not from the request or the
  enumerated list. `gdx_vr::RefreshWatch` counts 10 s windows while focused: after two consecutive windows
  under 90% of the display rate it reports once per session (windows restart on a focus change or a change
  of display rate, so only worn time at one rate counts). There is no automatic fallback ladder, because
  it could not take effect.
- The report is a log line saying the game is not holding the display rate and recommending the next lower
  standard per-app rate (`RecommendedPerAppRate`: 120 -> 90, 90 -> 72); at 72 it says there is nothing lower
  and suggests a lower `ResolutionScale`. The decision is pure code in `gdx_vr_system.*`, covered by
  `gdx_vr_system_tests`; `gdx_openxr.cpp` only feeds it frames and logs.
- The simulation does not depend on the rate: the scheduler places frames by `predictedDisplayTime`, and its
  tests cover steady 72, 90, 120, 144 and 45 Hz and mid-stream switches between 90 and 72.

**Wear checklist.** Set F-Zero X to 120 Hz in SteamVR per-app settings (Settings > Video > Per-application
video settings; the menu names are the closest the notes give, not checked on the device). Do it once, in the
headset, then launch the game from Steam. If the log later reports the rate not held, set the rate it
recommends instead.

**Migration.** The old default was `RefreshRate` 90. `gEnhancements.VR.DisplayStandard` is a settings
version: a config with it missing or 0 that has a saved 90 is rewritten to 120 once, then the version is
stored as 1 (`MigrateDisplayStandard`, run by `InitSettings` before the defaults are registered). Anything
else, such as a deliberate 72, is left alone, and a 90 chosen after the migration stays 90.

**Eye image size.** The eye swapchains are created at `round(recommended x ResolutionScale)`, rounded to
even and held to the view configuration's max image rect and the runtime's max swapchain size (`EyeImageSize`).
The Steam Frame recommends 1728x1728, so the default 1.25 gives 2160x2160. The eye render target follows the
swapchain size, so the game renders at the size it submits: nothing is rendered larger and blitted down
(`CopyIntoSwapchain` is a same-size flipped copy). The scale is read when the session is created, so a
change in the menu applies at the next VR start. Lower it if the game cannot hold the refresh rate;
resolution should be given up before anything else.

**Log lines** (look for these on the device): `[vr] eye 0: recommended ... resolution_scale 1.25 -> 2160x2160`
(and eye 1), `eye_size=2160x2160 resolution_scale=1.25` in the `[openxr]` block, `[vr] refresh rate:
requested 120 Hz once (wanted <= 120); offered [...]; runtime reports ...` (or `... none at or below it,
nothing requested`, or `request for ... failed ...; not retried`), `[vr] display refresh rate changed: ...`,
the `period=` in the `[vr] pacing:` line (8.333 ms means 120 Hz), and, if the game falls behind, once:
`[vr] refresh rate: the game is not holding the display rate: N frames/s over two 10 s windows at 120 Hz. The
app cannot change the rate on the Steam Frame; set F-Zero X's SteamVR per-app refresh rate to 90 Hz ...`. A
migrated config logs `[vr] display standard: saved ...RefreshRate=90 moved to 120 Hz (once)`.

### In-headset menu
Hold **Menu + View together for 0.5 s** to open or close the G-Diffuser menu on its **VR** tab (the same
chord closes it; View short press is Back). The decomp's pause menu has no item list to extend (fixed
sprite and string layout inside matching code), so the chord is used, as the standard prescribes for games
whose pause cannot be extended.
- It is the existing full-screen menu with gamepad navigation, shown on the flat quad. While it is visible
  the host presents flat (no stereo race behind it), the controllers drive the menu instead of the game,
  and a race in progress is paused with one synthetic START on open and resumed with another on close (only
  if that START paused it). START is also withheld from the game while View is held, after the chord and
  while the menu is open, so press View first.
- **Laser pointer** (standard section 3): a ray from the right hand's aim pose, hit-tested against the flat
  quad (`RayHitsPanel`, the same pose `FlatQuadPose()` submits), drives ImGui's mouse; ImGui draws its
  cursor at the hit. The trigger clicks (`trigger/value`, down above 0.6, up below 0.45) with a UI-click
  buzz on the right hand; A also clicks while the laser is on the panel; right stick Y scrolls. The laser
  is a 6 mm solid quad layer from the hand to the hit (1.5 m when off the panel), turned toward the head
  and composited over the panel. Right hand only: there is no dominant-hand setting yet.
- Navigation: D-pad (the left stick also acts as a D-pad, for the Index and Touch profiles), A confirm
  (when the laser is off the panel), B or X back out, bumpers switch tabs, View short press closes. The
  menu is fed through `gdx_imgui_nav_set_external()` because the session reads the controllers through
  OpenXR actions, not an SDL gamepad. **Fixed 2026-10-03:** on Linux the SDL2 ImGui backend claims
  `HasGamepad` (under Steam it sees Steam's idle virtual pad), and the nav bridge returned before feeding
  the headset pad, so no menu input worked. While a headset pad is installed the bridge now switches the
  backend to manual mode with no gamepads and is the only gamepad writer.
- The menu's visibility is saved (`gOpenMenuBar`), so quitting with it open reopened it at the next launch.
  A VR session closes it once on its first running frame; the chord opens it.
- VR tab (only on `--vr` launches): horizon lock, level chase pitch, cockpit view; world scale, HUD height,
  HUD aspect (applies on restart); haptics strength, refresh rate; how to recentre and "Reset VR settings
  to defaults"; **Quit to Steam** with a Quit / Cancel confirm. Quit calls `xrRequestExitSession` and the
  host loop leaves when the runtime reaches EXITING (after 3 s regardless).
- Not done yet: comfort turn settings, dominant-hand, HUD mode/distance, per-tab Defaults, gaze/pointer.
  Not verified: the pointer, the nav fix and the launch close are build-verified only (worn: the menu
  opened, but took no input before the fix).

Not verified on a headset (system layer and haptics): the View binding on the Frame profile, that the
Frame profile accepts `/output/haptic`, the hold feel, the buzz, and how the rumble pattern feels.

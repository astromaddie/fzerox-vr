// port/vr/gdx_openxr.cpp — see gdx_openxr.h.
//
// Linux/GLX only for now: the Steam Frame runtime (SteamVR/OpenXR) exposes XR_KHR_opengl_enable
// with the Xlib binding, and libultraship's SDL2 backend creates a GLX context under X11 (see
// docs/VR_ARCHITECTURE.md, M0-A). GLX entry points are resolved through SDL_GL_GetProcAddress so
// the executable gains no new link-time dependency beyond the statically linked OpenXR loader.

#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL

#include "gdx_openxr.h"

// Before any X11 header: Xlib defines `None` as a macro, which breaks libultraship's enums.
#include "fast/interpreter.h" // Fast::Interpreter::SetVrWorldSpriteResolver

#include "../gdx_imgui_nav.h" // GdxImguiNavPad: feeds the ImGui menu from the XR actions
#include "gdx_vr_math.h"
#include "gdx_vr_scheduler.h"

#include <SDL2/SDL.h>
#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../gdx_input_script.h" // GdxInputPad
#include "../port_log.h"
#include "gdx_vr_settings.h" // InitSettings, SettingInt, SettingFloat
#include "gdx_vr_system.h"

#include <sfvr/sfvr_haptics.h>
#include <sfvr/sfvr_perf.h>

#include <initializer_list>
#include <utility>

extern "C" void gdx_interp_mark_cut_src(const char* tag);
// libultraship interpreter.cpp: run after compositing, before the window swap.
extern "C" void (*gfx_pre_present_hook)(void);
extern "C" void CVarSetInteger(const char* name, int32_t value);
extern "C" void CVarSetFloat(const char* name, float value);
extern "C" void CVarSave();
// decomp globals (decomp/include/fzx_game.h).
extern "C" int gGameMode;
extern "C" int gNumPlayers;
// port/decomp_port.c: Race_Draw emits the HUD split marker only while this is set.
extern "C" int gGdxVrHudMarkerEnabled;
// port/decomp_port.c: distance-only culling for track chunks and machines while set.
extern "C" int gGdxVrWideCull;
// port/decomp_port.c: camera 0's track up (the game's smoothed gravity-up for the followed racer).
extern "C" void gdx_vr_camera_track_up(float out[3]);
// port/decomp_port.c: bumped every time Race_Draw runs.
extern "C" unsigned int gGdxVrRaceDrawCount;
// port/decomp_port.c: world anchors of sprites Racer_Draw places over machines.
extern "C" int gdx_vr_world_sprite_lookup(int id, float out[8]);
// libultraship interpreter.cpp: VR sprites drawn by the last replay.
extern "C" int gGdxVrWorldSpritesLastRun;
extern "C" int gGdxVrHudSpritesLastRun;
// port/gdx_menu.cpp: the full-screen G-Diffuser menu (libultraship Gui menu).
bool GdxMenuIsVisible();
bool GdxMenuShow(bool show, const char* section);
// decomp/src/game/game.c: nonzero while the game is paused.
extern "C" signed char gGamePaused;
// port/gdx_perf.cpp: since-start totals of the gametick phase and its xlate/run/mirror seams (ms).
// They advance only while GDX_PERF telemetry is on.
extern "C" void gdx_perf_lifetime_ms(double* gametick, double* xlate, double* run, double* mirror);
// port/input_bridge.c: the motor level the game last commanded per port (1 = rumbling).
extern "C" int gGdxVrMotorOn[4];
// port/input_bridge.c: merges headset-controller input into pad 1 while set.
extern "C" void (*gGdxVrInputMerge)(GdxInputPad* pad);

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

namespace gdx_vr {
namespace {

// Flat-mode quad placement in the LOCAL reference space (metres): ahead of the viewer at eye height.
constexpr float kQuadDistance = 2.0f;
constexpr float kQuadWidth = 2.4f;
constexpr float kBeamWidth = 0.006f;    // metres: the menu pointer's laser
constexpr float kBeamMissLength = 1.5f; // metres: laser length when it is off the menu
// Race HUD: head-locked in VIEW space 1.5 m ahead, gEnhancements.VR.HudHeight metres tall (default
// 1.2, about 44 degrees) at gEnhancements.VR.HudAspect (default 1.5:1, about 62 degrees wide). The
// aspect is read when the session starts (it sizes the swapchain); the height every frame.
constexpr int32_t kHudHeight = 960;
constexpr float kHudDistance = 1.5f;

// decomp/include/fzx_game.h GAMEMODE_* values that show a race view.
constexpr int kModeGpRace = 0x01;
constexpr int kModePractice = 0x02;
constexpr int kModeTimeAttack = 0x0E;
constexpr int kModeDeathRace = 0x15;

typedef void (*PFN_GenFramebuffers)(GLsizei, GLuint*);
typedef void (*PFN_DeleteFramebuffers)(GLsizei, const GLuint*);
typedef void (*PFN_BindFramebuffer)(GLenum, GLuint);
typedef void (*PFN_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (*PFN_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef Display* (*PFN_glXGetCurrentDisplay)(void);
typedef GLXContext (*PFN_glXGetCurrentContext)(void);
typedef GLXDrawable (*PFN_glXGetCurrentDrawable)(void);
typedef int (*PFN_glXQueryContext)(Display*, GLXContext, int, int*);
typedef GLXFBConfig* (*PFN_glXChooseFBConfig)(Display*, int, const int*, int*);

struct GlFns {
    PFN_GenFramebuffers genFramebuffers = nullptr;
    PFN_DeleteFramebuffers deleteFramebuffers = nullptr;
    PFN_BindFramebuffer bindFramebuffer = nullptr;
    PFN_FramebufferTexture2D framebufferTexture2D = nullptr;
    PFN_BlitFramebuffer blitFramebuffer = nullptr;
};

enum class Phase {
    Off,     // VR not requested, or permanently failed
    Uninit,  // requested; instance/session not created yet
    Idle,    // session exists, not running (IDLE / waiting for READY)
    Running, // between xrBeginSession and xrEndSession
    Exiting, // runtime asked us to leave
};

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLKHR> images;
    int32_t width = 0;
    int32_t height = 0;
    bool hasImage = false; // at least one image released, so a layer may reference it
};

// A waited XR frame and the simulation ticks it still needs before it can be shown.
struct PendingFrame {
    bool waited = false;
    XrFrameState state{ XR_TYPE_FRAME_STATE };
    int ticksOwed = 0;
};

struct ActiveEye {
    bool active = false;
    Pose pose{};
    Fov fov{};
    bool worldValid = false; // set when BuildActiveEyeProjView anchors this eye
    Vec3 worldPos{};
    Vec3 worldForward{};
    Vec3 worldUp{};
};

// Headset controllers read as OpenXR actions. Used when SteamVR routes the controllers to the
// XR session instead of to Steam Input's virtual gamepad; merged into pad 1 alongside SDL input.
struct XrInput {
    bool ready = false;
    XrActionSet set = XR_NULL_HANDLE;
    XrAction stick = XR_NULL_HANDLE;  // left thumbstick -> analog stick
    XrAction cstick = XR_NULL_HANDLE; // right thumbstick -> C buttons
    XrAction a = XR_NULL_HANDLE;
    XrAction b = XR_NULL_HANDLE;
    XrAction z = XR_NULL_HANDLE;      // left trigger
    XrAction r = XR_NULL_HANDLE;      // right trigger
    XrAction l = XR_NULL_HANDLE;      // left grip
    XrAction start = XR_NULL_HANDLE;
    // System layer (docs/VR_ARCHITECTURE.md): the left View button. Short press = N64 B outside
    // races, hold 1 s = recentre, hold 3 s = recentre + height. Stick clicks stay unbound.
    XrAction sysView = XR_NULL_HANDLE;
    gdx_vr::ViewButton viewBtn;
    double viewBPulseUntil = 0.0; // seconds; N64 B is held until then after a short press
    // Haptics (standard.md section 6): everything submitted during a host iteration coalesces here
    // and is flushed once per XR frame (FlushHaptics), scaled by gEnhancements.VR.Haptics.
    sfvr_haptic_queue haptics{};
    // In-headset menu chord (Menu + View held together 0.5 s). While View is held, after the chord,
    // or while the menu is open, START is not sent to the game.
    gdx_vr::Chord chord{ 0.5 };
    double startPulseUntil = 0.0; // a synthetic START (pausing / unpausing the game around the menu)
    double backPulseUntil = 0.0;  // View short press while the menu is open: ImGui Back (closes it)
    bool stickDir[4] = { false, false, false, false }; // left stick as D-pad for menu nav: U D L R
    XrAction hapticLeft = XR_NULL_HANDLE;  // vibration output, left hand
    XrAction hapticRight = XR_NULL_HANDLE; // vibration output, right hand
    XrAction view = XR_NULL_HANDLE; // C-right: F-Zero X cycles the race camera view on it
    XrAction cameraMode = XR_NULL_HANDLE; // toggles chase / cockpit
    bool cameraModeHeld = false;
    XrAction dUp = XR_NULL_HANDLE;
    XrAction dDown = XR_NULL_HANDLE;
    XrAction dLeft = XR_NULL_HANDLE;
    XrAction dRight = XR_NULL_HANDLE;
    // Menu pointer (standard.md section 3): the right hand's aim pose and trigger.
    XrAction aimPose = XR_NULL_HANDLE;
    XrAction pointerTrigger = XR_NULL_HANDLE;
    XrSpace aimSpace = XR_NULL_HANDLE;
    bool triggerDown = false;   // TriggerClick hysteresis state
    bool pointerDown = false;   // left mouse button as last sent
    bool pointerOnPanel = false;
    float pointerU = 0.0f;
    float pointerV = 0.0f;
    bool beamValid = false;     // beamPose / beamLength describe this frame's laser
    Pose beamPose{};
    float beamLength = 0.0f;
    GdxInputPad pad{};
    bool any = false;                 // pad holds input from this tick's sync
};

struct State {
    bool configured = false;
    bool requested = false;
    Phase phase = Phase::Off;
    bool exitRequested = false;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    int64_t colorFormat = 0;
    Swapchain quad;
    Swapchain eyes[2];
    Swapchain hud;
    Swapchain beam; // solid-colour texture for the menu pointer's laser quad
    long long hudRenderedTick = -1;
    XrTime lastDisplayTime = 0; // the latest xrWaitFrame prediction, for locating controller poses

    GlFns gl;
    GLuint drawFbo = 0;
    GLuint readFbo = 0;

    Scheduler scheduler;
    PendingFrame pending;

    bool stereoThisTick = false;
    bool lastTickStereo = false;
    long long hostTick = 0;
    long long countedHostTick = -1;
    ActiveEye activeEye;

    XrInput input;
    bool frameControllerExt = false; // XR_VALVE_frame_controller_interaction enabled

    // Display refresh rate (XR_FB_display_refresh_rate, when the runtime advertises it).
    PFN_xrEnumerateDisplayRefreshRatesFB enumerateRates = nullptr;
    PFN_xrRequestDisplayRefreshRateFB requestRate = nullptr;
    PFN_xrGetDisplayRefreshRateFB getRate = nullptr;
    int refreshWanted = 0;       // gEnhancements.VR.RefreshRate as last applied; 0 = not applied yet
    float refreshTarget = 0.0f;  // rate actually requested from the runtime, Hz (0 = none)
    int refreshMisses = 0;       // consecutive 10 s windows below 90% of the target
    double refreshWindowStartS = 0.0;
    long long refreshWindowFrames = 0;
    bool refreshWindowFocused = false; // focused for the whole window so far
    bool cockpit = false;            // cockpit view instead of the chase camera
    bool focused = false;

    // In-headset VR menu: the G-Diffuser menu on the flat quad (docs/VR_ARCHITECTURE.md). Presentation
    // is forced flat while it is visible; game input is withheld; XR actions drive its navigation.
    bool menuOpen = false;
    bool menuPausedGame = false; // we sent START to pause a race when the menu opened
    bool menuLaunchChecked = false; // the menu's saved-open state was cleared for this VR launch
    int cameraModeSeen = -1;     // gEnhancements.VR.CameraMode as last applied to `cockpit`
    double quitRequestedS = 0.0; // Quit to Steam: xrRequestExitSession time, 0 = not requested

    // App-level recentre: the head's yaw and position captured on request; rendered eye poses and
    // the flat quad are expressed relative to it, so "forward" is wherever the viewer faced then.
    // The poses submitted to the compositor stay in raw LOCAL space.
    //
    // Level recentres (View hold 1 s, camera toggle, flat/stereo switch) take yaw and horizontal
    // position and keep `heightAnchor`; Height recentres (View hold 3 s, focus gain, a runtime
    // reference-space change, and the very first one) replace it with the head's current height.
    gdx_vr::RecentreKind recenterPending = gdx_vr::RecentreKind::Height;
    bool recenterValid = false;
    Quat recenterYaw{ 0.0f, 0.0f, 0.0f, 1.0f };
    Vec3 recenterPos{ 0.0f, 0.0f, 0.0f }; // head position at the last recentre of either kind
    float heightAnchor = 0.0f;            // LOCAL y of the game camera and of the flat menu quad

    // Stereo only while Race_Draw is really drawing (see gGdxVrRaceDrawCount).
    unsigned int raceDrawCountSeen = 0;
    bool raceDrawnLastTick = false;
    bool tickRanSinceDecision = false;

    // Race HUD alpha recovery: the HUD is rendered over black and over white and combined.
    GLuint hudTex[2] = { 0, 0 };
    GLuint hudProgram = 0;
    GLuint hudVao = 0;
    GLint hudLocBlack = -1;
    GLint hudLocWhite = -1;
    bool hudCombinerFailed = false;

    long long xrFrames = 0;
    long long xrFramesNoTick = 0;
    long long stereoFrames = 0;
    long long rebases = 0;
    double copyMsSum = 0.0;

    // [vr-perf] (sfvr_perf, standard.md section 8). Stages: logic (game tick minus bridge work and
    // the time spent inside the stereo present), xlate (display-list translation), irun (the eye
    // replays), hud (the HUD pass). Per-frame values are what accrued since the previous XR frame.
    sfvr_perf perf{};
    bool perfReady = false;
    double perfEyeMs[2] = { 0.0, 0.0 };
    double perfIrunMs = 0.0;
    double perfHudMs = 0.0;
    double perfPresentMs = 0.0; // whole PresentStereoTick calls, which include xrWaitFrame blocking
    double perfPrevLife[4] = { 0.0, 0.0, 0.0, 0.0 };
    XrTime perfPrevDisplay = 0;
};

State& S() {
    static State s;
    return s;
}

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Ask for a recentre at the next stereo frame boundary (UpdateRecenter, from xrWaitFrame). Requests
// that land before then merge into the stronger one.
void RequestRecentre(gdx_vr::RecentreKind kind, const char* why) {
    State& s = S();
    s.recenterPending = gdx_vr::MergeRecentre(s.recenterPending, kind);
    gdx_port_logf("[vr] recentre requested (%s, %s)\n", why,
                  kind == gdx_vr::RecentreKind::Height ? "yaw+position+height" : "yaw+position");
}

// Diagnostics for unattended runs (no headset on): GDX_VR_FORCE_RENDER=1 renders eyes even when
// the runtime reports shouldRender = false (headset in standby), and GDX_VR_DUMP_FRAME=<n> writes
// both eye images of the n-th stereo frame to vr-eye<e>-<n>.bmp in the working directory.
bool EnvFlag(const char* name) {
    const char* e = std::getenv(name);
    return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
}

bool ForceRender() {
    static const bool on = [] {
        const bool v = EnvFlag("GDX_VR_FORCE_RENDER");
        gdx_port_logf("[openxr] GDX_VR_FORCE_RENDER=%d\n", v ? 1 : 0);
        return v;
    }();
    return on;
}

// GDX_VR_DUMP_ON_SPRITE=1: dump the eyes of the first stereo frame in which a world-anchored sprite
// was placed, and the HUD of the first tick in which a HUD-bracketed sprite was drawn.
bool DumpOnSprite() {
    static const bool on = [] {
        const bool v = EnvFlag("GDX_VR_DUMP_ON_SPRITE");
        gdx_port_logf("[openxr] GDX_VR_DUMP_ON_SPRITE=%d\n", v ? 1 : 0);
        return v;
    }();
    return on;
}

long long DumpFrame() {
    static const long long n = [] {
        const char* e = std::getenv("GDX_VR_DUMP_FRAME");
        const long long v = (e != nullptr && e[0] != '\0') ? std::strtoll(e, nullptr, 10) : -1;
        gdx_port_logf("[openxr] GDX_VR_DUMP_FRAME=%lld\n", v);
        return v;
    }();
    return n;
}

// GDX_VR_DUMP_COUNT=<k>: with GDX_VR_DUMP_FRAME, dump k consecutive stereo frames (left eye only when
// k > 1), to see what changes from one frame to the next (popping, flicker).
long long DumpCount() {
    static const long long n = [] {
        const char* e = std::getenv("GDX_VR_DUMP_COUNT");
        const long long v = (e != nullptr && e[0] != '\0') ? std::strtoll(e, nullptr, 10) : 1;
        return v < 1 ? 1 : v;
    }();
    return n;
}

typedef void (*PFN_GetTexImage)(GLenum, GLint, GLenum, GLenum, void*);

// Write a top-down RGBA8 texture as a bottom-up 24-bit BMP.
void DumpTexture(GLuint tex, int w, int h, const char* path) {
    static PFN_GetTexImage getTexImage = reinterpret_cast<PFN_GetTexImage>(SDL_GL_GetProcAddress("glGetTexImage"));
    if (getTexImage == nullptr || tex == 0 || w <= 0 || h <= 0) {
        return;
    }
    std::vector<unsigned char> rgba((size_t)w * (size_t)h * 4u);
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, tex);
    getTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        return;
    }
    const int rowBytes = ((w * 3) + 3) & ~3;
    const unsigned fileSize = 54u + (unsigned)rowBytes * (unsigned)h;
    unsigned char hdr[54] = { 'B', 'M' };
    auto put32 = [&](int at, unsigned v) {
        hdr[at] = v & 0xFF;
        hdr[at + 1] = (v >> 8) & 0xFF;
        hdr[at + 2] = (v >> 16) & 0xFF;
        hdr[at + 3] = (v >> 24) & 0xFF;
    };
    put32(2, fileSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, (unsigned)w);
    put32(22, (unsigned)h);
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, (unsigned)rowBytes * (unsigned)h);
    std::fwrite(hdr, 1, sizeof(hdr), f);
    std::vector<unsigned char> row((size_t)rowBytes, 0);
    // The texture is stored top-down; BMP rows run bottom-up.
    for (int y = h - 1; y >= 0; y--) {
        const unsigned char* src = &rgba[(size_t)y * (size_t)w * 4u];
        for (int x = 0; x < w; x++) {
            row[(size_t)x * 3 + 0] = src[x * 4 + 2];
            row[(size_t)x * 3 + 1] = src[x * 4 + 1];
            row[(size_t)x * 3 + 2] = src[x * 4 + 0];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    size_t transparent = 0;
    for (size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] == 0) {
            transparent++;
        }
    }
    gdx_port_logf("[openxr] wrote %s (%dx%d, %.1f%% fully transparent)\n", path, w, h,
                  100.0 * (double)transparent / ((double)w * (double)h));
}

bool Check(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) {
        return true;
    }
    char text[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (S().instance != XR_NULL_HANDLE) {
        xrResultToString(S().instance, r, text);
    }
    gdx_port_logf("[openxr] %s failed: %d (%s)\n", what, (int)r, text);
    return false;
}

const char* SessionStateName(XrSessionState st) {
    switch (st) {
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNKNOWN";
    }
}

template <typename T> bool LoadGl(T& fn, const char* name) {
    fn = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    if (fn == nullptr) {
        gdx_port_logf("[openxr] GL entry point %s unavailable\n", name);
        return false;
    }
    return true;
}

bool CreateSwapchain(Swapchain& sc, int32_t width, int32_t height, const char* what) {
    State& s = S();
    XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = s.colorFormat;
    ci.sampleCount = 1;
    ci.width = (uint32_t)width;
    ci.height = (uint32_t)height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!Check(xrCreateSwapchain(s.session, &ci, &sc.handle), what)) {
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(sc.handle, 0, &count, nullptr);
    sc.images.assign(count, XrSwapchainImageOpenGLKHR{ XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
    if (!Check(xrEnumerateSwapchainImages(sc.handle, count, &count,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data())),
               "xrEnumerateSwapchainImages")) {
        return false;
    }
    sc.width = width;
    sc.height = height;
    sc.hasImage = false;
    return true;
}

void DestroySwapchain(Swapchain& sc) {
    if (sc.handle != XR_NULL_HANDLE) {
        xrDestroySwapchain(sc.handle);
    }
    sc = Swapchain{};
}

void DestroySession() {
    State& s = S();
    gfx_pre_present_hook = nullptr;
    gGdxVrInputMerge = nullptr;
    Fast::Interpreter::SetVrWorldSpriteResolver(nullptr);
    if (s.input.aimSpace != XR_NULL_HANDLE) {
        xrDestroySpace(s.input.aimSpace);
    }
    if (s.input.set != XR_NULL_HANDLE) {
        xrDestroyActionSet(s.input.set);
    }
    s.input = XrInput{};
    if (s.gl.deleteFramebuffers != nullptr) {
        if (s.drawFbo != 0) {
            s.gl.deleteFramebuffers(1, &s.drawFbo);
        }
        if (s.readFbo != 0) {
            s.gl.deleteFramebuffers(1, &s.readFbo);
        }
    }
    s.drawFbo = 0;
    s.readFbo = 0;
    if (s.hudTex[0] != 0) {
        glDeleteTextures(2, s.hudTex);
        s.hudTex[0] = s.hudTex[1] = 0;
    }
    s.hudProgram = 0; // the program and VAO die with the GL context at shutdown
    DestroySwapchain(s.quad);
    DestroySwapchain(s.eyes[0]);
    DestroySwapchain(s.eyes[1]);
    DestroySwapchain(s.hud);
    DestroySwapchain(s.beam);
    if (s.localSpace != XR_NULL_HANDLE) {
        xrDestroySpace(s.localSpace);
        s.localSpace = XR_NULL_HANDLE;
    }
    if (s.viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(s.viewSpace);
        s.viewSpace = XR_NULL_HANDLE;
    }
    if (s.session != XR_NULL_HANDLE) {
        xrDestroySession(s.session);
        s.session = XR_NULL_HANDLE;
    }
    s.pending = PendingFrame{};
}

void PrePresentHook();

XrPath ToPath(const char* str) {
    XrPath path = XR_NULL_PATH;
    xrStringToPath(S().instance, str, &path);
    return path;
}

bool CreateAction(XrActionType type, const char* name, const char* label, XrAction* out) {
    XrActionCreateInfo ci{ XR_TYPE_ACTION_CREATE_INFO };
    ci.actionType = type;
    std::strncpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(ci.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    return Check(xrCreateAction(S().input.set, &ci, out), name);
}

// Suggest bindings for one interaction profile. A profile the runtime rejects is logged and
// skipped: SteamVR remaps suggested profiles onto the controllers actually present.
void SuggestBindings(const char* profile, std::initializer_list<std::pair<XrAction, const char*>> bindings) {
    std::vector<XrActionSuggestedBinding> list;
    for (const auto& b : bindings) {
        list.push_back({ b.first, ToPath(b.second) });
    }
    XrInteractionProfileSuggestedBinding sb{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    sb.interactionProfile = ToPath(profile);
    sb.countSuggestedBindings = (uint32_t)list.size();
    sb.suggestedBindings = list.data();
    const XrResult r = xrSuggestInteractionProfileBindings(S().instance, &sb);
    gdx_port_logf("[openxr] input bindings %s: %s\n", profile, XR_SUCCEEDED(r) ? "suggested" : "rejected");
}

void MergeXrInput(GdxInputPad* pad);

bool CreateInput() {
    State& s = S();
    XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO };
    std::strncpy(asci.actionSetName, "racing", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(asci.localizedActionSetName, "Racing", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (!Check(xrCreateActionSet(s.instance, &asci, &s.input.set), "xrCreateActionSet")) {
        return false;
    }
    XrInput& in = s.input;
    if (!CreateAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "steer", "Steer", &in.stick) ||
        !CreateAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "c_buttons", "C buttons", &in.cstick) ||
        !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "A (accelerate)", &in.a) ||
        !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "B", &in.b) ||
        !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_z", "Z (lean left)", &in.z) ||
        !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_r", "R (lean right)", &in.r) ||
        !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_l", "L", &in.l) ||
        !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_start", "Start", &in.start) ||
        !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "system_view", "System: Back / hold to recentre", &in.sysView) ||
        !CreateAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic_left", "Haptics (left)", &in.hapticLeft) ||
        !CreateAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic_right", "Haptics (right)", &in.hapticRight) ||
        !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "change_view", "Change view (C-right)", &in.view) ||
        !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "camera_mode", "Chase / cockpit view", &in.cameraMode) ||
        !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_up", "D-pad up", &in.dUp) ||
        !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_down", "D-pad down", &in.dDown) ||
        !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_left", "D-pad left", &in.dLeft) ||
        !CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_right", "D-pad right", &in.dRight) ||
        !CreateAction(XR_ACTION_TYPE_POSE_INPUT, "menu_pointer", "Menu pointer", &in.aimPose) ||
        !CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "menu_select", "Menu select", &in.pointerTrigger)) {
        return false;
    }
    if (s.frameControllerExt) {
        // Steam Frame controllers: a split gamepad. Right: A/B/X/Y, menu (Start), bumper,
        // trigger, grip. Left: D-pad (in place of face buttons), view, bumper, trigger, grip.
        // Paths per Steamworks "Steam Frame Input".
        SuggestBindings("/interaction_profiles/valve/frame_controller_valve",
                        { { in.stick, "/user/hand/left/input/thumbstick" },
                          { in.cstick, "/user/hand/right/input/thumbstick" },
                          { in.a, "/user/hand/right/input/a/click" },
                          // X, not B: N64 B is boost, and X is the easier reach next to A.
                          { in.b, "/user/hand/right/input/x/click" },
                          { in.view, "/user/hand/right/input/y/click" },
                          { in.cameraMode, "/user/hand/right/input/b/click" },
                          { in.z, "/user/hand/left/input/trigger/click" },
                          { in.r, "/user/hand/right/input/trigger/click" },
                          { in.l, "/user/hand/left/input/bumper/click" },
                          { in.r, "/user/hand/right/input/bumper/click" },
                          { in.start, "/user/hand/right/input/menu/click" },
                          { in.sysView, "/user/hand/left/input/view/click" },
                          { in.hapticLeft, "/user/hand/left/output/haptic" },
                          { in.hapticRight, "/user/hand/right/output/haptic" },
                          { in.dUp, "/user/hand/left/input/dpad_up/click" },
                          { in.dDown, "/user/hand/left/input/dpad_down/click" },
                          { in.dLeft, "/user/hand/left/input/dpad_left/click" },
                          { in.dRight, "/user/hand/left/input/dpad_right/click" },
                          // Paths as GoldenEye VR binds them on the Frame (worn there).
                          { in.aimPose, "/user/hand/right/input/aim/pose" },
                          { in.pointerTrigger, "/user/hand/right/input/trigger/value" } });
    }
    SuggestBindings("/interaction_profiles/valve/index_controller",
                    { { in.stick, "/user/hand/left/input/thumbstick" },
                      { in.cstick, "/user/hand/right/input/thumbstick" },
                      { in.a, "/user/hand/right/input/a/click" },
                      { in.b, "/user/hand/right/input/b/click" },
                      { in.z, "/user/hand/left/input/trigger/click" },
                      { in.r, "/user/hand/right/input/trigger/click" },
                      { in.l, "/user/hand/left/input/squeeze/value" },
                      { in.start, "/user/hand/left/input/b/click" },
                      // Index has no View button: the free left A button stands in for it.
                      { in.sysView, "/user/hand/left/input/a/click" },
                      { in.hapticLeft, "/user/hand/left/output/haptic" },
                      { in.hapticRight, "/user/hand/right/output/haptic" },
                      { in.aimPose, "/user/hand/right/input/aim/pose" },
                      { in.pointerTrigger, "/user/hand/right/input/trigger/value" } });
    SuggestBindings("/interaction_profiles/oculus/touch_controller",
                    { { in.stick, "/user/hand/left/input/thumbstick" },
                      { in.cstick, "/user/hand/right/input/thumbstick" },
                      { in.a, "/user/hand/right/input/a/click" },
                      { in.b, "/user/hand/right/input/b/click" },
                      { in.z, "/user/hand/left/input/trigger/value" },
                      { in.r, "/user/hand/right/input/trigger/value" },
                      { in.l, "/user/hand/left/input/squeeze/value" },
                      { in.start, "/user/hand/left/input/menu/click" },
                      // Touch has no View button: the free left Y button stands in for it.
                      { in.sysView, "/user/hand/left/input/y/click" },
                      { in.hapticLeft, "/user/hand/left/output/haptic" },
                      { in.hapticRight, "/user/hand/right/output/haptic" },
                      { in.aimPose, "/user/hand/right/input/aim/pose" },
                      { in.pointerTrigger, "/user/hand/right/input/trigger/value" } });
    XrSessionActionSetsAttachInfo attach{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &s.input.set;
    if (!Check(xrAttachSessionActionSets(s.session, &attach), "xrAttachSessionActionSets")) {
        return false;
    }
    XrActionSpaceCreateInfo asi{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
    asi.action = in.aimPose;
    asi.poseInActionSpace.orientation.w = 1.0f;
    if (!Check(xrCreateActionSpace(s.session, &asi, &in.aimSpace), "xrCreateActionSpace(menu pointer)")) {
        in.aimSpace = XR_NULL_HANDLE; // no pointer; pad navigation still works
    }
    in.ready = true;
    gGdxVrInputMerge = &MergeXrInput;
    return true;
}

void ApplyHaptic(int hand, float amplitude, float seconds, void*) {
    State& s = S();
    XrHapticActionInfo hai{ XR_TYPE_HAPTIC_ACTION_INFO };
    hai.action = hand == SFVR_HAND_LEFT ? s.input.hapticLeft : s.input.hapticRight;
    XrHapticVibration vib{ XR_TYPE_HAPTIC_VIBRATION };
    vib.amplitude = amplitude;
    vib.duration = (XrDuration)std::lround((double)seconds * 1e9);
    vib.frequency = XR_FREQUENCY_UNSPECIFIED;
    const XrResult r = xrApplyHapticFeedback(s.session, &hai, reinterpret_cast<const XrHapticBaseHeader*>(&vib));
    if (XR_FAILED(r)) {
        gdx_port_logf("[vr] haptic failed (%d)\n", (int)r);
    }
}

// Play what was submitted since the last XR frame, once. Called from BeginPending, which every
// presentation path runs exactly once per XR frame. Needs a focused session whose action set has
// been synced (SyncInput); otherwise whatever is queued is dropped, so a buzz never plays late.
void FlushHaptics() {
    State& s = S();
    if (!s.input.ready || !s.focused) {
        sfvr_haptic_queue_clear(&s.input.haptics);
        return;
    }
    sfvr_haptic_flush(&s.input.haptics, SettingFloat(kSetHaptics), &ApplyHaptic, nullptr);
}

void FillSwapchain(Swapchain& sc, float r, float g, float b, float a);
Pose FlatQuadPose();
float FlatQuadHeight();

// Track the G-Diffuser menu's visibility (it can also be closed with Back or F1). Around a race the
// game is paused for as long as the menu is up: opening sends the game one START (only when a race
// is on screen and not already paused), closing sends another if that START did pause it.
void SyncMenuState(const char* why) {
    State& s = S();
    const bool visible = GdxMenuIsVisible();
    if (visible == s.menuOpen) {
        return;
    }
    const double nowSec = NowMs() * 0.001;
    s.menuOpen = visible;
    if (visible) {
        s.menuPausedGame = s.raceDrawnLastTick && gGamePaused == 0;
        if (s.menuPausedGame) {
            s.input.startPulseUntil = nowSec + 0.1;
        }
    } else {
        if (s.menuPausedGame && gGamePaused != 0) {
            s.input.startPulseUntil = nowSec + 0.1;
        }
        s.menuPausedGame = false;
    }
    gdx_port_logf("[vr] menu %s (%s)\n", visible ? "opened" : "closed", why);
}

// Show or hide the in-headset menu (the G-Diffuser menu, opened on its VR page).
void SetMenuOpen(bool open, const char* why) {
    if (!GdxMenuShow(open, "VR")) {
        gdx_port_logf("[vr] menu: not available (%s)\n", why);
        return;
    }
    SyncMenuState(why);
}

void SyncInput() {
    State& s = S();
    XrInput& in = s.input;
    in.any = false;
    if (!in.ready || !s.focused) {
        in.viewBtn.Reset(); // a press in progress when focus is lost must not fire later
        sfvr_haptic_queue_clear(&in.haptics);
        in.chord.Reset();
        gdx_imgui_nav_set_external(nullptr);
        return;
    }
    XrActiveActionSet active{ in.set, XR_NULL_PATH };
    XrActionsSyncInfo si{ XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(s.session, &si))) {
        return;
    }
    auto boolean = [&](XrAction action) {
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = action;
        XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN };
        return XR_SUCCEEDED(xrGetActionStateBoolean(s.session, &gi, &st)) && st.isActive && st.currentState;
    };
    auto vec2 = [&](XrAction action, float* x, float* y) {
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = action;
        XrActionStateVector2f st{ XR_TYPE_ACTION_STATE_VECTOR2F };
        *x = 0.0f;
        *y = 0.0f;
        if (XR_SUCCEEDED(xrGetActionStateVector2f(s.session, &gi, &st)) && st.isActive) {
            *x = st.currentState.x;
            *y = st.currentState.y;
        }
    };
    auto value = [&](XrAction action) {
        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = action;
        XrActionStateFloat st{ XR_TYPE_ACTION_STATE_FLOAT };
        return (XR_SUCCEEDED(xrGetActionStateFloat(s.session, &gi, &st)) && st.isActive) ? st.currentState : 0.0f;
    };
    // N64 OSContPad bits (decomp/include/PR/os_cont.h).
    unsigned short buttons = 0;
    if (boolean(in.a)) buttons |= 0x8000;     // A
    if (boolean(in.b)) buttons |= 0x4000;     // B
    if (boolean(in.z)) buttons |= 0x2000;     // Z
    if (boolean(in.l)) buttons |= 0x0020;     // L
    if (boolean(in.r)) buttons |= 0x0010;     // R
    if (boolean(in.dUp)) buttons |= 0x0800;   // D-pad up
    if (boolean(in.dDown)) buttons |= 0x0400; // D-pad down
    if (boolean(in.dLeft)) buttons |= 0x0200; // D-pad left
    if (boolean(in.dRight)) buttons |= 0x0100; // D-pad right
    // System layer: left View. B is only sent for a short press outside races, so it is Back in the
    // flat menus and never doubles as boost in a race (any race on screen, stereo or not).
    const double nowSec = NowMs() * 0.001;
    const bool startDown = boolean(in.start);
    const bool viewDown = boolean(in.sysView);

    // Menu chord: Menu + View held together 0.5 s toggles the in-headset menu (the pause menu cannot
    // be extended cheaply in the decomp, so this is the standard's fallback for such games). Firing
    // cancels the View press so its release is neither a short press nor a recentre hold.
    if (in.chord.Update(startDown, viewDown, nowSec)) {
        in.viewBtn.Reset();
        sfvr_haptic_event(&in.haptics, SFVR_HAND_BOTH, SFVR_HAPTIC_UI_CLICK);
        SetMenuOpen(!s.menuOpen, "Menu+View chord");
    }
    // START belongs to the game only when it is not part of a chord attempt or the menu: otherwise
    // View-then-Menu would pause the race, and START would act on the game behind the open menu.
    if (startDown && !viewDown && !in.chord.Latched() && !s.menuOpen) {
        buttons |= 0x1000; // START
    }
    if (nowSec < in.startPulseUntil) {
        buttons |= 0x1000;
    }
    const gdx_vr::ViewEvents ve = in.viewBtn.Update(viewDown, nowSec);
    if (ve.recentre) {
        RequestRecentre(gdx_vr::RecentreKind::Level, "View hold 1 s");
        sfvr_haptic_event(&in.haptics, SFVR_HAND_BOTH, SFVR_HAPTIC_SYSTEM);
    }
    if (ve.recentreHeight) {
        RequestRecentre(gdx_vr::RecentreKind::Height, "View hold 3 s");
        sfvr_haptic_event(&in.haptics, SFVR_HAND_BOTH, SFVR_HAPTIC_SYSTEM);
    }
    if (ve.shortPress && s.menuOpen) {
        in.backPulseUntil = nowSec + 0.1; // ImGui Back: closes the menu (libultraship toggles on it)
    } else if (ve.shortPress && !s.lastTickStereo && !s.raceDrawnLastTick) {
        // The game polls pads once per tick and a tap can end between ticks: hold B for 100 ms.
        in.viewBPulseUntil = nowSec + 0.1;
    }
    if (nowSec < in.viewBPulseUntil && !s.menuOpen) {
        buttons |= 0x4000; // B
    }
    // N64 rumble: the game switches the motor on and off on its own duty cycle; while it is on,
    // submit a rumble pulse every host iteration (40 ms each, so consecutive "on" ticks join up
    // and the game's off gaps stay as gaps). Both hands: the pak has no handedness.
    if (gGdxVrMotorOn[0] != 0) {
        sfvr_haptic_rumble(&in.haptics, SFVR_HAND_BOTH, 0xFFFFu, 0xFFFFu);
    }
    const bool cameraModeNow = boolean(in.cameraMode);
    if (cameraModeNow && !in.cameraModeHeld && !s.menuOpen) {
        s.cockpit = !s.cockpit;
        RequestRecentre(gdx_vr::RecentreKind::Level, "camera toggle");
        gdx_interp_mark_cut_src("vr-camera-mode");
        gdx_port_logf("[openxr] camera -> %s\n", s.cockpit ? "cockpit" : "chase");
        // The chosen view is the next launch's starting view.
        CVarSetInteger(Setting(kSetCameraMode).cvar, s.cockpit ? 1 : 0);
        s.cameraModeSeen = s.cockpit ? 1 : 0;
        CVarSave();
    }
    in.cameraModeHeld = cameraModeNow;
    float cx, cy;
    vec2(in.cstick, &cx, &cy);
    if (cy > 0.5f) buttons |= 0x0008;  // C-up
    if (cy < -0.5f) buttons |= 0x0004; // C-down
    if (cx < -0.5f) buttons |= 0x0002; // C-left
    if (cx > 0.5f || boolean(in.view)) buttons |= 0x0001; // C-right (also the change-view button)
    float sx, sy;
    vec2(in.stick, &sx, &sy);
    auto toStick = [](float v) {
        float n = v * 80.0f;
        if (n > 80.0f) n = 80.0f;
        if (n < -80.0f) n = -80.0f;
        return (signed char)std::lround(n);
    };
    if (s.menuOpen) {
        // The menu has the controllers: nothing but the synthetic pause/unpause START reaches the game.
        buttons &= 0x1000;
        GdxImguiNavPad nav{};
        // The left stick doubles as a D-pad (hysteresis 0.55 on / 0.35 off): the Index and Touch
        // profiles have no D-pad, and ImGui spends the analog stick on scrolling, not selection.
        const float dirValue[4] = { sy, -sy, -sx, sx }; // up, down, left, right
        for (int d = 0; d < 4; d++) {
            in.stickDir[d] = gdx_vr::StickAsDpad(in.stickDir[d], dirValue[d]);
        }
        nav.up = boolean(in.dUp) || in.stickDir[0];
        nav.down = boolean(in.dDown) || in.stickDir[1];
        nav.left = boolean(in.dLeft) || in.stickDir[2];
        nav.right = boolean(in.dRight) || in.stickDir[3];
        // Laser pointer (standard.md section 3): the right hand's aim ray on the flat menu panel,
        // fed to ImGui as its mouse. Trigger clicks with hysteresis, A also clicks while the laser
        // is on the panel (and is pad confirm otherwise), right stick Y scrolls.
        bool onPanel = false;
        float u = 0.0f, v = 0.0f;
        in.beamValid = false;
        if (in.aimSpace != XR_NULL_HANDLE && s.lastDisplayTime != 0 && s.quad.width > 0) {
            const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            XrSpaceLocation aim{ XR_TYPE_SPACE_LOCATION };
            XrSpaceLocation head{ XR_TYPE_SPACE_LOCATION };
            if (XR_SUCCEEDED(xrLocateSpace(in.aimSpace, s.localSpace, s.lastDisplayTime, &aim)) &&
                (aim.locationFlags & valid) == valid &&
                XR_SUCCEEDED(xrLocateSpace(s.viewSpace, s.localSpace, s.lastDisplayTime, &head)) &&
                (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
                const Quat aq = { aim.pose.orientation.x, aim.pose.orientation.y, aim.pose.orientation.z,
                                  aim.pose.orientation.w };
                const Vec3 origin = { aim.pose.position.x, aim.pose.position.y, aim.pose.position.z };
                const Vec3 dir = Rotate(aq, { 0.0f, 0.0f, -1.0f });
                float dist = 0.0f;
                onPanel = RayHitsPanel(origin, dir, FlatQuadPose(), kQuadWidth, FlatQuadHeight(), &u, &v, &dist);
                if (!onPanel) {
                    dist = kBeamMissLength;
                }
                const Vec3 to = { origin.x + dir.x * dist, origin.y + dir.y * dist, origin.z + dir.z * dist };
                const Vec3 eye = { head.pose.position.x, head.pose.position.y, head.pose.position.z };
                in.beamValid = BeamPose(origin, to, eye, &in.beamPose, &in.beamLength);
            }
        }
        in.triggerDown = gdx_vr::TriggerClick(in.triggerDown, value(in.pointerTrigger));
        const bool aDown = boolean(in.a);
        const bool click = onPanel && (in.triggerDown || aDown);
        if (click && !in.pointerDown) {
            sfvr_haptic_event(&in.haptics, SFVR_HAND_RIGHT, SFVR_HAPTIC_UI_CLICK);
        }
        in.pointerDown = click;
        in.pointerOnPanel = onPanel;
        in.pointerU = u;
        in.pointerV = v;
        nav.pointer_active = onPanel ? 1 : 0;
        nav.pointer_x = u;
        nav.pointer_y = v;
        nav.pointer_down = click ? 1 : 0;
        nav.wheel_y = (onPanel && std::fabs(cy) > 0.2f) ? cy * 0.15f : 0.0f;
        nav.confirm = !onPanel && aDown;
        // Back out: the physical B (in a race it toggles the camera) or X (N64 B, the game's back).
        nav.cancel = boolean(in.cameraMode) || boolean(in.b);
        nav.back = nowSec < in.backPulseUntil;
        nav.l1 = boolean(in.l);
        nav.r1 = boolean(in.r);
        gdx_imgui_nav_set_external(&nav);
        sx = sy = 0.0f;
    } else {
        for (bool& d : in.stickDir) {
            d = false;
        }
        in.beamValid = false;
        in.pointerDown = false;
        in.pointerOnPanel = false;
        in.triggerDown = false;
        gdx_imgui_nav_set_external(nullptr);
    }
    in.pad.buttons = buttons;
    in.pad.stickX = toStick(sx);
    in.pad.stickY = toStick(sy);
    in.any = buttons != 0 || in.pad.stickX != 0 || in.pad.stickY != 0;
}

void MergeXrInput(GdxInputPad* pad) {
    const XrInput& in = S().input;
    if (!in.any) {
        return;
    }
    pad->buttons |= in.pad.buttons;
    if (std::abs(in.pad.stickX) > std::abs(pad->stickX)) {
        pad->stickX = in.pad.stickX;
    }
    if (std::abs(in.pad.stickY) > std::abs(pad->stickY)) {
        pad->stickY = in.pad.stickY;
    }
}

// Instance + system + session + swapchains. Runs on the host thread with libultraship's GL
// context current. Any failure disables VR for the process and leaves desktop behaviour intact.
bool CreateEverything() {
    State& s = S();
    InitSettings();
    bool refreshExt = false;

    // XR_VALVE_frame_controller_interaction gives the Steam Frame controllers their own binding
    // profile; enabled when the runtime offers it.
    static const char* kFrameControllerExt = "XR_VALVE_frame_controller_interaction";
    {
        uint32_t n = 0;
        xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
        std::vector<XrExtensionProperties> props(n, XrExtensionProperties{ XR_TYPE_EXTENSION_PROPERTIES });
        xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
        for (const XrExtensionProperties& p : props) {
            if (std::strcmp(p.extensionName, kFrameControllerExt) == 0) {
                s.frameControllerExt = true;
            }
            if (std::strcmp(p.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0) {
                refreshExt = true;
            }
        }
    }
    std::vector<const char*> exts = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    if (s.frameControllerExt) {
        exts.push_back(kFrameControllerExt);
    }
    if (refreshExt) {
        exts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }
    gdx_port_logf("[openxr] XR_FB_display_refresh_rate %s\n", refreshExt ? "advertised" : "not advertised");
    XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
    std::strncpy(ici.applicationInfo.applicationName, "G-Diffuser", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ici.applicationInfo.applicationVersion = 1;
    std::strncpy(ici.applicationInfo.engineName, "G-Diffuser", XR_MAX_ENGINE_NAME_SIZE - 1);
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.enabledExtensionNames = exts.data();
    if (!Check(xrCreateInstance(&ici, &s.instance), "xrCreateInstance")) {
        return false;
    }
    if (refreshExt) {
        xrGetInstanceProcAddr(s.instance, "xrEnumerateDisplayRefreshRatesFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&s.enumerateRates));
        xrGetInstanceProcAddr(s.instance, "xrRequestDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&s.requestRate));
        xrGetInstanceProcAddr(s.instance, "xrGetDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&s.getRate));
    }

    XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(s.instance, &ip);

    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(xrGetSystem(s.instance, &sgi, &s.systemId), "xrGetSystem")) {
        return false;
    }
    XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
    xrGetSystemProperties(s.instance, s.systemId, &sp);

    XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    uint32_t viewCount = 0;
    if (!Check(xrEnumerateViewConfigurationViews(s.instance, s.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                 &viewCount, vcv),
               "xrEnumerateViewConfigurationViews") ||
        viewCount != 2) {
        return false;
    }

    PFN_xrGetOpenGLGraphicsRequirementsKHR getReqs = nullptr;
    if (!Check(xrGetInstanceProcAddr(s.instance, "xrGetOpenGLGraphicsRequirementsKHR",
                                     reinterpret_cast<PFN_xrVoidFunction*>(&getReqs)),
               "xrGetInstanceProcAddr(xrGetOpenGLGraphicsRequirementsKHR)")) {
        return false;
    }
    XrGraphicsRequirementsOpenGLKHR reqs{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    if (!Check(getReqs(s.instance, s.systemId, &reqs), "xrGetOpenGLGraphicsRequirementsKHR")) {
        return false;
    }

    // GLX handles of libultraship's current context.
    auto getDisplay = reinterpret_cast<PFN_glXGetCurrentDisplay>(SDL_GL_GetProcAddress("glXGetCurrentDisplay"));
    auto getContext = reinterpret_cast<PFN_glXGetCurrentContext>(SDL_GL_GetProcAddress("glXGetCurrentContext"));
    auto getDrawable = reinterpret_cast<PFN_glXGetCurrentDrawable>(SDL_GL_GetProcAddress("glXGetCurrentDrawable"));
    auto queryContext = reinterpret_cast<PFN_glXQueryContext>(SDL_GL_GetProcAddress("glXQueryContext"));
    auto chooseFbConfig = reinterpret_cast<PFN_glXChooseFBConfig>(SDL_GL_GetProcAddress("glXChooseFBConfig"));
    Display* display = (getDisplay != nullptr) ? getDisplay() : nullptr;
    GLXContext context = (getContext != nullptr) ? getContext() : nullptr;
    GLXDrawable drawable = (getDrawable != nullptr) ? getDrawable() : 0;
    if (display == nullptr || context == nullptr) {
        gdx_port_logf("[openxr] current GL context is not GLX (SDL video driver %s); the Xlib binding "
                      "needs SDL_VIDEODRIVER=x11 without EGL\n",
                      SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
        return false;
    }
    GLXFBConfig fbConfig = nullptr;
    if (queryContext != nullptr && chooseFbConfig != nullptr) {
        int fbId = 0;
        queryContext(display, context, GLX_FBCONFIG_ID, &fbId);
        const int attrs[] = { GLX_FBCONFIG_ID, fbId, None };
        int n = 0;
        GLXFBConfig* cfgs = chooseFbConfig(display, DefaultScreen(display), attrs, &n);
        if (cfgs != nullptr && n > 0) {
            fbConfig = cfgs[0];
        }
        // cfgs is deliberately not XFree'd: one small allocation per session, and freeing it would
        // make the executable link libX11 directly.
    }

    if (!LoadGl(s.gl.genFramebuffers, "glGenFramebuffers") ||
        !LoadGl(s.gl.deleteFramebuffers, "glDeleteFramebuffers") ||
        !LoadGl(s.gl.bindFramebuffer, "glBindFramebuffer") ||
        !LoadGl(s.gl.framebufferTexture2D, "glFramebufferTexture2D") ||
        !LoadGl(s.gl.blitFramebuffer, "glBlitFramebuffer")) {
        return false;
    }

    XrGraphicsBindingOpenGLXlibKHR binding{ XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR };
    binding.xDisplay = display;
    binding.visualid = 0;
    binding.glxFBConfig = fbConfig;
    binding.glxDrawable = drawable;
    binding.glxContext = context;
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &binding;
    sci.systemId = s.systemId;
    if (!Check(xrCreateSession(s.instance, &sci, &s.session), "xrCreateSession")) {
        return false;
    }

    XrReferenceSpaceCreateInfo rsci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Check(xrCreateReferenceSpace(s.session, &rsci, &s.localSpace), "xrCreateReferenceSpace")) {
        return false;
    }
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(xrCreateReferenceSpace(s.session, &rsci, &s.viewSpace), "xrCreateReferenceSpace(VIEW)")) {
        return false;
    }

    // The game image is already gamma-encoded. Prefer an sRGB swapchain and copy into it with
    // GL_FRAMEBUFFER_SRGB disabled, so values land unconverted and the compositor decodes them as
    // the sRGB values they are. (SteamVR on the Frame offers no linear RGBA8.)
    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(s.session, 0, &fmtCount, nullptr);
    std::vector<int64_t> fmts(fmtCount);
    xrEnumerateSwapchainFormats(s.session, fmtCount, &fmtCount, fmts.data());
    for (int64_t want : { (int64_t)GL_SRGB8_ALPHA8, (int64_t)GL_RGBA8 }) {
        for (int64_t f : fmts) {
            if (f == want && s.colorFormat == 0) {
                s.colorFormat = f;
            }
        }
    }
    if (s.colorFormat == 0) {
        gdx_port_logf("[openxr] no RGBA8/SRGB8_ALPHA8 swapchain format offered\n");
        return false;
    }

    int drawW = 0;
    int drawH = 0;
    SDL_GL_GetDrawableSize(SDL_GL_GetCurrentWindow(), &drawW, &drawH);
    if (drawW <= 0 || drawH <= 0) {
        drawW = 1280;
        drawH = 720;
    }
    if (!CreateSwapchain(s.quad, drawW, drawH, "xrCreateSwapchain(quad)")) {
        return false;
    }
    for (int e = 0; e < 2; e++) {
        if (!CreateSwapchain(s.eyes[e], (int32_t)vcv[e].recommendedImageRectWidth,
                             (int32_t)vcv[e].recommendedImageRectHeight, "xrCreateSwapchain(eye)")) {
            return false;
        }
    }
    const float hudAspect = SettingFloat(kSetHudAspect);
    const int32_t hudWidth = ((int32_t)((float)kHudHeight * hudAspect) + 3) & ~3;
    if (!CreateSwapchain(s.hud, hudWidth, kHudHeight, "xrCreateSwapchain(hud)")) {
        return false;
    }
    s.gl.genFramebuffers(1, &s.drawFbo);
    s.gl.genFramebuffers(1, &s.readFbo);
    if (CreateSwapchain(s.beam, 8, 8, "xrCreateSwapchain(pointer beam)")) {
        FillSwapchain(s.beam, 0.55f, 0.85f, 1.0f, 0.9f);
    }
    if (!CreateInput()) {
        gdx_port_logf("[openxr] controller actions unavailable; relying on SDL/Steam Input\n");
    }

    gdx_port_logf("[openxr]\n"
                  "runtime=%s\n"
                  "version=%u.%u.%u\n"
                  "system=%s\n"
                  "view_config=PRIMARY_STEREO eye_size=%dx%d\n"
                  "gl_requirements=%u.%u..%u.%u gl_version=%s\n"
                  "graphics_binding=opengl_xlib video_driver=%s\n"
                  "swapchain_format=0x%llx quad=%dx%d\n"
                  "[/openxr]\n",
                  ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
                  XR_VERSION_PATCH(ip.runtimeVersion), sp.systemName, s.eyes[0].width, s.eyes[0].height,
                  XR_VERSION_MAJOR(reqs.minApiVersionSupported), XR_VERSION_MINOR(reqs.minApiVersionSupported),
                  XR_VERSION_MAJOR(reqs.maxApiVersionSupported), XR_VERSION_MINOR(reqs.maxApiVersionSupported),
                  (const char*)glGetString(GL_VERSION), SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?",
                  (long long)s.colorFormat, s.quad.width, s.quad.height);

    // Starting camera: gEnhancements.VR.CameraMode (0 chase, 1 cockpit); GDX_VR_COCKPIT=1 for tests.
    s.cockpit = SettingInt(kSetCameraMode) == 1 || EnvFlag("GDX_VR_COCKPIT");
    s.cameraModeSeen = SettingInt(kSetCameraMode);
    gdx_port_logf("[openxr] camera starts in %s view\n", s.cockpit ? "cockpit" : "chase");
    gfx_pre_present_hook = &PrePresentHook;
    Fast::Interpreter::SetVrWorldSpriteResolver([](uint32_t id, Fast::Interpreter::VrWorldSprite* out) {
        float v[8];
        if (gdx_vr_world_sprite_lookup((int)id, v) == 0) {
            return false;
        }
        *out = { v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7] };
        return true;
    });
    return true;
}

// Desktop presentation must not throttle the XR clock while a session runs.
void SetDesktopSwapBlocking(bool block) {
    SDL_GL_SetSwapInterval(block ? 1 : 0);
}

// --- Display refresh rate ----------------------------------------------------------------------
//
// gEnhancements.VR.RefreshRate (default 90) is a ceiling: the highest rate the runtime offers that
// is <= it is requested. Higher rates are only worth it if the game holds them while worn, so the
// monitor below falls back to 72 Hz after two consecutive 10 s windows in which the XR frame rate
// stayed under 90% of the target (the same policy as the Halo port). The fallback lasts for the
// session and is not written back to the CVar. The simulation is unaffected by any of this: the
// scheduler places frames by predictedDisplayTime (tests/gdx_vr_scheduler_tests.cpp).

// Ask the runtime for the best offered rate <= hz. Returns the rate requested, or 0.
float RequestRefreshRate(float hz, const char* why) {
    State& s = S();
    if (s.enumerateRates == nullptr || s.requestRate == nullptr || s.session == XR_NULL_HANDLE) {
        return 0.0f;
    }
    float rates[32];
    uint32_t count = 0;
    if (!Check(s.enumerateRates(s.session, 32, &count, rates), "xrEnumerateDisplayRefreshRatesFB")) {
        return 0.0f;
    }
    char list[256];
    size_t off = 0;
    for (uint32_t i = 0; i < count && off + 16 < sizeof(list); i++) {
        off += (size_t)std::snprintf(list + off, sizeof(list) - off, "%s%.0f", i == 0 ? "" : " ", rates[i]);
    }
    list[off < sizeof(list) ? off : sizeof(list) - 1] = '\0';
    const float chosen = gdx_vr::PickRefreshRate(rates, count, hz);
    if (chosen <= 0.0f || !Check(s.requestRate(s.session, chosen), "xrRequestDisplayRefreshRateFB")) {
        gdx_port_logf("[vr] refresh rate: wanted <= %.0f Hz (%s); offered [%s]: none requested\n", (double)hz, why,
                      list);
        return 0.0f;
    }
    float current = 0.0f;
    if (s.getRate != nullptr) {
        s.getRate(s.session, &current);
    }
    gdx_port_logf("[vr] refresh rate: requested %.0f Hz (wanted <= %.0f, %s); offered [%s]; runtime reports %.1f Hz\n",
                  (double)chosen, (double)hz, why, list, (double)current);
    s.refreshTarget = chosen;
    s.refreshMisses = 0;
    s.refreshWindowStartS = 0.0;
    s.refreshWindowFrames = 0;
    return chosen;
}

// Once per XR frame, from PerfFrame's call site: applies a changed CVar and runs the fallback.
void RefreshRateFrame(double nowS) {
    State& s = S();
    if (s.requestRate == nullptr) {
        return;
    }
    const int wanted = SettingInt(kSetRefreshRate);
    if (wanted != s.refreshWanted) {
        s.refreshWanted = wanted;
        RequestRefreshRate((float)wanted, s.refreshTarget == 0.0f ? "session start" : "setting changed");
        return;
    }
    if (s.refreshTarget <= 72.5f) {
        return; // nothing below 72 Hz is worth falling back from
    }
    if (s.refreshWindowStartS == 0.0 || s.focused != s.refreshWindowFocused) {
        // A window only counts while worn: restart it whenever focus changes.
        s.refreshWindowStartS = nowS;
        s.refreshWindowFrames = 0;
        s.refreshWindowFocused = s.focused;
        s.refreshMisses = 0;
        return;
    }
    s.refreshWindowFrames++;
    const double seconds = nowS - s.refreshWindowStartS;
    if (seconds < 10.0) {
        return;
    }
    const double fps = (double)s.refreshWindowFrames / seconds;
    s.refreshWindowStartS = nowS;
    s.refreshWindowFrames = 0;
    if (!s.focused) {
        return;
    }
    if (fps < (double)s.refreshTarget * 0.9) {
        if (++s.refreshMisses >= 2) {
            gdx_port_logf("[vr] refresh rate: %.0f Hz not held (%.1f frames/s over 10 s, twice): back to 72 Hz\n",
                          (double)s.refreshTarget, fps);
            RequestRefreshRate(72.0f, "fallback");
        }
    } else {
        s.refreshMisses = 0;
    }
}

void HandleSessionState(XrSessionState st) {
    State& s = S();
    gdx_port_logf("[openxr] session state %s\n", SessionStateName(st));
    if (st == XR_SESSION_STATE_FOCUSED && !s.focused) {
        // Facing direction and height when the app gains focus become forward and camera height
        // (possibly a different wearer after the headset came off).
        RequestRecentre(gdx_vr::RecentreKind::Height, "focus gain");
    }
    s.focused = (st == XR_SESSION_STATE_FOCUSED);
    switch (st) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo sbi{ XR_TYPE_SESSION_BEGIN_INFO };
            sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            if (Check(xrBeginSession(s.session, &sbi), "xrBeginSession")) {
                s.phase = Phase::Running;
                s.refreshWanted = 0; // (re)apply the refresh-rate setting on the first frame
                s.refreshTarget = 0.0f;
                s.scheduler.Reset();
                s.pending = PendingFrame{};
                SetDesktopSwapBlocking(false);
            }
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            Check(xrEndSession(s.session), "xrEndSession");
            s.phase = Phase::Idle;
            s.pending = PendingFrame{};
            SetDesktopSwapBlocking(true);
            gdx_interp_mark_cut_src("vr-session-stop");
            break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            s.phase = Phase::Exiting;
            s.exitRequested = true;
            s.pending = PendingFrame{};
            SetDesktopSwapBlocking(true);
            break;
        default:
            break;
    }
}

void PollEvents() {
    State& s = S();
    XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(s.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            HandleSessionState(reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev)->state);
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            gdx_port_logf("[openxr] instance loss pending\n");
            s.phase = Phase::Exiting;
            s.exitRequested = true;
        } else if (ev.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
            const auto* rc = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&ev);
            gdx_port_logf("[vr] display refresh rate changed: %.1f -> %.1f Hz\n", (double)rc->fromDisplayRefreshRate,
                          (double)rc->toDisplayRefreshRate);
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // A system recentre (the runtime moves the LOCAL origin): treat it like our own, with
            // height, since the origin's height may have moved too.
            RequestRecentre(gdx_vr::RecentreKind::Height, "runtime reference-space change");
        }
        ev = XrEventDataBuffer{ XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// --- Frame protocol --------------------------------------------------------------------------

Quat QMul(const Quat& a, const Quat& b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}

Quat QConj(const Quat& q) {
    return { -q.x, -q.y, -q.z, q.w };
}

// Capture the head's yaw (about LOCAL +y; LOCAL is gravity-aligned) and position as the new forward.
// A Level request keeps the height anchor; a Height request (and the first recentre) replaces it.
void UpdateRecenter(XrTime time) {
    State& s = S();
    if (s.recenterPending == gdx_vr::RecentreKind::Idle || s.viewSpace == XR_NULL_HANDLE) {
        return;
    }
    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(s.viewSpace, s.localSpace, time, &loc)) ||
        (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0 ||
        (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0) {
        return; // not tracked yet (headset in standby): try again next frame
    }
    const Quat q = { loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w };
    const Vec3 f = Rotate(q, { 0.0f, 0.0f, -1.0f });
    if (f.x * f.x + f.z * f.z < 1e-4f) {
        return; // looking straight up or down: no usable heading
    }
    // A rotation by theta about +y takes (0,0,-1) to (-sin theta, 0, -cos theta).
    const float theta = std::atan2(-f.x, -f.z);
    s.recenterYaw = { 0.0f, std::sin(theta * 0.5f), 0.0f, std::cos(theta * 0.5f) };
    s.recenterPos = { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z };
    const bool withHeight = !s.recenterValid || s.recenterPending == gdx_vr::RecentreKind::Height;
    if (withHeight) {
        s.heightAnchor = loc.pose.position.y;
    }
    s.recenterValid = true;
    s.recenterPending = gdx_vr::RecentreKind::Idle;
    gdx_port_logf("[openxr] recentred: yaw %.1f deg, head (%.2f, %.2f, %.2f), height anchor %.2f%s\n",
                  theta * 57.29578f, s.recenterPos.x, s.recenterPos.y, s.recenterPos.z, s.heightAnchor,
                  withHeight ? " (updated)" : " (kept)");
}

// A LOCAL-space pose expressed relative to the recentre frame.
Pose Recentred(const XrPosef& p) {
    const State& s = S();
    const Quat q = { p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w };
    Vec3 pos = { p.position.x, p.position.y, p.position.z };
    if (!s.recenterValid) {
        return { q, pos };
    }
    const Quat inv = QConj(s.recenterYaw);
    // Horizontal offset from the recentre point, vertical offset from the kept height anchor.
    pos = Rotate(inv, { pos.x - s.recenterPos.x, pos.y - s.heightAnchor, pos.z - s.recenterPos.z });
    return { QMul(inv, q), pos };
}

const char* const kPerfStages[4] = { "logic", "xlate", "irun", "hud" };

// Feed one XR frame to the [vr-perf] window and log the line when the window closes.
void PerfFrame(const XrFrameState& fs) {
    State& s = S();
    const double nowS = NowMs() * 0.001;
    if (!s.perfReady) {
        sfvr_perf_init(&s.perf, SFVR_PERF_DEFAULT_WINDOW_S, kPerfStages, 4);
        gdx_perf_lifetime_ms(&s.perfPrevLife[0], &s.perfPrevLife[1], &s.perfPrevLife[2], &s.perfPrevLife[3]);
        s.perfReady = true;
    }
    double life[4];
    gdx_perf_lifetime_ms(&life[0], &life[1], &life[2], &life[3]);
    const double gametick = life[0] - s.perfPrevLife[0];
    const double xlate = life[1] - s.perfPrevLife[1];
    const double run = life[2] - s.perfPrevLife[2];
    const double mirror = life[3] - s.perfPrevLife[3];
    for (int i = 0; i < 4; i++) {
        s.perfPrevLife[i] = life[i];
    }
    double logic = gametick - xlate - run - mirror - s.perfPresentMs;
    if (logic < 0.0) {
        logic = 0.0;
    }
    const float stages[4] = { (float)logic, (float)xlate, (float)s.perfIrunMs, (float)s.perfHudMs };
    const float eyes[2] = { (float)s.perfEyeMs[0], (float)s.perfEyeMs[1] };
    // A frame is missed when the runtime's predicted display time jumped by more than one and a
    // half display periods since the previous frame (we were late and a refresh went by).
    const bool missed = s.perfPrevDisplay != 0 && fs.predictedDisplayPeriod > 0 &&
                        (fs.predictedDisplayTime - s.perfPrevDisplay) * 2 > fs.predictedDisplayPeriod * 3;
    s.perfPrevDisplay = fs.predictedDisplayTime;
    sfvr_perf_add_frame(&s.perf, nowS, missed ? 1 : 0, stages, eyes, -1.0f); // no GPU timing yet
    s.perfIrunMs = s.perfHudMs = s.perfPresentMs = 0.0;
    s.perfEyeMs[0] = s.perfEyeMs[1] = 0.0;
    char line[256];
    if (sfvr_perf_poll(&s.perf, nowS, line, sizeof(line))) {
        gdx_port_logf("%s\n", line);
        const double n = (s.stereoFrames > 0) ? (double)s.stereoFrames : 1.0;
        gdx_port_logf("[vr] pacing: xr_frames=%lld no_tick=%lld stereo=%lld rebases=%lld sim_ticks=%lld "
                      "period=%.3fms copy_ms=%.2f (logic/xlate need GDX_PERF=1)\n",
                      s.xrFrames, s.xrFramesNoTick, s.stereoFrames, s.rebases, s.scheduler.TicksRun(),
                      (double)fs.predictedDisplayPeriod * 1e-6, s.copyMsSum / (2.0 * n));
    }
}

// Make sure a waited frame exists, planning how many ticks it needs.
bool EnsurePending() {
    State& s = S();
    if (s.pending.waited) {
        return true;
    }
    XrFrameWaitInfo fwi{ XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs{ XR_TYPE_FRAME_STATE };
    if (!Check(xrWaitFrame(s.session, &fwi, &fs), "xrWaitFrame")) {
        return false;
    }
    s.lastDisplayTime = fs.predictedDisplayTime;
    UpdateRecenter(fs.predictedDisplayTime);
    const FramePlan plan = s.scheduler.PlanFrame((double)fs.predictedDisplayTime * 1e-9);
    if (plan.rebased) {
        s.rebases++;
        gdx_interp_mark_cut_src("vr-timeline-rebase");
    }
    s.pending.waited = true;
    s.pending.state = fs;
    s.pending.ticksOwed = plan.ticksToRun;
    s.xrFrames++;
    PerfFrame(fs);
    RefreshRateFrame(NowMs() * 0.001);
    return true;
}

// One simulation tick has completed.
void CountTick() {
    State& s = S();
    if (!s.pending.waited && !EnsurePending()) {
        return;
    }
    if (s.pending.ticksOwed > 0) {
        s.pending.ticksOwed--;
    }
    s.scheduler.OnTickRan();
}

bool BeginPending() {
    State& s = S();
    FlushHaptics();
    XrFrameBeginInfo fbi{ XR_TYPE_FRAME_BEGIN_INFO };
    return Check(xrBeginFrame(s.session, &fbi), "xrBeginFrame");
}

void EndPending(const XrCompositionLayerBaseHeader* const* layers, uint32_t layerCount) {
    State& s = S();
    XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
    fei.displayTime = s.pending.state.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layerCount;
    fei.layers = layers;
    Check(xrEndFrame(s.session, &fei), "xrEndFrame");
    s.pending = PendingFrame{};
}

// --- GL copies ---------------------------------------------------------------------------------

struct GlStateSave {
    GLint read = 0;
    GLint draw = 0;
    GLboolean scissor = GL_FALSE;
    GLboolean srgb = GL_FALSE;
};

GlStateSave SaveGlState() {
    GlStateSave g;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &g.read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &g.draw);
    g.scissor = glIsEnabled(GL_SCISSOR_TEST);
    g.srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    if (g.scissor) {
        glDisable(GL_SCISSOR_TEST);
    }
    if (g.srgb) {
        glDisable(GL_FRAMEBUFFER_SRGB);
    }
    return g;
}

void RestoreGlState(const GlStateSave& g) {
    State& s = S();
    s.gl.bindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)g.read);
    s.gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)g.draw);
    if (g.scissor) {
        glEnable(GL_SCISSOR_TEST);
    }
    if (g.srgb) {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }
}

// Acquire the next image of `sc`, blit into it, release. `sourceTexture` == 0 copies the window
// back buffer (bottom-up); otherwise the texture is a top-down game framebuffer of srcW x srcH.
void CopyIntoSwapchain(Swapchain& sc, GLuint sourceTexture, int srcW, int srcH) {
    State& s = S();
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!Check(xrAcquireSwapchainImage(sc.handle, &ai, &index), "xrAcquireSwapchainImage")) {
        return;
    }
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (!Check(xrWaitSwapchainImage(sc.handle, &wi), "xrWaitSwapchainImage")) {
        return;
    }

    const GlStateSave saved = SaveGlState();
    if (sourceTexture == 0) {
        s.gl.bindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    } else {
        s.gl.bindFramebuffer(GL_READ_FRAMEBUFFER, s.readFbo);
        s.gl.framebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sourceTexture, 0);
    }
    s.gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, s.drawFbo);
    s.gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.images[index].image, 0);
    if (sourceTexture == 0) {
        s.gl.blitFramebuffer(0, 0, srcW, srcH, 0, 0, sc.width, sc.height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    } else {
        // Game framebuffers are stored top-down; OpenXR GL images are bottom-up.
        s.gl.blitFramebuffer(0, 0, srcW, srcH, 0, sc.height, sc.width, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        s.gl.framebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    }
    s.gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    RestoreGlState(saved);

    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (Check(xrReleaseSwapchainImage(sc.handle, &ri), "xrReleaseSwapchainImage")) {
        sc.hasImage = true;
    }
}

// Acquire the next image of `sc`, clear it to one colour and release it. Returns false (and
// leaves the swapchain as it was) when acquiring fails.
bool ClearSwapchainImage(Swapchain& sc, float r, float g, float b, float a) {
    State& s = S();
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!Check(xrAcquireSwapchainImage(sc.handle, &ai, &index), "xrAcquireSwapchainImage(clear)")) {
        return false;
    }
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (!Check(xrWaitSwapchainImage(sc.handle, &wi), "xrWaitSwapchainImage(clear)")) {
        return false;
    }
    const GlStateSave saved = SaveGlState();
    s.gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, s.drawFbo);
    s.gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.images[index].image, 0);
    GLfloat prev[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prev);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(prev[0], prev[1], prev[2], prev[3]);
    s.gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    RestoreGlState(saved);
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (!Check(xrReleaseSwapchainImage(sc.handle, &ri), "xrReleaseSwapchainImage(clear)")) {
        return false;
    }
    sc.hasImage = true;
    return true;
}

// Fill every image of a static-content swapchain with one colour (the pointer beam). A layer keeps
// showing the last released image.
void FillSwapchain(Swapchain& sc, float r, float g, float b, float a) {
    for (size_t i = 0; i < sc.images.size(); i++) {
        if (!ClearSwapchainImage(sc, r, g, b, a)) {
            return;
        }
    }
}

// --- Flat mode ---------------------------------------------------------------------------------

// The flat quad (menus, flat game screens): kQuadDistance ahead of where the viewer faced at the
// last recentre, in LOCAL. Yaw and horizontal position follow every recentre; the height is the
// height anchor, so it is re-anchored together with the eye poses (View hold 3 s, focus gain) and
// never left at a stale value. Before the first recentre lands the anchor is 0 (the LOCAL origin is
// at eye height). The menu pointer hit-tests this same pose.
Pose FlatQuadPose() {
    const State& s = S();
    const Vec3 ahead = Rotate(s.recenterYaw, { 0.0f, 0.0f, -kQuadDistance });
    return { s.recenterYaw, { s.recenterPos.x + ahead.x, s.heightAnchor, s.recenterPos.z + ahead.z } };
}

float FlatQuadHeight() {
    const State& s = S();
    return s.quad.width > 0 ? kQuadWidth * (float)s.quad.height / (float)s.quad.width : 0.0f;
}

void EndPendingWithQuad() {
    State& s = S();
    XrCompositionLayerProjection backdrop{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView backdropViews[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
                                                          { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad beam{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerBaseHeader* layers[3];
    uint32_t layerCount = 0;
    const bool render = s.pending.state.shouldRender || ForceRender();
    // A black projection layer under the panel, so nothing the compositor draws on its own shows
    // through around it. With only quad layers submitted, a corrupted, enlarged and mirrored copy of
    // the 64DD splash appeared behind the panel (2026-10-04 wear), although the image copied into
    // the panel was clean (GDX_VR_DUMP_FLAT_EVERY). Most likely SteamVR still showed the Steam
    // launch's desktop window on a theater screen until the app submitted a projection layer.
    if (render && s.eyes[0].handle != XR_NULL_HANDLE && s.eyes[1].handle != XR_NULL_HANDLE) {
        XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = s.pending.state.predictedDisplayTime;
        vli.space = s.localSpace;
        XrViewState vs{ XR_TYPE_VIEW_STATE };
        XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        uint32_t located = 0;
        if (XR_SUCCEEDED(xrLocateViews(s.session, &vli, &vs, 2, &located, views)) && located == 2 &&
            (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 &&
            ClearSwapchainImage(s.eyes[0], 0.0f, 0.0f, 0.0f, 1.0f) &&
            ClearSwapchainImage(s.eyes[1], 0.0f, 0.0f, 0.0f, 1.0f)) {
            for (int e = 0; e < 2; e++) {
                backdropViews[e].pose = views[e].pose;
                backdropViews[e].fov = views[e].fov;
                backdropViews[e].subImage.swapchain = s.eyes[e].handle;
                backdropViews[e].subImage.imageRect.offset = { 0, 0 };
                backdropViews[e].subImage.imageRect.extent = { s.eyes[e].width, s.eyes[e].height };
            }
            backdrop.space = s.localSpace;
            backdrop.viewCount = 2;
            backdrop.views = backdropViews;
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&backdrop);
        }
    }
    if (render && s.quad.hasImage) {
        const Pose pose = FlatQuadPose();
        quad.space = s.localSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = s.quad.handle;
        quad.subImage.imageRect.offset = { 0, 0 };
        quad.subImage.imageRect.extent = { s.quad.width, s.quad.height };
        quad.pose.orientation = { pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w };
        quad.pose.position = { pose.position.x, pose.position.y, pose.position.z };
        quad.size.width = kQuadWidth;
        quad.size.height = FlatQuadHeight();
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    }
    // The menu pointer's laser: a thin solid quad from the hand to the hit (or 1.5 m out), turned
    // toward the head. Composited after the panel, so it draws over it up to the hit point.
    const XrInput& in = s.input;
    if (render && s.menuOpen && in.beamValid && s.beam.hasImage) {
        beam.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                          XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        beam.space = s.localSpace;
        beam.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        beam.subImage.swapchain = s.beam.handle;
        beam.subImage.imageRect.offset = { 0, 0 };
        beam.subImage.imageRect.extent = { s.beam.width, s.beam.height };
        const Pose& bp = in.beamPose;
        beam.pose.orientation = { bp.orientation.x, bp.orientation.y, bp.orientation.z, bp.orientation.w };
        beam.pose.position = { bp.position.x, bp.position.y, bp.position.z };
        beam.size.width = kBeamWidth;
        beam.size.height = in.beamLength;
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&beam);
    }
    EndPending(layers, layerCount);
}

// GDX_VR_DUMP_FLAT_EVERY=<k> (with GDX_VR_DUMP_FLAT_UNTIL=<n>, default 600): write the window back
// buffer the flat quad shows (vr-flat-<i>.bmp) every k-th flat tick up to tick n, to see boot and
// menu screens as the headset gets them.
void DumpFlatIfDue(int w, int h) {
    static const long every = [] {
        const char* e = std::getenv("GDX_VR_DUMP_FLAT_EVERY");
        return (e != nullptr && e[0] != '\0') ? std::strtol(e, nullptr, 10) : 0L;
    }();
    static const long until = [] {
        const char* e = std::getenv("GDX_VR_DUMP_FLAT_UNTIL");
        return (e != nullptr && e[0] != '\0') ? std::strtol(e, nullptr, 10) : 600L;
    }();
    static long frame = 0;
    const long i = frame++;
    if (every <= 0 || i > until || (i % every) != 0 || w <= 0 || h <= 0) {
        return;
    }
    std::vector<unsigned char> rgba((size_t)w * (size_t)h * 4u);
    GLint prevRead = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    S().gl.bindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    S().gl.bindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevRead);
    char path[64];
    std::snprintf(path, sizeof(path), "vr-flat-%04ld.bmp", i);
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        return;
    }
    const int rowBytes = ((w * 3) + 3) & ~3;
    unsigned char hdr[54] = { 'B', 'M' };
    auto put32 = [&](int at, unsigned v) {
        hdr[at] = v & 0xFF;
        hdr[at + 1] = (v >> 8) & 0xFF;
        hdr[at + 2] = (v >> 16) & 0xFF;
        hdr[at + 3] = (v >> 24) & 0xFF;
    };
    put32(2, 54u + (unsigned)rowBytes * (unsigned)h);
    put32(10, 54);
    put32(14, 40);
    put32(18, (unsigned)w);
    put32(22, (unsigned)h);
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, (unsigned)rowBytes * (unsigned)h);
    std::fwrite(hdr, 1, sizeof(hdr), f);
    std::vector<unsigned char> row((size_t)rowBytes, 0);
    for (int y = 0; y < h; y++) { // glReadPixels rows are bottom-up, as BMP wants
        const unsigned char* src = &rgba[(size_t)y * (size_t)w * 4u];
        for (int x = 0; x < w; x++) {
            row[(size_t)x * 3 + 0] = src[x * 4 + 2];
            row[(size_t)x * 3 + 1] = src[x * 4 + 1];
            row[(size_t)x * 3 + 2] = src[x * 4 + 0];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

// Installed into libultraship's Interpreter::EndFrame while a session exists. Flat ticks only.
void PrePresentHook() {
    State& s = S();
    if (s.phase != Phase::Running || s.stereoThisTick) {
        return;
    }
    CountTick();
    {
        // Every flat tick, shown or not (an idle headset is in standby and shows nothing).
        int dumpW = 0;
        int dumpH = 0;
        SDL_GL_GetDrawableSize(SDL_GL_GetCurrentWindow(), &dumpW, &dumpH);
        DumpFlatIfDue(dumpW, dumpH);
    }
    if (!s.pending.waited || s.pending.ticksOwed > 0) {
        return; // a catch-up tick: only the last tick before the frame is shown
    }
    if (!BeginPending()) {
        s.pending = PendingFrame{};
        return;
    }
    if (s.pending.state.shouldRender || ForceRender()) {
        int drawW = 0;
        int drawH = 0;
        SDL_GL_GetDrawableSize(SDL_GL_GetCurrentWindow(), &drawW, &drawH);
        CopyIntoSwapchain(s.quad, 0, drawW, drawH);
    }
    EndPendingWithQuad();
}

// --- Race HUD layer ---------------------------------------------------------------------------
//
// The HUD is composited over the world by alpha, but the game draws much of it with render modes
// that never write a meaningful alpha (the N64 had no use for it), so a transparent clear left
// those elements invisible. Instead the HUD pass runs twice, over opaque black and over opaque
// white, and a small shader recovers coverage from the difference: for any "over" blend,
// result = c*a + bg*(1-a), so black gives the premultiplied colour and white - black = 1 - a.

typedef GLuint (*PFN_CreateShader)(GLenum);
typedef void (*PFN_ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
typedef void (*PFN_CompileShader)(GLuint);
typedef void (*PFN_GetShaderiv)(GLuint, GLenum, GLint*);
typedef GLuint (*PFN_CreateProgram)(void);
typedef void (*PFN_AttachShader)(GLuint, GLuint);
typedef void (*PFN_LinkProgram)(GLuint);
typedef void (*PFN_GetProgramiv)(GLuint, GLenum, GLint*);
typedef void (*PFN_DeleteShader)(GLuint);
typedef void (*PFN_UseProgram)(GLuint);
typedef GLint (*PFN_GetUniformLocation)(GLuint, const char*);
typedef void (*PFN_Uniform1i)(GLint, GLint);
typedef void (*PFN_ActiveTexture)(GLenum);
typedef void (*PFN_GenVertexArrays)(GLsizei, GLuint*);
typedef void (*PFN_BindVertexArray)(GLuint);
typedef void (*PFN_BindSampler)(GLuint, GLuint);

struct HudGl {
    PFN_CreateShader createShader = nullptr;
    PFN_ShaderSource shaderSource = nullptr;
    PFN_CompileShader compileShader = nullptr;
    PFN_GetShaderiv getShaderiv = nullptr;
    PFN_CreateProgram createProgram = nullptr;
    PFN_AttachShader attachShader = nullptr;
    PFN_LinkProgram linkProgram = nullptr;
    PFN_GetProgramiv getProgramiv = nullptr;
    PFN_DeleteShader deleteShader = nullptr;
    PFN_UseProgram useProgram = nullptr;
    PFN_GetUniformLocation getUniformLocation = nullptr;
    PFN_Uniform1i uniform1i = nullptr;
    PFN_ActiveTexture activeTexture = nullptr;
    PFN_GenVertexArrays genVertexArrays = nullptr;
    PFN_BindVertexArray bindVertexArray = nullptr;
    PFN_BindSampler bindSampler = nullptr;
};

HudGl& Hgl() {
    static HudGl g;
    return g;
}

#ifndef GL_COMPILE_STATUS
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_SAMPLER_BINDING 0x8919
#endif

bool EnsureHudCombiner() {
    State& s = S();
    if (s.hudProgram != 0) {
        return true;
    }
    if (s.hudCombinerFailed) {
        return false;
    }
    s.hudCombinerFailed = true; // cleared on success
    HudGl& g = Hgl();
    if (!LoadGl(g.createShader, "glCreateShader") || !LoadGl(g.shaderSource, "glShaderSource") ||
        !LoadGl(g.compileShader, "glCompileShader") || !LoadGl(g.getShaderiv, "glGetShaderiv") ||
        !LoadGl(g.createProgram, "glCreateProgram") || !LoadGl(g.attachShader, "glAttachShader") ||
        !LoadGl(g.linkProgram, "glLinkProgram") || !LoadGl(g.getProgramiv, "glGetProgramiv") ||
        !LoadGl(g.deleteShader, "glDeleteShader") || !LoadGl(g.useProgram, "glUseProgram") ||
        !LoadGl(g.getUniformLocation, "glGetUniformLocation") || !LoadGl(g.uniform1i, "glUniform1i") ||
        !LoadGl(g.activeTexture, "glActiveTexture") || !LoadGl(g.genVertexArrays, "glGenVertexArrays") ||
        !LoadGl(g.bindVertexArray, "glBindVertexArray") || !LoadGl(g.bindSampler, "glBindSampler")) {
        return false;
    }
    static const char* kVs = "#version 410 core\n"
                             "out vec2 uv;\n"
                             "void main() {\n"
                             "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                             "    uv = p;\n"
                             "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                             "}\n";
    // Sources are stored top-down; the swapchain image is bottom-up.
    static const char* kFs = "#version 410 core\n"
                             "uniform sampler2D overBlack;\n"
                             "uniform sampler2D overWhite;\n"
                             "in vec2 uv;\n"
                             "out vec4 outColor;\n"
                             "void main() {\n"
                             "    vec2 st = vec2(uv.x, 1.0 - uv.y);\n"
                             "    vec3 b = texture(overBlack, st).rgb;\n"
                             "    vec3 w = texture(overWhite, st).rgb;\n"
                             "    float a = clamp(1.0 - dot(w - b, vec3(1.0 / 3.0)), 0.0, 1.0);\n"
                             "    outColor = vec4(min(b, vec3(a)), a);\n"
                             "}\n";
    auto compile = [&](GLenum type, const char* src) -> GLuint {
        GLuint sh = g.createShader(type);
        g.shaderSource(sh, 1, &src, nullptr);
        g.compileShader(sh);
        GLint ok = 0;
        g.getShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            gdx_port_logf("[openxr] HUD combine shader failed to compile\n");
            g.deleteShader(sh);
            return 0;
        }
        return sh;
    };
    const GLuint vs = compile(GL_VERTEX_SHADER, kVs);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFs);
    if (vs == 0 || fs == 0) {
        return false;
    }
    const GLuint prog = g.createProgram();
    g.attachShader(prog, vs);
    g.attachShader(prog, fs);
    g.linkProgram(prog);
    g.deleteShader(vs);
    g.deleteShader(fs);
    GLint linked = 0;
    g.getProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        gdx_port_logf("[openxr] HUD combine shader failed to link\n");
        return false;
    }
    s.hudLocBlack = g.getUniformLocation(prog, "overBlack");
    s.hudLocWhite = g.getUniformLocation(prog, "overWhite");
    g.genVertexArrays(1, &s.hudVao);
    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGenTextures(2, s.hudTex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, s.hudTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s.hud.width, s.hud.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    s.hudProgram = prog;
    s.hudCombinerFailed = false;
    return true;
}

// GPU copy of a w x h texture into another of the same size, orientation unchanged.
void CopyTexture(GLuint src, GLuint dst, int w, int h) {
    State& s = S();
    const GlStateSave saved = SaveGlState();
    s.gl.bindFramebuffer(GL_READ_FRAMEBUFFER, s.readFbo);
    s.gl.framebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    s.gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, s.drawFbo);
    s.gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
    s.gl.blitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    s.gl.framebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    s.gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    RestoreGlState(saved);
}

// Combine hudTex[0] (over black) and hudTex[1] (over white) into the next HUD swapchain image.
// Every piece of GL state touched is restored, so libultraship's cached state stays valid.
void CombineHudIntoSwapchain() {
    State& s = S();
    HudGl& g = Hgl();
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!Check(xrAcquireSwapchainImage(s.hud.handle, &ai, &index), "xrAcquireSwapchainImage(hud)")) {
        return;
    }
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (!Check(xrWaitSwapchainImage(s.hud.handle, &wi), "xrWaitSwapchainImage(hud)")) {
        return;
    }

    const GlStateSave saved = SaveGlState();
    GLint prevProgram = 0, prevVao = 0, prevActive = 0, prevViewport[4] = { 0, 0, 0, 0 };
    GLint prevTex[2] = { 0, 0 }, prevSampler[2] = { 0, 0 };
    GLboolean prevMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, prevMask);
    for (int i = 0; i < 2; i++) {
        g.activeTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex[i]);
        glGetIntegerv(GL_SAMPLER_BINDING, &prevSampler[i]);
    }
    const GLboolean blend = glIsEnabled(GL_BLEND);
    const GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cull = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    s.gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, s.drawFbo);
    s.gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.hud.images[index].image, 0);
    glViewport(0, 0, s.hud.width, s.hud.height);
    g.useProgram(s.hudProgram);
    g.bindVertexArray(s.hudVao);
    for (int i = 0; i < 2; i++) {
        g.activeTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, s.hudTex[i]);
        g.bindSampler(i, 0);
    }
    g.uniform1i(s.hudLocBlack, 0);
    g.uniform1i(s.hudLocWhite, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    s.gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

    for (int i = 0; i < 2; i++) {
        g.activeTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex[i]);
        g.bindSampler(i, (GLuint)prevSampler[i]);
    }
    g.activeTexture((GLenum)prevActive);
    g.bindVertexArray((GLuint)prevVao);
    g.useProgram((GLuint)prevProgram);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glColorMask(prevMask[0], prevMask[1], prevMask[2], prevMask[3]);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    RestoreGlState(saved);

    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (Check(xrReleaseSwapchainImage(s.hud.handle, &ri), "xrReleaseSwapchainImage(hud)")) {
        s.hud.hasImage = true;
    }
}

void RenderHudLayer(const StereoCallbacks& cb) {
    State& s = S();
    if (!EnsureHudCombiner()) {
        return;
    }
    const unsigned overBlack = cb.renderHud(cb.user, s.hud.width, s.hud.height, 0);
    if (overBlack == 0) {
        return;
    }
    CopyTexture(overBlack, s.hudTex[0], s.hud.width, s.hud.height);
    const int hudSprites = gGdxVrHudSpritesLastRun;
    const unsigned overWhite = cb.renderHud(cb.user, s.hud.width, s.hud.height, 1);
    if (overWhite == 0) {
        return;
    }
    CopyTexture(overWhite, s.hudTex[1], s.hud.width, s.hud.height);
    CombineHudIntoSwapchain();
    if (s.stereoFrames == DumpFrame()) {
        DumpTexture(s.hudTex[0], s.hud.width, s.hud.height, "vr-hud-black.bmp");
        DumpTexture(s.hudTex[1], s.hud.width, s.hud.height, "vr-hud-white.bmp");
    }
    static bool sHudSpriteDumped = false;
    if (DumpOnSprite() && !sHudSpriteDumped && hudSprites > 0) {
        sHudSpriteDumped = true;
        DumpTexture(s.hudTex[0], s.hud.width, s.hud.height, "vr-sprite-hud.bmp");
        gdx_port_logf("[openxr] HUD sprites drawn: %d\n", hudSprites);
    }
}

bool StereoWanted() {
    State& s = S();
    if (s.phase != Phase::Running || SettingInt(kSetStereo) == 0 || s.menuOpen) {
        return false; // the menu is a flat-quad affair: no stereo race while it is up
    }
    if (gNumPlayers != 1 || !s.raceDrawnLastTick) {
        return false;
    }
    const int mode = gGameMode & 0x1F;
    return mode == kModeGpRace || mode == kModePractice || mode == kModeTimeAttack || mode == kModeDeathRace;
}

} // namespace

void Configure(int argc, char** argv) {
    State& s = S();
    if (s.configured) {
        return;
    }
    s.configured = true;
    const char* env = std::getenv("GDX_VR");
    bool fromEnv = env != nullptr && env[0] != '\0' && std::strcmp(env, "0") != 0;
    bool fromArg = false;
    for (int i = 1; i < argc; i++) {
        if (argv[i] != nullptr && std::strcmp(argv[i], "--vr") == 0) {
            fromArg = true;
        }
    }
    s.requested = fromEnv || fromArg;
    s.phase = s.requested ? Phase::Uninit : Phase::Off;
    gGdxVrHudMarkerEnabled = s.requested ? 1 : 0;
    // Announced unconditionally: a run whose VR arm state is not in the log is not a test.
    gdx_port_logf("[openxr] VR mode %s (--vr=%d GDX_VR=%s)\n", s.requested ? "REQUESTED" : "off", fromArg ? 1 : 0,
                  env != nullptr ? env : "unset");
}

bool Requested() {
    return S().requested;
}

bool PacesHost() {
    return S().phase == Phase::Running;
}

bool ExitRequested() {
    return S().exitRequested;
}

bool StereoThisTick() {
    return S().stereoThisTick;
}

bool BeginHostIteration() {
    State& s = S();
    s.stereoThisTick = false;
    gGdxVrWideCull = 0;
    if (s.phase == Phase::Off || s.phase == Phase::Exiting) {
        return true;
    }
    if (s.phase == Phase::Uninit) {
        if (CreateEverything()) {
            s.phase = Phase::Idle;
        } else {
            gdx_port_logf("[openxr] VR unavailable; continuing on the desktop\n");
            DestroySession();
            if (s.instance != XR_NULL_HANDLE) {
                xrDestroyInstance(s.instance);
                s.instance = XR_NULL_HANDLE;
            }
            s.phase = Phase::Off;
            return true;
        }
    }

    PollEvents();
    if (s.quitRequestedS != 0.0 && !s.exitRequested && NowMs() * 0.001 - s.quitRequestedS > 3.0) {
        // The runtime did not take the exit request to EXITING in time: leave anyway.
        gdx_port_logf("[vr] quit: no EXITING from the runtime after 3 s, exiting\n");
        s.exitRequested = true;
    }
    if (s.phase != Phase::Running) {
        return true;
    }
    // libultraship saves the menu's visibility (gOpenMenuBar), so quitting with it open reopened it
    // over the next VR launch. In the headset it opens only on the Menu + View chord.
    if (!s.menuLaunchChecked) {
        s.menuLaunchChecked = true;
        if (GdxMenuIsVisible() && GdxMenuShow(false, nullptr)) {
            gdx_port_logf("[vr] menu closed at VR launch (it was saved open)\n");
        }
    }
    SyncMenuState("menu visibility changed");
    // A CVar edit in the menu (Cockpit view) takes effect now, as a Level recentre like the toggle.
    if (s.cameraModeSeen != SettingInt(kSetCameraMode)) {
        if (s.cameraModeSeen >= 0) {
            s.cockpit = SettingInt(kSetCameraMode) == 1;
            RequestRecentre(gdx_vr::RecentreKind::Level, "camera view setting");
            gdx_interp_mark_cut_src("vr-camera-mode");
            gdx_port_logf("[openxr] camera -> %s (setting)\n", s.cockpit ? "cockpit" : "chase");
        }
        s.cameraModeSeen = SettingInt(kSetCameraMode);
    }
    SyncInput();

    if (s.tickRanSinceDecision) {
        s.raceDrawnLastTick = gGdxVrRaceDrawCount != s.raceDrawCountSeen;
        s.raceDrawCountSeen = gGdxVrRaceDrawCount;
    }
    s.stereoThisTick = StereoWanted();
    gGdxVrWideCull = (s.stereoThisTick && SettingInt(kSetWideCull) != 0) ? 1 : 0;
    if (s.stereoThisTick != s.lastTickStereo) {
        gdx_port_logf("[openxr] presentation -> %s (mode=0x%02X players=%d)\n", s.stereoThisTick ? "stereo" : "flat",
                      gGameMode & 0x1F, gNumPlayers);
        s.lastTickStereo = s.stereoThisTick;
        // Each race starts (and each menu returns) facing forward.
        RequestRecentre(gdx_vr::RecentreKind::Level, "flat/stereo switch");
        gdx_interp_mark_cut_src("vr-presentation-switch");
    }
    if (s.stereoThisTick) {
        // The bridge waits on XR frames inside this tick's replay; the host just runs the tick.
        s.hostTick++;
        s.tickRanSinceDecision = true;
        return true;
    }

    if (!EnsurePending() || s.pending.ticksOwed > 0) {
        s.hostTick++;
        s.tickRanSinceDecision = true;
        return true;
    }
    s.tickRanSinceDecision = false;
    // No tick due for this frame: show the previous image again.
    s.xrFramesNoTick++;
    if (BeginPending()) {
        EndPendingWithQuad();
    } else {
        s.pending = PendingFrame{};
    }
    return false;
}

void PresentStereoTick(const StereoCallbacks& cb) {
    State& s = S();
    if (s.phase != Phase::Running || !s.stereoThisTick || cb.renderEye == nullptr) {
        return;
    }
    if (s.countedHostTick == s.hostTick) {
        return; // a later gfx task of a tick that already presented
    }
    // Everything below, including xrWaitFrame's blocking, runs inside the game tick; [vr-perf]
    // subtracts it from the tick time to get `logic`.
    struct PresentTimer {
        double t0 = NowMs();
        ~PresentTimer() { S().perfPresentMs += NowMs() - t0; }
    } presentTimer;
    s.countedHostTick = s.hostTick;
    CountTick();

    while (s.pending.waited && s.pending.ticksOwed == 0) {
        if (!BeginPending()) {
            s.pending = PendingFrame{};
            break;
        }
        XrCompositionLayerProjectionView projViews[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
                                                          { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
        XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        XrCompositionLayerQuad hudLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        const XrCompositionLayerBaseHeader* layers[2];
        uint32_t layerCount = 0;

        if (s.pending.state.shouldRender || ForceRender()) {
            XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = s.pending.state.predictedDisplayTime;
            vli.space = s.localSpace;
            XrViewState vs{ XR_TYPE_VIEW_STATE };
            XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
            uint32_t located = 0;
            bool poseOk = Check(xrLocateViews(s.session, &vli, &vs, 2, &located, views), "xrLocateViews") &&
                          located == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
            if (!poseOk && ForceRender()) {
                // Diagnostics without a tracked headset: a straight-ahead head at the origin.
                for (int e = 0; e < 2; e++) {
                    views[e].pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
                    views[e].pose.position = { e == 0 ? -0.032f : 0.032f, 0.0f, 0.0f };
                    views[e].fov = { -0.9f, 0.9f, 0.9f, -0.9f };
                }
                poseOk = true;
            }
            // Diagnostics: GDX_VR_DIAG_YAW=<degrees> turns the located head about LOCAL +y, so an
            // unattended forced-render run can capture what a head turn shows.
            static const float sDiagYawDeg = [] {
                const char* e = std::getenv("GDX_VR_DIAG_YAW");
                const float v = (e != nullptr && e[0] != '\0') ? std::strtof(e, nullptr) : 0.0f;
                if (v != 0.0f) {
                    gdx_port_logf("[openxr] GDX_VR_DIAG_YAW=%.1f\n", v);
                }
                return v;
            }();
            if (poseOk && sDiagYawDeg != 0.0f) {
                const float half = sDiagYawDeg * 3.14159265f / 360.0f;
                const Quat yaw = { 0.0f, std::sin(half), 0.0f, std::cos(half) };
                for (int e = 0; e < 2; e++) {
                    const Quat q = { views[e].pose.orientation.x, views[e].pose.orientation.y,
                                     views[e].pose.orientation.z, views[e].pose.orientation.w };
                    const Quat r = { yaw.w * q.x + yaw.x * q.w + yaw.y * q.z - yaw.z * q.y,
                                     yaw.w * q.y - yaw.x * q.z + yaw.y * q.w + yaw.z * q.x,
                                     yaw.w * q.z + yaw.x * q.y - yaw.y * q.x + yaw.z * q.w,
                                     yaw.w * q.w - yaw.x * q.x - yaw.y * q.y - yaw.z * q.z };
                    views[e].pose.orientation = { r.x, r.y, r.z, r.w };
                    const Vec3 p = Rotate(yaw, { views[e].pose.position.x, views[e].pose.position.y,
                                                 views[e].pose.position.z });
                    views[e].pose.position = { p.x, p.y, p.z };
                }
            }
            if (poseOk) {
                const float t = s.scheduler.InterpT((double)s.pending.state.predictedDisplayTime * 1e-9);
                for (int e = 0; e < 2; e++) {
                    s.activeEye.active = true;
                    s.activeEye.worldValid = false;
                    s.activeEye.pose = Recentred(views[e].pose);
                    s.activeEye.fov = { views[e].fov.angleLeft, views[e].fov.angleRight, views[e].fov.angleUp,
                                        views[e].fov.angleDown };
                    const double t0 = NowMs();
                    const unsigned tex = cb.renderEye(cb.user, e, t, s.eyes[e].width, s.eyes[e].height);
                    const double t1 = NowMs();
                    s.activeEye.active = false;
                    if (tex != 0) {
                        static bool sWorldSpriteDumped = false;
                        if (DumpOnSprite() && !sWorldSpriteDumped && gGdxVrWorldSpritesLastRun > 0) {
                            char path[64];
                            std::snprintf(path, sizeof(path), "vr-sprite-eye%d.bmp", e);
                            DumpTexture(tex, s.eyes[e].width, s.eyes[e].height, path);
                            gdx_port_logf("[openxr] world sprites placed in eye %d: %d\n", e, gGdxVrWorldSpritesLastRun);
                            if (e == 1) {
                                sWorldSpriteDumped = true;
                            }
                        }
                        if (DumpFrame() >= 0 && s.stereoFrames >= DumpFrame() &&
                            s.stereoFrames < DumpFrame() + DumpCount() && (DumpCount() == 1 || e == 0)) {
                            char path[64];
                            std::snprintf(path, sizeof(path), "vr-eye%d-%lld.bmp", e, s.stereoFrames);
                            DumpTexture(tex, s.eyes[e].width, s.eyes[e].height, path);
                        }
                        CopyIntoSwapchain(s.eyes[e], tex, s.eyes[e].width, s.eyes[e].height);
                    }
                    s.perfEyeMs[e] += t1 - t0;
                    s.perfIrunMs += t1 - t0;
                    s.copyMsSum += NowMs() - t1;
                    projViews[e].pose = views[e].pose;
                    projViews[e].fov = views[e].fov;
                    projViews[e].subImage.swapchain = s.eyes[e].handle;
                    projViews[e].subImage.imageRect.offset = { 0, 0 };
                    projViews[e].subImage.imageRect.extent = { s.eyes[e].width, s.eyes[e].height };
                }
                // The HUD only changes at the tick rate: draw it on the tick's first XR frame and
                // let later frames of the same tick show the released image again.
                if (cb.renderHud != nullptr && s.hudRenderedTick != s.hostTick) {
                    s.hudRenderedTick = s.hostTick;
                    const double hudT0 = NowMs();
                    RenderHudLayer(cb);
                    s.perfHudMs += NowMs() - hudT0;
                }
                if (s.eyes[0].hasImage && s.eyes[1].hasImage) {
                    if (s.pending.state.shouldRender) {
                        layer.space = s.localSpace;
                        layer.viewCount = 2;
                        layer.views = projViews;
                        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
                        if (s.hud.hasImage) {
                            hudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                            hudLayer.space = s.viewSpace;
                            hudLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                            hudLayer.subImage.swapchain = s.hud.handle;
                            hudLayer.subImage.imageRect.offset = { 0, 0 };
                            hudLayer.subImage.imageRect.extent = { s.hud.width, s.hud.height };
                            hudLayer.pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
                            hudLayer.pose.position = { 0.0f, 0.0f, -kHudDistance };
                            const float hudHeight = SettingFloat(kSetHudHeight);
                            hudLayer.size.height = hudHeight;
                            hudLayer.size.width = hudHeight * (float)s.hud.width / (float)s.hud.height;
                            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudLayer);
                        }
                    }
                    s.stereoFrames++;
                }
            }
        }
        EndPending(layers, layerCount);
        if (!EnsurePending()) {
            break;
        }
    }
}

void ClearBoundFramebufferColor(float r, float g, float b, float a) {
    // libultraship's ClearFramebuffer re-enables scissoring (with the last draw's rectangle) before
    // this hook runs, so clear with it off or only part of the target is cleared.
    GLfloat prev[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prev);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) {
        glDisable(GL_SCISSOR_TEST);
    }
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(prev[0], prev[1], prev[2], prev[3]);
    if (scissor) {
        glEnable(GL_SCISSOR_TEST);
    }
}

bool CockpitMode() {
    return S().cockpit;
}

void CockpitOffsets(int machine, float* forward, float* up, float* tilt) {
    *forward = SettingFloat(kSetRoofForward);
    *up = gdx_vr::CockpitRoofHeight(machine, *forward) + SettingFloat(kSetRoofClearance);
    *tilt = SettingFloat(kSetCockpitTilt);
}

bool BuildActiveEyeProjView(const float eye[3], const float at[3], const float up[3], float nearZ, float farZ,
                            float out[4][4], bool levelPitch, float unitsPerMeterIn) {
    State& s = S();
    if (!s.activeEye.active) {
        return false;
    }
    // The chase camera looks down at the machine; carried into the headset, that pitch tilts the
    // whole world and the viewer has to look up to see the track. Keep the camera's heading and its
    // up vector (the game's smoothed track up, so the view still banks with the track) but level
    // its forward onto the track plane: the viewer looks down at their machine like a drone behind.
    float lookAt[3] = { at[0], at[1], at[2] };
    float anchorUp[3] = { up[0], up[1], up[2] };
    if (levelPitch && SettingInt(kSetLevelPitch) != 0) {
        // The camera's own up is pitched with it, so level against the track's up instead.
        gdx_vr_camera_track_up(anchorUp);
        const float* up = anchorUp;
        const float ulen = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        if (ulen > 1e-6f) {
            const float u[3] = { up[0] / ulen, up[1] / ulen, up[2] / ulen };
            const float back[3] = { eye[0] - at[0], eye[1] - at[1], eye[2] - at[2] };
            const float d = back[0] * u[0] + back[1] * u[1] + back[2] * u[2];
            const float level[3] = { back[0] - d * u[0], back[1] - d * u[1], back[2] - d * u[2] };
            if (level[0] * level[0] + level[1] * level[1] + level[2] * level[2] > 1e-6f) {
                lookAt[0] = eye[0] - level[0];
                lookAt[1] = eye[1] - level[1];
                lookAt[2] = eye[2] - level[2];
            }
        }
    }
    Anchor anchor;
    if (!AnchorFromLookAt({ eye[0], eye[1], eye[2] }, { lookAt[0], lookAt[1], lookAt[2] },
                          { anchorUp[0], anchorUp[1], anchorUp[2] }, &anchor)) {
        return false;
    }
    if (SettingInt(kSetHorizonLock) != 0) {
        // Comfort: keep the chase camera's heading and pitch but not its roll, so the viewer stays
        // level while the track banks. World up is +y. Near-vertical views (inside a loop) have no
        // usable horizon and keep the full camera frame.
        Anchor level;
        if (AnchorFromLookAt({ eye[0], eye[1], eye[2] }, { at[0], at[1], at[2] }, { 0.0f, 1.0f, 0.0f }, &level) &&
            std::fabs(anchor.back.y) < 0.95f) {
            anchor = level;
        }
    }
    const float unitsPerMeter = unitsPerMeterIn > 0.0f ? unitsPerMeterIn : SettingFloat(kSetWorldScale);
    EyeProjView(anchor, s.activeEye.pose, s.activeEye.fov, unitsPerMeter, nearZ, farZ, out);
    const Vec3 offset = AnchorToWorld(anchor, s.activeEye.pose.position);
    s.activeEye.worldPos = { anchor.origin.x + offset.x * unitsPerMeter, anchor.origin.y + offset.y * unitsPerMeter,
                             anchor.origin.z + offset.z * unitsPerMeter };
    s.activeEye.worldForward = AnchorToWorld(anchor, Rotate(s.activeEye.pose.orientation, { 0.0f, 0.0f, -1.0f }));
    s.activeEye.worldUp = AnchorToWorld(anchor, Rotate(s.activeEye.pose.orientation, { 0.0f, 1.0f, 0.0f }));
    s.activeEye.worldValid = true;
    return true;
}

float ActiveEyeTanHalfY() {
    const ActiveEye& e = S().activeEye;
    if (!e.active) {
        return 0.0f;
    }
    return 0.5f * (std::tan(e.fov.angleUp) - std::tan(e.fov.angleDown));
}

bool ActiveEyeWorldFrame(float pos[3], float forward[3], float up[3], float* tanHalfX, float* tanHalfY) {
    const ActiveEye& e = S().activeEye;
    if (!e.active || !e.worldValid) {
        return false;
    }
    pos[0] = e.worldPos.x;
    pos[1] = e.worldPos.y;
    pos[2] = e.worldPos.z;
    forward[0] = e.worldForward.x;
    forward[1] = e.worldForward.y;
    forward[2] = e.worldForward.z;
    up[0] = e.worldUp.x;
    up[1] = e.worldUp.y;
    up[2] = e.worldUp.z;
    *tanHalfX = std::fmax(std::fabs(std::tan(e.fov.angleLeft)), std::fabs(std::tan(e.fov.angleRight)));
    *tanHalfY = std::fmax(std::fabs(std::tan(e.fov.angleUp)), std::fabs(std::tan(e.fov.angleDown)));
    return true;
}

void QuitToSteam() {
    State& s = S();
    if (s.session != XR_NULL_HANDLE && s.phase == Phase::Running) {
        gdx_port_logf("[vr] quit to Steam: xrRequestExitSession\n");
        if (XR_SUCCEEDED(xrRequestExitSession(s.session))) {
            s.quitRequestedS = NowMs() * 0.001;
            return;
        }
    }
    gdx_port_logf("[vr] quit to Steam: no running session, exiting\n");
    s.exitRequested = true;
}

void ResetSettingsToDefaults() {
    for (int i = 0; i < kSettingCount; i++) {
        const SettingInfo& si = Setting((SettingId)i);
        if (si.isInteger) {
            CVarSetInteger(si.cvar, (int32_t)si.def);
        } else {
            CVarSetFloat(si.cvar, (float)si.def);
        }
    }
    CVarSave();
    gdx_port_logf("[vr] settings reset to defaults\n");
}

void Shutdown() {
    State& s = S();
    gdx_imgui_nav_set_external(nullptr);
    if (s.session != XR_NULL_HANDLE && s.phase == Phase::Running) {
        xrRequestExitSession(s.session);
    }
    DestroySession();
    if (s.instance != XR_NULL_HANDLE) {
        xrDestroyInstance(s.instance);
        s.instance = XR_NULL_HANDLE;
    }
    if (s.phase != Phase::Off) {
        gdx_port_logf("[openxr] shut down\n");
    }
    s.phase = Phase::Off;
}

} // namespace gdx_vr

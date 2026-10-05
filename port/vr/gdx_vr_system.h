// port/vr/gdx_vr_system.h -- the VR "system layer" and settings table, as pure policy.
//
// The cross-port standard (steam-frame-vr-port, standard.md section 1) puts the same system actions
// on the same physical inputs in every VR port. For F-Zero X that is the left controller's View
// button on the Steam Frame:
//   short press  N64 B, only outside a stereo race (flat menus), so it is Back there and never
//                collides with in-race boost
//   hold 1 s     recentre (yaw and horizontal position; the standing-height anchor is kept)
//   hold 3 s     recentre and re-anchor the height too (the 1 s recentre has already fired)
// Both holds buzz both hands (SFVR_HAPTIC_SYSTEM: 0.6 amplitude, 80 ms, then scaled by the
// gEnhancements.VR.Haptics setting).
//
// Nothing here talks to OpenXR or reads a clock: the caller passes the button state and a monotonic
// time in seconds, so hold timing is unit-tested without a runtime (tests/gdx_vr_system_tests.cpp).
// The settings table is likewise data only: the CVar names, defaults and ranges that
// gdx_openxr.cpp registers, clamps on load and clamps again on every read.

#ifndef GDX_VR_SYSTEM_H
#define GDX_VR_SYSTEM_H

#include <sfvr/sfvr_view.h>

namespace gdx_vr {

// What a pending recentre should re-anchor. Level keeps the height anchor; Height replaces it too.
// (Idle, not None: Xlib #defines None, and this header is included after X11.)
enum class RecentreKind { Idle, Level, Height };

// Two requests landing before the next stereo frame boundary merge into the stronger one.
inline RecentreKind MergeRecentre(RecentreKind pending, RecentreKind requested) {
    return static_cast<int>(requested) > static_cast<int>(pending) ? requested : pending;
}

struct ViewEvents {
    bool shortPress = false;      // released before the recentre hold: fires on release
    bool recentre = false;        // held 1 s (fires once per press)
    bool recentreHeight = false;  // held 3 s (fires once per press; replaces recentre if both are due)
};

// Thin adapter over sfvr_view (port/third_party/sfvr, the cross-port library that owns the timing
// rules). The adapter only maps the bitmask onto this port's three events; where sfvr reports both
// hold events from one very late update, the height recentre supersedes the 1 s one.
// Note sfvr's rule: a press already down on the first update after construction or Reset() is
// ignored until the button is released and pressed again.
class ViewButton {
  public:
    static constexpr double kRecentreSeconds = SFVR_VIEW_DEFAULT_RECENTRE_HOLD_S;
    static constexpr double kHeightSeconds = SFVR_VIEW_DEFAULT_HEIGHT_HOLD_S;

    // Feed the button state once per host iteration. `nowSeconds` must be monotonic.
    ViewEvents Update(bool down, double nowSeconds);

    // Forget a press in progress without reporting it (focus loss, session stop).
    void Reset();

  private:
    sfvr_view_button button_{};
};

// gEnhancements.VR.* CVars. Registered with these defaults, clamped to [lo, hi] on load and on every
// read (a hand-edited config or a later settings UI can write anything).
enum SettingId {
    kSetStereo,
    kSetWideCull,
    kSetCameraMode,
    kSetHorizonLock,
    kSetLevelPitch,
    kSetHudAspect,
    kSetHudHeight,
    kSetRoofForward, // cockpit eye, world units along the machine's forward from its origin
    kSetRoofClearance, // cockpit eye, world units above the machine's roof (CockpitRoofHeight)
    kSetCockpitTilt, // cockpit view follows the machine's lean and tilt: 0 travel frame .. 1 the body's
    kSetCockpitStyle, // 0: on the roof; 1: inside a cabin (gdx_vr_cabin_table.h); roof when a machine has none
    kSetCabinSeatUp, // cabin eye offset from the measured seat, world units
    kSetCabinSeatForward,
    kSetDashHudSize, // cabin dash HUD panels, as a fraction of the size that fills the dash
    kSetCabinMirror, // rear-view mirror in the cabin: one extra low-resolution render per tick
    kSetWorldScale,
    kSetHaptics, // scale for every haptic pulse (standard.md section 6), 0 = off
    kSetRefreshRate, // Hz ceiling for the display refresh-rate request (XR_FB_display_refresh_rate)
    kSetResolutionScale, // eye image size = runtime-recommended size x this; applies at the next VR start
    kSetDisplayStandard, // settings-migration version for the display defaults (kDisplayStandardVersion)
    kSettingCount
};

struct SettingInfo {
    const char* cvar;
    bool isInteger; // 0/1 switches and integer-valued settings; floats otherwise
    double def;
    double lo;
    double hi;
};

const SettingInfo& Setting(SettingId id);

// Two buttons held together for `holdSeconds` fire once (the headset menu chord: Menu + View, 0.5 s).
// After it fires, Latched() stays true until both buttons are up, so the caller can keep the chord's
// buttons away from the game (START must not pause the race behind the menu).
class Chord {
  public:
    explicit Chord(double holdSeconds = 0.5) : hold_(holdSeconds) {}

    // Returns true on the update where the chord fires (once per hold).
    bool Update(bool a, bool b, double nowSeconds);
    bool Latched() const { return fired_; }
    void Reset();

  private:
    double hold_;
    double since_ = 0.0;
    bool both_ = false;
    bool fired_ = false;
};

// A stick axis read as a D-pad direction for menu navigation, with hysteresis: it turns on above
// 0.55 and stays on until it falls below 0.35. `positive` is the axis value signed toward the
// direction (so "down" passes -y).
bool StickAsDpad(bool wasOn, float positive);

// Menu pointer trigger as a click, with hysteresis: down above 0.6, up again below 0.45
// (standard.md section 3), so a trigger resting near one threshold does not chatter.
bool TriggerClick(bool wasDown, float value);

// The highest of `rates` (Hz, as enumerated by the runtime) that is <= hz, allowing 0.5 Hz of slack
// for values such as 89.99; 0 when none qualifies or the list is empty.
float PickRefreshRate(const float* rates, unsigned count, float hz);

// The next rate down the fallback ladder: the highest of `rates` that is below `current` by more than
// 0.5 Hz and is not under kRefreshFloorHz; 0 when there is none (already at the bottom, or the runtime
// offers nothing lower). With 120, 90 and 72 offered that is 120 -> 90 -> 72, but it is picked from
// what the runtime enumerates, not from a fixed list.
constexpr float kRefreshFloorHz = 72.0f;
float NextLowerRefreshRate(const float* rates, unsigned count, float current);

// What RefreshWatch decided for one frame.
struct RefreshDecision {
    bool stepDown = false; // request `next` now (the ladder has run out of patience at the current rate)
    float next = 0.0f;     // the lower enumerated rate to ask for
    double fps = 0.0;      // the frame rate that failed the last window (for the log)
};

// The fallback ladder's bookkeeping, pure so it is unit-tested without a runtime or a clock. Fed once
// per XR frame while a rate `target` is requested. A window is 10 s; one that ends under 90% of the
// target while focused is a miss, a good one clears the misses, and two misses in a row step down to
// the next lower enumerated rate. Windows only count while worn: any focus change restarts the window
// and forgets the misses. After a step the caller calls Restart() (the new rate gets two fresh windows),
// so 120 -> 90 -> 72 needs four bad windows in all, and nothing below 72.
class RefreshWatch {
  public:
    static constexpr double kWindowSeconds = 10.0;
    static constexpr double kMissFraction = 0.9;
    static constexpr int kMissesBeforeStep = 2;

    RefreshDecision Frame(double nowSeconds, bool focused, float target, const float* rates, unsigned count);

    // A new rate was requested (or the setting changed): measure from scratch.
    void Restart();

    int Misses() const { return misses_; }

  private:
    bool started_ = false;
    bool windowFocused_ = false;
    double windowStart_ = 0.0;
    long long frames_ = 0;
    int misses_ = 0;
};

// Eye image size on one axis: round(recommended x scale), even, held to the view's and the swapchain's
// largest sizes (a limit of 0 means the runtime gave none). The scale is clamped to the setting's range
// first. Never returns less than 2.
int EyeImageSize(int recommended, double scale, int maxImageRect, int maxSwapchain);

// Display-standard migration (standard.md section 9). Configs from before it saved the old default
// RefreshRate 90; those move to the new default 120 once, and only a saved 90 (a deliberate 72 stays).
// `savedVersion` is the stored DisplayStandard (0 when absent), `haveRate` whether a RefreshRate was
// saved at all (a fresh config gets the new default anyway), `savedRate` its value.
constexpr int kDisplayStandardVersion = 1;
struct DisplayMigration {
    bool changed = false;  // anything to write back
    int version = 0;       // DisplayStandard to store
    bool setRate = false;  // RefreshRate should be rewritten
    int rate = 0;          // ... to this
};
DisplayMigration MigrateDisplayStandard(int savedVersion, bool haveRate, double savedRate);

// Cockpit view: the height (world units above the machine's underside) of the roof the eye must see
// over, for an eye `forward` world units ahead of the machine's origin. `machine` is the stock model
// index (sMachineDrawFuncs order: 0..29 pilots, 30.. super machines); -1, or any model this table
// does not describe (edited custom machines, model packs), gets kCockpitFallbackRoof. Heights come
// from the stock LOD1 meshes (gdx_vr_roof_table.h) and are interpolated between table rows.
constexpr float kCockpitFallbackRoof = 15.0f;
float CockpitRoofHeight(int machine, float forward);

// Clamp `value` into the setting's range. NaN and infinities fall back to the default; integer
// settings are rounded to the nearest integer first.
double SanitizeSetting(SettingId id, double value);

} // namespace gdx_vr

#endif

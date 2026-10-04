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

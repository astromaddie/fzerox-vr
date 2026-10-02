#include "gdx_vr_system.h"

#include <cmath>

namespace gdx_vr {

ViewEvents ViewButton::Update(bool down, double nowSeconds) {
    const unsigned bits = sfvr_view_update(&button_, nullptr, down ? 1 : 0, nowSeconds);
    ViewEvents ev;
    ev.shortPress = (bits & SFVR_VIEW_SHORT_PRESS) != 0;
    ev.recentreHeight = (bits & SFVR_VIEW_RECALIBRATE_HEIGHT) != 0;
    ev.recentre = (bits & SFVR_VIEW_RECENTRE) != 0 && !ev.recentreHeight;
    return ev;
}

void ViewButton::Reset() {
    sfvr_view_reset(&button_);
}

namespace {

// Ranges are deliberately generous: they exist to reject nonsense (NaN, a zero or negative scale, a
// HUD a kilometre tall), not to second-guess a tuned value.
const SettingInfo kSettings[kSettingCount] = {
    { "gEnhancements.VR.Stereo", true, 1.0, 0.0, 1.0 },
    { "gEnhancements.VR.WideCull", true, 1.0, 0.0, 1.0 },
    { "gEnhancements.VR.CameraMode", true, 0.0, 0.0, 1.0 },
    { "gEnhancements.VR.HorizonLock", true, 0.0, 0.0, 1.0 },
    { "gEnhancements.VR.LevelPitch", true, 1.0, 0.0, 1.0 },
    { "gEnhancements.VR.HudAspect", false, 1.5, 4.0 / 3.0, 4.0 }, // swapchain width / height
    { "gEnhancements.VR.HudHeight", false, 1.2, 0.3, 3.0 },       // metres
    { "gEnhancements.VR.NoseForward", false, 8.0, -40.0, 40.0 },  // cockpit eye offset, world units
    { "gEnhancements.VR.NoseUp", false, 6.0, -40.0, 40.0 },
    { "gEnhancements.VR.WorldScale", false, 10.0, 2.0, 50.0 },    // world units per metre
    { "gEnhancements.VR.Haptics", false, 0.6, 0.0, 1.0 },         // pulse strength scale
};

} // namespace

const SettingInfo& Setting(SettingId id) {
    return kSettings[id];
}

double SanitizeSetting(SettingId id, double value) {
    const SettingInfo& s = kSettings[id];
    if (!std::isfinite(value)) {
        return s.def;
    }
    if (s.isInteger) {
        value = std::floor(value + 0.5);
    }
    if (value < s.lo) {
        return s.lo;
    }
    if (value > s.hi) {
        return s.hi;
    }
    return value;
}

} // namespace gdx_vr

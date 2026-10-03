#include "gdx_vr_system.h"
#include "gdx_vr_roof_table.h"

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
    { "gEnhancements.VR.CameraMode", true, 1.0, 0.0, 1.0 },   // 1 cockpit (worn default)
    { "gEnhancements.VR.HorizonLock", true, 0.0, 0.0, 1.0 },
    { "gEnhancements.VR.LevelPitch", true, 1.0, 0.0, 1.0 },
    { "gEnhancements.VR.HudAspect", false, 1.5, 4.0 / 3.0, 4.0 }, // swapchain width / height
    { "gEnhancements.VR.HudHeight", false, 1.2, 0.3, 3.0 },       // metres
    { "gEnhancements.VR.RoofForward", false, 5.8, -12.0, 12.0 },  // cockpit eye, world units
    { "gEnhancements.VR.RoofClearance", false, 3.5, 0.0, 20.0 },  // (cockpit defaults: worn tuning)
    { "gEnhancements.VR.CockpitTilt", false, 0.5, 0.0, 1.0 },     // 0 travel frame .. 1 machine body
    { "gEnhancements.VR.WorldScale", false, 1.0, 1.0, 50.0 },     // world units per metre
    { "gEnhancements.VR.Haptics", false, 0.6, 0.0, 1.0 },         // pulse strength scale
    { "gEnhancements.VR.RefreshRate", true, 90.0, 60.0, 144.0 },  // Hz ceiling, highest offered <= it
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

bool Chord::Update(bool a, bool b, double nowSeconds) {
    bool fires = false;
    if (a && b) {
        if (!both_) {
            both_ = true;
            since_ = nowSeconds;
        } else if (!fired_ && nowSeconds - since_ >= hold_) {
            fired_ = true;
            fires = true;
        }
    } else {
        both_ = false;
        if (!a && !b) {
            fired_ = false;
        }
    }
    return fires;
}

void Chord::Reset() {
    both_ = false;
    fired_ = false;
}

bool StickAsDpad(bool wasOn, float positive) {
    return wasOn ? positive > 0.35f : positive > 0.55f;
}

float CockpitRoofHeight(int machine, float forward) {
    if (machine < 0 || machine >= kRoofMachineCount || !std::isfinite(forward)) {
        return kCockpitFallbackRoof;
    }
    const float* h = kRoofHeight[machine];
    if (forward <= kRoofForward[0]) {
        return h[0];
    }
    for (int i = 1; i < kRoofForwardCount; i++) {
        if (forward <= kRoofForward[i]) {
            const float t = (forward - kRoofForward[i - 1]) / (kRoofForward[i] - kRoofForward[i - 1]);
            return h[i - 1] + (h[i] - h[i - 1]) * t;
        }
    }
    return h[kRoofForwardCount - 1];
}

bool TriggerClick(bool wasDown, float value) {
    return wasDown ? value > 0.45f : value > 0.6f;
}

float PickRefreshRate(const float* rates, unsigned count, float hz) {
    float chosen = 0.0f;
    for (unsigned i = 0; i < count; i++) {
        if (rates[i] <= hz + 0.5f && rates[i] > chosen) {
            chosen = rates[i];
        }
    }
    return chosen;
}

} // namespace gdx_vr

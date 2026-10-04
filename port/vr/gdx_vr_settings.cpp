// port/vr/gdx_vr_settings.cpp -- see gdx_vr_settings.h.
//
// libultraship keeps CVars in the config file (CVarSave writes every CVar, Context startup loads
// them), but only values someone saved are in it, and nothing here ever set these. So the first VR
// launch registers every key with its default and saves, which makes them appear (and persist) in
// the config file; later launches load whatever is there. Anything out of range (a hand-edited
// file, NaN) is clamped on load and written back, and every read clamps again because a later
// settings UI or a console command can write any value. Ranges are in gdx_vr_system.cpp.

#include "gdx_vr_settings.h"

#include <cmath>
#include <cstdint>
#include <limits>

#include "../port_log.h"

extern "C" int32_t CVarGetInteger(const char* name, int32_t defaultValue);
extern "C" float CVarGetFloat(const char* name, float defaultValue);
extern "C" void CVarSetInteger(const char* name, int32_t value);
extern "C" void CVarSetFloat(const char* name, float value);
extern "C" void CVarRegisterInteger(const char* name, int32_t defaultValue);
extern "C" void CVarRegisterFloat(const char* name, float defaultValue);
extern "C" void CVarClear(const char* name);
extern "C" void CVarSave();

namespace gdx_vr {

int SettingInt(SettingId id) {
    const SettingInfo& si = Setting(id);
    return (int)SanitizeSetting(id, (double)CVarGetInteger(si.cvar, (int32_t)si.def));
}

float SettingFloat(SettingId id) {
    const SettingInfo& si = Setting(id);
    return (float)SanitizeSetting(id, (double)CVarGetFloat(si.cvar, (float)si.def));
}

bool InitSettings() {
    // libultraship's typed getters ignore a CVar of the other type (a hand-edited `"HudHeight": 2`
    // loads as an integer), so a value of the wrong type is read through the other getter and
    // rewritten with the right type instead of being dropped.
    const int32_t kAbsent = INT32_MIN;
    const float kNaN = std::numeric_limits<float>::quiet_NaN();
    bool dirty = false;
    for (int i = 0; i < kSettingCount; i++) {
        const SettingId id = (SettingId)i;
        const SettingInfo& si = Setting(id);
        const int32_t asInt = CVarGetInteger(si.cvar, kAbsent);
        const float asFloat = CVarGetFloat(si.cvar, kNaN);
        const bool haveInt = asInt != kAbsent;
        const bool haveFloat = !std::isnan(asFloat);
        if (!haveInt && !haveFloat) {
            // Missing, or a type neither getter reads (a string or colour). Register only sets a
            // CVar that does not exist, so clear first or a `"WorldScale": "x"` would be kept and
            // re-saved on every launch.
            CVarClear(si.cvar);
            si.isInteger ? CVarRegisterInteger(si.cvar, (int32_t)si.def) : CVarRegisterFloat(si.cvar, (float)si.def);
            dirty = true;
            continue;
        }
        const double raw = si.isInteger ? (haveInt ? (double)asInt : (double)asFloat)
                                        : (haveFloat ? (double)asFloat : (double)asInt);
        const double fixed = SanitizeSetting(id, raw);
        const bool wrongType = si.isInteger ? !haveInt : !haveFloat;
        if (fixed != raw || wrongType) {
            if (fixed != raw) {
                gdx_port_logf("[vr] setting %s=%g out of range, clamped to %g\n", si.cvar, raw, fixed);
            }
            si.isInteger ? CVarSetInteger(si.cvar, (int32_t)fixed) : CVarSetFloat(si.cvar, (float)fixed);
            dirty = true;
        }
    }
    if (dirty) {
        CVarSave();
    }
    gdx_port_logf("[vr] settings: stereo=%d wide_cull=%d camera_mode=%d horizon_lock=%d level_pitch=%d "
                  "hud_aspect=%.3f hud_height=%.2f roof_forward=%.1f roof_clearance=%.1f cockpit_tilt=%.2f cockpit_style=%d cabin_seat=%.1f/%.1f world_scale=%.1f haptics=%.2f refresh_rate=%d%s\n",
                  SettingInt(kSetStereo), SettingInt(kSetWideCull), SettingInt(kSetCameraMode),
                  SettingInt(kSetHorizonLock), SettingInt(kSetLevelPitch), SettingFloat(kSetHudAspect), SettingFloat(kSetHudHeight),
                  SettingFloat(kSetRoofForward), SettingFloat(kSetRoofClearance), SettingFloat(kSetCockpitTilt),
                  SettingInt(kSetCockpitStyle), SettingFloat(kSetCabinSeatUp), SettingFloat(kSetCabinSeatForward),
                  SettingFloat(kSetWorldScale),
                  SettingFloat(kSetHaptics), SettingInt(kSetRefreshRate), dirty ? " (saved)" : "");
    return dirty;
}

} // namespace gdx_vr

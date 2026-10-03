#pragma once

// gEnhancements.VR.* CVars: load, repair and save. Split out of gdx_openxr.cpp so the first-launch
// path (keys missing from the config, keys of the wrong type) can be run by a console test against
// libultraship's real Config / ConsoleVariable code without OpenXR or a window.

#include "gdx_vr_system.h"

namespace gdx_vr {

// The setting's current value, clamped to its range (gdx_vr_system.cpp).
int SettingInt(SettingId id);
float SettingFloat(SettingId id);

// Registers every missing key with its default, rewrites out-of-range values and values of the wrong
// CVar type, and saves the config if anything changed. Logs a `[vr] settings:` line. Returns true
// when it saved.
bool InitSettings();

} // namespace gdx_vr

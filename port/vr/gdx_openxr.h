// port/vr/gdx_openxr.h — optional OpenXR output mode (GDX_OPENXR builds only).
//
// Two presentation modes share one XR session and one frame protocol:
//   * Flat: the normal mono game image (including the F1 menu) on an OpenXR quad layer in front of
//     the viewer. Used for every screen that is not a one-player race.
//   * Stereo: during a one-player race the retained display list of each tick is replayed once per
//     eye, with the game camera's projection-view matrix replaced by each eye's (gdx_vr_math.h),
//     and submitted as a projection layer.
//
// The OpenXR runtime owns the frame clock while a session runs. Each XR frame is waited on once;
// the display-time scheduler (gdx_vr_scheduler.h) then says how many 60 Hz simulation ticks must
// complete before that frame can be shown. The frame stays pending across those ticks, and is
// shown by whichever path runs after the last one: the pre-present hook in flat mode, the gfx
// bridge in stereo mode. Game speed never follows the display rate.
//
// Activation is a launch-time choice (`--vr` or GDX_VR=1). Without it nothing here runs: no
// OpenXR instance, no hook, no pacing change.

#ifndef GDX_OPENXR_H
#define GDX_OPENXR_H

namespace gdx_vr {

// Read `--vr` / GDX_VR once. Logs the decision. Call before the host loop.
void Configure(int argc, char** argv);

// True when VR was requested at launch (whether or not a session ever starts).
bool Requested();

// True while an XR session is running and the runtime clocks the host loop.
bool PacesHost();

// Called once per host-loop iteration, after input polling and before the game tick. Creates the
// instance/session on first use (the GL context must be current), pumps OpenXR events and decides
// this tick's presentation mode. In flat mode it may block in xrWaitFrame, and returns false when
// that XR frame needs no simulation tick (the frame has been resubmitted with the previous image
// and the caller should end the iteration without running the game). Returns true otherwise,
// including whenever no session is running.
bool BeginHostIteration();

// True when the tick about to run (latched by BeginHostIteration) is presented in stereo by the
// gfx bridge rather than by the desktop present path.
bool StereoThisTick();

// Stereo presentation, called by the gfx bridge once the tick's display list is converted and its
// interpolation records exist. For each XR frame due within this tick it calls renderEye for the
// left (0) and right (1) eye; renderEye must replay the display list into a width x height eye target at
// interpolation t and return the GL texture holding the result (stored top-down), or 0.
// renderHud replays only the race HUD into a width x height target cleared to opaque black
// (whiteBackground == 0) or opaque white (1) and returns its texture the same way; the presenter
// recovers the HUD's coverage from the two. Called twice per tick at most; may be null.
struct StereoCallbacks {
    void* user;
    unsigned (*renderEye)(void* user, int eye, float t, int width, int height);
    unsigned (*renderHud)(void* user, int width, int height, int whiteBackground);
};

// Clear the currently bound draw framebuffer's colour. For the bridge's HUD pass (installed as the
// interpreter's after-clear hook for that one replay).
void ClearBoundFramebufferColor(float r, float g, float b, float a);
void PresentStereoTick(const StereoCallbacks& callbacks);

// For the bridge's camera-matrix rebuild during an eye replay: the projection-view matrix of the
// active eye, anchored on the given (interpolated) game camera. Returns false outside an eye
// replay or on degenerate camera input; `out` is then untouched.
// levelPitch: level the forward direction against the track's up (chase view; the chase camera
// looks down at the machine). The cockpit view passes false.
// unitsPerMeter: world units per tracked metre; 0 uses gEnhancements.VR.WorldScale (the chase view).
bool BuildActiveEyeProjView(const float eye[3], const float at[3], const float up[3], float nearZ, float farZ,
                            float out[4][4], bool levelPitch, float unitsPerMeter);

// Cockpit view: the viewer rides on the machine's roof instead of the chase camera.
// Toggled with the Steam Frame controller's right B button; starts from gEnhancements.VR.CameraMode
// (0 chase, 1 cockpit).
bool CockpitMode();
// Where the cockpit eye sits relative to the machine's origin, in world units along its forward and
// up axes: gEnhancements.VR.RoofForward ahead, then that model's roof height there
// (gdx_vr::CockpitRoofHeight; `machine` as returned by gdx_vr_read_player_vehicle) plus
// gEnhancements.VR.RoofClearance. `tilt` (gEnhancements.VR.CockpitTilt) blends the view's up from the
// machine's travel frame (0) to its body, lean and tilt included (1).
// `side`: world units along the machine's model +x (up x forward); the cabin seat is off-centre in
// Twin Noritta's hull, 0 everywhere else.
void CockpitOffsets(int machine, float* forward, float* up, float* side, float* tilt);
// The followed machine's body frame (world position, and its model +x, up and forward axes) for this
// frame's cockpit eyes, so the cabin dash can carry the energy and speed HUD crops.
void SetCockpitBody(const float pos[3], const float side[3], const float up[3], const float fwd[3], int machine);

// World-space frame of the active eye as last built by BuildActiveEyeProjView (position, forward,
// up), with the tangents of its widest horizontal and vertical half-angles. For per-eye rebuilds
// of camera-facing geometry such as the backdrop. False when no eye matrix was built this replay.
bool ActiveEyeWorldFrame(float pos[3], float forward[3], float up[3], float* tanHalfX, float* tanHalfY);

// Mean vertical half-FOV tangent of the eye being rendered (0 outside an eye replay).
float ActiveEyeTanHalfY();

// In-headset menu support (the G-Diffuser menu's VR page, opened with Menu + View held 0.5 s).
// "Quit to Steam": request a clean session exit (xrRequestExitSession); the host loop leaves once the
// runtime reaches EXITING, or after 3 s regardless. Without a running session it exits directly.
void QuitToSteam();
// Write every gEnhancements.VR.* CVar back to its default and save.
void ResetSettingsToDefaults();

// True when the runtime has asked the application to exit (session EXITING / instance loss).
bool ExitRequested();

// Tear down the session and instance. Safe to call when nothing was created.
void Shutdown();

} // namespace gdx_vr

#endif // GDX_OPENXR_H

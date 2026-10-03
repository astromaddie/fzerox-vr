// port/gdx_imgui_nav.h — gamepad menu navigation glue for ImGui.
//
// Two jobs:
//
// 1. Fallback gamepad feed. libultraship drives ImGui gamepad nav through the platform backend's
//    own reader. The SDL2 backend feeds every gamepad key itself; the Win32 backend reads XInput,
//    which cannot see a raw DualSense, so libultraship compiles its gamepad path out and nothing
//    feeds ImGui on DX11. This fills that gap for any SDL-recognized pad, and ONLY when no backend
//    has claimed ImGuiBackendFlags_HasGamepad — two writers on one ImGuiKey stall ImGui's event
//    queue (see the check in gdx_imgui_nav.cpp).
//
// 2. Keeps ImGuiConfigFlags_NavEnableGamepad in sync with Gui's effective navigation policy every
//    frame; libultraship only re-evaluates it when its own toggle key fires.
//
// Called from GdxFast3dGui::ImGuiWMNewFrame (port/gdx_gui.cpp): after the platform backend's
// per-frame gamepad poll and before ImGui::NewFrame consumes the queued events. Both halves of that
// ordering are load-bearing.

#ifdef __cplusplus
extern "C" {
#endif

// An external navigation pad: a headset host that has controller state but no SDL gamepad (the VR
// session reads the controllers through OpenXR actions). Fields are levels, not edges. While one is
// installed it is the only gamepad source: the SDL2 platform backend is switched to manual mode
// with no gamepads (it would otherwise feed Steam's idle virtual pad over it), and restored when
// the pad is removed. Set it every host iteration; NULL (or never calling) removes it. `back` is
// the pad's Back button (libultraship toggles the menu on it), stick values are -1..1 with +y up.
// The pointer fields drive ImGui's mouse: a laser hit at (pointer_x, pointer_y), 0..1 across the
// menu image left to right and top to bottom, with ImGui's own cursor drawn there.
typedef struct GdxImguiNavPad {
    int up, down, left, right; // D-pad
    int confirm;               // bottom face button: activate
    int cancel;                // right face button: back out of a widget or page
    int back;                  // Back/View: close the menu
    int l1, r1;                // shoulder buttons: tab switching, slider fine/coarse
    float stick_x, stick_y;    // left stick: scroll
    int pointer_active;        // the laser is on the menu image
    float pointer_x, pointer_y; // 0..1, image order
    int pointer_down;          // left mouse button
    float wheel_y;             // mouse wheel this frame, +up
} GdxImguiNavPad;

void gdx_imgui_nav_set_external(const GdxImguiNavPad* pad);

// Sync the nav config flag and, where no backend feeds one, publish the SDL controller's state as
// ImGui gamepad keys. Safe to call every frame from boot; no-op without a controller.
void gdx_imgui_nav_tick(void);

#ifdef __cplusplus
}
#endif

/* Standalone unit-test harness for port/vr/gdx_vr_system.cpp.
 *
 * Checks the VR system layer's View-button timing (short press / 1 s recentre / 3 s height
 * recentre), the recentre-request merge rule, and the VR settings table's clamping. No OpenXR, no
 * window, no game data.
 */
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "../vr/gdx_vr_system.h"

static int gChecks = 0;
static int gFails = 0;

static void check(const char* what, bool ok) {
    gChecks++;
    if (!ok) {
        gFails++;
        std::printf("    [x] %s\n", what);
    }
}

using gdx_vr::ViewButton;
using gdx_vr::ViewEvents;

// sfvr ignores a press that is already down on the very first update, so a test button first sees
// the button released (what the host does: it updates every iteration from startup).
static ViewButton Primed() {
    ViewButton b;
    b.Update(false, -100.0);
    return b;
}

// Feed a button held from t0 to t1 at ~90 Hz and count what fired.
struct Tally {
    int shortPress = 0;
    int recentre = 0;
    int height = 0;
    double firstRecentre = -1.0;
    double firstHeight = -1.0;
};

static void Feed(ViewButton& b, Tally& t, bool down, double from, double to, double step = 1.0 / 90.0) {
    for (double now = from; now < to; now += step) {
        const ViewEvents ev = b.Update(down, now);
        t.shortPress += ev.shortPress;
        if (ev.recentre) {
            t.recentre++;
            if (t.firstRecentre < 0) t.firstRecentre = now;
        }
        if (ev.recentreHeight) {
            t.height++;
            if (t.firstHeight < 0) t.firstHeight = now;
        }
    }
}

static void CaseShortPress() {
    ViewButton b = Primed();
    Tally t;
    Feed(b, t, false, 0.0, 1.0);
    Feed(b, t, true, 1.0, 1.4);  // 0.4 s tap
    check("tap: nothing fires while held", t.shortPress == 0 && t.recentre == 0 && t.height == 0);
    Feed(b, t, false, 1.4, 2.0);
    check("tap: exactly one short press on release", t.shortPress == 1 && t.recentre == 0 && t.height == 0);
}

static void CaseJustUnderHold() {
    ViewButton b = Primed();
    b.Update(true, 10.0);
    ViewEvents ev = b.Update(true, 10.0 + ViewButton::kRecentreSeconds - 0.001);
    check("0.999 s: no recentre yet", !ev.recentre && !ev.recentreHeight);
    ev = b.Update(false, 10.0 + ViewButton::kRecentreSeconds - 0.0005);
    check("0.999 s release is still a short press", ev.shortPress);
}

static void CaseRecentreHold() {
    ViewButton b = Primed();
    Tally t;
    Feed(b, t, true, 5.0, 7.0); // 2 s hold
    check("2 s hold: one level recentre at ~1 s", t.recentre == 1 && t.height == 0);
    check("2 s hold: recentre fires at the 1 s mark", std::fabs(t.firstRecentre - 6.0) < 0.02);
    Feed(b, t, false, 7.0, 7.5);
    check("2 s hold: no short press on release", t.shortPress == 0);
}

static void CaseHeightHold() {
    ViewButton b = Primed();
    Tally t;
    Feed(b, t, true, 0.0, 4.0); // 4 s hold
    check("4 s hold: one level recentre and one height recentre", t.recentre == 1 && t.height == 1);
    check("4 s hold: height fires at the 3 s mark, after the 1 s recentre",
          std::fabs(t.firstHeight - 3.0) < 0.02 && t.firstHeight > t.firstRecentre);
    Feed(b, t, false, 4.0, 4.5);
    check("4 s hold: no short press on release", t.shortPress == 0);
}

static void CaseSparseUpdates() {
    // A stalled host (one update 3.5 s after the press) must fire the height recentre once, not both.
    ViewButton b = Primed();
    b.Update(true, 0.0);
    ViewEvents ev = b.Update(true, 3.5);
    check("gap past 3 s: only the height recentre fires", ev.recentreHeight && !ev.recentre);
    ev = b.Update(true, 3.6);
    check("gap past 3 s: nothing re-fires", !ev.recentreHeight && !ev.recentre && !ev.shortPress);
}

static void CaseRepeatPress() {
    ViewButton b = Primed();
    Tally t;
    Feed(b, t, true, 0.0, 1.5);
    Feed(b, t, false, 1.5, 2.0);
    Feed(b, t, true, 2.0, 3.5); // a second hold re-arms
    Feed(b, t, false, 3.5, 4.0);
    check("two holds: two recentres, no short presses", t.recentre == 2 && t.height == 0 && t.shortPress == 0);
}

static void CaseReset() {
    ViewButton b = Primed();
    Tally t;
    Feed(b, t, true, 0.0, 0.3);
    b.Reset(); // focus lost mid-press
    Feed(b, t, false, 0.3, 1.0);
    check("reset: the abandoned press reports nothing", t.shortPress == 0 && t.recentre == 0);
    // Held through a reset (button still down on focus return) starts a fresh hold.
    Feed(b, t, true, 1.0, 1.5);
    check("reset: a fresh press is not credited with the old hold time", t.recentre == 0);
}

static void CaseFirstUpdateWhileDown() {
    // A button already down when updates begin (or resume after a Reset, e.g. focus regained with
    // the button held) is ignored until released: no hold, no short press.
    ViewButton b;
    Tally t;
    Feed(b, t, true, 0.0, 4.0);
    Feed(b, t, false, 4.0, 4.5);
    check("already down at the first update: ignored entirely",
          t.shortPress == 0 && t.recentre == 0 && t.height == 0);
    Feed(b, t, true, 5.0, 5.3);
    Feed(b, t, false, 5.3, 5.6);
    check("...and the next press works normally", t.shortPress == 1);
}

static void CaseMerge() {
    using gdx_vr::MergeRecentre;
    using gdx_vr::RecentreKind;
    check("merge none+level", MergeRecentre(RecentreKind::Idle, RecentreKind::Level) == RecentreKind::Level);
    check("merge level+height", MergeRecentre(RecentreKind::Level, RecentreKind::Height) == RecentreKind::Height);
    check("merge height+level keeps height", MergeRecentre(RecentreKind::Height, RecentreKind::Level) == RecentreKind::Height);
    check("merge height+none keeps height", MergeRecentre(RecentreKind::Height, RecentreKind::Idle) == RecentreKind::Height);
}

static void CaseRefreshPick() {
    using gdx_vr::PickRefreshRate;
    const float steam[] = { 72.0f, 80.0f, 90.0f, 120.0f }; // unordered lists must work too
    const float shuffled[] = { 120.0f, 72.0f, 90.0f, 80.0f };
    check("wants 90: picks 90", PickRefreshRate(steam, 4, 90.0f) == 90.0f);
    check("wants 90: order of the list is irrelevant", PickRefreshRate(shuffled, 4, 90.0f) == 90.0f);
    check("wants 100: picks the highest offered below it", PickRefreshRate(steam, 4, 100.0f) == 90.0f);
    check("wants 144: picks 120", PickRefreshRate(steam, 4, 144.0f) == 120.0f);
    check("wants 72: picks 72", PickRefreshRate(steam, 4, 72.0f) == 72.0f);
    check("wants 60 with nothing that low: picks nothing", PickRefreshRate(steam, 4, 60.0f) == 0.0f);
    check("89.99 counts as 90", PickRefreshRate(steam, 4, 89.99f) == 90.0f);
    check("empty list: nothing", PickRefreshRate(steam, 0, 90.0f) == 0.0f);
}

static void CaseSettings() {
    using namespace gdx_vr;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    check("defaults are in range", [] {
        for (int i = 0; i < kSettingCount; i++) {
            const SettingInfo& s = Setting(static_cast<SettingId>(i));
            if (SanitizeSetting(static_cast<SettingId>(i), s.def) != s.def) return false;
        }
        return true;
    }());
    check("NaN falls back to the default", SanitizeSetting(kSetHudHeight, nan) == 1.2);
    check("inf falls back to the default", SanitizeSetting(kSetWorldScale, inf) == 10.0);
    check("-inf falls back to the default", SanitizeSetting(kSetStereo, -inf) == 1.0);
    check("switch above range clamps to 1", SanitizeSetting(kSetHorizonLock, 7.0) == 1.0);
    check("switch below range clamps to 0", SanitizeSetting(kSetStereo, -3.0) == 0.0);
    check("switch rounds to nearest", SanitizeSetting(kSetWideCull, 0.4) == 0.0 && SanitizeSetting(kSetWideCull, 0.6) == 1.0);
    check("zero world scale clamps up", SanitizeSetting(kSetWorldScale, 0.0) == 2.0);
    check("huge world scale clamps down", SanitizeSetting(kSetWorldScale, 1e9) == 50.0);
    check("HUD height clamps", SanitizeSetting(kSetHudHeight, 0.01) == 0.3 && SanitizeSetting(kSetHudHeight, 99.0) == 3.0);
    check("HUD aspect clamps", std::fabs(SanitizeSetting(kSetHudAspect, 0.5) - 4.0 / 3.0) < 1e-12 &&
                                   SanitizeSetting(kSetHudAspect, 10.0) == 4.0);
    check("haptics scale clamps to 0..1", SanitizeSetting(kSetHaptics, -0.5) == 0.0 &&
                                              SanitizeSetting(kSetHaptics, 3.0) == 1.0 &&
                                              SanitizeSetting(kSetHaptics, nan) == 0.6);
    check("refresh rate clamps to 60..144 and rounds", SanitizeSetting(kSetRefreshRate, 10.0) == 60.0 &&
                                                          SanitizeSetting(kSetRefreshRate, 500.0) == 144.0 &&
                                                          SanitizeSetting(kSetRefreshRate, 89.6) == 90.0);
    check("in-range value is untouched", SanitizeSetting(kSetNoseForward, 12.5) == 12.5);
    check("negative nose offsets are allowed", SanitizeSetting(kSetNoseUp, -5.0) == -5.0);
    check("all CVar names are in the gEnhancements.VR namespace", [] {
        for (int i = 0; i < kSettingCount; i++) {
            const char* n = Setting(static_cast<SettingId>(i)).cvar;
            if (std::string(n).rfind("gEnhancements.VR.", 0) != 0) return false;
        }
        return true;
    }());
}

int main() {
    std::printf("gdx_vr_system_tests\n");
    CaseShortPress();
    CaseJustUnderHold();
    CaseRecentreHold();
    CaseHeightHold();
    CaseSparseUpdates();
    CaseRepeatPress();
    CaseReset();
    CaseFirstUpdateWhileDown();
    CaseMerge();
    CaseRefreshPick();
    CaseSettings();
    std::printf("%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}

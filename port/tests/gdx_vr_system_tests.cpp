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
    ViewButton b;
    Tally t;
    Feed(b, t, false, 0.0, 1.0);
    Feed(b, t, true, 1.0, 1.4);  // 0.4 s tap
    check("tap: nothing fires while held", t.shortPress == 0 && t.recentre == 0 && t.height == 0);
    Feed(b, t, false, 1.4, 2.0);
    check("tap: exactly one short press on release", t.shortPress == 1 && t.recentre == 0 && t.height == 0);
}

static void CaseJustUnderHold() {
    ViewButton b;
    b.Update(true, 10.0);
    ViewEvents ev = b.Update(true, 10.0 + ViewButton::kRecentreSeconds - 0.001);
    check("0.999 s: no recentre yet", !ev.recentre && !ev.recentreHeight);
    ev = b.Update(false, 10.0 + ViewButton::kRecentreSeconds - 0.0005);
    check("0.999 s release is still a short press", ev.shortPress);
}

static void CaseRecentreHold() {
    ViewButton b;
    Tally t;
    Feed(b, t, true, 5.0, 7.0); // 2 s hold
    check("2 s hold: one level recentre at ~1 s", t.recentre == 1 && t.height == 0);
    check("2 s hold: recentre fires at the 1 s mark", std::fabs(t.firstRecentre - 6.0) < 0.02);
    Feed(b, t, false, 7.0, 7.5);
    check("2 s hold: no short press on release", t.shortPress == 0);
}

static void CaseHeightHold() {
    ViewButton b;
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
    ViewButton b;
    b.Update(true, 0.0);
    ViewEvents ev = b.Update(true, 3.5);
    check("gap past 3 s: only the height recentre fires", ev.recentreHeight && !ev.recentre);
    ev = b.Update(true, 3.6);
    check("gap past 3 s: nothing re-fires", !ev.recentreHeight && !ev.recentre && !ev.shortPress);
}

static void CaseRepeatPress() {
    ViewButton b;
    Tally t;
    Feed(b, t, true, 0.0, 1.5);
    Feed(b, t, false, 1.5, 2.0);
    Feed(b, t, true, 2.0, 3.5); // a second hold re-arms
    Feed(b, t, false, 3.5, 4.0);
    check("two holds: two recentres, no short presses", t.recentre == 2 && t.height == 0 && t.shortPress == 0);
}

static void CaseReset() {
    ViewButton b;
    Tally t;
    Feed(b, t, true, 0.0, 0.3);
    b.Reset(); // focus lost mid-press
    Feed(b, t, false, 0.3, 1.0);
    check("reset: the abandoned press reports nothing", t.shortPress == 0 && t.recentre == 0);
    // Held through a reset (button still down on focus return) starts a fresh hold.
    Feed(b, t, true, 1.0, 1.5);
    check("reset: a fresh press is not credited with the old hold time", t.recentre == 0);
}

static void CaseMerge() {
    using gdx_vr::MergeRecentre;
    using gdx_vr::RecentreKind;
    check("merge none+level", MergeRecentre(RecentreKind::Idle, RecentreKind::Level) == RecentreKind::Level);
    check("merge level+height", MergeRecentre(RecentreKind::Level, RecentreKind::Height) == RecentreKind::Height);
    check("merge height+level keeps height", MergeRecentre(RecentreKind::Height, RecentreKind::Level) == RecentreKind::Height);
    check("merge height+none keeps height", MergeRecentre(RecentreKind::Height, RecentreKind::Idle) == RecentreKind::Height);
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
    check("NaN falls back to the default", SanitizeSetting(kHudHeight, nan) == 1.2);
    check("inf falls back to the default", SanitizeSetting(kWorldScale, inf) == 10.0);
    check("-inf falls back to the default", SanitizeSetting(kStereo, -inf) == 1.0);
    check("switch above range clamps to 1", SanitizeSetting(kHorizonLock, 7.0) == 1.0);
    check("switch below range clamps to 0", SanitizeSetting(kStereo, -3.0) == 0.0);
    check("switch rounds to nearest", SanitizeSetting(kWideCull, 0.4) == 0.0 && SanitizeSetting(kWideCull, 0.6) == 1.0);
    check("zero world scale clamps up", SanitizeSetting(kWorldScale, 0.0) == 2.0);
    check("huge world scale clamps down", SanitizeSetting(kWorldScale, 1e9) == 50.0);
    check("HUD height clamps", SanitizeSetting(kHudHeight, 0.01) == 0.3 && SanitizeSetting(kHudHeight, 99.0) == 3.0);
    check("HUD aspect clamps", std::fabs(SanitizeSetting(kHudAspect, 0.5) - 4.0 / 3.0) < 1e-12 &&
                                   SanitizeSetting(kHudAspect, 10.0) == 4.0);
    check("in-range value is untouched", SanitizeSetting(kNoseForward, 12.5) == 12.5);
    check("negative nose offsets are allowed", SanitizeSetting(kNoseUp, -5.0) == -5.0);
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
    CaseMerge();
    CaseSettings();
    std::printf("%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}

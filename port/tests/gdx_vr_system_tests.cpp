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

static void CaseChord() {
    using gdx_vr::Chord;
    // Holding both for 0.5 s fires exactly once and latches until both are released.
    Chord c;
    int fires = 0;
    double firedAt = -1.0;
    for (double t = 0.0; t < 2.0; t += 1.0 / 90.0) {
        if (c.Update(true, true, t)) {
            fires++;
            if (firedAt < 0) firedAt = t;
        }
    }
    check("chord: fires once at ~0.5 s", fires == 1 && std::fabs(firedAt - 0.5) < 0.02);
    check("chord: latched while held", c.Latched());
    c.Update(true, false, 2.1);
    check("chord: still latched with one button down", c.Latched());
    c.Update(false, false, 2.2);
    check("chord: released after both are up", !c.Latched());
    // Not long enough, or only one button: nothing.
    Chord d;
    int n = 0;
    for (double t = 0.0; t < 0.45; t += 1.0 / 90.0) n += d.Update(true, true, t);
    n += d.Update(true, false, 0.5);
    check("chord: released at 0.45 s never fires", n == 0 && !d.Latched());
    Chord e;
    for (double t = 0.0; t < 3.0; t += 1.0 / 90.0) n += e.Update(true, false, t);
    check("chord: one button alone never fires", n == 0);
    // The two presses need not start together: the clock starts when both are down.
    Chord f;
    f.Update(true, false, 0.0);
    f.Update(true, false, 1.0);
    check("chord: clock starts when the second button joins", !f.Update(true, true, 1.0) && !f.Update(true, true, 1.4) &&
                                                                  f.Update(true, true, 1.5));
    // Reset forgets a chord attempt.
    Chord g;
    g.Update(true, true, 0.0);
    g.Reset();
    check("chord: reset restarts the clock", !g.Update(true, true, 0.6));
}

static void CaseStickAsDpad() {
    using gdx_vr::StickAsDpad;
    check("stick dpad: off below 0.55", !StickAsDpad(false, 0.5f));
    check("stick dpad: on above 0.55", StickAsDpad(false, 0.6f));
    check("stick dpad: stays on down to 0.36", StickAsDpad(true, 0.36f));
    check("stick dpad: off below 0.35 once on", !StickAsDpad(true, 0.3f));
    check("stick dpad: the opposite direction (negative) is off", !StickAsDpad(false, -0.9f));
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
    check("in-range value is untouched", SanitizeSetting(kSetRoofForward, -2.5) == -2.5);
    check("roof forward clamps to the measured span", SanitizeSetting(kSetRoofForward, 40.0) == 12.0 &&
                                                        SanitizeSetting(kSetRoofForward, -40.0) == -12.0);
    check("roof clearance cannot go below the roof", SanitizeSetting(kSetRoofClearance, -5.0) == 0.0);
    check("cockpit tilt clamps to 0..1", SanitizeSetting(kSetCockpitTilt, -1.0) == 0.0 &&
                                         SanitizeSetting(kSetCockpitTilt, 2.0) == 1.0 &&
                                         SanitizeSetting(kSetCockpitTilt, 0.5) == 0.5);
    check("all CVar names are in the gEnhancements.VR namespace", [] {
        for (int i = 0; i < kSettingCount; i++) {
            const char* n = Setting(static_cast<SettingId>(i)).cvar;
            if (std::string(n).rfind("gEnhancements.VR.", 0) != 0) return false;
        }
        return true;
    }());
}

static void CaseRoofHeight() {
    using gdx_vr::CockpitRoofHeight;
    using gdx_vr::kCockpitFallbackRoof;
    // Rows from gdx_vr_roof_table.h: Blue Falcon (0) is flat at 11.7; Twin Noritta (9) falls
    // 5.0 -> 4.8 between forward 0 and 2; Black Bull (15) is the tallest at the origin.
    check("table row read at a sample", std::fabs(CockpitRoofHeight(0, 0.0f) - 11.7f) < 1e-4f);
    check("interpolates between samples", std::fabs(CockpitRoofHeight(9, 1.0f) - 4.9f) < 1e-4f);
    check("clamps before the first sample", CockpitRoofHeight(9, -99.0f) == CockpitRoofHeight(9, -12.0f));
    check("clamps after the last sample", CockpitRoofHeight(9, 99.0f) == CockpitRoofHeight(9, 12.0f));
    check("machines differ", CockpitRoofHeight(15, 0.0f) > CockpitRoofHeight(9, 0.0f) + 10.0f);
    check("super machines are covered", CockpitRoofHeight(32, 0.0f) != kCockpitFallbackRoof);
    check("unknown machine falls back", CockpitRoofHeight(-1, 0.0f) == kCockpitFallbackRoof &&
                                            CockpitRoofHeight(33, 0.0f) == kCockpitFallbackRoof);
    check("NaN forward falls back", CockpitRoofHeight(0, std::nanf("")) == kCockpitFallbackRoof);
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
    CaseChord();
    CaseStickAsDpad();
    CaseRefreshPick();
    CaseSettings();
    CaseRoofHeight();
    std::printf("%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}

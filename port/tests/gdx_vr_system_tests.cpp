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

static void CaseDisplayRate() {
    using gdx_vr::DisplayRateHz;
    using gdx_vr::RecommendedPerAppRate;
    check("8.333 ms is 120 Hz", std::fabs(DisplayRateHz(8333333) - 120.0f) < 0.01f);
    check("11.111 ms is 90 Hz", std::fabs(DisplayRateHz(11111111) - 90.0f) < 0.01f);
    check("13.889 ms is 72 Hz", std::fabs(DisplayRateHz(13888889) - 72.0f) < 0.01f);
    check("a missing period is 0", DisplayRateHz(0) == 0.0f && DisplayRateHz(-5) == 0.0f);
    check("at 120 recommend 90", RecommendedPerAppRate(120.0f) == 90.0f);
    check("at 119.97 (a real 8.334 ms period) recommend 90", RecommendedPerAppRate(119.97f) == 90.0f);
    check("at 90 recommend 72", RecommendedPerAppRate(90.0f) == 72.0f);
    check("at 72 there is nothing lower", RecommendedPerAppRate(72.0f) == 0.0f);
    check("at 144 recommend 120", RecommendedPerAppRate(144.0f) == 120.0f);
    check("unknown or NaN: nothing", RecommendedPerAppRate(0.0f) == 0.0f && RecommendedPerAppRate(std::nanf("")) == 0.0f);
}

// Feeds a RefreshWatch `windows` whole 10 s windows of `fps` frames per second against `displayHz`, each
// ending exactly on its 10 s mark, and returns the first advice that fired (or an empty one). `prime`
// makes one call first at `t`, which is what starts the first window after a Restart() or a focus change.
static gdx_vr::RefreshAdvice RunWindows(gdx_vr::RefreshWatch& w, double& t, int windows, double fps, bool focused,
                                        float displayHz, bool prime = false) {
    if (prime) {
        w.Frame(t, focused, displayHz);
    }
    const int n = (int)(fps * 10.0);
    for (int i = 0; i < windows; i++) {
        for (int k = 1; k <= n; k++) {
            const gdx_vr::RefreshAdvice a = w.Frame(t + 10.0 * k / n, focused, displayHz);
            if (a.notHeld) {
                t += 10.0 * k / n;
                return a;
            }
        }
        t += 10.0;
    }
    return {};
}

static void CaseRefreshWatch() {
    using gdx_vr::RefreshWatch;

    {
        std::printf("  watch: holding the display rate says nothing\n");
        RefreshWatch w;
        double t = 100.0;
        check("a minute at 120 on a 120 display", !RunWindows(w, t, 6, 120.0, true, 120.0f, true).notHeld);
        check("110 fps is within 90% of 120", !RunWindows(w, t, 6, 110.0, true, 120.0f).notHeld);
    }
    {
        std::printf("  watch: two bad windows report once\n");
        RefreshWatch w;
        double t = 100.0;
        const double t0 = t;
        // 100 fps is under 108 (90% of 120).
        check("one bad window is not enough", !RunWindows(w, t, 1, 100.0, true, 120.0f, true).notHeld && w.Misses() == 1);
        const gdx_vr::RefreshAdvice a = RunWindows(w, t, 1, 100.0, true, 120.0f);
        check("the second one reports", a.notHeld && w.Reported());
        check("it recommends 90 for a 120 display", a.recommendHz == 90.0f && std::fabs(a.displayHz - 120.0f) < 0.01f);
        check("and carries the failing frame rate", std::fabs(a.fps - 100.0) < 0.5);
        check("after two 10 s windows, not before", std::fabs((t - t0) - 20.0) < 0.01);
        check("never again: no matter how long it stays bad", !RunWindows(w, t, 30, 40.0, true, 120.0f).notHeld);
        w.Restart();
        check("even after the window restarts", !RunWindows(w, t, 6, 40.0, true, 120.0f, true).notHeld);
    }
    {
        std::printf("  watch: measured against the display, not a request\n");
        RefreshWatch w;
        double t = 0.0;
        check("100 fps is fine on a 90 Hz display", !RunWindows(w, t, 6, 100.0, true, 90.0f, true).notHeld);
        RefreshWatch w2;
        t = 0.0;
        const gdx_vr::RefreshAdvice a = RunWindows(w2, t, 2, 70.0, true, 90.0f, true);
        check("70 fps on a 90 Hz display reports and recommends 72", a.notHeld && a.recommendHz == 72.0f);
        RefreshWatch w3;
        t = 0.0;
        const gdx_vr::RefreshAdvice b = RunWindows(w3, t, 2, 40.0, true, 72.0f, true);
        check("at 72 it reports with nothing lower to recommend", b.notHeld && b.recommendHz == 0.0f);
    }
    {
        std::printf("  watch: the counter resets\n");
        RefreshWatch w;
        double t = 0.0;
        check("a bad window", !RunWindows(w, t, 1, 60.0, true, 120.0f, true).notHeld && w.Misses() == 1);
        check("a good window clears the miss", !RunWindows(w, t, 1, 120.0, true, 120.0f).notHeld && w.Misses() == 0);
        check("so one bad one afterwards still does not report", !RunWindows(w, t, 1, 60.0, true, 120.0f).notHeld);
        check("the second consecutive one does", RunWindows(w, t, 1, 60.0, true, 120.0f).notHeld);
    }
    {
        std::printf("  watch: only while worn, and at one display rate\n");
        RefreshWatch w;
        double t = 0.0;
        check("a bad minute unfocused never reports", !RunWindows(w, t, 6, 30.0, false, 120.0f, true).notHeld);
        check("a miss while worn counts", !RunWindows(w, t, 1, 30.0, true, 120.0f, true).notHeld && w.Misses() == 1);
        w.Frame(t + 1.0, false, 120.0f);
        check("losing focus forgets it", w.Misses() == 0);
        t += 1.0;
        check("and a window after regaining focus is only the first miss",
              !RunWindows(w, t, 1, 30.0, true, 120.0f, true).notHeld && w.Misses() == 1);
        w.Frame(t + 0.1, true, 90.0f);
        check("a display rate change restarts the count", w.Misses() == 0);
        check("jitter within 2 Hz does not", [&] {
            RefreshWatch j;
            double u = 0.0;
            RunWindows(j, u, 1, 30.0, true, 120.0f, true);
            j.Frame(u + 0.01, true, 119.7f);
            return j.Misses() == 1;
        }());
        check("no display rate, no counting", !w.Frame(t + 50.0, true, 0.0f).notHeld);
    }
}

static void CaseEyeSize() {
    using gdx_vr::EyeImageSize;
    check("1728 x 1.25 is the native 2160", EyeImageSize(1728, 1.25, 0, 0) == 2160);
    check("scale 1 keeps the recommendation", EyeImageSize(1728, 1.0, 0, 0) == 1728);
    check("0.75 scale", EyeImageSize(1728, 0.75, 0, 0) == 1296);
    check("1.5 scale", EyeImageSize(1728, 1.5, 0, 0) == 2592);
    check("result is even", EyeImageSize(1001, 1.0, 0, 0) % 2 == 0 && EyeImageSize(1003, 1.25, 0, 0) % 2 == 0);
    check("rounds before evening: 1001 x 1.25 = 1251.25 -> 1250", EyeImageSize(1001, 1.25, 0, 0) == 1250);
    check("held to the view's maximum", EyeImageSize(1728, 1.5, 2400, 0) == 2400);
    check("held to the swapchain maximum", EyeImageSize(1728, 1.5, 0, 2301) == 2300);
    check("the smaller of the two wins", EyeImageSize(1728, 1.5, 2500, 2200) == 2200);
    check("limits of 0 mean none", EyeImageSize(1728, 1.5, 0, 0) == 2592);
    check("scale below the range is clamped", EyeImageSize(1728, 0.1, 0, 0) == 1296);
    check("scale above the range is clamped", EyeImageSize(1728, 8.0, 0, 0) == 2592);
    check("NaN scale uses the default", EyeImageSize(1728, std::nan(""), 0, 0) == 2160);
    check("never below 2", EyeImageSize(0, 1.25, 0, 0) == 2);
}

static void CaseDisplayMigration() {
    using gdx_vr::MigrateDisplayStandard;
    using gdx_vr::kDisplayStandardVersion;
    check("version is 1", kDisplayStandardVersion == 1);
    auto m = MigrateDisplayStandard(0, true, 90.0);
    check("old config with the old default 90 moves to 120", m.changed && m.setRate && m.rate == 120 &&
                                                               m.version == kDisplayStandardVersion);
    m = MigrateDisplayStandard(0, true, 89.6);
    check("a rate that rounds to 90 counts", m.setRate && m.rate == 120);
    m = MigrateDisplayStandard(0, true, 72.0);
    check("a deliberate 72 stays, but the version is stored", m.changed && !m.setRate && m.version == 1);
    m = MigrateDisplayStandard(0, true, 120.0);
    check("120 stays", m.changed && !m.setRate);
    m = MigrateDisplayStandard(0, false, 0.0);
    check("no saved rate: just the version (the default is already 120)", m.changed && !m.setRate && m.version == 1);
    m = MigrateDisplayStandard(0, true, std::nan(""));
    check("a NaN rate is left to the clamp", m.changed && !m.setRate);
    m = MigrateDisplayStandard(1, true, 90.0);
    check("already migrated: a 90 chosen afterwards is kept", !m.changed && !m.setRate && m.version == 1);
    m = MigrateDisplayStandard(5, true, 90.0);
    check("a newer version is left alone", !m.changed && m.version == 5);
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
    check("inf falls back to the default", SanitizeSetting(kSetWorldScale, inf) == 1.0);
    check("-inf falls back to the default", SanitizeSetting(kSetStereo, -inf) == 1.0);
    check("switch above range clamps to 1", SanitizeSetting(kSetHorizonLock, 7.0) == 1.0);
    check("switch below range clamps to 0", SanitizeSetting(kSetStereo, -3.0) == 0.0);
    check("switch rounds to nearest", SanitizeSetting(kSetWideCull, 0.4) == 0.0 && SanitizeSetting(kSetWideCull, 0.6) == 1.0);
    check("zero world scale clamps up to 1", SanitizeSetting(kSetWorldScale, 0.0) == 1.0);
    check("huge world scale clamps down", SanitizeSetting(kSetWorldScale, 1e9) == 50.0);
    check("HUD height clamps", SanitizeSetting(kSetHudHeight, 0.01) == 0.3 && SanitizeSetting(kSetHudHeight, 99.0) == 3.0);
    check("HUD aspect clamps", std::fabs(SanitizeSetting(kSetHudAspect, 0.5) - 4.0 / 3.0) < 1e-12 &&
                                   SanitizeSetting(kSetHudAspect, 10.0) == 4.0);
    check("haptics scale clamps to 0..1", SanitizeSetting(kSetHaptics, -0.5) == 0.0 &&
                                              SanitizeSetting(kSetHaptics, 3.0) == 1.0 &&
                                              SanitizeSetting(kSetHaptics, nan) == 0.6);
    check("refresh rate clamps to 72..120 and rounds", SanitizeSetting(kSetRefreshRate, 10.0) == 72.0 &&
                                                          SanitizeSetting(kSetRefreshRate, 500.0) == 120.0 &&
                                                          SanitizeSetting(kSetRefreshRate, 89.6) == 90.0);
    check("refresh rate defaults to 120", Setting(kSetRefreshRate).def == 120.0 && Setting(kSetRefreshRate).isInteger);
    check("resolution scale clamps to 0.75..1.5, default 1.25",
          SanitizeSetting(kSetResolutionScale, 0.1) == 0.75 && SanitizeSetting(kSetResolutionScale, 9.0) == 1.5 &&
              SanitizeSetting(kSetResolutionScale, nan) == 1.25 && Setting(kSetResolutionScale).def == 1.25 &&
              !Setting(kSetResolutionScale).isInteger);
    check("display standard defaults to the current version",
          Setting(kSetDisplayStandard).def == kDisplayStandardVersion);
    check("in-range value is untouched", SanitizeSetting(kSetRoofForward, -2.5) == -2.5);
    check("roof forward clamps to the measured span", SanitizeSetting(kSetRoofForward, 40.0) == 12.0 &&
                                                        SanitizeSetting(kSetRoofForward, -40.0) == -12.0);
    check("roof clearance cannot go below the roof", SanitizeSetting(kSetRoofClearance, -5.0) == 0.0);
    check("cockpit style is 0 (roof) or 1 (cabin), cabin by default",
          SanitizeSetting(kSetCockpitStyle, 3.0) == 1.0 && SanitizeSetting(kSetCockpitStyle, -1.0) == 0.0 &&
              Setting(kSetCockpitStyle).def == 1.0);
    check("dash HUD size clamps to 0.15..1", SanitizeSetting(kSetDashHudSize, 0.0) == 0.15 &&
                                                SanitizeSetting(kSetDashHudSize, 5.0) == 1.0);
    check("cabin seat offsets clamp to +-10", SanitizeSetting(kSetCabinSeatUp, 50.0) == 10.0 &&
                                                SanitizeSetting(kSetCabinSeatForward, -50.0) == -10.0);
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

static void CaseTriggerClick() {
    using gdx_vr::TriggerClick;
    check("trigger below 0.6 does not click", !TriggerClick(false, 0.59f));
    check("trigger above 0.6 clicks", TriggerClick(false, 0.61f));
    check("held click survives a dip to 0.5", TriggerClick(true, 0.5f));
    check("click releases below 0.45", !TriggerClick(true, 0.44f));
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
    CaseDisplayRate();
    CaseRefreshWatch();
    CaseEyeSize();
    CaseDisplayMigration();
    CaseSettings();
    CaseRoofHeight();
    CaseTriggerClick();
    std::printf("%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}

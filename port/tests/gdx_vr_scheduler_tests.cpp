/* Standalone unit-test harness for port/vr/gdx_vr_scheduler.cpp.
 *
 * The scheduler decides, for each OpenXR frame, how many 60 Hz simulation ticks must run first and
 * at which interpolation t the frame renders. The properties that matter, and that a headset run
 * cannot measure precisely, are checked here against synthetic predictedDisplayTime sequences:
 *   - displayed game time advances by exactly one display period per frame (no judder) at any
 *     display rate, including non-integer ratios such as 90/59.94;
 *   - the simulation runs at exactly one tick per tickSeconds of display time (game speed is never
 *     derived from the display rate);
 *   - hitches within the catch-up budget stay continuous, larger ones re-anchor and report a cut.
 *
 * No OpenXR, no window, no game data.
 */
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "../vr/gdx_vr_scheduler.h"

static int gChecks = 0;
static int gFails = 0;

static void check(const char* what, bool ok) {
    gChecks++;
    if (!ok) {
        gFails++;
        std::printf("    [x] %s\n", what);
    }
}

static void checkNear(const char* what, double got, double expected, double tolerance) {
    gChecks++;
    if (!(std::fabs(got - expected) <= tolerance)) {
        gFails++;
        std::printf("    [x] %s: got %.9f, expected %.9f (+-%.1e)\n", what, got, expected, tolerance);
    }
}

static const double kTick = 1.001 / 60.0;

// predictedDisplayTime is integer nanoseconds; convert the way the runtime glue will.
static double NsToSeconds(int64_t ns) {
    return static_cast<double>(ns) * 1e-9;
}

struct FrameResult {
    int ticks;
    bool rebased;
    float t;
    double shownTicks; // displayed game time in ticks since the first tick: newest - 1 + t
};

static FrameResult RunFrame(gdx_vr::Scheduler& s, double displaySeconds) {
    const gdx_vr::FramePlan plan = s.PlanFrame(displaySeconds);
    for (int i = 0; i < plan.ticksToRun; i++) {
        s.OnTickRan();
    }
    FrameResult r;
    r.ticks = plan.ticksToRun;
    r.rebased = plan.rebased;
    r.t = s.InterpT(displaySeconds);
    r.shownTicks = static_cast<double>(s.TicksRun() - 1) + static_cast<double>(r.t);
    return r;
}

// Steady display at `hz` for `seconds`: every frame must advance shown game time by exactly one
// display period, need at most ceil(tick/period) ticks, and never rebase after the first frame.
static void CaseSteadyRate(double hz, double seconds) {
    char label[160];
    gdx_vr::Scheduler s;
    const int64_t periodNs = static_cast<int64_t>(std::llround(1e9 / hz));
    const double periodTicks = (static_cast<double>(periodNs) * 1e-9) / kTick;
    const int frames = static_cast<int>(seconds * hz);
    const int64_t startNs = 1234567890123LL; // arbitrary runtime epoch

    int rebases = 0;
    int maxTicks = 0;
    double worstStepError = 0.0;
    double prevShown = 0.0;
    for (int i = 0; i < frames; i++) {
        const FrameResult r = RunFrame(s, NsToSeconds(startNs + periodNs * i));
        if (r.rebased) {
            rebases++;
        }
        if (r.ticks > maxTicks) {
            maxTicks = r.ticks;
        }
        if (i > 0) {
            const double err = std::fabs((r.shownTicks - prevShown) - periodTicks);
            if (err > worstStepError) {
                worstStepError = err;
            }
        }
        prevShown = r.shownTicks;
    }

    std::snprintf(label, sizeof(label), "%.2f Hz: only the anchoring frame rebases", hz);
    check(label, rebases == 1);
    std::snprintf(label, sizeof(label), "%.2f Hz: ticks per frame within ceil(rate ratio)", hz);
    check(label, maxTicks <= static_cast<int>(std::ceil(periodTicks - 1e-9)));
    // t is a float: ~1e-7 relative, so steps hold to well under a thousandth of a tick.
    std::snprintf(label, sizeof(label), "%.2f Hz: displayed game time steps are uniform", hz);
    checkNear(label, worstStepError, 0.0, 1e-4);
    // One tick per tickSeconds of display time, +-1 for the partial tick at either end.
    const double expectedTicks = (static_cast<double>(periodNs) * 1e-9 * (frames - 1)) / kTick + 1.0;
    std::snprintf(label, sizeof(label), "%.2f Hz: simulation rate is 59.94 Hz", hz);
    checkNear(label, static_cast<double>(s.TicksRun()), expectedTicks, 1.0);
}

// Every rendered t must lie in [0, 1], and the first frame after a rebase renders the newest tick.
static void CaseTRange() {
    gdx_vr::Scheduler s;
    bool inRange = true;
    for (int i = 0; i < 2000; i++) {
        const FrameResult r = RunFrame(s, 5.0 + i / 90.0);
        if (r.t < 0.0f || r.t > 1.0f) {
            inRange = false;
        }
        if (r.rebased && r.t != 1.0f) {
            inRange = false;
        }
    }
    check("t stays within [0, 1]; rebased frames render at t = 1", inRange);
}

// Runtime jitter (+-1 ms around a 90 Hz cadence) must not cause rebases, and shown game time must
// track each frame's own display time rather than the nominal cadence.
static void CaseJitter() {
    gdx_vr::Scheduler s;
    uint32_t lcg = 12345u;
    int rebases = 0;
    double worst = 0.0;
    double prevDisplay = 0.0;
    double prevShown = 0.0;
    for (int i = 0; i < 5000; i++) {
        lcg = lcg * 1664525u + 1013904223u;
        const double jitter = ((static_cast<double>(lcg >> 8) / 16777216.0) - 0.5) * 0.002;
        const double display = 100.0 + i / 90.0 + jitter;
        const FrameResult r = RunFrame(s, display);
        if (r.rebased) {
            rebases++;
        }
        if (i > 0) {
            const double err = std::fabs((r.shownTicks - prevShown) - (display - prevDisplay) / kTick);
            if (err > worst) {
                worst = err;
            }
        }
        prevDisplay = display;
        prevShown = r.shownTicks;
    }
    check("jitter: no rebase beyond the anchor", rebases == 1);
    checkNear("jitter: shown time follows each frame's display time", worst, 0.0, 1e-4);
}

// A dropped 90 Hz frame (22 ms gap) fits the catch-up budget: continuous, no cut, 2 ticks at most.
static void CaseMissedFrame() {
    gdx_vr::Scheduler s;
    for (int i = 0; i < 100; i++) {
        RunFrame(s, i / 90.0);
    }
    const FrameResult before = RunFrame(s, 100 / 90.0);
    const FrameResult after = RunFrame(s, 102 / 90.0); // frame 101 never presented
    check("missed frame: no rebase", !after.rebased);
    check("missed frame: at most 2 ticks", after.ticks <= 2);
    checkNear("missed frame: shown time continuous", after.shownTicks - before.shownTicks, (2.0 / 90.0) / kTick,
              1e-4);
}

// A 100 ms stall exceeds the budget: rebase once (caller cuts), resume uniform motion, and do not
// fast-forward the game by the stalled time.
static void CaseHitch() {
    gdx_vr::Scheduler s;
    for (int i = 0; i < 100; i++) {
        RunFrame(s, i / 90.0);
    }
    const long long ticksBefore = s.TicksRun();
    const FrameResult hitch = RunFrame(s, 99 / 90.0 + 0.100);
    check("hitch: rebases", hitch.rebased);
    check("hitch: runs a single tick", hitch.ticks == 1 && s.TicksRun() == ticksBefore + 1);
    check("hitch: renders the newest tick", hitch.t == 1.0f);

    double prevShown = hitch.shownTicks;
    bool uniform = true;
    bool rebasedAgain = false;
    for (int i = 1; i < 200; i++) {
        const FrameResult r = RunFrame(s, 99 / 90.0 + 0.100 + i / 90.0);
        rebasedAgain = rebasedAgain || r.rebased;
        if (std::fabs((r.shownTicks - prevShown) - (1.0 / 90.0) / kTick) > 1e-4) {
            uniform = false;
        }
        prevShown = r.shownTicks;
    }
    check("hitch: no further rebases", !rebasedAgain);
    check("hitch: uniform motion afterwards", uniform);
}

// Reset (session resumed) must re-anchor on the next frame and report it.
static void CaseReset() {
    gdx_vr::Scheduler s;
    for (int i = 0; i < 50; i++) {
        RunFrame(s, i / 90.0);
    }
    s.Reset();
    const FrameResult r = RunFrame(s, 50.0);
    check("reset: next frame rebases", r.rebased);
    check("reset: runs one tick at t = 1", r.ticks == 1 && r.t == 1.0f);
}

int main() {
    std::printf("gdx_vr_scheduler_tests\n");
    CaseSteadyRate(59.94, 60.0);
    CaseSteadyRate(60.0, 60.0);
    CaseSteadyRate(72.0, 60.0);
    CaseSteadyRate(90.0, 60.0);
    CaseSteadyRate(120.0, 60.0);
    CaseSteadyRate(144.0, 60.0);
    CaseSteadyRate(45.0, 60.0); // runtime halving the frame rate under load
    CaseSteadyRate(90.0, 3600.0); // an hour: no drift, no rebase
    CaseTRange();
    CaseJitter();
    CaseMissedFrame();
    CaseHitch();
    CaseReset();
    std::printf("%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}

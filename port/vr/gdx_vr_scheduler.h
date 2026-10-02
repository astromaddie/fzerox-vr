// port/vr/gdx_vr_scheduler.h — maps OpenXR display times onto the 60 Hz simulation timeline.
//
// The desktop sub-frame loop spaces passes evenly inside a tick (t = (k+1)/count). That is exact
// when the display rate is an integer multiple of the tick rate, but at 90 Hz the 1,2,1,2 pass
// pattern shows game time in uneven steps (1/2, 1/2, 1 tick), which reads as judder once the view
// is head-tracked. Here every XR frame is instead placed on a fixed tick timeline by its
// predictedDisplayTime, so displayed game time advances by exactly one display period per frame.
//
// Timeline: tick n's state is shown at time T(n) = base + n * tickSeconds. A frame displayed at
// time r needs the newest simulated tick N to satisfy T(N) >= r, and is rendered at
// t = 1 - (T(N) - r) / tickSeconds, i.e. between tick N-1 (t=0) and tick N (t=1).
//
// Pure policy: no OpenXR calls, no clock reads, no game state. The caller converts XrTime
// (nanoseconds) to seconds, runs the ticks this asks for, and marks an interpolation cut whenever
// a call reports `rebased`. Simulation speed is never derived from the display rate: over any
// span without a rebase, exactly one tick runs per tickSeconds of display time.

#ifndef GDX_VR_SCHEDULER_H
#define GDX_VR_SCHEDULER_H

namespace gdx_vr {

struct SchedulerConfig {
    double tickSeconds = 1.001 / 60.0; // same period as main.cpp kGdxInterpTickSeconds
    // A frame that would need more ticks than this (a hitch, a long xrWaitFrame, a suspended
    // session) re-anchors the timeline instead of fast-forwarding the game.
    int maxCatchUpTicks = 2;
};

struct FramePlan {
    int ticksToRun; // simulation ticks to run before this frame is rendered (0..maxCatchUpTicks)
    bool rebased;   // the timeline was re-anchored: the caller must mark an interpolation cut
};

class Scheduler {
  public:
    explicit Scheduler(const SchedulerConfig& config = SchedulerConfig());

    // Forget the timeline. The next PlanFrame re-anchors on its display time and reports a rebase.
    // Call when a session (re)starts rendering after a stop, focus loss or other suspension.
    void Reset();

    // Decide how many ticks must run before the frame shown at `displaySeconds` can be rendered.
    // Call once per XR frame, after xrWaitFrame, then call OnTickRan() once per tick actually run.
    FramePlan PlanFrame(double displaySeconds);

    // Record that one simulation tick completed.
    void OnTickRan();

    // Interpolation t in [0, 1] for a frame shown at `displaySeconds`, given the ticks run so far.
    float InterpT(double displaySeconds) const;

    // Number of ticks run since construction (diagnostics and tests).
    long long TicksRun() const {
        return mTicksRun;
    }

  private:
    // Display time of the newest tick run, or of the tick before the first one if none has run.
    double NewestTickTime() const;

    SchedulerConfig mConfig;
    bool mAnchored = false;
    double mBase = 0.0;          // display time of tick index 0 on the current timeline
    long long mNewest = -1;      // index on the current timeline of the newest tick run
    long long mTicksRun = 0;
};

} // namespace gdx_vr

#endif // GDX_VR_SCHEDULER_H

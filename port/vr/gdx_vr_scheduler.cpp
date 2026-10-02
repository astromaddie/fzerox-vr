// port/vr/gdx_vr_scheduler.cpp — see gdx_vr_scheduler.h.

#include "gdx_vr_scheduler.h"

#include <cmath>

namespace gdx_vr {

// Display times within this of a tick boundary count as on it. predictedDisplayTime arrives as
// integer nanoseconds and is converted to seconds by the caller, so a frame landing exactly on a
// boundary can come out a few ulps either side of it.
static constexpr double kBoundaryEpsilonSeconds = 1e-7;

Scheduler::Scheduler(const SchedulerConfig& config) : mConfig(config) {
    if (!(mConfig.tickSeconds > 0.0)) {
        mConfig.tickSeconds = 1.001 / 60.0;
    }
    if (mConfig.maxCatchUpTicks < 1) {
        mConfig.maxCatchUpTicks = 1;
    }
}

void Scheduler::Reset() {
    mAnchored = false;
}

double Scheduler::NewestTickTime() const {
    return mBase + static_cast<double>(mNewest) * mConfig.tickSeconds;
}

FramePlan Scheduler::PlanFrame(double displaySeconds) {
    FramePlan plan = { 0, false };

    if (!mAnchored) {
        // The next tick to run becomes index 0 and is shown exactly at this frame.
        mAnchored = true;
        mBase = displaySeconds;
        mNewest = -1;
        plan.ticksToRun = 1;
        plan.rebased = true;
        return plan;
    }

    const double behind = (displaySeconds - NewestTickTime()) / mConfig.tickSeconds;
    const double needed = std::ceil(behind - kBoundaryEpsilonSeconds / mConfig.tickSeconds);
    if (needed <= 0.0) {
        return plan;
    }
    if (needed > static_cast<double>(mConfig.maxCatchUpTicks)) {
        // Too far behind to catch up without visibly fast-forwarding the game. Slide the timeline
        // so a single tick lands exactly on this frame; the caller cuts interpolation.
        mBase = displaySeconds - static_cast<double>(mNewest + 1) * mConfig.tickSeconds;
        plan.ticksToRun = 1;
        plan.rebased = true;
        return plan;
    }
    plan.ticksToRun = static_cast<int>(needed);
    return plan;
}

void Scheduler::OnTickRan() {
    mNewest++;
    mTicksRun++;
}

float Scheduler::InterpT(double displaySeconds) const {
    if (!mAnchored || mNewest < 0) {
        return 1.0f;
    }
    const double t = 1.0 - (NewestTickTime() - displaySeconds) / mConfig.tickSeconds;
    if (!(t > 0.0)) {
        return 0.0f;
    }
    if (t > 1.0) {
        return 1.0f;
    }
    return static_cast<float>(t);
}

} // namespace gdx_vr

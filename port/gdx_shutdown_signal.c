// port/gdx_shutdown_signal.c -- see gdx_shutdown_signal.h. Dependency-free C so the standalone
// test target compiles it unchanged.

#include "gdx_shutdown_signal.h"

#ifndef _WIN32
#include <signal.h>
#include <string.h>
#include <time.h>

// A repeat this soon after the first request is the same request delivered twice, not a user
// insisting: timeout(1) and other supervisors signal both the process and its process group, so a
// single "stop" often arrives as two SIGTERMs microseconds apart (seen on the Frame: one run in four
// was killed outright that way, skipping the orderly exit).
#define GDX_SHUTDOWN_REPEAT_GRACE_S 2

static volatile sig_atomic_t sRequested = 0;
static volatile sig_atomic_t sFirstSec = 0; // CLOCK_MONOTONIC seconds of the first request

static int MonotonicSeconds(void) {
    struct timespec ts; // clock_gettime is async-signal-safe (POSIX)
    return clock_gettime(CLOCK_MONOTONIC, &ts) == 0 ? (int)ts.tv_sec : 0;
}

static void gdx_shutdown_signal_handler(int sig) {
    const int now = MonotonicSeconds();
    if (sRequested != 0) {
        if (now - sFirstSec >= GDX_SHUTDOWN_REPEAT_GRACE_S) {
            // A deliberate second request: give up on the orderly path. Default action, no exit
            // handlers.
            signal(sig, SIG_DFL);
            raise(sig);
        }
        return;
    }
    sFirstSec = now;
    sRequested = sig;
}

void gdx_shutdown_signal_install(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = gdx_shutdown_signal_handler;
    sigemptyset(&action.sa_mask);
    // Restart interrupted calls: the request is polled once per frame, so nothing needs EINTR.
    action.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
}

int gdx_shutdown_signal_requested(void) {
    return (int)sRequested;
}

#else

void gdx_shutdown_signal_install(void) {
}

int gdx_shutdown_signal_requested(void) {
    return 0;
}

#endif

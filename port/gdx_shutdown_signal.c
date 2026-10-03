// port/gdx_shutdown_signal.c -- see gdx_shutdown_signal.h. Dependency-free C so the standalone
// test target compiles it unchanged.

#include "gdx_shutdown_signal.h"

#ifndef _WIN32
#include <signal.h>
#include <string.h>

static volatile sig_atomic_t sRequested = 0;

static void gdx_shutdown_signal_handler(int sig) {
    if (sRequested != 0) {
        // Second request: give up on the orderly path. Default action, no exit handlers.
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
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

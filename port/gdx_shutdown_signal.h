#pragma once

// SIGTERM / SIGINT turned into a request the frame loop acts on, instead of an exit() from inside
// the signal handler.
//
// libultraship's CrashHandler (Linux) answers SIGTERM, SIGINT and SIGQUIT with exit(1) straight
// from the handler. exit() runs atexit handlers and static destructors in reverse registration
// order while every other thread is still running. The dedicated audio thread (gdx_audio_thread.cpp)
// is only joined by an atexit handler registered when it starts, so every function-local static it
// creates later (osAiSetNextBuffer's sample buffers in libultraship os.cpp, the perf state) is
// destroyed first, and the still-running thread keeps writing into the freed buffers. That is the
// glibc "corrupted double-linked list" abort seen at the end of SIGTERM'd unattended VR runs.
//
// After gdx_shutdown_signal_install(), the first SIGTERM or SIGINT only records the signal; the
// main loop sees it, closes the window and leaves through the normal teardown (VR session, audio
// thread join) before main() returns. A second one at least 2 s later restores the default action
// and re-raises it, so a wedged process can still be killed; that path runs no destructors at all.
// Repeats inside those 2 s are absorbed: supervisors such as timeout(1) signal both the process and
// its group, so one request often arrives twice.
//
// No-op on Windows, where nothing turns these signals into exit().

#ifdef __cplusplus
extern "C" {
#endif

// Replaces whatever handler is installed for SIGTERM and SIGINT (call it after libultraship's
// CrashHandler exists, or it will be overwritten).
void gdx_shutdown_signal_install(void);

// The first shutdown signal received since install, or 0.
int gdx_shutdown_signal_requested(void);

#ifdef __cplusplus
}
#endif

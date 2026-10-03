/* Console test for the VR settings start-up path and the SIGTERM exit that followed it.
 *
 * Runs port/vr/gdx_vr_settings.cpp against libultraship's real Config and ConsoleVariable code: a
 * true first launch (no config file), a config from before the RoofForward/RoofClearance rename
 * (keys missing, so InitSettings registers and saves), and values of the wrong CVar type. Each
 * checks what lands in the file and that a second pass has nothing left to save.
 *
 * Then it replays what killed the unattended headset runs. Those were SIGTERM'd after a startup
 * save, and libultraship's CrashHandler answers SIGTERM with exit(1) from the signal handler. exit()
 * destroys statics newest first, so osAiSetNextBuffer's sample buffer (created by the audio thread
 * after its atexit join was registered) is freed while the audio thread is still copying into it.
 * The child process below sets that up with a worker calling the real osAiSetNextBuffer and an
 * atexit join that waits 50 ms first (on the headset the OpenXR loader's teardown sits in that
 * window), then raises SIGTERM. With gdx_shutdown_signal_install() the signal is only recorded and
 * the child joins the worker before exiting, so it exits 0. Set GDX_VR_SETTINGS_TESTS_OLD_SIGTERM=1
 * to skip the install and see the old behaviour: under AddressSanitizer that is a
 * heap-use-after-free in osAiSetNextBuffer, and the child exits 1 either way.
 *
 * No window, no OpenXR, no game data. Linux and macOS (fork); the signal cases are skipped on
 * Windows.
 */
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "ship/Context.h"

#include "../gdx_shutdown_signal.h"
#include "../vr/gdx_vr_settings.h"

#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

extern "C" {
void CVarLoad();
int32_t osAiSetNextBuffer(void* buff, size_t len);

// libultraship's os.cpp (pulled in by osAiSetNextBuffer) resolves these against the port at link
// time. None of them is reached by the calls made here.
uint8_t* gdx_rom_buffer = nullptr;
size_t gdx_rom_size = 0;
int GdxSegmentSourceRead(uint32_t, uint32_t, void*) {
    return 0;
}
int gdx_audio_thread_active(void) {
    return 1;
}
int32_t osSendMesg(void*, void*, int32_t) {
    return 0;
}
}

static int gChecks = 0;
static int gFails = 0;

static void check(const char* what, bool ok) {
    gChecks++;
    if (!ok) {
        gFails++;
        std::printf("    [x] %s\n", what);
    }
}

// Reachable from a global so LeakSanitizer does not report it. Never destroyed: ~Context needs a
// window, which this test does not have.
static std::shared_ptr<Ship::Context>* gContext = nullptr;
static std::string gDir;

static std::string ConfigPath() {
    return gDir + "/gdiffuser.cfg.json";
}

static void WriteConfig(const char* text) {
    std::ofstream(ConfigPath(), std::ios::trunc) << text;
}

static nlohmann::json ReadVr() {
    std::ifstream in(ConfigPath());
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.contains("CVars") || !j["CVars"].contains("gEnhancements") ||
        !j["CVars"]["gEnhancements"].contains("VR")) {
        return nlohmann::json::object();
    }
    return j["CVars"]["gEnhancements"]["VR"];
}

// Every key present with the JSON type its CVar type saves as.
static bool AllKeysTyped(const nlohmann::json& vr) {
    for (int i = 0; i < gdx_vr::kSettingCount; i++) {
        const gdx_vr::SettingInfo& si = gdx_vr::Setting((gdx_vr::SettingId)i);
        const char* key = std::strrchr(si.cvar, '.') + 1;
        if (!vr.contains(key)) {
            std::printf("    missing %s\n", key);
            return false;
        }
        const bool ok = si.isInteger ? vr[key].is_number_integer() : vr[key].is_number_float();
        if (!ok) {
            std::printf("    %s has the wrong type: %s\n", key, vr[key].dump().c_str());
            return false;
        }
    }
    return true;
}

static void CaseFirstLaunch() {
    std::printf("  first launch, no config file\n");
    check("first pass saves", gdx_vr::InitSettings());
    check("every key written with its type", AllKeysTyped(ReadVr()));
    check("second pass has nothing to save", !gdx_vr::InitSettings());
}

static void CaseRenamedKeys() {
    std::printf("  config from before the RoofForward/RoofClearance rename\n");
    WriteConfig(R"({
        "CVars": {
            "gEnhancements": {
                "Graphics": { "FramePacing": 0 },
                "VR": {
                    "Stereo": 1, "WideCull": 1, "CameraMode": 1, "HorizonLock": 0, "LevelPitch": 1,
                    "HudAspect": 1.5, "HudHeight": 1.2, "NoseForward": 8.0, "NoseUp": 6.0,
                    "CockpitTilt": 1.0, "WorldScale": 10.0, "Haptics": 0.6, "RefreshRate": 90
                }
            },
            "gdx": { "Migrations": { "FramePacingDefaultOff": 1 } },
            "gTestString": "kept"
        },
        "Window": { "Width": 1280 }
    })");
    CVarLoad();
    check("missing keys make it save", gdx_vr::InitSettings());
    const nlohmann::json vr = ReadVr();
    check("every key written with its type", AllKeysTyped(vr));
    check("existing value kept (CameraMode 1)", vr.value("CameraMode", -1) == 1);
    check("old keys left alone", vr.contains("NoseForward") && vr.contains("NoseUp"));
    std::ifstream in(ConfigPath());
    const nlohmann::json all = nlohmann::json::parse(in, nullptr, false);
    check("unrelated string CVar kept", !all.is_discarded() && all["CVars"].value("gTestString", "") == "kept");
    check("non-CVar config kept", !all.is_discarded() && all["Window"].value("Width", 0) == 1280);
    check("second pass has nothing to save", !gdx_vr::InitSettings());
}

static void CaseWrongTypes() {
    std::printf("  values of the wrong type and out of range\n");
    WriteConfig(R"({
        "CVars": { "gEnhancements": { "VR": {
            "Stereo": 1.0, "HudHeight": 2, "WorldScale": "x", "RefreshRate": 500, "Haptics": 0.25
        } } }
    })");
    CVarLoad();
    check("first pass saves", gdx_vr::InitSettings());
    const nlohmann::json vr = ReadVr();
    check("every key written with its type", AllKeysTyped(vr));
    check("float for an int converted (Stereo 1)", vr.value("Stereo", -1) == 1);
    check("int for a float converted (HudHeight 2.0)", std::fabs(vr.value("HudHeight", 0.0) - 2.0) < 1e-6);
    check("string replaced by the default (WorldScale 1)", std::fabs(vr.value("WorldScale", 0.0) - 1.0) < 1e-6);
    check("out of range clamped (RefreshRate 144)", vr.value("RefreshRate", 0) == 144);
    check("in-range value kept (Haptics 0.25)", std::fabs(vr.value("Haptics", 0.0) - 0.25) < 1e-6);
    check("second pass has nothing to save", !gdx_vr::InitSettings());
}

#ifndef _WIN32

// Stand-in for the dedicated audio thread: hands osAiSetNextBuffer one 2240-byte buffer of
// non-silent samples after another, as AudioThread_CreateTaskImpl does.
static std::atomic<bool> sWorkerStop{ false };
static std::atomic<long> sWorkerCalls{ 0 };
static std::thread* sWorker = nullptr;

static void WorkerMain() {
    std::vector<uint8_t> buf(2240, 0x11);
    while (!sWorkerStop.load()) {
        osAiSetNextBuffer(buf.data(), buf.size());
        sWorkerCalls++;
    }
}

// Registered before the worker's first osAiSetNextBuffer, like gdx_audio_thread_start's atexit
// join, so exit() runs it after the buffer's destructor.
static void StopWorker() {
    if (sWorker == nullptr) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sWorkerStop = true;
    sWorker->join();
    delete sWorker;
    sWorker = nullptr;
}

static void CaseSigtermAfterStartupSave() {
    std::printf("  SIGTERM after a startup save, with a thread still producing audio\n");
    const bool oldBehaviour = std::getenv("GDX_VR_SETTINGS_TESTS_OLD_SIGTERM") != nullptr;
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        WriteConfig(R"({ "CVars": { "gEnhancements": { "VR": { "Stereo": 1 } } } })");
        CVarLoad();
        if (!gdx_vr::InitSettings()) {
            _exit(3);
        }
        (*gContext)->InitCrashHandler(); // libultraship: SIGTERM/SIGINT -> exit(1) (Linux)
        if (!oldBehaviour) {
            gdx_shutdown_signal_install();
        }
        std::atexit(StopWorker);
        sWorker = new std::thread(WorkerMain);
        while (sWorkerCalls.load() < 4) {
            std::this_thread::yield();
        }
        raise(SIGTERM);
        // What the frame loop does when it sees the request: close, join the worker, then leave.
        if (gdx_shutdown_signal_requested() != SIGTERM) {
            _exit(4);
        }
        StopWorker();
        std::exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        std::printf("    child died on signal %d\n", WTERMSIG(status));
    } else if (WEXITSTATUS(status) != 0) {
        std::printf("    child exited %d\n", WEXITSTATUS(status));
    }
    check("child survives the signal and exits cleanly", WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// timeout(1) and other supervisors signal the process and its group: one request, two SIGTERMs.
static void CaseDuplicateSignalAbsorbed() {
    std::printf("  a SIGTERM delivered twice at once is still one orderly request\n");
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        gdx_shutdown_signal_install();
        raise(SIGTERM);
        raise(SIGTERM);
        _exit(gdx_shutdown_signal_requested() == SIGTERM ? 0 : 6);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    check("child survives a duplicate and exits cleanly", WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void CaseSecondSignalKills() {
    std::printf("  a second SIGTERM 2 s later still kills, without running exit handlers\n");
    int fds[2];
    if (pipe(fds) != 0) {
        check("pipe", false);
        return;
    }
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        static int sWriteFd = fds[1];
        close(fds[0]);
        std::atexit([] { (void)!write(sWriteFd, "x", 1); });
        gdx_shutdown_signal_install();
        raise(SIGTERM);
        const struct timespec wait = { 2, 200000000 }; // past the 2 s duplicate window
        nanosleep(&wait, nullptr);
        raise(SIGTERM);
        _exit(5); // not reached
    }
    close(fds[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    char c = 0;
    const ssize_t n = read(fds[0], &c, 1);
    close(fds[0]);
    check("child died on SIGTERM", WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    check("no exit handler ran", n == 0);
}

#endif

int main() {
    std::printf("gdx_vr_settings_tests\n");
    char tmpl[] = "/tmp/gdx_vr_settings_tests.XXXXXX";
#ifndef _WIN32
    if (mkdtemp(tmpl) == nullptr) {
        std::printf("mkdtemp failed\n");
        return 1;
    }
    setenv("SHIP_HOME", tmpl, 1);
    gDir = tmpl;
#else
    gDir = "."; // libultraship's portable app directory
    std::remove(ConfigPath().c_str());
#endif
    gContext = new std::shared_ptr<Ship::Context>(
        Ship::Context::CreateUninitializedInstance("gdx_vr_settings_tests", "gdxvrtests", "gdiffuser.cfg.json"));
    (*gContext)->InitConfiguration();
    (*gContext)->InitConsoleVariables();

    CaseFirstLaunch(); // must stay first: it needs a config file that has never existed
    CaseRenamedKeys();
    CaseWrongTypes();
#ifndef _WIN32
    CaseSigtermAfterStartupSave();
    CaseDuplicateSignalAbsorbed();
    CaseSecondSignalKills();
#endif

    std::remove(ConfigPath().c_str());
#ifndef _WIN32
    rmdir(tmpl);
#endif
    std::printf("%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}

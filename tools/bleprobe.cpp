// ---------------------------------------------------------------------------
// bleprobe - run the BLE link standalone, without the game.
//
// Why it exists: validating BLE inside the game costs a full round of
// "edit config -> launch game -> drive -> read log", and when it fails it is
// hard to tell whether BLE itself is broken or the mod environment is. This
// probe does one thing: bring up the BLE transport, feed it frames at
// frame_ms, and echo every line the transport logs. Adapter / advertised-name
// / GATT / notify-subscription / ack problems all surface here, with no game
// involvement.
//
// It links the SAME src\transport\ble_winrt.cpp the game uses, so what gets
// validated is the shipping code path, not an "approximately equivalent" copy.
//
// Usage:
//   bleprobe.exe <DGLab_IDAC.ini> [seconds] [--live]
//
//   Without --live (recommended for the first run) both channel strengths are
//   forced to 0: the whole link path still runs (scan / connect / discover
//   GATT / subscribe notify / write B0 / await B1), but the device cannot
//   produce any output. Use it to prove the link works.
//   With --live the strengths from [channel_*] base_strength are used, which
//   is what you want to check the device LED, feel the output, or exercise the
//   B1 closed loop.
//
// Build: tools\build_bleprobe.cmd
//
// NOTE: all console output here is deliberately ASCII/English. This is a
// console tool and Chinese text gets mangled by the OEM codepage in a cmd
// window; the Chinese lines that matter (the transport's own diagnostics) go
// to DGLab_IDAC.log, which is written as UTF-8.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "logger.hpp"
#include "transport/transport.hpp"

using namespace dglab_idac;

static std::atomic<int> g_frames_submitted{0};

// ---- --freeze: suspend every thread of THIS process except the caller, then
//      resume it. ----
// Why not Task Manager / Resource Monitor? Their "Suspend Process" command is
// unreliable on some Windows 10/11 builds (the context menu entry does nothing),
// and suspending another process needs cross-process rights that the tool would
// have to be granted. Suspending ourselves needs no extra rights and produces
// the same thing as seen from the device: a long stretch with no B0 frame.
//
// Threads are resumed through the very handles we suspended with, so a thread
// created during the freeze is never over-resumed.
static int freeze_other_threads(std::vector<HANDLE>* out) {
    const DWORD my_pid = GetCurrentProcessId();
    const DWORD self_tid = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    int n = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != my_pid) continue;
            if (te.th32ThreadID == self_tid) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (h == nullptr) continue;
            if (SuspendThread(h) == static_cast<DWORD>(-1)) {
                CloseHandle(h);
                continue;
            }
            out->push_back(h);
            ++n;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

static int resume_threads(std::vector<HANDLE>& hs) {
    int n = 0;
    for (HANDLE h : hs) {
        if (ResumeThread(h) != static_cast<DWORD>(-1)) ++n;
        CloseHandle(h);
    }
    hs.clear();
    return n;
}

// ---- Build a frame on the frame_ms beat. A simple ramp waveform, so the
//      device LED has something visible to show. ----
// Pulse intensity and frequency are overridable so a hardware/LED test can use
// a strong, steady output instead of the gentle default ramp. Both default to
// the original values when the caller leaves them alone.
static int g_pulse_a = 60;   // 0..100
static int g_pulse_b = 40;   // 0..100
static int g_pulse_freq = 30;  // transmitted value; 10 = 100 Hz, 240 = 1 Hz
static bool g_steady = false;

static Frame make_frame(const Config& cfg, int tick) {
    Frame f{};

    const int str_a = cfg.channel_a.enable ? cfg.channel_a.base_strength : 0;
    const int str_b = cfg.channel_b.enable ? cfg.channel_b.base_strength : 0;

    f.a.strength = str_a;
    f.b.strength = str_b;
    f.muted = false;

    if (g_steady) {
        // Constant full-width pulses on every slot: the most visible output the
        // device can produce, so an indicator LED (if output-driven) must react.
        for (int i = 0; i < kSlotsPerFrame; ++i) {
            f.a.slots[i].pulse_intensity = static_cast<uint8_t>(g_pulse_a);
            f.a.slots[i].pulse_frequency = static_cast<uint8_t>(g_pulse_freq);
            f.b.slots[i].pulse_intensity = static_cast<uint8_t>(g_pulse_b);
            f.b.slots[i].pulse_frequency = static_cast<uint8_t>(g_pulse_freq);
        }
        return f;
    }

    // Phase: one full up/down every 20 frames (~2 s at frame_ms=100)
    const double phase = static_cast<double>(tick % 20) / 20.0;
    const double tri = phase < 0.5 ? phase * 2.0 : (1.0 - phase) * 2.0;

    for (int i = 0; i < kSlotsPerFrame; ++i) {
        // Stagger the 4 slots a little so they are not identical
        const double p = tri + 0.08 * i;
        const double v = p - static_cast<int>(p);
        f.a.slots[i].pulse_intensity = static_cast<uint8_t>(v * g_pulse_a);
        f.a.slots[i].pulse_frequency = static_cast<uint8_t>(g_pulse_freq);
        f.b.slots[i].pulse_intensity = static_cast<uint8_t>(v * g_pulse_b);
        f.b.slots[i].pulse_frequency = static_cast<uint8_t>(g_pulse_freq);
    }
    return f;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf(
            "usage: bleprobe.exe <DGLab_IDAC.ini> [seconds] [--live]\n"
            "\n"
            "  without --live (recommended first run): forces both channel strengths to 0.\n"
            "      The whole link path still runs (scan / connect / discover GATT /\n"
            "      subscribe notify / write B0 / await B1), but the device produces no\n"
            "      output. Use it to prove the link works.\n"
            "  --live: use base_strength from [channel_*] (currently 15/10).\n"
            "      Use it to check the device LED, feel the output, or see B1 acks.\n"
            "  --setstrength=N: force both channel strengths to N, bypassing the ini\n"
            "      clamp, to find out what the device will accept.\n"
            "  --pulse=A[,B]: pulse intensity 0..100 for channels A,B (default 60,40).\n"
            "  --freq=N: transmitted pulse frequency 10..240 (10 = 100 Hz, 240 = 1 Hz).\n"
            "  --steady: constant pulses on every slot instead of the gentle ramp.\n"
            "      The strongest, most visible output the device can make -- use it\n"
            "      when asking 'does the indicator LED react to output at all?'.\n"
            "  --stall=N: half way through the run, stop submitting frames for N\n"
            "      seconds. This is the only way to exercise [safety] watchdog_ms on\n"
            "      demand: the transport's own send loop keeps beating, so once the\n"
            "      mailbox goes older than watchdog_ms it must switch to the idle\n"
            "      frame and log 'watchdog triggered'. Expect exactly one warning,\n"
            "      wd=1, trips=1, idle counting up, then normal frames again.\n"
            "  --freeze=N: at 35%% of the run, suspend every other thread of this\n"
            "      process for N seconds, then resume. Same effect as Task Manager's\n"
            "      'Suspend Process' (K2 bench case) but needs no extra rights and\n"
            "      no UI. The link is dead for those N seconds: no B0 reaches the\n"
            "      device, so whatever the device does then is what a frozen game\n"
            "      would leave the user with.\n"
            "\n"
            "Transport diagnostics are written to DGLab_IDAC.log next to the ini's\n"
            "working directory (UTF-8). Watch that file for the detailed reason.\n");
        return 2;
    }

    const std::string ini = argv[1];
    int seconds = 20;
    if (argc >= 3) {
        const int v = std::atoi(argv[2]);
        if (v > 0) seconds = v;
    }
    bool live = false;
    int force_strength = -1;
    int stall_seconds = 0;
    int freeze_seconds = 0;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--live") == 0) live = true;
        if (std::strcmp(argv[i], "--steady") == 0) g_steady = true;
        if (std::strncmp(argv[i], "--stall=", 8) == 0) {
            const int v = std::atoi(argv[i] + 8);
            stall_seconds = v < 1 ? 0 : v;
        }
        if (std::strncmp(argv[i], "--freeze=", 9) == 0) {
            const int v = std::atoi(argv[i] + 9);
            freeze_seconds = v < 1 ? 0 : v;
        }
        if (std::strncmp(argv[i], "--setstrength=", 14) == 0) force_strength = std::atoi(argv[i] + 14);
        if (std::strncmp(argv[i], "--freq=", 7) == 0) {
            const int v = std::atoi(argv[i] + 7);
            // The device discards a channel whose pulse frequency is out of
            // range, so clamp rather than let a typo silently kill the output.
            g_pulse_freq = v < 10 ? 10 : (v > 240 ? 240 : v);
        }
        if (std::strncmp(argv[i], "--pulse=", 8) == 0) {
            const char* s = argv[i] + 8;
            int a = std::atoi(s);
            const char* comma = std::strchr(s, ',');
            int b = comma ? std::atoi(comma + 1) : a;
            g_pulse_a = a < 0 ? 0 : (a > 100 ? 100 : a);
            g_pulse_b = b < 0 ? 0 : (b > 100 ? 100 : b);
        }
    }

    // Must open the log. Every transport diagnostic (HRESULT, GATT state, ack)
    // goes through log_*, and logger.cpp's emit() is a no-op while g_file is
    // null -- without this you see nothing but a nullptr and lose every clue.
    log_init();

    Config cfg = load_config(ini);
    if (!live) {
        // No --live: force 0 so this run cannot possibly stimulate the device,
        // while still exercising the whole link.
        cfg.channel_a.base_strength = 0;
        cfg.channel_b.base_strength = 0;
    }
    if (force_strength >= 0) {
        // Bypass the [safety] clamp on purpose: this mode is for finding out
        // what the device will accept, and clamping it would hide the answer.
        cfg.channel_a.base_strength = force_strength;
        cfg.channel_b.base_strength = force_strength;
        cfg.clamp_strength = 200;
        cfg.max_strength = 200;
    }

    std::printf("==================== bleprobe ====================\n");
    std::printf("  config file : %s\n", ini.c_str());
    std::printf("  link mode   : %s\n", to_string(cfg.link));
    std::printf("  device name : %s\n", cfg.ble_device_name.c_str());
    std::printf("  frame beat  : %d ms\n", cfg.frame_ms);
    std::printf("  strength A  : %d\n", cfg.channel_a.base_strength);
    std::printf("  strength B  : %d\n", cfg.channel_b.base_strength);
    std::printf("  dry_run     : %d\n", cfg.dry_run ? 1 : 0);
    std::printf("  pulse A/B   : %d / %d  (0..100)\n", g_pulse_a, g_pulse_b);
    std::printf("  pulse freq  : %d  (%s)\n", g_pulse_freq,
                g_pulse_freq <= 100 ? "10..100 ms unit" : "compressed 100..240");
    std::printf("  pattern     : %s\n", g_steady ? "STEADY (constant, maximum visibility)"
                                                 : "ramp (gentle)");
    std::printf("  duration    : %d s\n", seconds);
    if (stall_seconds > 0) {
        std::printf("  stall test  : stop submitting for %d s at the half-way point\n",
                    stall_seconds);
    }
    if (freeze_seconds > 0) {
        std::printf("  freeze test : suspend all other threads for %d s at 35%% of the run\n",
                    freeze_seconds);
    }
    if (force_strength >= 0) {
        std::printf("  mode        : SETSTRENGTH %d (device-acceptance probe)\n", force_strength);
    } else {
        std::printf("  mode        : %s\n",
                    live ? "LIVE (strengths from ini)" : "LINK ONLY (strengths forced to 0)");
    }
    std::printf("--------------------------------------------------\n");
    std::fflush(stdout);

    if (cfg.dry_run) {
        std::printf("[!] ini has dry_run=1. This probe IGNORES that switch (otherwise\n");
        std::printf("    nothing could ever be tested). Output safety comes from A/B=0.\n");
        std::fflush(stdout);
    }

    ITransport* t = create_ble_transport(cfg);
    if (t == nullptr) {
        std::printf("[x] create_ble_transport returned nullptr.\n");
        std::printf("    check the \"BLE: ...\" lines in DGLab_IDAC.log for the HRESULT\n");
        return 1;
    }
    std::printf("[i] transport impl: %s\n", t->name());
    std::fflush(stdout);

    if (!t->start()) {
        std::printf("[x] start() failed -- see the BLE log lines\n");
        destroy_transport(t);
        return 1;
    }

    const int frame_ms = cfg.frame_ms > 0 ? cfg.frame_ms : 100;
    const auto t0 = std::chrono::steady_clock::now();
    const auto deadline = t0 + std::chrono::seconds(seconds);

    int tick = 0;
    auto next_status = t0 + std::chrono::milliseconds(1000);

    // --stall: half way through, stop calling submit() for N seconds. The
    // transport's send loop is untouched, so it keeps beating; once the newest
    // frame in the mailbox is older than [safety] watchdog_ms it must decide to
    // send the idle frame instead. That is the one path the offline logic_check
    // cannot cover end to end (device ack included), so it needs a real run.
    const auto stall_at = t0 + std::chrono::seconds(seconds / 2);
    bool stall_armed = stall_seconds > 0;
    bool stall_seen = false;

    // --freeze: earlier than --stall so the two never overlap if both are given.
    const auto freeze_at = t0 + std::chrono::seconds(seconds * 35 / 100);
    bool freeze_armed = freeze_seconds > 0;
    bool freeze_seen = false;

    while (std::chrono::steady_clock::now() < deadline) {
        const auto now_pre = std::chrono::steady_clock::now();
        if (freeze_armed && !freeze_seen && now_pre >= freeze_at) {
            freeze_seen = true;
            std::vector<HANDLE> suspended;
            const int n = freeze_other_threads(&suspended);
            std::printf("[>>] FREEZE: suspended %d thread(s) (this one keeps running) for %d s.\n"
                        "     No B0 leaves the process now. What the device does in the next\n"
                        "     %d s is exactly what a frozen game would leave you with.\n",
                        n, freeze_seconds, freeze_seconds);
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::seconds(freeze_seconds));
            const int rn = resume_threads(suspended);
            std::printf("[<<] FREEZE over: resumed %d thread(s).\n", rn);
            std::fflush(stdout);
            next_status = std::chrono::steady_clock::now();  // report immediately
            continue;
        }
        if (stall_armed && !stall_seen && now_pre >= stall_at) {
            stall_seen = true;
            std::printf("[>>] STALL starts: no submit() for %d s. The transport keeps\n"
                        "     beating, so wd must flip to 1 and trips to 1 after %d ms.\n",
                        stall_seconds, cfg.watchdog_ms);
            std::fflush(stdout);
            const auto stall_end = now_pre + std::chrono::seconds(stall_seconds);
            while (std::chrono::steady_clock::now() < stall_end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                const auto now = std::chrono::steady_clock::now();
                if (now >= next_status) {
                    const auto el = std::chrono::duration_cast<std::chrono::seconds>(now - t0).count();
                    std::printf("[status %3llds] STALLED, submitted %d frames | %s\n",
                                static_cast<long long>(el), tick, t->status().c_str());
                    std::fflush(stdout);
                    next_status = now + std::chrono::milliseconds(1000);
                }
            }
            std::printf("[<<] STALL over: resuming submit(). Watch trips stay at 1 and\n"
                        "     normal frames come back (idle should stop counting up).\n");
            std::fflush(stdout);
            next_status = std::chrono::steady_clock::now();  // report immediately
            continue;
        }

        t->submit(make_frame(cfg, tick));
        ++tick;

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_status) {
            const auto el = std::chrono::duration_cast<std::chrono::seconds>(now - t0).count();
            std::printf("[status %3llds] submitted %d frames | %s\n",
                        static_cast<long long>(el), tick, t->status().c_str());
            std::fflush(stdout);
            next_status = now + std::chrono::milliseconds(1000);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(frame_ms));
    }

    // 最终状态再打一次 —— B1 计数只在 status() 里, 收尾这一条是诊断 A 的判据。
    std::printf("--------------------------------------------------\n");
    std::printf("[i] 收尾状态 (重点看 b1): %s\n", t->status().c_str());
    std::printf("[i] submitted %d frames total\n", tick);
    std::fflush(stdout);

    t->stop();
    destroy_transport(t);

    std::printf("==================== done ====================\n");
    return 0;
}

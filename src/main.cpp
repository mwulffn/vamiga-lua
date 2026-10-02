// vamiga-lua: the vAmiga core with a Lua scripting engine and a control
// port, and nothing else. There is no window: the picture is read from the
// frame buffer of the core by Lua code (see the video table).

#include "engine.h"

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace vamiga;

VAmiga *g_vamiga;

static bool g_warp;
static bool g_quit_requested;

// The emulator thread tells the main thread when it has paused, and what
// happened before that.
static std::mutex g_pause_mutex;
static std::condition_variable g_pause_condition;
static long g_pause_count;
static std::vector<host_event> g_events;

void host_log(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}

Amiga &core(void)
{
    return *g_vamiga->amiga.amiga;
}

void host_set_option(Opt option, i64 value)
{
    // The functions of the emulator object change the option at once. The
    // ones in the public API put the change in a queue, which the emulator
    // thread empties before it runs the next frame, so that Lua code would
    // see the old value for a while.
    Emulator &emulator = core().emulator;
    emulator.check(option, value);
    emulator.set(option, value);
}

void host_set_warp(bool warp)
{
    g_warp = warp;
}

void host_request_quit(void)
{
    g_quit_requested = true;
}

bool host_quit_requested(void)
{
    return g_quit_requested;
}

// Called by the core, on the emulator thread, for every message it sends.
static void process_message(const void *listener, Message message)
{
    std::lock_guard<std::mutex> guard(g_pause_mutex);
    switch (message.type) {
        case Msg::EOF_REACHED:
            g_events.push_back({HOST_FRAME_END, 0, -1});
            break;
        case Msg::BREAKPOINT_REACHED:
            g_events.push_back({HOST_BREAKPOINT, message.cpu.pc, -1});
            break;
        case Msg::CATCHPOINT_REACHED:
            g_events.push_back({HOST_EXCEPTION, message.cpu.pc, message.cpu.vector});
            break;
        case Msg::STEP:
            g_events.push_back({HOST_STEP, 0, -1});
            break;
        case Msg::CPU_HALT:
            g_events.push_back({HOST_HALT, 0, -1});
            break;
        case Msg::PAUSE:
            g_pause_count++;
            g_pause_condition.notify_one();
            break;
        default:
            break;
    }
}

std::vector<host_event> host_run(bool single_step)
{
    long target;
    {
        std::lock_guard<std::mutex> guard(g_pause_mutex);
        target = g_pause_count + 1;
        g_events.clear();
    }
    // The emulator pauses at the end of the frame (this is what the
    // finishFrame function of the core does), if nothing pauses it before
    // that.
    core().agnus.dmaDebugger.eofTrap = true;
    // The emulator is set running directly. The run and stepInto functions
    // of the core first check that the emulator is ready to run, which
    // includes computing the checksum of the ROM, and that takes about as
    // long as emulating a third of a frame. It was checked once, in main.
    Emulator &emulator = core().emulator;
    if (single_step) {
        core().cpu.debugger.stepInto();
    }
    emulator.switchState(ExecState::RUNNING);
    emulator.wakeUp();
    // The messages about what happened are sent before the emulator has
    // paused, so the one to wait for is the one about the pause.
    std::unique_lock<std::mutex> lock(g_pause_mutex);
    g_pause_condition.wait(lock, [&] { return g_pause_count >= target; });
    return g_events;
}

static const struct {
    const char *name;
    ConfigScheme scheme;
} models[] = {
    {"A1000", ConfigScheme::A1000_OCS_1MB},
    {"A500", ConfigScheme::A500_OCS_1MB},
    {"A500-ECS", ConfigScheme::A500_ECS_1MB},
    {"A500+", ConfigScheme::A500_PLUS_1MB},
    {"A1200", ConfigScheme::A1200_2MB},
};

static void usage(void)
{
    fprintf(
        stderr,
        "Usage: vamiga-lua --rom <file> [options]\n"
        "\n"
        "  --model <name>      A1000, A500 (default), A500-ECS, A500+ or A1200\n"
        "  --rom <file>        Kickstart ROM\n"
        "  --ext <file>        Extension ROM (needed by the AROS ROM)\n"
        "  --df0 <file>        Disk image for drive DF0 (also --df1 to --df3)\n"
        "  --set <opt>=<value> Set an option of the vAmiga core, for example\n"
        "                      MEM.SLOW_RAM=512 (can be given several times)\n"
        "  --lua <file>        Run a Lua script (can be given several times)\n"
        "  --port <number>     Listen on this port on 127.0.0.1 for Lua code\n"
        "  --warp              Run as fast as possible from the start\n");
}

int main(int argc, char *argv[])
{
    std::string model = "A500", rom, ext;
    std::string disks[4];
    std::vector<std::string> scripts, settings;
    int port = 0;

    for (int i = 1; i < argc; i++) {
        std::string argument = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s needs a value\n", argument.c_str());
                exit(2);
            }
            return argv[++i];
        };
        if (argument == "--model") {
            model = value();
        } else if (argument == "--rom") {
            rom = value();
        } else if (argument == "--ext") {
            ext = value();
        } else if (argument.size() == 5 && argument.compare(0, 4, "--df") == 0 &&
                   argument[4] >= '0' && argument[4] <= '3') {
            disks[argument[4] - '0'] = value();
        } else if (argument == "--set") {
            settings.push_back(value());
        } else if (argument == "--lua") {
            scripts.push_back(value());
        } else if (argument == "--port") {
            port = atoi(value().c_str());
        } else if (argument == "--warp") {
            g_warp = true;
        } else {
            usage();
            return argument == "--help" ? 0 : 2;
        }
    }
    if (rom.empty()) {
        usage();
        return 2;
    }

    // Writing to a remote client which has gone away must not end the
    // program.
    signal(SIGPIPE, SIG_IGN);

    // The emulator is never destroyed: the program ends by returning from
    // main, also when there is an error in the command line.
    VAmiga &emulator = *new VAmiga;
    g_vamiga = &emulator;
    const ConfigScheme *scheme = NULL;
    for (const auto &entry : models) {
        if (model == entry.name) {
            scheme = &entry.scheme;
        }
    }
    if (scheme == NULL) {
        fprintf(stderr, "Unknown model '%s'\n", model.c_str());
        return 2;
    }
    try {
        // The emulator thread is started first: starting it puts the
        // configuration back to its defaults. The listener must not be NULL
        // for the function to be called.
        emulator.launch(&emulator, process_message);
        core().emulator.set(*scheme);
        // The core never waits for real time. This program does, in the
        // frame loop below, when warp is off.
        host_set_option(Opt::AMIGA_WARP_MODE, (i64) Warp::ALWAYS);
        // In warp mode the core only draws some of the frames, unless it is
        // told to draw them all. Lua code must be able to see every frame.
        host_set_option(Opt::DENISE_FRAME_SKIPPING, 0);
        // The colours of the Amiga are given to Lua code as they are, and
        // not adjusted to look like they do on a monitor.
        host_set_option(Opt::MON_PALETTE, (i64) Palette::RGB);
        for (const std::string &setting : settings) {
            size_t equals = setting.find('=');
            auto option = OptEnum::parseEnum(setting.substr(0, equals));
            if (equals == std::string::npos || !option) {
                fprintf(stderr, "Unknown option in '--set %s'\n", setting.c_str());
                return 2;
            }
            host_set_option(*option, OptionParser::parse(*option, setting.substr(equals + 1)));
        }
        emulator.mem.loadRom(rom);
        if (!ext.empty()) {
            emulator.mem.loadExt(ext);
        }
        for (int drive = 0; drive < 4; drive++) {
            if (!disks[drive].empty()) {
                emulator.df[drive]->insert(disks[drive], false);
                engine_set_media_path(drive, disks[drive]);
            }
        }
    } catch (std::exception &e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }

    try {
        // The emulator is not asked again whether it can run (see host_run).
        emulator.isReady();
    } catch (std::exception &e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }

    engine_init(port);
    for (const std::string &script : scripts) {
        engine_load(script.c_str());
    }

    auto next_frame_at = std::chrono::steady_clock::now();
    while (!g_quit_requested) {
        if (engine_stop_requested()) {
            engine_stopped_loop();
            next_frame_at = std::chrono::steady_clock::now();
            continue;
        }
        bool frame_ended;
        try {
            frame_ended = engine_run();
        } catch (std::exception &e) {
            host_log("The emulator cannot run: %s", e.what());
            break;
        }
        if (!frame_ended) {
            continue;
        }
        if (!g_warp) {
            auto frame_time = std::chrono::duration<double>(1.0 / core().refreshRate());
            next_frame_at += std::chrono::duration_cast<std::chrono::nanoseconds>(frame_time);
            auto now = std::chrono::steady_clock::now();
            if (next_frame_at < now) {
                next_frame_at = now;
            }
            std::this_thread::sleep_until(next_frame_at);
        }
        engine_frame();
    }

    engine_free();
    emulator.removeListener();
    emulator.halt();
    return 0;
}

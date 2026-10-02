// vamiga-lua: the vAmiga core with a Lua scripting engine and a control
// port. The picture is read from the frame buffer of the core by Lua code
// (see the video table). A window which shows it, and plays the sound, can be
// opened for a person to watch, but nothing depends on it (see window.h).

#include "engine.h"
#include "window.h"

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
    window_set_playing(!warp);
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
        case Msg::COPPERBP_REACHED:
            g_events.push_back({HOST_COPPER_BREAKPOINT, 0, -1});
            break;
        case Msg::COPPERWP_REACHED:
            g_events.push_back({HOST_COPPER_WATCHPOINT, 0, -1});
            break;
        case Msg::BEAMTRAP_REACHED:
            g_events.push_back({HOST_BEAM, 0, -1});
            break;
        case Msg::PAUSE:
            g_pause_count++;
            g_pause_condition.notify_one();
            break;
        default:
            break;
    }
}

// Where a frame ends
//
// A frame ends, for this program, where the core ends it: when the beam has
// passed the horizontal position below in line 0, where the core has just
// swapped its frame buffers, and the CPU has finished the instruction it was
// running. There are two ways to stop the emulator there, and both stop it
// at exactly the same point, so a script sees the same with and without
// sound:
//
// - Without sound, a beam trap of the core at that position pauses the
//   emulator (run_until_pause).
// - With sound, the emulator is not paused, as that would silence it. The
//   core leaves its frame function at this point, and the main thread takes
//   hold of the emulator thread there (run_frame_with_sound).
#define FRAME_END_POSITION HOST_FRAME_END_TRAP

// The clock of the core when the emulator was last set running.
static Cycle g_run_start;
// True while the emulator is in its running state and is held between two
// frames by the main thread.
static bool g_held;

// True if the emulator has stopped where a frame ends, and got there since
// it was last set running.
static bool at_frame_end(void)
{
    const Agnus &agnus = core().agnus;
    // The CPU can be up to the length of its longest instruction past the
    // position, and an interrupt can be taken on top of that.
    isize passed = agnus.pos.h - FRAME_END_POSITION;
    if (agnus.pos.v != 0 || passed < 0 || passed >= 150) {
        return false;
    }
    // A reset starts the clock, and the frame, from the beginning, so the
    // beam has passed the position since then.
    return agnus.clock < g_run_start || agnus.clock - DMA_CYCLES(passed) > g_run_start;
}

// Sets or removes the beam trap which pauses the emulator where a frame
// ends. A beam breakpoint set from Lua at the same position is left alone.
static void set_frame_end_trap(bool wanted)
{
    Agnus &agnus = core().agnus;
    Beamtraps &traps = agnus.dmaDebugger.beamtraps;
    if (wanted && !traps.isSetAt(FRAME_END_POSITION)) {
        traps.setAt(FRAME_END_POSITION);
    } else if (!wanted && traps.isSetAt(FRAME_END_POSITION) &&
               !engine_debug_has_beam_break(FRAME_END_POSITION)) {
        traps.removeAt(FRAME_END_POSITION);
    }
    // The event of the beam traps is part of the state of the machine, so
    // loading a state saved without them leaves them unscheduled.
    if (traps.elements() > 0 && !agnus.isPending<SLOT_BTR>()) {
        traps.scheduleNextEvent();
    }
}

// Sets the emulator running. The run and stepInto functions of the core are
// not used: they first check that the emulator is ready to run, which
// includes computing the checksum of the ROM, and that takes about as long
// as emulating a third of a frame. It was checked once, in main.
static void set_running(bool single_step)
{
    if (single_step) {
        core().cpu.debugger.stepInto();
    }
    core().emulator.switchState(ExecState::RUNNING);
}

// Prepares for a run, and returns the number the pause count has when the
// emulator has paused.
static long prepare_run(bool frame_end_trap)
{
    set_frame_end_trap(frame_end_trap);
    g_run_start = core().agnus.clock;
    std::lock_guard<std::mutex> guard(g_pause_mutex);
    g_events.clear();
    return g_pause_count + 1;
}

// Runs the emulator the way it is done without sound: it is set running, and
// pauses itself at the end of the frame or before that.
static std::vector<host_event> run_until_pause(bool single_step)
{
    long target = prepare_run(true);
    set_running(single_step);
    core().emulator.wakeUp();
    // The messages about what happened are sent before the emulator has
    // paused, so the one to wait for is the one about the pause.
    std::unique_lock<std::mutex> lock(g_pause_mutex);
    g_pause_condition.wait(lock, [&] { return g_pause_count >= target; });
    return g_events;
}

// The two locks of the emulator thread of the core. The thread holds the
// frame lock while it emulates, and takes the other one for a moment between
// two frames. The core has them locked and unlocked together, by its
// functions suspend and resume, which is not enough to let the thread run
// one frame and no more, so they are reached through a class derived from
// the one they are in.
struct thread_locks : Thread {
    static auto &frame(Thread &thread) { return thread.*(&thread_locks::lock); }
    static auto &between_frames(Thread &thread)
    {
        return thread.*(&thread_locks::suspensionLock);
    }
};

static bool paused_since(long target)
{
    std::lock_guard<std::mutex> guard(g_pause_mutex);
    return g_pause_count >= target;
}

// Runs one frame while sound is played. The core mutes the sound, and fades
// out what it has produced, every time the emulator is paused, so here the
// emulator is left in its running state, and the main thread holds both
// locks of the emulator thread between two frames.
//
// To run a frame, the main thread first lets go of the lock the emulator
// thread needs to get from one frame to the next, and then of the frame
// lock. It never lets go of both, so the emulator thread gets to emulate
// once: to the end of the frame, or to where a breakpoint pauses the
// emulator, and the sound with it.
static std::vector<host_event> run_frame_with_sound(void)
{
    Emulator &emulator = core().emulator;
    long target = prepare_run(false);
    if (!g_held) {
        emulator.suspend();
        set_running(false);
        g_held = true;
    }
    auto &frame = thread_locks::frame(emulator);
    auto &between_frames = thread_locks::between_frames(emulator);
    // The emulator thread takes the locks when it gets to run, which need
    // not be the first time they are free.
    while (core().agnus.clock == g_run_start && emulator.isRunning() && !paused_since(target)) {
        between_frames.unlock();
        emulator.wakeUp();
        std::this_thread::yield();
        between_frames.lock();
        frame.unlock();
        std::this_thread::yield();
        frame.lock();
    }
    if (!emulator.isRunning()) {
        emulator.resume();
        g_held = false;
    }
    std::lock_guard<std::mutex> guard(g_pause_mutex);
    return g_events;
}

// Pauses an emulator which is held in its running state. The emulator
// thread does that itself, before it emulates anything, when it is let go
// of: it has been waiting between two frames since the last one.
static void pause_held_emulator(void)
{
    if (!g_held) {
        return;
    }
    Emulator &emulator = core().emulator;
    long target;
    {
        std::lock_guard<std::mutex> guard(g_pause_mutex);
        target = g_pause_count + 1;
    }
    g_vamiga->pause();
    emulator.resume();
    g_held = false;
    std::unique_lock<std::mutex> lock(g_pause_mutex);
    do {
        emulator.wakeUp();
    } while (!g_pause_condition.wait_for(lock, std::chrono::milliseconds(1),
                                         [&] { return g_pause_count >= target; }));
}

std::vector<host_event> host_run(bool single_step)
{
    std::vector<host_event> events;
    if (window_sound_active() && !single_step) {
        events = run_frame_with_sound();
    } else {
        pause_held_emulator();
        events = run_until_pause(single_step);
    }
    if (at_frame_end()) {
        events.push_back({HOST_FRAME_END, 0, -1});
    }
    return events;
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
        "  --warp              Run as fast as possible from the start\n"
        "  --window            Show the picture in a window and play the sound\n"
        "  --volume <percent>  How loud the sound is played, 0 to 100 (default 100)\n");
}

int main(int argc, char *argv[])
{
    std::string model = "A500", rom, ext;
    std::string disks[4];
    std::vector<std::string> scripts, settings;
    int port = 0;
    bool window = false;

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
        } else if (argument == "--window") {
            window = true;
        } else if (argument == "--volume") {
            window_set_volume(atoi(value().c_str()));
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
    window_set_playing(!g_warp);
    if (window) {
        std::string error;
        if (!window_open(error)) {
            fprintf(stderr, "Could not open the window: %s\n", error.c_str());
            return 1;
        }
    }
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
            auto frame_time = std::chrono::duration<double>(1.0 / core().nativeRefreshRate());
            next_frame_at += std::chrono::duration_cast<std::chrono::nanoseconds>(frame_time);
            auto now = std::chrono::steady_clock::now();
            if (next_frame_at < now) {
                next_frame_at = now;
            }
            std::this_thread::sleep_until(next_frame_at);
        }
        engine_frame();
        window_frame();
    }

    pause_held_emulator();
    window_close();
    engine_free();
    emulator.removeListener();
    emulator.halt();
    return 0;
}

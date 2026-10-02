// The Lua engine: tasks, the emu table and the frame loop.
//
// One Lua state is shared by all scripts and remote requests, and everything
// runs on the main thread while the emulator is paused (see host.h).

#include "engine.h"

#include "window.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace vamiga;

lua_State *g_engine_state;

static std::vector<engine_task *> g_tasks;
// Registry references to the functions registered with emu.on_frame.
static std::vector<int> g_frame_callbacks;
static int64_t g_frame;
// The task being resumed, or NULL.
static engine_task *g_current_task;
// The emulation is stopped (in engine_stopped_loop) while this is set.
static bool g_stop_requested;
// When not 0, the number of frames left to run before stopping again.
static int64_t g_step_frames;
// Why the emulation was stopped, for dbg.wait and the stopped event.
static struct {
    const char *reason;
    int id;
    uint32_t address;
    // The exception vector or halt reason, or -1.
    int vector;
} g_stop_info;

void engine_log_error(lua_State *L, const char *context)
{
    const char *message = lua_tostring(L, -1);
    host_log("[LUA] %s: %s", context, message ? message : "(no message)");
    lua_pop(L, 1);
}

// Concatenates the arguments like print does, leaving the result on the
// stack.
static void concat_print_arguments(lua_State *L)
{
    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) {
            luaL_addchar(&b, '\t');
        }
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
}

static int l_print(lua_State *L)
{
    concat_print_arguments(L);
    size_t len;
    const char *text = lua_tolstring(L, -1, &len);
    if (g_current_task != NULL && g_current_task->client != -1) {
        g_current_task->output.append(text, len);
        g_current_task->output += '\n';
    } else {
        host_log("[LUA] %s", text);
        std::string fields = "\"text\":";
        engine_json_append_string(fields, text, len);
        engine_remote_send_event("print", fields);
    }
    return 0;
}

// Resumes the task with nargs arguments on its stack. The task is freed if
// it finished, or put on the waiting list if it yielded.
static void resume_task(engine_task *task, int nargs)
{
    lua_State *L = g_engine_state;
    int nresults;
    engine_task *previous_task = g_current_task;
    g_current_task = task;
    task->wait_stop = false;
    int status = lua_resume(task->thread, L, nargs, &nresults);
    g_current_task = previous_task;
    if (status == LUA_YIELD) {
        // The yielded value is the number of frames to wait.
        lua_Integer frames = nresults > 0 ? lua_tointeger(task->thread, -1) : 1;
        lua_pop(task->thread, nresults);
        task->wake_frame = g_frame + (frames > 1 ? frames : 1);
        g_tasks.push_back(task);
        return;
    }
    if (task->client != -1) {
        engine_remote_task_finished(task, status == LUA_OK, nresults);
    } else if (status != LUA_OK) {
        luaL_traceback(L, task->thread, lua_tostring(task->thread, -1), 0);
        engine_log_error(L, "Error");
    }
    luaL_unref(L, LUA_REGISTRYINDEX, task->ref);
    delete task;
}

void engine_start_task(lua_State *L, int nargs, int client, const char *request_id)
{
    engine_task *task = new engine_task();
    task->thread = lua_newthread(L);
    task->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    task->wake_frame = 0;
    task->wait_stop = false;
    task->client = client;
    task->request_id = request_id;
    lua_xmove(L, task->thread, nargs + 1);
    resume_task(task, nargs);
}

static void resume_due_tasks(void)
{
    // Tasks resumed here may be added to g_tasks again.
    std::vector<engine_task *> tasks;
    tasks.swap(g_tasks);
    for (engine_task *task : tasks) {
        if (task->wake_frame <= g_frame) {
            resume_task(task, 0);
        } else {
            g_tasks.push_back(task);
        }
    }
}

static void run_frame_callbacks(void)
{
    lua_State *L = g_engine_state;
    // Callbacks may register or remove callbacks.
    std::vector<int> callbacks = g_frame_callbacks;
    for (int ref : callbacks) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            engine_log_error(L, "Error in frame callback");
        }
    }
}

void engine_stop(const char *reason, int id, uint32_t address, int vector)
{
    g_stop_requested = true;
    g_step_frames = 0;
    g_stop_info.reason = reason;
    g_stop_info.id = id;
    g_stop_info.address = address;
    g_stop_info.vector = vector;
}

void engine_resume(void)
{
    g_stop_requested = false;
    g_step_frames = 0;
}

bool engine_stop_requested(void)
{
    return g_stop_requested;
}

// Pushes the result of dbg.lookup(address), which is nil if no symbol is
// known for the address.
static void push_symbol(lua_State *L, uint32_t address)
{
    if (lua_getglobal(L, "dbg") != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return;
    }
    lua_getfield(L, -1, "lookup");
    lua_remove(L, -2);
    lua_pushinteger(L, address);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
}

void engine_push_stop_info(lua_State *L)
{
    if (!g_stop_requested) {
        lua_pushboolean(L, false);
        return;
    }
    lua_createtable(L, 0, 7);
    lua_pushstring(L, g_stop_info.reason);
    lua_setfield(L, -2, "reason");
    lua_pushinteger(L, core().cpu.getPC0());
    lua_setfield(L, -2, "pc");
    if (g_stop_info.id != 0) {
        lua_pushinteger(L, g_stop_info.id);
        lua_setfield(L, -2, "id");
        lua_pushinteger(L, g_stop_info.address);
        lua_setfield(L, -2, "address");
    }
    if (g_stop_info.vector != -1) {
        lua_pushinteger(L, g_stop_info.vector);
        lua_setfield(L, -2, "vector");
    }
    // The names of the addresses, if symbols have been loaded.
    push_symbol(L, core().cpu.getPC0());
    lua_setfield(L, -2, "symbol");
    if (g_stop_info.id != 0) {
        push_symbol(L, g_stop_info.address);
        lua_setfield(L, -2, "address_symbol");
    }
}

static int stop_wait_finished(lua_State *L, int status, lua_KContext context)
{
    // The stack holds the stop information, or nothing after a timeout.
    return lua_gettop(L);
}

int engine_yield_until_stopped(lua_State *L, lua_Integer max_frames)
{
    if (g_stop_requested) {
        engine_push_stop_info(L);
        return 1;
    }
    if (g_current_task == NULL) {
        return luaL_error(L, "cannot wait outside a task");
    }
    g_current_task->wait_stop = true;
    lua_settop(L, 0);
    lua_pushinteger(L, max_frames > 0 ? max_frames : INT64_MAX / 2);
    return lua_yieldk(L, 1, 0, stop_wait_finished);
}

int engine_yield_frames(
    lua_State *L, lua_Integer frames, lua_KFunction k, lua_KContext context)
{
    if (g_stop_requested) {
        g_step_frames = frames;
        g_stop_requested = false;
    }
    lua_pushinteger(L, frames);
    return lua_yieldk(L, 1, context, k);
}

static int l_emu_log(lua_State *L)
{
    host_log("[LUA] %s", luaL_checkstring(L, 1));
    return 0;
}

static int l_emu_frame(lua_State *L)
{
    lua_pushinteger(L, g_frame);
    return 1;
}

// emu.cycles() returns the emulated time in cycles of the 7.09 MHz (PAL)
// clock: two per colour clock, and the unit of 68000 instruction timings.
static int l_emu_cycles(lua_State *L)
{
    lua_pushinteger(L, core().cpu.getClock());
    return 1;
}

// emu.beam() returns the line and the horizontal position (in colour
// clocks) the display has reached.
static int l_emu_beam(lua_State *L)
{
    lua_pushinteger(L, core().agnus.pos.v);
    lua_pushinteger(L, core().agnus.pos.h);
    return 2;
}

// emu.timing() returns the frame timing of the current display mode:
// {lines = ..., cycles_per_line = ..., cycles_per_frame = ..., hz = ...},
// with cycles as in emu.cycles. Lines can differ by a colour clock and
// interlaced frames by a line, so the cycle counts are rounded.
static int l_emu_timing(lua_State *L)
{
    const Beam &pos = core().agnus.pos;
    // A long frame (every frame when not interlaced) has one more line.
    int lines = (core().agnus.isPAL() ? 312 : 262) + (pos.lof ? 1 : 0);
    int cycles_per_line = 227 * 2;
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, lines);
    lua_setfield(L, -2, "lines");
    lua_pushinteger(L, cycles_per_line);
    lua_setfield(L, -2, "cycles_per_line");
    lua_pushinteger(L, (lua_Integer) lines * cycles_per_line);
    lua_setfield(L, -2, "cycles_per_frame");
    lua_pushnumber(L, core().nativeRefreshRate());
    lua_setfield(L, -2, "hz");
    return 1;
}

static int l_emu_wait_frames(lua_State *L)
{
    lua_pushinteger(L, luaL_optinteger(L, 1, 1));
    return lua_yield(L, 1);
}

static int l_emu_wait_next_frame(lua_State *L)
{
    lua_pushinteger(L, 1);
    return lua_yield(L, 1);
}

static int l_emu_on_frame(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_settop(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_frame_callbacks.push_back(ref);
    lua_pushinteger(L, ref);
    return 1;
}

static int l_emu_remove_frame_callback(lua_State *L)
{
    int ref = (int) luaL_checkinteger(L, 1);
    for (size_t i = 0; i < g_frame_callbacks.size(); i++) {
        if (g_frame_callbacks[i] == ref) {
            g_frame_callbacks.erase(g_frame_callbacks.begin() + i);
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            break;
        }
    }
    return 0;
}

static int l_emu_pause(lua_State *L)
{
    // Keep the reason if the emulation is already about to stop.
    if (!g_stop_requested) {
        engine_stop("pause");
    }
    return 0;
}

static int l_emu_resume(lua_State *L)
{
    engine_resume();
    return 0;
}

static int l_emu_paused(lua_State *L)
{
    lua_pushboolean(L, g_stop_requested);
    return 1;
}

// Runs the given number of frames (default 1) and pauses again. Returns
// when the frames have been run.
static int l_emu_step(lua_State *L)
{
    lua_Integer frames = luaL_optinteger(L, 1, 1);
    luaL_argcheck(L, frames >= 1, 1, "must be at least 1");
    g_stop_requested = true;
    return engine_yield_frames(L, frames);
}

static int l_emu_warp(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    host_set_warp(lua_toboolean(L, 1));
    return 0;
}

// emu.window(on, volume) opens or closes the window which shows the picture
// and plays the sound. The volume, from 0 to 100, is optional. Returns
// whether the window is open, also when called with no argument.
static int l_emu_window(lua_State *L)
{
    if (!lua_isnone(L, 1)) {
        luaL_checktype(L, 1, LUA_TBOOLEAN);
        engine_check_not_in_tap(L);
        if (!lua_isnoneornil(L, 2)) {
            lua_Integer volume = luaL_checkinteger(L, 2);
            luaL_argcheck(L, volume >= 0 && volume <= 100, 2, "the volume is from 0 to 100");
            window_set_volume((int) volume);
        }
        if (lua_toboolean(L, 1)) {
            bool opened;
            {
                // The message is copied to Lua before the error is raised,
                // which leaves this function with longjmp.
                std::string error;
                opened = window_open(error);
                if (!opened) {
                    lua_pushfstring(L, "could not open the window: %s", error.c_str());
                }
            }
            if (!opened) {
                return lua_error(L);
            }
        } else {
            window_close();
        }
    }
    lua_pushboolean(L, window_is_open());
    return 1;
}

static int l_emu_reset(lua_State *L)
{
    engine_check_not_in_tap(L);
    if (lua_toboolean(L, 1)) {
        g_vamiga->hardReset();
    } else {
        g_vamiga->softReset();
    }
    return 0;
}

static int l_emu_quit(lua_State *L)
{
    host_request_quit();
    return 0;
}

// The options are the ones of the vAmiga core, with the names it uses in its
// own shell ("CPU.REVISION", "MEM.CHIP_RAM", ...).
static Opt check_option(lua_State *L, int arg)
{
    const char *name = luaL_checkstring(L, arg);
    auto option = OptEnum::parseEnum(name);
    if (!option) {
        luaL_error(L, "unknown option '%s'", name);
    }
    return *option;
}

// Returns the value of a configuration option as a number.
static int l_emu_config_get(lua_State *L)
{
    Opt option = check_option(L, 1);
    i64 value = 0;
    engine_call(L, "could not read the option", [&] { value = g_vamiga->get(option); });
    lua_pushinteger(L, value);
    return 1;
}

// Changes a configuration option. The value is a number, or a name the core
// knows for the option. Returns false if the value was not accepted.
static int l_emu_config_set(lua_State *L)
{
    engine_check_not_in_tap(L);
    Opt option = check_option(L, 1);
    bool is_number = lua_isinteger(L, 2);
    lua_Integer number = is_number ? lua_tointeger(L, 2) : 0;
    const char *text = is_number ? "" : luaL_checkstring(L, 2);
    bool accepted = true;
    try {
        host_set_option(option, is_number ? (i64) number : OptionParser::parse(option, text));
    } catch (std::exception &) {
        accepted = false;
    }
    lua_pushboolean(L, accepted);
    return 1;
}

static const luaL_Reg emu_functions[] = {
    {"beam", l_emu_beam},
    {"config_get", l_emu_config_get},
    {"config_set", l_emu_config_set},
    {"cycles", l_emu_cycles},
    {"frame", l_emu_frame},
    {"log", l_emu_log},
    {"on_frame", l_emu_on_frame},
    {"pause", l_emu_pause},
    {"paused", l_emu_paused},
    {"quit", l_emu_quit},
    {"remove_frame_callback", l_emu_remove_frame_callback},
    {"reset", l_emu_reset},
    {"resume", l_emu_resume},
    {"step", l_emu_step},
    {"timing", l_emu_timing},
    {"wait_frames", l_emu_wait_frames},
    {"wait_next_frame", l_emu_wait_next_frame},
    {"warp", l_emu_warp},
    {"window", l_emu_window},
    {NULL, NULL},
};

void engine_init(int port)
{
    lua_State *L = luaL_newstate();
    g_engine_state = L;
    luaL_openlibs(L);
    lua_register(L, "print", l_print);
    luaL_newlib(L, emu_functions);
    lua_setglobal(L, "emu");
    engine_open_cpu(L);
    engine_open_input(L);
    engine_open_mem(L);
    engine_open_state(L);
    engine_open_video(L);
    // Adds functions to the mem table.
    engine_open_dbg(L);
    // Adds functions to the dbg table.
    engine_open_symbols(L);
    if (port != 0) {
        engine_remote_open(port);
    }
}

void engine_load(const char *filename)
{
    lua_State *L = g_engine_state;
    host_log("[LUA] Loading %s", filename);
    if (luaL_loadfile(L, filename) != LUA_OK) {
        engine_log_error(L, "Load failed");
        return;
    }
    engine_start_task(L, 0);
}

bool engine_run(void)
{
    std::vector<host_event> events = host_run(engine_debug_stepping());
    engine_debug_events(events);
    for (const host_event &event : events) {
        if (event.type == HOST_FRAME_END) {
            return true;
        }
    }
    return false;
}

int64_t engine_frame_number(void)
{
    return g_frame;
}

void engine_frame(void)
{
    g_frame += 1;
    if (g_step_frames > 0) {
        g_step_frames -= 1;
        if (g_step_frames == 0) {
            engine_stop("pause");
        }
    }
    run_frame_callbacks();
    resume_due_tasks();
    engine_remote_poll();
}

// Resumes the tasks waiting for the emulation to stop, and tells the remote
// clients that it stopped for another reason than a request to pause.
static void notify_stopped(void)
{
    lua_State *L = g_engine_state;
    if (strcmp(g_stop_info.reason, "pause") != 0) {
        std::string fields;
        engine_push_stop_info(L);
        engine_json_append_value(fields, L, -1);
        lua_pop(L, 1);
        // The table is sent as the fields of the event.
        engine_remote_send_event("stopped", fields.substr(1, fields.size() - 2));
    }
    std::vector<engine_task *> tasks;
    tasks.swap(g_tasks);
    for (engine_task *task : tasks) {
        if (task->wait_stop) {
            engine_push_stop_info(L);
            lua_xmove(L, task->thread, 1);
            resume_task(task, 1);
        } else {
            g_tasks.push_back(task);
        }
    }
}

void engine_stopped_loop(void)
{
    if (!g_stop_requested) {
        return;
    }
    notify_stopped();
    while (g_stop_requested && !host_quit_requested()) {
        window_idle();
        if (!engine_remote_poll()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void engine_free(void)
{
    engine_remote_close();
    engine_debug_free();
    for (engine_task *task : g_tasks) {
        delete task;
    }
    g_tasks.clear();
    g_frame_callbacks.clear();
    if (g_engine_state != NULL) {
        lua_close(g_engine_state);
        g_engine_state = NULL;
    }
}

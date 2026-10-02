// The Lua engine: one Lua state, tasks which can wait for frames, and the
// tables with the Lua API. Everything runs on the main thread.

#ifndef VAMIGA_LUA_ENGINE_H
#define VAMIGA_LUA_ENGINE_H

#include <lua.hpp>

#include <cstdint>
#include <exception>
#include <string>

#include "host.h"

extern lua_State *g_engine_state;

// A task is a Lua coroutine which can wait for emulated frames to pass.
struct engine_task {
    lua_State *thread;
    // Registry reference keeping the thread alive.
    int ref;
    int64_t wake_frame;
    // The task is resumed when the emulation stops (or at wake_frame).
    bool wait_stop;
    // The remote client and request which started the task, or -1.
    int client;
    std::string request_id;
    // Text printed by a task started by a remote client.
    std::string output;
};

// Creates the Lua state and listens on the port (if not 0).
void engine_init(int port);
void engine_free(void);
// Runs a Lua file as a task.
void engine_load(const char *filename);
// Runs the emulation until the end of the frame, or until a breakpoint or
// something else stops it before that. The Lua callbacks of breakpoints and
// exception watches are run from here. Returns true if the frame ended.
bool engine_run(void);
// Called when a frame has been emulated: runs frame callbacks, resumes the
// tasks which are due, and handles remote requests.
void engine_frame(void);
// Handles remote requests while the emulation is stopped, until Lua code
// resumes it. Returns at once if it is not stopped.
void engine_stopped_loop(void);

// Runs the function on the stack, with nargs arguments above it, as a new
// task. The function and its arguments are popped. If client is not -1, the
// result is sent to that remote client as the reply to request_id.
void engine_start_task(lua_State *L, int nargs, int client = -1, const char *request_id = "");

// Yields the calling task until the given number of frames have been
// emulated, and then continues in k (if not NULL), which is given context.
// If the emulation is paused, it runs for these frames only.
int engine_yield_frames(
    lua_State *L, lua_Integer frames, lua_KFunction k = NULL, lua_KContext context = 0);

// Stops the emulation, as emu.pause does. The reason, and the id, address
// and vector when given, are what dbg.wait returns.
void engine_stop(const char *reason, int id = 0, uint32_t address = 0, int vector = -1);
void engine_resume(void);
bool engine_stop_requested(void);
// Pushes a table describing why the emulation is stopped, or false.
void engine_push_stop_info(lua_State *L);

// Calls f, which can throw C++ exceptions (the functions of the vAmiga core
// do). An exception is turned into a Lua error with the text of what and the
// message of the exception. The Lua error is raised after the exception has
// been destroyed, since raising it leaves the function with longjmp.
void engine_check_not_in_tap(lua_State *L);

template <class F> void engine_call(lua_State *L, const char *what, F &&f)
{
    engine_check_not_in_tap(L);
    bool failed = false;
    try {
        f();
    } catch (std::exception &e) {
        lua_pushfstring(L, "%s: %s", what, e.what());
        failed = true;
    }
    if (failed) {
        lua_error(L);
    }
}

// Yields the calling task until the emulation stops, and returns the stop
// information to it. If max_frames is not 0, the task also continues (with
// no values returned) when that many frames have been emulated.
int engine_yield_until_stopped(lua_State *L, lua_Integer max_frames);

// Logs the error message on top of the stack and pops it.
void engine_log_error(lua_State *L, const char *context);

// Functions creating the global tables with the Lua API.

void engine_open_cpu(lua_State *L);
void engine_open_dbg(lua_State *L);
void engine_open_input(lua_State *L);
void engine_open_mem(lua_State *L);
void engine_open_state(lua_State *L);
void engine_open_symbols(lua_State *L);
void engine_open_video(lua_State *L);

// Records the path of a disk image which was inserted without media.insert.
void engine_set_media_path(int drive, const std::string &path);

// lua_debug.cpp

// Handles what host_run returned, apart from the end of the frame.
void engine_debug_events(const std::vector<host_event> &events);
// True while instructions are run one at a time for dbg.step.
bool engine_debug_stepping(void);
void engine_debug_free(void);
// Raises a Lua error if called from a tap callback. Tap callbacks run on the
// emulator thread, where the functions of the core which wait for that
// thread cannot be used.
void engine_check_not_in_tap(lua_State *L);

// json.cpp

void engine_json_append_string(std::string &out, const char *s, size_t len);
void engine_json_append_value(std::string &out, lua_State *L, int index);
// Parses {"id": ..., "code": "..."}. The id is returned as JSON text.
bool engine_json_parse_request(
    const char *text, size_t len, std::string &id, std::string &code, std::string &error);

// remote.cpp

bool engine_remote_open(int port);
void engine_remote_close(void);
// Handles new connections and requests. Returns false if there were none.
bool engine_remote_poll(void);
void engine_remote_task_finished(engine_task *task, bool ok, int nresults);
// Sends {"event": <event>, <fields>} to all clients. The fields are JSON
// text ("key": value, ...) and can be empty.
void engine_remote_send_event(const char *event, const std::string &fields);

#endif

// Lua functions for save states (the state table) and floppy disks (the
// media table).
//
// Lua code runs while the emulator is paused between two instructions, so a
// state can be saved and loaded at once. Lua variables, breakpoints and
// tasks are not part of a state.

#include "engine.h"

#include <cstdio>
#include <filesystem>

using namespace vamiga;

// A state file holds a snapshot of the core as it is. The functions read and
// write the files themselves, since the core only accepts its own file name
// suffix.

// state.save(path) saves a state file.
static int l_state_save(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    Snapshot *snapshot = NULL;
    engine_call(L, "could not save the state", [&] {
        snapshot = g_vamiga->amiga.takeSnapshot(Compressor::NONE).release();
    });
    FILE *f = snapshot ? fopen(path, "wb") : NULL;
    bool ok = f != NULL;
    if (ok) {
        size_t size = (size_t) snapshot->data.size;
        ok = fwrite(snapshot->data.ptr, 1, size, f) == size;
        ok = fclose(f) == 0 && ok;
    }
    delete snapshot;
    if (!ok) {
        return luaL_error(L, "could not save the state to '%s'", path);
    }
    return 0;
}

// state.load(path) loads a state file.
static int l_state_load(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return luaL_error(L, "state file '%s' does not exist", path);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    // The buffer belongs to Lua, so it is freed if an error is raised.
    u8 *data = (u8 *) lua_newuserdatauv(L, size > 0 ? (size_t) size : 1, 0);
    bool ok = size > 0 && fread(data, 1, (size_t) size, f) == (size_t) size;
    fclose(f);
    if (!ok) {
        return luaL_error(L, "could not read the state file '%s'", path);
    }
    engine_call(L, "could not load the state", [&] {
        Snapshot snapshot(data, (isize) size);
        g_vamiga->amiga.loadSnapshot(snapshot);
    });
    return 0;
}

// A state kept in memory, as a Lua userdata.
struct state_snapshot {
    Snapshot *snapshot;
};

#define SNAPSHOT_TYPE "vamiga.snapshot"

static int snapshot_gc(lua_State *L)
{
    state_snapshot *state = (state_snapshot *) luaL_checkudata(L, 1, SNAPSHOT_TYPE);
    delete state->snapshot;
    state->snapshot = NULL;
    return 0;
}

// #snapshot is its size in bytes.
static int snapshot_len(lua_State *L)
{
    state_snapshot *state = (state_snapshot *) luaL_checkudata(L, 1, SNAPSHOT_TYPE);
    lua_pushinteger(L, state->snapshot->data.size);
    return 1;
}

// state.snapshot() saves a state in memory and returns it.
static int l_state_snapshot(lua_State *L)
{
    state_snapshot *state = (state_snapshot *) lua_newuserdatauv(L, sizeof(state_snapshot), 0);
    state->snapshot = NULL;
    luaL_setmetatable(L, SNAPSHOT_TYPE);
    engine_call(L, "could not save the state", [&] { state->snapshot = g_vamiga->amiga.takeSnapshot(Compressor::NONE).release(); });
    if (state->snapshot == NULL) {
        return luaL_error(L, "could not save the state");
    }
    return 1;
}

// state.restore(snapshot) loads a state returned by state.snapshot. It can
// be loaded any number of times.
static int l_state_restore(lua_State *L)
{
    state_snapshot *state = (state_snapshot *) luaL_checkudata(L, 1, SNAPSHOT_TYPE);
    engine_call(L, "could not load the state", [&] { g_vamiga->amiga.loadSnapshot(*state->snapshot); });
    return 0;
}

static const luaL_Reg snapshot_metamethods[] = {
    {"__gc", snapshot_gc},
    {"__len", snapshot_len},
    {NULL, NULL},
};

static const luaL_Reg state_functions[] = {
    {"load", l_state_load},
    {"restore", l_state_restore},
    {"save", l_state_save},
    {"snapshot", l_state_snapshot},
    {NULL, NULL},
};

// The paths of the disk images in the drives.
static std::string g_disk_paths[4];

void engine_set_media_path(int drive, const std::string &path)
{
    g_disk_paths[drive] = path;
}

static int check_drive(lua_State *L, int arg)
{
    lua_Integer drive = luaL_checkinteger(L, arg);
    luaL_argcheck(L, drive >= 0 && drive <= 3, arg, "drive must be 0 to 3");
    return (int) drive;
}

// media.insert(drive, path) inserts a disk image in DF0 to DF3.
static int l_media_insert(lua_State *L)
{
    int drive = check_drive(L, 1);
    const char *path = luaL_checkstring(L, 2);
    if (!std::filesystem::exists(path)) {
        return luaL_error(L, "disk image '%s' does not exist", path);
    }
    engine_call(L, "could not insert", [&] { g_vamiga->df[drive]->insert(std::filesystem::path(path), false); });
    g_disk_paths[drive] = path;
    return 0;
}

static int l_media_eject(lua_State *L)
{
    engine_check_not_in_tap(L);
    int drive = check_drive(L, 1);
    g_vamiga->df[drive]->ejectDisk();
    g_disk_paths[drive].clear();
    return 0;
}

// media.path(drive) returns the path of the disk image in the drive, or an
// empty string.
static int l_media_path(lua_State *L)
{
    lua_pushstring(L, g_disk_paths[check_drive(L, 1)].c_str());
    return 1;
}

static const luaL_Reg media_functions[] = {
    {"eject", l_media_eject},
    {"insert", l_media_insert},
    {"path", l_media_path},
    {NULL, NULL},
};

void engine_open_state(lua_State *L)
{
    luaL_newmetatable(L, SNAPSHOT_TYPE);
    luaL_setfuncs(L, snapshot_metamethods, 0);
    lua_pop(L, 1);
    luaL_newlib(L, state_functions);
    lua_setglobal(L, "state");
    luaL_newlib(L, media_functions);
    lua_setglobal(L, "media");
}

// Lua breakpoints for the CPU, the copper and the beam, instruction
// stepping, exception watches (the dbg table) and memory taps (mem.tap_*).
//
// The breakpoints and catchpoints of the vAmiga core are used to make the
// emulator pause, and the Lua side is handled here on the main thread while
// it is paused: the emulator stops before the instruction at a breakpoint,
// and before the first instruction of an exception handler. Callbacks are
// run at that point and the emulation is continued, unless the breakpoint
// has no callback or the callback calls emu.pause.

#include "engine.h"

#include "Agnus.h"

#include <cstring>
#include <set>
#include <vector>

using namespace vamiga;

struct lua_breakpoint {
    int id;
    u32 address;
    // Registry reference to the callback, or LUA_NOREF.
    int callback;
};

// A request to stop or call a function when the CPU takes an exception.
struct lua_exception_watch {
    int id;
    // The exception vector number, or one of the values below.
    int vector;
    int callback;
};

// Exceptions which normally mean that the program has crashed.
#define VECTOR_CRASH -1
// The CPU has halted (after a double fault).
#define VECTOR_HALT -2

// What the callbacks and dbg.wait get as the vector when the CPU has halted.
// It is the number the UAE core has for a double fault, so that scripts get
// the same from both emulators.
#define HALT_DOUBLE_FAULT 2

// Breakpoints for the copper and the beam. They use the guards of the core
// in the same way as the CPU breakpoints do, but the emulator pauses when
// the CPU has finished the instruction it was running, a few colour clocks
// after the copper or the beam got there.
enum chip_guard_kind {
    // The copper is about to run the instruction at this address.
    COPPER_BREAKPOINT,
    // The copper has written this register (an address, as in mem.custom).
    COPPER_WATCHPOINT,
    // The beam has reached this line and horizontal position.
    BEAM_BREAKPOINT,
};

struct lua_chip_guard {
    int id;
    chip_guard_kind kind;
    // The address, or the line in the high and the position in the low 16
    // bits.
    u32 target;
    int callback;
    // The frame the guard was last reported in (beam breakpoints only).
    i64 last_frame;
};

static std::vector<lua_breakpoint> g_breakpoints;
static std::vector<lua_chip_guard> g_chip_guards;
static std::vector<lua_exception_watch> g_exception_watches;
static int g_next_id = 1;
// When not 0, the number of instructions left to run before stopping.
static int64_t g_step_instructions;

bool engine_debug_stepping(void)
{
    return g_step_instructions > 0;
}

static bool is_crash_vector(int nr)
{
    // Bus error, address error, illegal instruction, division by zero and
    // the unimplemented (line A and line F) instructions.
    return nr == 2 || nr == 3 || nr == 4 || nr == 5 || nr == 10 || nr == 11;
}

// Makes the core have a breakpoint at every address with a Lua breakpoint.
static void update_core_breakpoints(void)
{
    moira::Guards &guards = core().cpu.debugger.breakpoints;
    std::set<u32> wanted;
    for (const lua_breakpoint &breakpoint : g_breakpoints) {
        wanted.insert(breakpoint.address);
    }
    for (isize i = guards.elements() - 1; i >= 0; i--) {
        u32 address = *guards.guardAddr(i);
        if (wanted.erase(address) == 0) {
            guards.removeAt(address);
        }
    }
    for (u32 address : wanted) {
        guards.setAt(address);
    }
}

// Makes the core have a catchpoint for every vector which is watched.
static void update_core_catchpoints(void)
{
    moira::Guards &guards = core().cpu.debugger.catchpoints;
    std::set<u32> wanted;
    for (const lua_exception_watch &watch : g_exception_watches) {
        if (watch.vector == VECTOR_CRASH) {
            for (int vector = 2; vector <= 11; vector++) {
                if (is_crash_vector(vector)) {
                    wanted.insert(vector);
                }
            }
        } else if (watch.vector >= 0) {
            wanted.insert(watch.vector);
        }
    }
    for (isize i = guards.elements() - 1; i >= 0; i--) {
        u32 vector = *guards.guardAddr(i);
        if (wanted.erase(vector) == 0) {
            guards.removeAt(vector);
        }
    }
    for (u32 vector : wanted) {
        guards.setAt(vector);
    }
}

// The list of guards in the core for a kind of chip guard.
static GuardList &core_guards(chip_guard_kind kind)
{
    Agnus &agnus = core().agnus;
    switch (kind) {
        case COPPER_BREAKPOINT: return agnus.copper.debugger.breakpoints;
        case COPPER_WATCHPOINT: return agnus.copper.debugger.watchpoints;
        default: return agnus.dmaDebugger.beamtraps;
    }
}

// What the guards of the core for the copper watchpoints hold: the offset
// of the register from $DFF000.
static u32 core_target(const lua_chip_guard &guard)
{
    return guard.kind == COPPER_WATCHPOINT ? (guard.target & 0x1fe) : guard.target;
}

// Makes the core have a guard for every target with a Lua guard of the kind.
static void update_core_chip_guards(chip_guard_kind kind)
{
    GuardList &guards = core_guards(kind);
    std::set<u32> wanted;
    for (const lua_chip_guard &guard : g_chip_guards) {
        if (guard.kind == kind) {
            wanted.insert(core_target(guard));
        }
    }
    for (isize i = guards.elements() - 1; i >= 0; i--) {
        u32 target = guards.guardNr(i)->addr;
        if (wanted.erase(target) == 0) {
            guards.removeAt(target);
        }
    }
    for (u32 target : wanted) {
        guards.setAt(target);
    }
}

static u32 peek_u16(u32 address)
{
    Memory &mem = core().mem;
    return mem.spypeek8<Accessor::CPU>(address) << 8 | mem.spypeek8<Accessor::CPU>(address + 1);
}

static u32 peek_u32(u32 address)
{
    return peek_u16(address) << 16 | peek_u16(address + 2);
}

// Returns the address of the instruction which caused the exception the CPU
// has just taken. This is called when the CPU is about to run the first
// instruction of the exception handler, and the address is worked out from
// what the CPU saved on the stack:
// - For most exceptions the saved PC is that of the instruction itself.
// - For TRAP and TRAPV it is that of the next instruction, two bytes on.
// - A 68020 also saves the address of the instruction for the exceptions
//   where the saved PC is that of the next instruction.
// - For a division by zero and for CHK on a 68000 or 68010, the saved PC is
//   that of the next instruction and the length of the instruction is not
//   known, so the instruction is looked for in the words before it.
// - For bus and address errors on a 68000 the saved PC is some words past
//   the instruction, and that is what is returned.
static u32 exception_instruction(int vector)
{
    CPU &cpu = core().cpu;
    bool is_68000 = cpu.getConfig().revision == CPURev::CPU_68000;
    u32 frame = cpu.getSP();
    if (is_68000 && (vector == 2 || vector == 3)) {
        // These frames start with eight bytes about the failed access.
        frame += 8;
    }
    u32 saved_pc = peek_u32(frame + 2);
    if (!is_68000 && (peek_u16(frame + 6) >> 12) == 2) {
        // Stack frame format 2 has the address of the instruction.
        return peek_u32(frame + 8);
    }
    if (vector == 7 || (vector >= 32 && vector <= 47)) {
        return saved_pc - 2;
    }
    if (vector == 5 || vector == 6) {
        for (u32 length = 2; length <= 10; length += 2) {
            u32 opcode = peek_u16(saved_pc - length);
            bool divide = (opcode & 0xf0c0) == 0x80c0;
            bool chk = (opcode & 0xf140) == 0x4100;
            isize size = 0;
            if ((vector == 5 ? divide : chk) &&
                (cpu.disassembleInstr(saved_pc - length, &size), (u32) size == length)) {
                return saved_pc - length;
            }
        }
    }
    return saved_pc;
}

// Runs a callback with nargs arguments, which are on the stack above it.
// Returns true if it stopped the emulation (by calling emu.pause).
static bool run_callback(int nargs, const char *what)
{
    lua_State *L = g_engine_state;
    bool was_stopped = engine_stop_requested();
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
        engine_log_error(L, what);
    }
    return !was_stopped && engine_stop_requested();
}

// The emulator has paused before the instruction at pc, which has
// breakpoints.
static void run_breakpoints(u32 pc)
{
    lua_State *L = g_engine_state;
    // Callbacks can add and remove breakpoints.
    std::vector<lua_breakpoint> breakpoints = g_breakpoints;
    for (const lua_breakpoint &breakpoint : breakpoints) {
        if (breakpoint.address != pc) {
            continue;
        }
        bool stop = breakpoint.callback == LUA_NOREF;
        if (!stop) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, breakpoint.callback);
            lua_pushinteger(L, breakpoint.address);
            stop = run_callback(1, "Error in breakpoint callback");
        }
        if (stop) {
            engine_stop("breakpoint", breakpoint.id, pc);
        }
    }
}

// Runs the watches matching the vector. reason is "exception" or "halt",
// number is the exception vector or the halt reason, and pc the address of
// the instruction which caused it.
static void run_exception_watches(const char *reason, int vector, int number, u32 pc)
{
    lua_State *L = g_engine_state;
    // Callbacks can add and remove watches.
    std::vector<lua_exception_watch> watches = g_exception_watches;
    for (const lua_exception_watch &watch : watches) {
        bool crash = vector == VECTOR_HALT || is_crash_vector(vector);
        if (watch.vector != vector && !(watch.vector == VECTOR_CRASH && crash)) {
            continue;
        }
        bool stop = watch.callback == LUA_NOREF;
        if (!stop) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, watch.callback);
            lua_pushinteger(L, number);
            lua_pushinteger(L, pc);
            stop = run_callback(2, "Error in exception callback");
        }
        if (stop) {
            engine_stop(reason, watch.id, pc, number);
        }
    }
}

// How far (in colour clocks) the beam can have moved past a beam breakpoint
// when the emulator pauses for it: the CPU finishes its instruction first,
// and can be held up by an interrupt being taken. One line is far more than
// that.
#define BEAM_WINDOW 227

// Runs the copper and beam guards of a kind which the core has just paused
// the emulator for.
static void run_chip_guards(chip_guard_kind kind)
{
    static const char *const reasons[] = {"copper_breakpoint", "copper_watchpoint", "beam"};
    lua_State *L = g_engine_state;
    Agnus &agnus = core().agnus;
    // The guard of the core which matched (the copper ones remember it).
    u32 hit = 0;
    if (kind != BEAM_BREAKPOINT) {
        auto info = core_guards(kind).hit();
        if (!info) {
            return;
        }
        hit = info->addr;
    }
    // Callbacks can add and remove guards.
    std::vector<lua_chip_guard> guards = g_chip_guards;
    for (const lua_chip_guard &guard : guards) {
        if (guard.kind != kind) {
            continue;
        }
        if (kind == BEAM_BREAKPOINT) {
            // The core does not say which trap it was. It is one the beam
            // has just passed, and which has not been reported this frame.
            isize passed = (agnus.pos.v - (isize) (guard.target >> 16)) * BEAM_WINDOW +
                           agnus.pos.h - (isize) (guard.target & 0xffff);
            if (passed < 0 || passed > BEAM_WINDOW || guard.last_frame == agnus.pos.frame) {
                continue;
            }
            for (lua_chip_guard &original : g_chip_guards) {
                if (original.id == guard.id) {
                    original.last_frame = agnus.pos.frame;
                }
            }
        } else if (core_target(guard) != hit) {
            continue;
        }
        bool stop = guard.callback == LUA_NOREF;
        if (!stop) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, guard.callback);
            if (kind == BEAM_BREAKPOINT) {
                lua_pushinteger(L, guard.target >> 16);
                lua_pushinteger(L, guard.target & 0xffff);
                stop = run_callback(2, "Error in beam breakpoint callback");
            } else {
                lua_pushinteger(L, guard.target);
                stop = run_callback(1, "Error in copper breakpoint callback");
            }
        }
        if (stop) {
            engine_stop(reasons[kind], guard.id, guard.target);
        }
    }
}

void engine_debug_events(const std::vector<host_event> &events)
{
    for (const host_event &event : events) {
        switch (event.type) {
            case HOST_STEP:
                if (g_step_instructions > 0) {
                    g_step_instructions -= 1;
                    if (g_step_instructions == 0) {
                        u32 pc = core().cpu.getPC0();
                        engine_stop("step", 0, pc);
                    }
                }
                break;
            case HOST_BREAKPOINT:
                run_breakpoints(core().cpu.getPC0());
                break;
            case HOST_EXCEPTION:
                run_exception_watches(
                    "exception", event.vector, event.vector, exception_instruction(event.vector));
                break;
            case HOST_HALT:
                run_exception_watches(
                    "halt", VECTOR_HALT, HALT_DOUBLE_FAULT, core().cpu.getPC0());
                break;
            case HOST_COPPER_BREAKPOINT:
                run_chip_guards(COPPER_BREAKPOINT);
                break;
            case HOST_COPPER_WATCHPOINT:
                run_chip_guards(COPPER_WATCHPOINT);
                break;
            case HOST_BEAM:
                run_chip_guards(BEAM_BREAKPOINT);
                break;
            case HOST_FRAME_END:
                break;
        }
    }
}

// dbg.bpset(address, callback) sets a breakpoint and returns its id.
// Without a callback, the emulation stops when the breakpoint is reached,
// before the instruction at the address. With one, callback(address) is
// called and the emulation continues, unless the callback calls emu.pause.
static int l_dbg_bpset(lua_State *L)
{
    lua_Integer address = luaL_checkinteger(L, 1);
    lua_breakpoint breakpoint;
    breakpoint.id = g_next_id++;
    breakpoint.address = (u32) address;
    breakpoint.callback = LUA_NOREF;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_settop(L, 2);
        breakpoint.callback = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    g_breakpoints.push_back(breakpoint);
    update_core_breakpoints();
    lua_pushinteger(L, breakpoint.id);
    return 1;
}

// dbg.bpclear(id) removes a breakpoint, dbg.bpclear() removes all.
static int l_dbg_bpclear(lua_State *L)
{
    bool all = lua_isnoneornil(L, 1);
    lua_Integer id = all ? 0 : luaL_checkinteger(L, 1);
    for (size_t i = g_breakpoints.size(); i > 0; i--) {
        if (all || g_breakpoints[i - 1].id == id) {
            luaL_unref(L, LUA_REGISTRYINDEX, g_breakpoints[i - 1].callback);
            g_breakpoints.erase(g_breakpoints.begin() + i - 1);
        }
    }
    update_core_breakpoints();
    return 0;
}

// dbg.bplist() returns a list of {id = ..., address = ...}.
static int l_dbg_bplist(lua_State *L)
{
    lua_createtable(L, (int) g_breakpoints.size(), 0);
    for (size_t i = 0; i < g_breakpoints.size(); i++) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, g_breakpoints[i].id);
        lua_setfield(L, -2, "id");
        lua_pushinteger(L, g_breakpoints[i].address);
        lua_setfield(L, -2, "address");
        lua_rawseti(L, -2, (lua_Integer) i + 1);
    }
    return 1;
}

// dbg.exset(vector, callback) watches for a CPU exception and returns an
// id. vector is the exception vector number (4 is illegal instruction, 32
// is TRAP #0 and so on), "crash" for the exceptions which normally mean that
// the program has crashed (and for the CPU halting), or "halt" for the CPU
// halting only. Without a callback, the emulation stops when the exception
// is taken, at the first instruction of the exception handler. With one,
// callback(vector, pc) is called and the emulation continues, unless the
// callback calls emu.pause. pc is the address of the instruction which
// caused the exception (see exception_instruction). A halted CPU is noticed
// at the end of the frame, and the callback gets 2 instead of a vector.
static int l_dbg_exset(lua_State *L)
{
    lua_exception_watch watch;
    if (lua_type(L, 1) == LUA_TSTRING) {
        const char *name = lua_tostring(L, 1);
        if (strcmp(name, "crash") == 0) {
            watch.vector = VECTOR_CRASH;
        } else if (strcmp(name, "halt") == 0) {
            watch.vector = VECTOR_HALT;
        } else {
            return luaL_argerror(L, 1, "must be a vector number, 'crash' or 'halt'");
        }
    } else {
        lua_Integer vector = luaL_checkinteger(L, 1);
        luaL_argcheck(L, vector >= 2 && vector <= 255, 1, "must be 2 to 255");
        watch.vector = (int) vector;
    }
    watch.id = g_next_id++;
    watch.callback = LUA_NOREF;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_settop(L, 2);
        watch.callback = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    g_exception_watches.push_back(watch);
    update_core_catchpoints();
    lua_pushinteger(L, watch.id);
    return 1;
}

// dbg.exclear(id) removes an exception watch, dbg.exclear() removes all.
static int l_dbg_exclear(lua_State *L)
{
    bool all = lua_isnoneornil(L, 1);
    lua_Integer id = all ? 0 : luaL_checkinteger(L, 1);
    for (size_t i = g_exception_watches.size(); i > 0; i--) {
        if (all || g_exception_watches[i - 1].id == id) {
            luaL_unref(L, LUA_REGISTRYINDEX, g_exception_watches[i - 1].callback);
            g_exception_watches.erase(g_exception_watches.begin() + i - 1);
        }
    }
    update_core_catchpoints();
    return 0;
}

// dbg.go() continues after a stop. This is the same as emu.resume.
static int l_dbg_go(lua_State *L)
{
    engine_resume();
    return 0;
}

// dbg.wait(frames) waits until the emulation stops and returns a table
// describing why: {reason = "breakpoint", "tap", "exception", "halt",
// "step", "copper_breakpoint", "copper_watchpoint", "beam" or "pause",
// pc = ..., id = ..., address = ...}. If frames is given and the
// emulation has not stopped after that many frames, nothing is returned.
static int l_dbg_wait(lua_State *L)
{
    return engine_yield_until_stopped(L, luaL_optinteger(L, 1, 0));
}

// dbg.step(count) runs count instructions (default 1) and stops. Returns
// the same as dbg.wait.
static int l_dbg_step(lua_State *L)
{
    lua_Integer count = luaL_optinteger(L, 1, 1);
    luaL_argcheck(L, count >= 1, 1, "must be at least 1");
    g_step_instructions = count;
    engine_resume();
    return engine_yield_until_stopped(L, 0);
}

// dbg.stopped() returns the same table as dbg.wait if the emulation is
// stopped, and false if it is running.
static int l_dbg_stopped(lua_State *L)
{
    engine_push_stop_info(L);
    return 1;
}

static int add_chip_guard(lua_State *L, chip_guard_kind kind, u32 target, int callback_arg)
{
    lua_chip_guard guard;
    guard.id = g_next_id++;
    guard.kind = kind;
    guard.target = target;
    guard.callback = LUA_NOREF;
    guard.last_frame = -1;
    if (!lua_isnoneornil(L, callback_arg)) {
        luaL_checktype(L, callback_arg, LUA_TFUNCTION);
        lua_settop(L, callback_arg);
        guard.callback = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    g_chip_guards.push_back(guard);
    update_core_chip_guards(kind);
    lua_pushinteger(L, guard.id);
    return 1;
}

// Removes the chip guard with the id, or all of them, of the kinds from
// first to last.
static int clear_chip_guards(lua_State *L, chip_guard_kind first, chip_guard_kind last)
{
    bool all = lua_isnoneornil(L, 1);
    lua_Integer id = all ? 0 : luaL_checkinteger(L, 1);
    for (size_t i = g_chip_guards.size(); i > 0; i--) {
        const lua_chip_guard &guard = g_chip_guards[i - 1];
        if (guard.kind >= first && guard.kind <= last && (all || guard.id == id)) {
            luaL_unref(L, LUA_REGISTRYINDEX, guard.callback);
            g_chip_guards.erase(g_chip_guards.begin() + i - 1);
        }
    }
    for (int kind = first; kind <= last; kind++) {
        update_core_chip_guards((chip_guard_kind) kind);
    }
    return 0;
}

// dbg.copper_break(address, callback) sets a breakpoint for the copper, at
// the address of an instruction in a copper list, and returns its id.
// Without a callback, the emulation stops when the copper gets there. With
// one, callback(address) is called and the emulation continues, unless the
// callback calls emu.pause.
static int l_dbg_copper_break(lua_State *L)
{
    lua_Integer address = luaL_checkinteger(L, 1);
    luaL_argcheck(L, address >= 0 && address <= 0xffffffffLL, 1, "address out of range");
    return add_chip_guard(L, COPPER_BREAKPOINT, (u32) address, 2);
}

// dbg.copper_watch(register, callback) does the same when the copper writes
// a custom chip register, given by its address (mem.custom.COLOR00). The
// callback is called as callback(register).
static int l_dbg_copper_watch(lua_State *L)
{
    lua_Integer address = luaL_checkinteger(L, 1);
    luaL_argcheck(
        L, address >= 0xdff000 && address <= 0xdff1fe && address % 2 == 0, 1,
        "must be the address of a custom chip register");
    return add_chip_guard(L, COPPER_WATCHPOINT, (u32) address, 2);
}

// dbg.copper_clear(id) removes a copper breakpoint or watch, and
// dbg.copper_clear() removes all of them.
static int l_dbg_copper_clear(lua_State *L)
{
    return clear_chip_guards(L, COPPER_BREAKPOINT, COPPER_WATCHPOINT);
}

// dbg.beam_break(line, position, callback) sets a breakpoint for the beam:
// a raster line and a horizontal position in colour clocks (default 0), as
// emu.beam gives them. It is reached once per frame. The callback is called
// as callback(line, position).
static int l_dbg_beam_break(lua_State *L)
{
    lua_Integer line = luaL_checkinteger(L, 1);
    bool has_position = lua_type(L, 2) == LUA_TNUMBER;
    lua_Integer position = has_position ? luaL_checkinteger(L, 2) : 0;
    luaL_argcheck(L, line >= 0 && line <= 312, 1, "must be 0 to 312");
    luaL_argcheck(L, position >= 0 && position <= 226, 2, "must be 0 to 226");
    return add_chip_guard(
        L, BEAM_BREAKPOINT, (u32) (line << 16 | position), has_position ? 3 : 2);
}

// dbg.beam_clear(id) removes a beam breakpoint, dbg.beam_clear() all.
static int l_dbg_beam_clear(lua_State *L)
{
    return clear_chip_guards(L, BEAM_BREAKPOINT, BEAM_BREAKPOINT);
}

// dbg.copper() returns the state of the copper: {pc = ..., cop1lc = ...,
// cop2lc = ...}. pc is the address of the instruction it is running or
// waiting at.
static int l_dbg_copper(lua_State *L)
{
    CopperInfo info = core().agnus.copper.cacheInfo();
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, info.coppc0);
    lua_setfield(L, -2, "pc");
    lua_pushinteger(L, info.cop1lc);
    lua_setfield(L, -2, "cop1lc");
    lua_pushinteger(L, info.cop2lc);
    lua_setfield(L, -2, "cop2lc");
    return 1;
}

// dbg.copper_disasm(address, count) returns a list with a table for each
// copper instruction from the address: {address = ..., words = {first,
// second}, text = "..."}. An instruction is four bytes.
static int l_dbg_copper_disasm(lua_State *L)
{
    lua_Integer address = luaL_checkinteger(L, 1);
    lua_Integer count = luaL_optinteger(L, 2, 1);
    luaL_argcheck(L, address >= 0 && address <= 0xffffffffLL, 1, "address out of range");
    luaL_argcheck(L, count >= 1 && count <= 10000, 2, "invalid count");
    const CopperDebugger &debugger = core().agnus.copper.debugger;
    lua_createtable(L, (int) count, 0);
    for (lua_Integer i = 0; i < count; i++) {
        u32 at = (u32) address + 4 * (u32) i;
        std::string text = debugger.disassemble(at, true);
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, at);
        lua_setfield(L, -2, "address");
        lua_createtable(L, 2, 0);
        lua_pushinteger(L, peek_u16(at));
        lua_rawseti(L, -2, 1);
        lua_pushinteger(L, peek_u16(at + 2));
        lua_rawseti(L, -2, 2);
        lua_setfield(L, -2, "words");
        lua_pushstring(L, text.c_str());
        lua_setfield(L, -2, "text");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static const luaL_Reg dbg_functions[] = {
    {"beam_break", l_dbg_beam_break},
    {"beam_clear", l_dbg_beam_clear},
    {"bpclear", l_dbg_bpclear},
    {"bplist", l_dbg_bplist},
    {"bpset", l_dbg_bpset},
    {"copper", l_dbg_copper},
    {"copper_break", l_dbg_copper_break},
    {"copper_clear", l_dbg_copper_clear},
    {"copper_disasm", l_dbg_copper_disasm},
    {"copper_watch", l_dbg_copper_watch},
    {"exclear", l_dbg_exclear},
    {"exset", l_dbg_exset},
    {"go", l_dbg_go},
    {"step", l_dbg_step},
    {"stopped", l_dbg_stopped},
    {"wait", l_dbg_wait},
    {NULL, NULL},
};

// dbg.measure(from, to, count, frames) measures how long the program takes
// to get from one address to another, using breakpoints and emu.cycles.
// - from and to are addresses (or symbol names, when symbols are loaded).
// - Without to (nil), from is the start of a subroutine, and the time until
//   it returns is measured.
// - With to equal to from, the time between two passes is measured.
// - count is the number of measurements to take (default 1), and frames the
//   number of frames to wait for them (default 500).
// Returns {count = ..., min = ..., max = ..., average = ..., samples =
// {...}} in the cycles of emu.cycles.
static const char *measure_source = R"LUA(
function dbg.measure(from, to, count, frames)
    count = count or 1
    frames = frames or 500
    local samples = {}
    local breakpoints = {}
    local start

    local function add_sample()
        if start and #samples < count then
            samples[#samples + 1] = emu.cycles() - start
        end
        start = nil
    end

    if to == nil then
        local return_breakpoint
        breakpoints[1] = dbg.bpset(from, function()
            -- Ignore calls made while one is being measured (recursion).
            if start or #samples >= count then return end
            start = emu.cycles()
            local stack = cpu.a7
            return_breakpoint = dbg.bpset(mem.peek_u32(stack), function()
                -- The return address has been taken off the stack.
                if cpu.a7 == stack + 4 then
                    add_sample()
                    dbg.bpclear(return_breakpoint)
                    return_breakpoint = nil
                end
            end)
        end)
        breakpoints[2] = function()
            if return_breakpoint then dbg.bpclear(return_breakpoint) end
        end
    elseif to == from then
        breakpoints[1] = dbg.bpset(from, function()
            local now = emu.cycles()
            add_sample()
            start = now
        end)
    else
        breakpoints[1] = dbg.bpset(from, function() start = emu.cycles() end)
        breakpoints[2] = dbg.bpset(to, add_sample)
    end

    local waited = 0
    while #samples < count and waited < frames do
        emu.wait_frames(1)
        waited = waited + 1
    end
    for _, breakpoint in ipairs(breakpoints) do
        if type(breakpoint) == "function" then
            breakpoint()
        else
            dbg.bpclear(breakpoint)
        end
    end
    if #samples == 0 then
        error("the addresses were not reached in " .. frames .. " frames", 2)
    end

    local result = {count = #samples, samples = samples, min = samples[1], max = samples[1]}
    local total = 0
    for _, sample in ipairs(samples) do
        result.min = math.min(result.min, sample)
        result.max = math.max(result.max, sample)
        total = total + sample
    end
    result.average = total / #samples
    return result
end
)LUA";

// Memory taps
//
// The core calls memory_observer for every data access of the CPU while
// there are taps. That happens on the emulator thread, in the middle of an
// instruction. The main thread is waiting for the emulator to pause at that
// time (in host_run), so the Lua callback is run right there, on the
// emulator thread, and can give the CPU another value.
//
// Functions of the core which wait for the emulator thread cannot be used
// from that thread. The Lua functions which need them (the state, media and
// input functions and a few of the emu ones) raise an error in a tap
// callback (see engine_check_not_in_tap).

struct lua_tap {
    int id;
    u32 first, last;
    bool write;
    int callback;
};

// The UAE core has room for about this many, and scripts written for it can
// depend on the error.
#define MAX_TAPS 20

static std::vector<lua_tap> g_taps;
static bool g_in_tap;

void engine_check_not_in_tap(lua_State *L)
{
    if (g_in_tap) {
        luaL_error(L, "this function cannot be used in a tap callback");
    }
}

static u32 memory_observer(void *context, u32 addr, u32 value, isize size, bool write)
{
    lua_State *L = g_engine_state;
    // An access a callback causes is not reported.
    if (g_in_tap || L == NULL) {
        return value;
    }
    u32 last = addr + (u32) size - 1;
    for (size_t i = 0; i < g_taps.size(); i++) {
        const lua_tap &tap = g_taps[i];
        if (tap.write != write || last < tap.first || addr > tap.last) {
            continue;
        }
        int id = tap.id;
        u32 mask = size == 4 ? 0xffffffff : (1u << (size * 8)) - 1;
        bool was_stopped = engine_stop_requested();
        g_in_tap = true;
        lua_rawgeti(L, LUA_REGISTRYINDEX, tap.callback);
        lua_pushinteger(L, addr);
        lua_pushinteger(L, value & mask);
        lua_pushinteger(L, size);
        lua_pushinteger(L, core().cpu.getPC0());
        if (lua_pcall(L, 4, 1, 0) != LUA_OK) {
            engine_log_error(L, "Error in tap callback");
        } else {
            if (lua_isinteger(L, -1)) {
                value = (u32) lua_tointeger(L, -1) & mask;
            }
            lua_pop(L, 1);
        }
        g_in_tap = false;
        if (!was_stopped && engine_stop_requested()) {
            engine_stop("tap", id, addr);
            // Makes the emulator pause when the instruction has finished.
            core().signalStop();
        }
        // Only the first tap which matches is run.
        break;
    }
    return value;
}

static void update_memory_observer(void)
{
    core().cpu.memoryObserver = g_taps.empty() ? nullptr : memory_observer;
}

static int add_tap(lua_State *L, bool write)
{
    lua_Integer first = luaL_checkinteger(L, 1);
    lua_Integer last = luaL_checkinteger(L, 2);
    luaL_argcheck(L, first >= 0 && first <= 0xffffffffLL, 1, "address out of range");
    luaL_argcheck(L, last >= first && last <= 0xffffffffLL, 2, "address out of range");
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (g_taps.size() >= MAX_TAPS) {
        return luaL_error(L, "too many taps");
    }
    lua_tap tap;
    tap.id = g_next_id++;
    tap.first = (u32) first;
    tap.last = (u32) last;
    tap.write = write;
    lua_settop(L, 3);
    tap.callback = luaL_ref(L, LUA_REGISTRYINDEX);
    g_taps.push_back(tap);
    update_memory_observer();
    lua_pushinteger(L, tap.id);
    return 1;
}

// mem.tap_read(first, last, callback) calls callback(address, value, size,
// pc) when the CPU reads from the address range. If the callback returns an
// integer, the CPU reads that value instead. Returns the id of the tap.
// The callback is called for each bus access, and a 68000 accesses a long
// word as two words. Instruction fetches and reads with PC-relative
// addressing are not reported.
static int l_mem_tap_read(lua_State *L)
{
    return add_tap(L, false);
}

// mem.tap_write(first, last, callback) is the same for writes. If the
// callback returns an integer, that value is written instead.
static int l_mem_tap_write(lua_State *L)
{
    return add_tap(L, true);
}

// mem.tap_remove(id) removes a tap, mem.tap_remove() removes all.
static int l_mem_tap_remove(lua_State *L)
{
    bool all = lua_isnoneornil(L, 1);
    lua_Integer id = all ? 0 : luaL_checkinteger(L, 1);
    for (size_t i = g_taps.size(); i > 0; i--) {
        if (all || g_taps[i - 1].id == id) {
            luaL_unref(L, LUA_REGISTRYINDEX, g_taps[i - 1].callback);
            g_taps.erase(g_taps.begin() + i - 1);
        }
    }
    update_memory_observer();
    return 0;
}

static const luaL_Reg tap_functions[] = {
    {"tap_read", l_mem_tap_read},
    {"tap_remove", l_mem_tap_remove},
    {"tap_write", l_mem_tap_write},
    {NULL, NULL},
};

void engine_open_dbg(lua_State *L)
{
    luaL_newlib(L, dbg_functions);
    lua_setglobal(L, "dbg");
    lua_getglobal(L, "mem");
    luaL_setfuncs(L, tap_functions, 0);
    lua_pop(L, 1);
    if (luaL_loadbuffer(L, measure_source, strlen(measure_source), "=measure") != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
        engine_log_error(L, "measure");
    }
}

void engine_debug_free(void)
{
    g_taps.clear();
    update_memory_observer();
    g_chip_guards.clear();
    g_breakpoints.clear();
    g_exception_watches.clear();
    g_step_instructions = 0;
}

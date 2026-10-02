// Lua access to the CPU registers (the cpu table).
//
// The registers are fields: cpu.d0 to cpu.d7, cpu.a0 to cpu.a7, cpu.pc,
// cpu.sr, cpu.usp, cpu.isp, cpu.msp and cpu.vbr.

#include "engine.h"

#include <cstring>

using namespace vamiga;

// Returns 0 to 7 for d0-d7 and 8 to 15 for a0-a7, or -1.
static int general_register_index(const char *name)
{
    if ((name[0] == 'd' || name[0] == 'a') && name[1] >= '0' && name[1] <= '7' && name[2] == '\0') {
        return (name[0] == 'a' ? 8 : 0) + name[1] - '0';
    }
    return -1;
}

static int l_cpu_index(lua_State *L)
{
    const char *name = luaL_checkstring(L, 2);
    CPU &cpu = core().cpu;
    int index = general_register_index(name);
    if (index >= 8) {
        lua_pushinteger(L, cpu.getA(index - 8));
    } else if (index >= 0) {
        lua_pushinteger(L, cpu.getD(index));
    } else if (strcmp(name, "pc") == 0) {
        lua_pushinteger(L, cpu.getPC0());
    } else if (strcmp(name, "sr") == 0) {
        lua_pushinteger(L, cpu.getSR());
    } else if (strcmp(name, "usp") == 0) {
        lua_pushinteger(L, cpu.getUSP());
    } else if (strcmp(name, "isp") == 0) {
        lua_pushinteger(L, cpu.getISP());
    } else if (strcmp(name, "msp") == 0) {
        lua_pushinteger(L, cpu.getMSP());
    } else if (strcmp(name, "vbr") == 0) {
        lua_pushinteger(L, cpu.getVBR());
    } else {
        return luaL_error(L, "unknown CPU register '%s'", name);
    }
    return 1;
}

static int l_cpu_newindex(lua_State *L)
{
    const char *name = luaL_checkstring(L, 2);
    u32 value = (u32) luaL_checkinteger(L, 3);
    CPU &cpu = core().cpu;
    int index = general_register_index(name);
    if (index >= 8) {
        cpu.setA(index - 8, value);
    } else if (index >= 0) {
        cpu.setD(index, value);
    } else if (strcmp(name, "pc") == 0) {
        // Also fills the prefetch queue from the new address.
        cpu.jump(value);
    } else if (strcmp(name, "sr") == 0) {
        cpu.setSR((u16) value);
    } else if (strcmp(name, "usp") == 0) {
        cpu.setUSP(value);
    } else if (strcmp(name, "isp") == 0) {
        cpu.setISP(value);
    } else if (strcmp(name, "msp") == 0) {
        cpu.setMSP(value);
    } else if (strcmp(name, "vbr") == 0) {
        cpu.setVBR(value);
    } else {
        return luaL_error(L, "unknown CPU register '%s'", name);
    }
    return 0;
}

// cpu.disasm(address, count) returns a list with a table for each
// instruction: {address = ..., size = ..., text = "..."}.
static int l_cpu_disasm(lua_State *L)
{
    u32 addr = (u32) luaL_checkinteger(L, 1);
    lua_Integer count = luaL_optinteger(L, 2, 1);
    luaL_argcheck(L, count >= 1 && count <= 10000, 2, "invalid count");
    lua_createtable(L, (int) count, 0);
    for (lua_Integer i = 1; i <= count; i++) {
        isize size = 0;
        const char *text = core().cpu.disassembleInstr(addr, &size);
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, addr);
        lua_setfield(L, -2, "address");
        lua_pushinteger(L, size);
        lua_setfield(L, -2, "size");
        lua_pushstring(L, text);
        lua_setfield(L, -2, "text");
        lua_rawseti(L, -2, i);
        addr += (u32) size;
    }
    return 1;
}

void engine_open_cpu(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_cpu_disasm);
    lua_setfield(L, -2, "disasm");
    lua_newtable(L);
    lua_pushcfunction(L, l_cpu_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_cpu_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "cpu");
}

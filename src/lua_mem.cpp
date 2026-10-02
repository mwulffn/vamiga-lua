// Lua functions for reading and writing Amiga memory (the mem table).
//
// read_* and write_* go through the memory map like the CPU does, so they
// have the same side effects as the CPU reading or writing a hardware
// register. peek_* and poke_* access RAM and ROM without side effects, and
// fail for other addresses.

#include "engine.h"

using namespace vamiga;

// Reading megabytes at a time is fine, this only catches mistakes.
#define MAX_RANGE_SIZE (64 * 1024 * 1024)

static u32 check_address(lua_State *L, int arg)
{
    lua_Integer addr = luaL_checkinteger(L, arg);
    luaL_argcheck(L, addr >= 0 && addr <= 0xffffffffLL, arg, "address out of range");
    return (u32) addr;
}

static bool is_ram_or_rom(u32 addr, int size)
{
    Memory &mem = core().mem;
    for (int i = 0; i < size; i++) {
        if (!mem.inRam(addr + i) && !mem.inRom(addr + i)) {
            return false;
        }
    }
    return true;
}

static void check_ram_or_rom(lua_State *L, u32 addr, int size)
{
    if (!is_ram_or_rom(addr, size)) {
        luaL_error(L, "address %p is not RAM or ROM", (void *) (uintptr_t) addr);
    }
}

// Called when Lua has written to memory. A 68020 can have the old contents
// in its instruction cache, and would then keep running the old code. The
// cache is emptied the way a program does it: by setting the "clear" bit of
// the cache control register, which does not change the other bits.
static void memory_written(void)
{
    CPU &cpu = core().cpu;
    cpu.setCACR(cpu.getCACR() | 1 << 3);
}

static int l_read_u8(lua_State *L)
{
    lua_pushinteger(L, core().mem.peek8<Accessor::CPU>(check_address(L, 1)));
    return 1;
}

static int l_read_u16(lua_State *L)
{
    lua_pushinteger(L, core().mem.peek16<Accessor::CPU>(check_address(L, 1)));
    return 1;
}

static int l_read_u32(lua_State *L)
{
    u32 addr = check_address(L, 1);
    Memory &mem = core().mem;
    u32 high = mem.peek16<Accessor::CPU>(addr);
    lua_pushinteger(L, high << 16 | mem.peek16<Accessor::CPU>(addr + 2));
    return 1;
}

static int l_write_u8(lua_State *L)
{
    core().mem.poke8<Accessor::CPU>(check_address(L, 1), (u8) luaL_checkinteger(L, 2));
    memory_written();
    return 0;
}

static int l_write_u16(lua_State *L)
{
    core().mem.poke16<Accessor::CPU>(check_address(L, 1), (u16) luaL_checkinteger(L, 2));
    memory_written();
    return 0;
}

static int l_write_u32(lua_State *L)
{
    u32 addr = check_address(L, 1);
    u32 value = (u32) luaL_checkinteger(L, 2);
    Memory &mem = core().mem;
    mem.poke16<Accessor::CPU>(addr, (u16) (value >> 16));
    mem.poke16<Accessor::CPU>(addr + 2, (u16) value);
    memory_written();
    return 0;
}

static int l_peek_u8(lua_State *L)
{
    u32 addr = check_address(L, 1);
    check_ram_or_rom(L, addr, 1);
    lua_pushinteger(L, core().mem.spypeek8<Accessor::CPU>(addr));
    return 1;
}

static int l_peek_u16(lua_State *L)
{
    u32 addr = check_address(L, 1);
    check_ram_or_rom(L, addr, 2);
    Memory &mem = core().mem;
    lua_pushinteger(L, mem.spypeek8<Accessor::CPU>(addr) << 8 | mem.spypeek8<Accessor::CPU>(addr + 1));
    return 1;
}

static int l_peek_u32(lua_State *L)
{
    u32 addr = check_address(L, 1);
    check_ram_or_rom(L, addr, 4);
    Memory &mem = core().mem;
    lua_Integer value = 0;
    for (int i = 0; i < 4; i++) {
        value = value << 8 | mem.spypeek8<Accessor::CPU>(addr + i);
    }
    lua_pushinteger(L, value);
    return 1;
}

// Writes size bytes of value, high byte first, to RAM or ROM.
static void poke(lua_State *L, u32 addr, lua_Integer value, int size)
{
    check_ram_or_rom(L, addr, size);
    for (int i = 0; i < size; i++) {
        core().mem.patch(addr + i, (u8) (value >> (8 * (size - 1 - i))));
    }
    memory_written();
}

static int l_poke_u8(lua_State *L)
{
    poke(L, check_address(L, 1), luaL_checkinteger(L, 2), 1);
    return 0;
}

static int l_poke_u16(lua_State *L)
{
    poke(L, check_address(L, 1), luaL_checkinteger(L, 2), 2);
    return 0;
}

static int l_poke_u32(lua_State *L)
{
    poke(L, check_address(L, 1), luaL_checkinteger(L, 2), 4);
    return 0;
}

// read_range(address, length) returns the bytes as a string. Bytes which
// are not RAM or ROM are returned as 0.
static int l_read_range(lua_State *L)
{
    u32 addr = check_address(L, 1);
    lua_Integer length = luaL_checkinteger(L, 2);
    luaL_argcheck(L, length >= 0 && length <= MAX_RANGE_SIZE, 2, "invalid length");
    Memory &mem = core().mem;
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, (size_t) length);
    for (lua_Integer i = 0; i < length; i++) {
        u32 a = addr + (u32) i;
        out[i] = is_ram_or_rom(a, 1) ? (char) mem.spypeek8<Accessor::CPU>(a) : 0;
    }
    luaL_pushresultsize(&b, (size_t) length);
    return 1;
}

// write_range(address, string) writes the bytes of the string to RAM or
// ROM.
static int l_write_range(lua_State *L)
{
    u32 addr = check_address(L, 1);
    size_t length;
    const char *data = luaL_checklstring(L, 2, &length);
    check_ram_or_rom(L, addr, (int) length);
    for (size_t i = 0; i < length; i++) {
        core().mem.patch(addr + (u32) i, (u8) data[i]);
    }
    memory_written();
    return 0;
}

static const luaL_Reg mem_functions[] = {
    {"peek_u16", l_peek_u16},
    {"peek_u32", l_peek_u32},
    {"peek_u8", l_peek_u8},
    {"poke_u16", l_poke_u16},
    {"poke_u32", l_poke_u32},
    {"poke_u8", l_poke_u8},
    {"read_range", l_read_range},
    {"read_u16", l_read_u16},
    {"read_u32", l_read_u32},
    {"read_u8", l_read_u8},
    {"write_range", l_write_range},
    {"write_u16", l_write_u16},
    {"write_u32", l_write_u32},
    {"write_u8", l_write_u8},
    {NULL, NULL},
};

void engine_open_mem(lua_State *L)
{
    luaL_newlib(L, mem_functions);
    // mem.custom.COLOR00 and so on are the addresses of the custom chip
    // registers.
    lua_newtable(L);
    for (long i = 0; i <= (long) Reg::NO_OP; i++) {
        const char *name = RegEnum::key(Reg(i));
        if (strncmp(name, "REG_", 4) != 0) {
            lua_pushinteger(L, 0xdff000 + 2 * i);
            lua_setfield(L, -2, name);
        }
    }
    lua_setfield(L, -2, "custom");
    lua_setglobal(L, "mem");
}

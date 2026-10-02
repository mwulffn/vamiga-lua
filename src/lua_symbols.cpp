// Symbols of Amiga programs for the Lua debugger functions.
//
// This part of the Lua API is written in Lua: it reads the symbol hunks of
// an Amiga executable (a file on the host), finds where AmigaDOS has loaded
// the program by following the structures of exec and dos.library in the
// memory of the Amiga, and combines the two into addresses.

#include "engine.h"

#include <cstring>

static const char *symbols_source = R"LUA(
local HUNK_NAME, HUNK_CODE, HUNK_DATA, HUNK_BSS = 0x3e8, 0x3e9, 0x3ea, 0x3eb
local HUNK_RELOC32, HUNK_RELOC16, HUNK_RELOC8 = 0x3ec, 0x3ed, 0x3ee
local HUNK_SYMBOL, HUNK_DEBUG, HUNK_END, HUNK_HEADER = 0x3f0, 0x3f1, 0x3f2, 0x3f3
local HUNK_OVERLAY, HUNK_BREAK, HUNK_DREL32, HUNK_RELOC32SHORT = 0x3f5, 0x3f6, 0x3f7, 0x3fc

-- The loaded programs, and their symbols as {address, name} sorted by
-- address.
local programs = {}
local sorted_symbols = {}

-- Reads an executable in the hunk format. Returns a list with a table for
-- each hunk: {size = <bytes>, symbols = {name = offset}}.
local function read_hunks(path)
    local file, message = io.open(path, "rb")
    if not file then
        error("cannot open " .. message, 0)
    end
    local data = file:read("a")
    file:close()

    local pos = 1
    local function long()
        if pos + 3 > #data then
            error(path .. " ends in the middle of a hunk", 0)
        end
        local value = string.unpack(">I4", data, pos)
        pos = pos + 4
        return value
    end
    local function word()
        local value = string.unpack(">I2", data, pos)
        pos = pos + 2
        return value
    end

    if #data < 4 or long() ~= HUNK_HEADER then
        error(path .. " is not an Amiga executable", 0)
    end
    -- Names of resident libraries (not used since Kickstart 1.x).
    while true do
        local length = long()
        if length == 0 then break end
        pos = pos + length * 4
    end
    long() -- the size of the hunk table
    local first, last = long(), long()
    local hunks = {}
    for _ = first, last do
        local size = long()
        if size & 0xc0000000 == 0xc0000000 then
            long() -- memory attributes
        end
        hunks[#hunks + 1] = {size = (size & 0x3fffffff) * 4, symbols = {}}
    end

    local index = 1
    while pos <= #data and index <= #hunks do
        local id = long() & 0x3fffffff
        if id == HUNK_CODE or id == HUNK_DATA or id == HUNK_DEBUG or id == HUNK_NAME then
            pos = pos + (long() & 0x3fffffff) * 4
        elseif id == HUNK_BSS then
            long()
        elseif id == HUNK_RELOC32 or id == HUNK_RELOC16 or id == HUNK_RELOC8 then
            while true do
                local count = long()
                if count == 0 then break end
                pos = pos + 4 + count * 4
            end
        elseif id == HUNK_RELOC32SHORT or id == HUNK_DREL32 then
            while true do
                local count = word()
                if count == 0 then break end
                pos = pos + 2 + count * 2
            end
            pos = pos + (pos - 1) % 4
        elseif id == HUNK_SYMBOL then
            while true do
                local length = long() & 0xffffff
                if length == 0 then break end
                local name = data:sub(pos, pos + length * 4 - 1):gsub("%z+$", "")
                pos = pos + length * 4
                hunks[index].symbols[name] = long()
            end
        elseif id == HUNK_END then
            index = index + 1
        elseif id == HUNK_OVERLAY or id == HUNK_BREAK then
            break
        else
            error(string.format("%s has an unknown hunk type $%x", path, id), 0)
        end
    end
    return hunks
end

local function bptr(value)
    return (value << 2) & 0xffffffff
end

local function c_string(address)
    return (mem.read_range(address, 64):match("^[^%z]*"))
end

local function bcpl_string(address)
    return mem.read_range(address + 1, mem.peek_u8(address))
end

-- "DF0:c/Game" and "game" are the same program.
local function same_program(path, name)
    return path:match("[^:/]*$"):lower() == name:lower()
end

-- Calls f(task) for the tasks in an exec list.
local function each_task(list, f)
    local node = mem.peek_u32(list)
    for _ = 1, 1000 do
        local next_node = mem.peek_u32(node)
        if next_node == 0 then break end
        f(node)
        node = next_node
    end
end

-- Returns the address of the first segment of the running program with
-- the given name, or nil.
local function find_segment_list(name)
    local NT_PROCESS = 13
    local exec = mem.peek_u32(4)
    local found
    local function check(task)
        if found or mem.peek_u8(task + 8) ~= NT_PROCESS then return end
        -- A program started from a CLI: pr_CLI, cli_CommandName, cli_Module.
        local cli = bptr(mem.peek_u32(task + 172))
        if cli ~= 0 then
            local module = bptr(mem.peek_u32(cli + 60))
            local command = bptr(mem.peek_u32(cli + 16))
            if module ~= 0 and command ~= 0 and same_program(bcpl_string(command), name) then
                found = module
            end
        elseif same_program(c_string(mem.peek_u32(task + 10)), name) then
            -- A process of its own: the fourth entry of pr_SegList.
            local segment_array = bptr(mem.peek_u32(task + 128))
            if segment_array ~= 0 then
                found = bptr(mem.peek_u32(segment_array + 12))
            end
        end
    end
    check(mem.peek_u32(exec + 276)) -- ThisTask
    each_task(exec + 406, check)     -- TaskReady
    each_task(exec + 420, check)     -- TaskWait
    return found
end

-- Returns the segments of a loaded program as a list of {address = ...,
-- size = ...}, or nil if the program is not found. The memory of the Amiga
-- can hold anything, so failures to read it also give nil.
local function find_segments(name)
    local ok, result = pcall(function()
        local segment = find_segment_list(name)
        if not segment then return nil end
        local segments = {}
        while segment ~= 0 and #segments < 1000 do
            -- The size of the allocation is in the long word before the
            -- segment, and the segment starts with the pointer to the next.
            segments[#segments + 1] = {address = segment + 4, size = mem.peek_u32(segment - 4) - 8}
            segment = bptr(mem.peek_u32(segment))
        end
        return segments
    end)
    return ok and result or nil
end

local function update_sorted_symbols()
    sorted_symbols = {}
    for _, program in ipairs(programs) do
        for name, address in pairs(program.symbols) do
            sorted_symbols[#sorted_symbols + 1] = {address, name, program}
        end
    end
    table.sort(sorted_symbols, function(a, b)
        if a[1] ~= b[1] then return a[1] < b[1] end
        return a[2] < b[2]
    end)
end

-- C compilers put an underscore before the names from the source code.
local symbols_metatable = {
    __index = function(symbols, name)
        return rawget(symbols, "_" .. name)
    end,
}

function dbg.load_symbols(path, name, frames)
    name = name or path:match("[^/\\]*$")
    local hunks = read_hunks(path)
    local segments = find_segments(name)
    local waited = 0
    while not segments and frames and waited < frames do
        emu.wait_frames(10)
        waited = waited + 10
        segments = find_segments(name)
    end
    if not segments then
        error("the program '" .. name .. "' is not running", 0)
    end
    if #segments ~= #hunks then
        error(string.format("%s has %d hunks, but the running program '%s' has %d segments",
            path, #hunks, name, #segments), 0)
    end
    local program = {name = name, path = path, segments = segments, symbols = {}}
    for i, hunk in ipairs(hunks) do
        if hunk.size > segments[i].size then
            error(string.format("hunk %d of %s is larger than the segment of the running program",
                i - 1, path), 0)
        end
        for symbol, offset in pairs(hunk.symbols) do
            program.symbols[symbol] = segments[i].address + offset
        end
    end
    if next(program.symbols) == nil then
        error(path .. " has no symbols (it may have been stripped)", 0)
    end
    setmetatable(program.symbols, symbols_metatable)
    dbg.unload_symbols(name)
    programs[#programs + 1] = program
    update_sorted_symbols()
    return program
end

function dbg.unload_symbols(name)
    for i = #programs, 1, -1 do
        if not name or programs[i].name == name then
            table.remove(programs, i)
        end
    end
    update_sorted_symbols()
end

function dbg.symbol(name)
    for _, program in ipairs(programs) do
        local address = program.symbols[name]
        if address then return address end
    end
    return nil
end

-- Returns "name" or "name+$offset" for an address inside a loaded program,
-- using the nearest symbol before the address in the same segment.
function dbg.lookup(address)
    local low, high = 1, #sorted_symbols
    while low <= high do
        local middle = (low + high) // 2
        if sorted_symbols[middle][1] <= address then
            low = middle + 1
        else
            high = middle - 1
        end
    end
    local entry = sorted_symbols[high]
    if not entry then return nil end
    for _, segment in ipairs(entry[3].segments) do
        local first, last = segment.address, segment.address + segment.size - 1
        if entry[1] >= first and entry[1] <= last and address <= last then
            if address == entry[1] then return entry[2] end
            return string.format("%s+$%x", entry[2], address - entry[1])
        end
    end
    return nil
end

-- dbg.bpset also takes the name of a symbol.
local bpset = dbg.bpset
function dbg.bpset(address, callback)
    if type(address) == "string" then
        local name = address
        address = dbg.symbol(name)
        if not address then
            error("unknown symbol '" .. name .. "'", 2)
        end
    end
    return bpset(address, callback)
end
)LUA";

void engine_open_symbols(lua_State *L)
{
    if (luaL_loadbuffer(L, symbols_source, strlen(symbols_source), "=symbols") != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
        engine_log_error(L, "symbols");
    }
}


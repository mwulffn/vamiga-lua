// Lua functions for keyboard, joystick and mouse input (the input table).

#include "engine.h"

#include <strings.h>

using namespace vamiga;

struct named_code {
    const char *name;
    int code;
};

// The keys of the Amiga keyboard and their key codes. The names are the
// ones FS-UAE uses (from the input events of UAE, without KEY_).
static const named_code key_codes[] = {
    {"backquote", 0x00},
    {"1", 0x01},
    {"2", 0x02},
    {"3", 0x03},
    {"4", 0x04},
    {"5", 0x05},
    {"6", 0x06},
    {"7", 0x07},
    {"8", 0x08},
    {"9", 0x09},
    {"0", 0x0a},
    {"sub", 0x0b},
    {"equals", 0x0c},
    {"backslash", 0x0d},
    {"np_0", 0x0f},
    {"q", 0x10},
    {"w", 0x11},
    {"e", 0x12},
    {"r", 0x13},
    {"t", 0x14},
    {"y", 0x15},
    {"u", 0x16},
    {"i", 0x17},
    {"o", 0x18},
    {"p", 0x19},
    {"leftbracket", 0x1a},
    {"rightbracket", 0x1b},
    {"np_1", 0x1d},
    {"np_2", 0x1e},
    {"np_3", 0x1f},
    {"a", 0x20},
    {"s", 0x21},
    {"d", 0x22},
    {"f", 0x23},
    {"g", 0x24},
    {"h", 0x25},
    {"j", 0x26},
    {"k", 0x27},
    {"l", 0x28},
    {"semicolon", 0x29},
    {"singlequote", 0x2a},
    {"numbersign", 0x2b},
    {"np_4", 0x2d},
    {"np_5", 0x2e},
    {"np_6", 0x2f},
    {"30", 0x30},
    {"z", 0x31},
    {"x", 0x32},
    {"c", 0x33},
    {"v", 0x34},
    {"b", 0x35},
    {"n", 0x36},
    {"m", 0x37},
    {"comma", 0x38},
    {"period", 0x39},
    {"div", 0x3a},
    {"np_period", 0x3c},
    {"np_7", 0x3d},
    {"np_8", 0x3e},
    {"np_9", 0x3f},
    {"space", 0x40},
    {"backspace", 0x41},
    {"tab", 0x42},
    {"enter", 0x43},
    {"return", 0x44},
    {"esc", 0x45},
    {"del", 0x46},
    {"np_sub", 0x4a},
    {"cursor_up", 0x4c},
    {"cursor_down", 0x4d},
    {"cursor_right", 0x4e},
    {"cursor_left", 0x4f},
    {"f1", 0x50},
    {"f2", 0x51},
    {"f3", 0x52},
    {"f4", 0x53},
    {"f5", 0x54},
    {"f6", 0x55},
    {"f7", 0x56},
    {"f8", 0x57},
    {"f9", 0x58},
    {"f10", 0x59},
    {"np_lparen", 0x5a},
    {"np_rparen", 0x5b},
    {"np_div", 0x5c},
    {"np_mul", 0x5d},
    {"np_add", 0x5e},
    {"help", 0x5f},
    {"shift_left", 0x60},
    {"shift_right", 0x61},
    {"caps_lock", 0x62},
    {"ctrl", 0x63},
    {"alt_left", 0x64},
    {"alt_right", 0x65},
    {"amiga_left", 0x66},
    {"amiga_right", 0x67},
    {NULL, 0},
};

static const named_code port_modes[] = {
    {"none", 0},
    {"mouse", 1},
    {"joystick", 2},
    {NULL, 0},
};

static int check_port(lua_State *L, int arg)
{
    lua_Integer port = luaL_checkinteger(L, arg);
    luaL_argcheck(L, port == 0 || port == 1, arg, "port must be 0 or 1");
    return (int) port;
}

static int check_named_code(lua_State *L, int arg, const named_code *codes, const char *what)
{
    const char *name = luaL_checkstring(L, arg);
    for (int i = 0; codes[i].name; i++) {
        if (strcasecmp(codes[i].name, name) == 0) {
            return codes[i].code;
        }
    }
    return luaL_error(L, "unknown %s '%s'", what, name);
}

// Port 0 of the Lua functions is the mouse port of the Amiga, and port 1 the
// joystick port.
static ControlPortAPI &control_port(int port)
{
    return port == 0 ? g_vamiga->controlPort1 : g_vamiga->controlPort2;
}

// input.key(name, down) presses or releases a key, for example "a", "f1"
// and "return".
static int l_input_key(lua_State *L)
{
    engine_check_not_in_tap(L);
    KeyCode code = (KeyCode) check_named_code(L, 1, key_codes, "key");
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    if (lua_toboolean(L, 2)) {
        g_vamiga->keyboard.press(code);
    } else {
        g_vamiga->keyboard.release(code);
    }
    return 0;
}

// input.joy(port, button, down). Port 1 is the normal joystick port. The
// buttons are "left", "right", "up", "down", "fire", "fire2" and "fire3".
// Releasing a direction puts the stick back in the middle on that axis.
static int l_input_joy(lua_State *L)
{
    engine_check_not_in_tap(L);
    static const struct {
        const char *name;
        GamePadAction press, release;
    } buttons[] = {
        {"left", GamePadAction::PULL_LEFT, GamePadAction::RELEASE_X},
        {"right", GamePadAction::PULL_RIGHT, GamePadAction::RELEASE_X},
        {"up", GamePadAction::PULL_UP, GamePadAction::RELEASE_Y},
        {"down", GamePadAction::PULL_DOWN, GamePadAction::RELEASE_Y},
        {"fire", GamePadAction::PRESS_FIRE, GamePadAction::RELEASE_FIRE},
        {"fire2", GamePadAction::PRESS_FIRE2, GamePadAction::RELEASE_FIRE2},
        {"fire3", GamePadAction::PRESS_FIRE3, GamePadAction::RELEASE_FIRE3},
    };
    int port = check_port(L, 1);
    const char *name = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    for (const auto &button : buttons) {
        if (strcasecmp(button.name, name) == 0) {
            control_port(port).joystick.trigger(lua_toboolean(L, 3) ? button.press : button.release);
            return 0;
        }
    }
    return luaL_error(L, "unknown joystick button '%s'", name);
}

// input.mouse(dx, dy) moves the mouse in port 0.
static int l_input_mouse(lua_State *L)
{
    engine_check_not_in_tap(L);
    double dx = (double) luaL_checkinteger(L, 1);
    double dy = (double) luaL_checkinteger(L, 2);
    control_port(0).mouse.setDxDy(dx, dy);
    return 0;
}

// input.mouse_button(button, down), where button is 1 (left), 2 (right) or
// 3 (middle).
static int l_input_mouse_button(lua_State *L)
{
    engine_check_not_in_tap(L);
    static const GamePadAction press[] = {
        GamePadAction::PRESS_LEFT, GamePadAction::PRESS_RIGHT, GamePadAction::PRESS_MIDDLE};
    static const GamePadAction release[] = {
        GamePadAction::RELEASE_LEFT, GamePadAction::RELEASE_RIGHT, GamePadAction::RELEASE_MIDDLE};
    lua_Integer button = luaL_checkinteger(L, 1);
    luaL_argcheck(L, button >= 1 && button <= 3, 1, "button must be 1, 2 or 3");
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    control_port(0).mouse.trigger(lua_toboolean(L, 2) ? press[button - 1] : release[button - 1]);
    return 0;
}

// input.port_mode(port, mode) is accepted for scripts written for FS-UAE.
// In the vAmiga core both ports always have a mouse and a joystick, and the
// one which is moved is the one the Amiga sees, so there is nothing to
// select.
static int l_input_port_mode(lua_State *L)
{
    check_port(L, 1);
    check_named_code(L, 2, port_modes, "port mode");
    return 0;
}

static const luaL_Reg input_functions[] = {
    {"joy", l_input_joy},
    {"key", l_input_key},
    {"mouse", l_input_mouse},
    {"mouse_button", l_input_mouse_button},
    {"port_mode", l_input_port_mode},
    {NULL, NULL},
};

// input.type(text, frames) types the text on a US keyboard, holding each
// key for the given number of frames (default 2).
static const char *input_type_source = R"LUA(
local plain = {
    [" "] = "space", ["\n"] = "return", ["\t"] = "tab", ["-"] = "sub",
    ["="] = "equals", ["\\"] = "backslash", ["["] = "leftbracket",
    ["]"] = "rightbracket", [";"] = "semicolon", ["'"] = "singlequote",
    [","] = "comma", ["."] = "period", ["/"] = "div", ["`"] = "backquote",
}
local shifted = {
    ["!"] = "1", ["@"] = "2", ["#"] = "3", ["$"] = "4", ["%"] = "5",
    ["^"] = "6", ["&"] = "7", ["*"] = "8", ["("] = "9", [")"] = "0",
    ["_"] = "sub", ["+"] = "equals", ["|"] = "backslash",
    ["{"] = "leftbracket", ["}"] = "rightbracket", [":"] = "semicolon",
    ['"'] = "singlequote", ["<"] = "comma", [">"] = "period", ["?"] = "div",
    ["~"] = "backquote",
}

function input.type(text, frames)
    frames = frames or 2
    for c in text:gmatch(".") do
        local key, shift = plain[c], false
        if not key then
            if shifted[c] then
                key, shift = shifted[c], true
            elseif c:match("%u") then
                key, shift = c:lower(), true
            elseif c:match("[%l%d]") then
                key = c
            else
                error("cannot type the character '" .. c .. "'")
            end
        end
        if shift then input.key("shift_left", true) end
        input.key(key, true)
        emu.wait_frames(frames)
        input.key(key, false)
        if shift then input.key("shift_left", false) end
        emu.wait_frames(frames)
    end
end
)LUA";

void engine_open_input(lua_State *L)
{
    luaL_newlib(L, input_functions);
    lua_setglobal(L, "input");
    if (luaL_dostring(L, input_type_source) != LUA_OK) {
        engine_log_error(L, "input.type");
    }
}


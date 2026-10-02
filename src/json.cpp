// JSON encoding and decoding for the Lua remote protocol.

#include "engine.h"

#include <math.h>

#define MAX_TABLE_DEPTH 16

// Lua strings are byte strings, so bytes above 127 are written as the code
// points U+0080 to U+00FF. The receiver gets the original bytes back by
// encoding the string as Latin-1.
void engine_json_append_string(std::string &out, const char *s, size_t len)
{
    char buf[8];
    out += '"';
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) s[i];
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c < 0x20 || c >= 0x7f) {
            snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += (char) c;
        }
    }
    out += '"';
}

// Returns true if the table at index is a sequence (1..n without holes).
static bool is_sequence(lua_State *L, int index, lua_Integer *length)
{
    lua_Integer count = 0;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        lua_pop(L, 1);
        if (!lua_isinteger(L, -1)) {
            lua_pop(L, 1);
            return false;
        }
        count += 1;
    }
    *length = (lua_Integer) lua_rawlen(L, index);
    return count == *length;
}

static void append_value(std::string &out, lua_State *L, int index, int depth)
{
    char buf[64];
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
    case LUA_TNIL:
        out += "null";
        break;
    case LUA_TBOOLEAN:
        out += lua_toboolean(L, index) ? "true" : "false";
        break;
    case LUA_TNUMBER:
        if (lua_isinteger(L, index)) {
            snprintf(buf, sizeof buf, "%lld", (long long) lua_tointeger(L, index));
            out += buf;
        } else if (isfinite(lua_tonumber(L, index))) {
            snprintf(buf, sizeof buf, "%.17g", lua_tonumber(L, index));
            out += buf;
        } else {
            out += "null";
        }
        break;
    case LUA_TSTRING: {
        size_t len;
        const char *s = lua_tolstring(L, index, &len);
        engine_json_append_string(out, s, len);
        break;
    }
    case LUA_TTABLE: {
        if (depth >= MAX_TABLE_DEPTH || !lua_checkstack(L, 4)) {
            out += "\"<table nested too deeply>\"";
            break;
        }
        lua_Integer length;
        if (is_sequence(L, index, &length)) {
            out += '[';
            for (lua_Integer i = 1; i <= length; i++) {
                if (i > 1) {
                    out += ',';
                }
                lua_rawgeti(L, index, i);
                append_value(out, L, -1, depth + 1);
                lua_pop(L, 1);
            }
            out += ']';
        } else {
            out += '{';
            bool first = true;
            lua_pushnil(L);
            while (lua_next(L, index) != 0) {
                if (!first) {
                    out += ',';
                }
                first = false;
                // Convert a copy of the key, lua_next needs the original.
                lua_pushvalue(L, -2);
                size_t len;
                const char *key = luaL_tolstring(L, -1, &len);
                engine_json_append_string(out, key, len);
                lua_pop(L, 2);
                out += ':';
                append_value(out, L, -1, depth + 1);
                lua_pop(L, 1);
            }
            out += '}';
        }
        break;
    }
    default: {
        // Functions, userdata and threads are sent as their description.
        size_t len;
        const char *s = luaL_tolstring(L, index, &len);
        engine_json_append_string(out, s, len);
        lua_pop(L, 1);
        break;
    }
    }
}

void engine_json_append_value(std::string &out, lua_State *L, int index)
{
    append_value(out, L, index, 0);
}

// -----------------------------------------------------------------------------

struct json_reader {
    const char *p;
    const char *end;
};

static void skip_whitespace(json_reader &r)
{
    while (r.p < r.end && (*r.p == ' ' || *r.p == '\t' || *r.p == '\r' || *r.p == '\n')) {
        r.p++;
    }
}

static bool read_hex4(json_reader &r, unsigned *value)
{
    if (r.end - r.p < 4) {
        return false;
    }
    *value = 0;
    for (int i = 0; i < 4; i++) {
        char c = *r.p++;
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return false;
        }
        *value = *value * 16 + digit;
    }
    return true;
}

static void append_utf8(std::string &out, unsigned c)
{
    if (c < 0x80) {
        out += (char) c;
    } else if (c < 0x800) {
        out += (char) (0xc0 | (c >> 6));
        out += (char) (0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        out += (char) (0xe0 | (c >> 12));
        out += (char) (0x80 | ((c >> 6) & 0x3f));
        out += (char) (0x80 | (c & 0x3f));
    } else {
        out += (char) (0xf0 | (c >> 18));
        out += (char) (0x80 | ((c >> 12) & 0x3f));
        out += (char) (0x80 | ((c >> 6) & 0x3f));
        out += (char) (0x80 | (c & 0x3f));
    }
}

static bool read_string(json_reader &r, std::string &out)
{
    if (r.p >= r.end || *r.p != '"') {
        return false;
    }
    r.p++;
    while (r.p < r.end) {
        char c = *r.p++;
        if (c == '"') {
            return true;
        }
        if (c != '\\') {
            out += c;
            continue;
        }
        if (r.p >= r.end) {
            return false;
        }
        c = *r.p++;
        switch (c) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
            unsigned code;
            if (!read_hex4(r, &code)) {
                return false;
            }
            if (code >= 0xd800 && code < 0xdc00 && r.end - r.p >= 6 &&
                r.p[0] == '\\' && r.p[1] == 'u') {
                unsigned low;
                r.p += 2;
                if (!read_hex4(r, &low)) {
                    return false;
                }
                code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
            }
            append_utf8(out, code);
            break;
        }
        default:
            // Covers \", \\ and \/
            out += c;
            break;
        }
    }
    return false;
}

// Reads a number, true, false or null as it is written.
static bool read_literal(json_reader &r, std::string &out)
{
    while (r.p < r.end && *r.p != ',' && *r.p != '}' && *r.p != ' ' &&
           *r.p != '\t' && *r.p != '\r' && *r.p != '\n') {
        if (*r.p == '{' || *r.p == '[' || *r.p == '"') {
            return false;
        }
        out += *r.p++;
    }
    return !out.empty();
}

bool engine_json_parse_request(
    const char *text, size_t len, std::string &id, std::string &code, std::string &error)
{
    json_reader r = {text, text + len};
    bool have_code = false;
    id = "null";
    skip_whitespace(r);
    if (r.p >= r.end || *r.p != '{') {
        error = "Request is not a JSON object";
        return false;
    }
    r.p++;
    skip_whitespace(r);
    while (r.p < r.end && *r.p != '}') {
        std::string key;
        if (!read_string(r, key)) {
            error = "Invalid JSON: expected a key";
            return false;
        }
        skip_whitespace(r);
        if (r.p >= r.end || *r.p != ':') {
            error = "Invalid JSON: expected ':'";
            return false;
        }
        r.p++;
        skip_whitespace(r);
        std::string value;
        bool is_string = r.p < r.end && *r.p == '"';
        if (!(is_string ? read_string(r, value) : read_literal(r, value))) {
            error = "Invalid JSON: values must be strings, numbers, booleans or null";
            return false;
        }
        if (key == "code" && is_string) {
            code = value;
            have_code = true;
        } else if (key == "id") {
            id.clear();
            if (is_string) {
                engine_json_append_string(id, value.data(), value.size());
            } else {
                id = value;
            }
        }
        skip_whitespace(r);
        if (r.p < r.end && *r.p == ',') {
            r.p++;
            skip_whitespace(r);
        }
    }
    if (r.p >= r.end) {
        error = "Invalid JSON: missing '}'";
        return false;
    }
    if (!have_code) {
        error = "Request has no \"code\" string";
        return false;
    }
    return true;
}


// Lua access to the emulated display (the video table).
//
// The functions read the frame buffers of the core. Lua code runs between
// two frames, so they hold the frames which were just finished.
//
// The core draws every raster line once, with one pixel per hires pixel. The
// frame given to Lua leaves out the blanking areas to the left and at the
// top, and has two lines per raster line, so that the picture has the
// proportions it has on a monitor (see video_frame).

#include "engine.h"

#include "Texture.h"

#include <zlib.h>

#include <cstdio>
#include <vector>

using namespace vamiga;

// The first pixel and line outside the blanking areas.
static const int FIRST_X = 4 * HBLANK_CNT;
static const int FIRST_Y = 26;
static const int FRAME_WIDTH = HPIXELS - FIRST_X;
static const int FRAME_HEIGHT = 2 * (VPIXELS - FIRST_Y);

// Gives the red, green and blue values of a texel in the texture of the
// core. For the chipsets with four bits per colour component, the core has
// the bits as the high four of a byte, so white is 240, 240, 240. They are
// repeated in the low four here, which makes white 255, 255, 255, as it is
// with the AGA chipset.
static void texel_rgb(u32 texel, bool four_bits, u8 *out)
{
    for (int i = 0; i < 3; i++) {
        u8 value = (u8) (texel >> (8 * i));
        out[i] = four_bits ? (u8) (value | value >> 4) : value;
    }
}

// The lines of the frame, as pointers into the textures of the core.
//
// The core keeps the last frames it has drawn. Without interlace, a line of
// the frame is a line of the newest one, twice. With interlace, the frames
// are alternately long and short, and are the two fields of one picture: the
// lines of the long frame are the even lines and those of the short frame
// the odd ones.
//
// The emulator is paused when Lua code runs, so the textures do not change
// while they are read.
struct video_frame {
    const Texture *even;
    const Texture *odd;
    bool four_bits;

    video_frame()
    {
        const Texture &newest = core().videoPort.getTexture(0);
        const Texture &previous = core().videoPort.getTexture(-1);
        bool interlaced = newest.lof != previous.lof;
        even = interlaced && !newest.lof ? &previous : &newest;
        odd = interlaced && newest.lof ? &previous : &newest;
        four_bits = !core().denise.isAGA();
    }

    // Gives the red, green and blue values of a pixel.
    void pixel(int x, int y, u8 *out) const
    {
        const Texture *texture = y % 2 ? odd : even;
        // A texel holds the two super-hires pixels of a hires pixel.
        Texel texel = texture->pixels.ptr[(size_t) (FIRST_Y + y / 2) * HPIXELS + FIRST_X + x];
        texel_rgb((u32) texel, four_bits, out);
    }
};

// video.size() returns the width and height of the frame in pixels.
static int l_video_size(lua_State *L)
{
    lua_pushinteger(L, FRAME_WIDTH);
    lua_pushinteger(L, FRAME_HEIGHT);
    return 2;
}

// video.pixel(x, y) returns the red, green and blue values (0 to 255) of a
// pixel. The top left pixel is 0, 0.
static int l_video_pixel(lua_State *L)
{
    lua_Integer x = luaL_checkinteger(L, 1);
    lua_Integer y = luaL_checkinteger(L, 2);
    luaL_argcheck(L, x >= 0 && x < FRAME_WIDTH, 1, "outside the frame");
    luaL_argcheck(L, y >= 0 && y < FRAME_HEIGHT, 2, "outside the frame");
    u8 rgb[3];
    video_frame().pixel((int) x, (int) y, rgb);
    lua_pushinteger(L, rgb[0]);
    lua_pushinteger(L, rgb[1]);
    lua_pushinteger(L, rgb[2]);
    return 3;
}

// Returns the frame as red, green and blue bytes, line by line.
static std::vector<u8> frame_rgb(void)
{
    video_frame frame;
    std::vector<u8> pixels((size_t) FRAME_WIDTH * FRAME_HEIGHT * 3);
    u8 *out = pixels.data();
    for (int y = 0; y < FRAME_HEIGHT; y++) {
        for (int x = 0; x < FRAME_WIDTH; x++, out += 3) {
            frame.pixel(x, y, out);
        }
    }
    return pixels;
}

// video.pixels() returns the frame as a string with three bytes (red,
// green, blue) per pixel, line by line from the top, and the width and
// height.
static int l_video_pixels(lua_State *L)
{
    std::vector<u8> pixels = frame_rgb();
    lua_pushlstring(L, (const char *) pixels.data(), pixels.size());
    lua_pushinteger(L, FRAME_WIDTH);
    lua_pushinteger(L, FRAME_HEIGHT);
    return 3;
}

static void append_u32(std::vector<u8> &out, u32 value)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back((u8) (value >> shift));
    }
}

static void append_chunk(std::vector<u8> &out, const char *type, const std::vector<u8> &data)
{
    append_u32(out, (u32) data.size());
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    append_u32(out, (u32) crc32(0, out.data() + start, (uInt) (out.size() - start)));
}

// Returns the frame as a PNG file, or nothing if it could not be compressed.
static std::vector<u8> frame_png(void)
{
    std::vector<u8> pixels = frame_rgb();
    size_t line_size = (size_t) FRAME_WIDTH * 3;
    // Every line starts with a byte giving its filter (0: none).
    std::vector<u8> raw;
    raw.reserve((line_size + 1) * FRAME_HEIGHT);
    for (int y = 0; y < FRAME_HEIGHT; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), pixels.begin() + y * line_size, pixels.begin() + (y + 1) * line_size);
    }
    uLongf compressed_size = compressBound((uLong) raw.size());
    std::vector<u8> compressed(compressed_size);
    if (compress(compressed.data(), &compressed_size, raw.data(), (uLong) raw.size()) != Z_OK) {
        return {};
    }
    compressed.resize(compressed_size);

    std::vector<u8> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<u8> header;
    append_u32(header, FRAME_WIDTH);
    append_u32(header, FRAME_HEIGHT);
    // 8 bits per channel, RGB, and the only compression, filter and
    // interlace methods there are.
    header.insert(header.end(), {8, 2, 0, 0, 0});
    append_chunk(png, "IHDR", header);
    append_chunk(png, "IDAT", compressed);
    append_chunk(png, "IEND", {});
    return png;
}

// video.screenshot(path) saves the frame as a PNG file, and returns the
// width and height.
static int l_video_screenshot(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    std::vector<u8> png = frame_png();
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return luaL_error(L, "could not open '%s' for writing", path);
    }
    bool ok = !png.empty() && fwrite(png.data(), 1, png.size(), f) == png.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        return luaL_error(L, "could not write '%s'", path);
    }
    lua_pushinteger(L, FRAME_WIDTH);
    lua_pushinteger(L, FRAME_HEIGHT);
    return 2;
}

static const luaL_Reg video_functions[] = {
    {"pixel", l_video_pixel},
    {"pixels", l_video_pixels},
    {"screenshot", l_video_screenshot},
    {"size", l_video_size},
    {NULL, NULL},
};

void engine_open_video(lua_State *L)
{
    luaL_newlib(L, video_functions);
    lua_setglobal(L, "video");
}

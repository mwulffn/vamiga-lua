"""Tests for the video table (src/lua_video.cpp)."""

import struct
import unittest

import harness
from harness import EmulatorTestCase, LuaError, to_bytes


class VideoTest(EmulatorTestCase):
    test_disk = True

    def set_colour(self, colour: int) -> None:
        """Make the test program fill the screen with a 12-bit colour."""
        address = self.program + harness.COLOUR_OFFSET
        self.lua.call(f"mem.poke_u16({address}, {colour}) emu.wait_frames(3)")

    def test_size(self) -> None:
        width, height = self.lua.call("return video.size()")
        self.assertGreaterEqual(width, 640)
        self.assertGreaterEqual(height, 400)

    def test_pixel(self) -> None:
        for colour, rgb in [(0x0F00, [255, 0, 0]), (0x00F0, [0, 255, 0]), (0x000F, [0, 0, 255])]:
            self.set_colour(colour)
            self.assertEqual(self.lua.call("return video.pixel(200, 150)"), rgb)

    def test_pixel_outside_the_frame(self) -> None:
        with self.assertRaisesRegex(LuaError, "outside the frame"):
            self.lua.call("local w, h = video.size() return video.pixel(w, 0)")
        with self.assertRaisesRegex(LuaError, "outside the frame"):
            self.lua.call("return video.pixel(0, -1)")

    def test_pixels(self) -> None:
        self.set_colour(0x0F80)
        self.lua.call("emu.pause()")
        try:
            pixels, width, height = self.lua.call("return video.pixels()")
            pixel = self.lua.call("return video.pixel(200, 150)")
        finally:
            self.lua.call("emu.resume()")
        data = to_bytes(pixels)
        self.assertEqual(len(data), width * height * 3)
        self.assertEqual(pixel, [255, 136, 0])
        offset = (150 * width + 200) * 3
        self.assertEqual(list(data[offset : offset + 3]), pixel)
        # All the pixels on a line in the middle of the screen are the same
        # colour, except for the blanked ones at the edges.
        line = data[150 * width * 3 : 151 * width * 3]
        colours = {bytes(line[i : i + 3]) for i in range(0, len(line), 3)}
        self.assertLessEqual(colours, {bytes(pixel), bytes(3)})

    def test_screenshot(self) -> None:
        self.set_colour(0x000F)
        path = self.emulator.path / "screenshot.png"
        size = self.lua.call(f"return video.screenshot('{path}')")
        self.assertEqual(size, self.lua.call("return video.size()"))
        data = path.read_bytes()
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        # The IHDR chunk holds the width, height, bit depth and colour type.
        self.assertEqual(data[12:16], b"IHDR")
        width, height, depth, colour_type = struct.unpack(">IIBB", data[16:26])
        self.assertEqual([width, height], size)
        self.assertEqual((depth, colour_type), (8, 2))
        self.assertEqual(data[-8:-4], b"IEND")

    def test_screenshot_to_invalid_path(self) -> None:
        with self.assertRaisesRegex(LuaError, "could not open"):
            self.lua.call("video.screenshot('/nonexistent/directory/a.png')")


if __name__ == "__main__":
    unittest.main()

"""Tests for the input table (src/lua_input.cpp)."""

import unittest

import harness
from harness import EmulatorTestCase, LuaError

JOY0DAT = 0xDFF00A
CIAA_PRA = 0xBFE001
CIAA_SDR = 0xBFEC01


class InputTest(EmulatorTestCase):
    test_disk = True

    def joystick(self) -> int:
        """Return JOY1DAT as the test program last read it."""
        self.lua.call("emu.wait_frames(2)")
        return self.lua.eval(f"mem.peek_u16({self.program + harness.JOYSTICK_OFFSET})")

    def test_joystick_directions(self) -> None:
        self.lua.call("input.port_mode(1, 'joystick')")
        self.assertEqual(self.joystick() & 0x0303, 0)
        # Bit 1 is right and bit 9 is left. Up and down are XORed with those.
        expected = {"right": 0x0003, "left": 0x0300, "down": 0x0001, "up": 0x0100}
        for direction, bits in expected.items():
            self.lua.call(f"input.joy(1, '{direction}', true)")
            self.assertEqual(self.joystick() & 0x0303, bits, direction)
            self.lua.call(f"input.joy(1, '{direction}', false)")
            self.assertEqual(self.joystick() & 0x0303, 0, direction)

    def test_joystick_fire(self) -> None:
        self.lua.call("input.port_mode(1, 'joystick')")
        # Fire in port 1 is bit 7 of CIA-A PRA, and it is active low.
        self.lua.call("input.joy(1, 'fire', true) emu.wait_frames(2)")
        self.assertEqual(self.lua.eval(f"mem.read_u8({CIAA_PRA})") & 0x80, 0)
        self.lua.call("input.joy(1, 'fire', false) emu.wait_frames(2)")
        self.assertEqual(self.lua.eval(f"mem.read_u8({CIAA_PRA})") & 0x80, 0x80)

    def test_mouse(self) -> None:
        self.lua.call("input.port_mode(0, 'mouse') emu.wait_frames(2)")
        before = self.lua.eval(f"mem.read_u16({JOY0DAT})")
        self.lua.call("input.mouse(10, 5) emu.wait_frames(3)")
        after = self.lua.eval(f"mem.read_u16({JOY0DAT})")
        self.assertEqual((after - before) & 0xFF, 10)
        self.assertEqual(((after >> 8) - (before >> 8)) & 0xFF, 5)

    def test_mouse_button(self) -> None:
        self.lua.call("input.port_mode(0, 'mouse')")
        # The left button in port 0 is bit 6 of CIA-A PRA, active low.
        self.lua.call("input.mouse_button(1, true) emu.wait_frames(2)")
        self.assertEqual(self.lua.eval(f"mem.read_u8({CIAA_PRA})") & 0x40, 0)
        self.lua.call("input.mouse_button(1, false) emu.wait_frames(2)")
        self.assertEqual(self.lua.eval(f"mem.read_u8({CIAA_PRA})") & 0x40, 0x40)

    def test_key(self) -> None:
        # The keyboard sends the key code rotated left one bit and inverted
        # to the serial register of CIA-A. The code for A is $20.
        self.lua.call("input.key('a', true) emu.wait_frames(3)")
        self.assertEqual(self.lua.eval(f"mem.read_u8({CIAA_SDR})"), ~(0x20 << 1) & 0xFF)
        self.lua.call("input.key('a', false)")

    def test_unknown_names(self) -> None:
        with self.assertRaisesRegex(LuaError, "unknown key 'nosuchkey'"):
            self.lua.call("input.key('nosuchkey', true)")
        with self.assertRaisesRegex(LuaError, "unknown joystick button"):
            self.lua.call("input.joy(1, 'sideways', true)")
        with self.assertRaisesRegex(LuaError, "port must be 0 or 1"):
            self.lua.call("input.joy(2, 'left', true)")

    def test_type_rejects_unknown_characters(self) -> None:
        with self.assertRaisesRegex(LuaError, "cannot type"):
            self.lua.call("input.type('\\1')")


if __name__ == "__main__":
    unittest.main()

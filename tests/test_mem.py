"""Tests for the mem table (src/lua_mem.cpp)."""

import unittest

import harness
from harness import EmulatorTestCase, LuaError, to_bytes

# An address in chip RAM which the test program does not use.
SCRATCH = 0x70000


class MemTest(EmulatorTestCase):
    test_disk = True

    def test_poke_and_peek(self) -> None:
        self.lua.call(f"mem.poke_u32({SCRATCH}, 0x12345678)")
        self.assertEqual(self.lua.eval(f"mem.peek_u32({SCRATCH})"), 0x12345678)
        self.assertEqual(self.lua.eval(f"mem.peek_u16({SCRATCH + 2})"), 0x5678)
        self.assertEqual(self.lua.eval(f"mem.peek_u8({SCRATCH + 1})"), 0x34)
        self.lua.call(f"mem.poke_u16({SCRATCH}, 0xabcd) mem.poke_u8({SCRATCH + 3}, 0xef)")
        self.assertEqual(self.lua.eval(f"mem.peek_u32({SCRATCH})"), 0xABCD56EF)

    def test_read_and_write(self) -> None:
        self.lua.call(f"mem.write_u32({SCRATCH}, 0xfedcba98)")
        self.assertEqual(self.lua.eval(f"mem.read_u32({SCRATCH})"), 0xFEDCBA98)
        self.assertEqual(self.lua.eval(f"mem.read_u16({SCRATCH})"), 0xFEDC)
        self.assertEqual(self.lua.eval(f"mem.read_u8({SCRATCH + 3})"), 0x98)
        self.lua.call(f"mem.write_u16({SCRATCH}, 0x1122) mem.write_u8({SCRATCH + 2}, 0x33)")
        self.assertEqual(self.lua.eval(f"mem.peek_u32({SCRATCH})"), 0x11223398)

    def test_ranges(self) -> None:
        self.lua.call(rf"mem.write_range({SCRATCH}, 'ab\0\255')")
        self.assertEqual(self.lua.eval(f"mem.peek_u32({SCRATCH})"), 0x616200FF)
        data = to_bytes(self.lua.eval(f"mem.read_range({SCRATCH}, 4)"))
        self.assertEqual(data, b"ab\x00\xff")

    def test_read_range_finds_the_test_program(self) -> None:
        size = harness.DATA_OFFSET
        data = to_bytes(self.lua.eval(f"mem.read_range({self.program}, {size})"))
        self.assertEqual(data, harness.TEST_PROGRAM[:size])

    def test_kickstart_rom_is_readable(self) -> None:
        # The ROM starts with $1111 or $1114, followed by a JMP instruction.
        self.assertEqual(self.lua.eval("mem.peek_u16(0xf80002)"), 0x4EF9)

    def test_peek_fails_for_hardware_registers(self) -> None:
        with self.assertRaisesRegex(LuaError, "not RAM or ROM"):
            self.lua.call("mem.peek_u16(mem.custom.VPOSR)")
        with self.assertRaisesRegex(LuaError, "not RAM or ROM"):
            self.lua.call("mem.poke_u16(mem.custom.COLOR00, 0)")

    def test_read_range_returns_zero_for_hardware_registers(self) -> None:
        data = to_bytes(self.lua.eval("mem.read_range(0xdff000, 8)"))
        self.assertEqual(data, bytes(8))

    def test_custom_register_addresses(self) -> None:
        self.assertEqual(self.lua.eval("mem.custom.COLOR00"), 0xDFF180)
        self.assertEqual(self.lua.eval("mem.custom.JOY1DAT"), 0xDFF00C)

    def test_read_hardware_register(self) -> None:
        # The beam position changes while the emulation is running.
        values = {self.lua.eval("mem.read_u16(mem.custom.VHPOSR)") for _ in range(10)}
        self.lua.call("emu.wait_frames(1)")
        self.assertTrue(all(0 <= value <= 0xFFFF for value in values))

    def test_frame_counter_of_test_program(self) -> None:
        address = self.program + harness.COUNTER_OFFSET
        results = self.lua.call(
            f"local a = mem.peek_u32({address}) emu.wait_frames(10) return mem.peek_u32({address}) - a"
        )
        self.assertEqual(results, [10])

    def test_poked_code_is_run(self) -> None:
        # The loop of the test program is in the instruction cache of a
        # 68020 or later CPU. Change ADDQ.L #1,D0 to ADDQ.L #2,D0.
        addq = self.program + harness.ADDQ_OFFSET
        address = self.program + harness.COUNTER_OFFSET
        self.lua.call(f"emu.wait_frames(3) mem.poke_u16({addq}, 0x5480)")
        try:
            results = self.lua.call(
                f"emu.wait_frames(2) local a = mem.peek_u32({address}) emu.wait_frames(10) "
                f"return mem.peek_u32({address}) - a"
            )
        finally:
            self.lua.call(f"mem.poke_u16({addq}, 0x5280)")
        self.assertEqual(results, [20])

    def test_invalid_address(self) -> None:
        with self.assertRaisesRegex(LuaError, "address out of range"):
            self.lua.call("mem.read_u8(-1)")


if __name__ == "__main__":
    unittest.main()

"""Tests for the copper and beam breakpoints and the copper functions (src/lua_debug.cpp)."""

import unittest

from harness import EmulatorTestCase, LuaError

# Where the tests put their copper list. The test program does not use it.
COPPER_LIST = 0x70000

# The copper list: green from the top, blue from line 100.
SETUP = f"""
mem.write_range({COPPER_LIST}, string.pack(">I2I2I2I2I2I2I2I2",
    0x0180, 0x00f0,   -- MOVE #$0F0,COLOR00
    0x6401, 0xfffe,   -- WAIT for line 100
    0x0180, 0x000f,   -- MOVE #$00F,COLOR00
    0xffff, 0xfffe))  -- the end
mem.write_u32(mem.custom.COP1LCH, {COPPER_LIST})
mem.write_u16(mem.custom.DMACON, 0x8280)   -- copper DMA on
emu.wait_frames(3)
"""

SECOND_MOVE = COPPER_LIST + 8


class ChipTestCase(EmulatorTestCase):
    test_disk = True

    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        try:
            cls.lua.call(SETUP)
        except Exception:
            cls.tearDownClass()
            raise

    def tearDown(self) -> None:
        self.lua.call("dbg.copper_clear() dbg.beam_clear() emu.resume()")
        self.lua.events.clear()


class CopperTest(ChipTestCase):
    def test_copper_list_is_shown(self) -> None:
        # The test program makes the screen red once per frame, right after
        # the copper has made it green at the top, so the red is what shows
        # until the copper makes it blue at line 100. Lines of the frame are
        # two per raster line, and the frame starts at raster line 26.
        self.assertEqual(self.lua.call("return video.pixel(400, 2 * (90 - 26))"), [255, 0, 0])
        self.assertEqual(self.lua.call("return video.pixel(400, 2 * (110 - 26))"), [0, 0, 255])

    def test_copper_state(self) -> None:
        copper = self.lua.eval("dbg.copper()")
        self.assertEqual(copper["cop1lc"], COPPER_LIST)
        self.assertTrue(COPPER_LIST <= copper["pc"] <= COPPER_LIST + 16)

    def test_copper_disasm(self) -> None:
        lines = self.lua.eval(f"dbg.copper_disasm({COPPER_LIST}, 3)")
        self.assertEqual(
            [line["address"] for line in lines], [COPPER_LIST + 4 * i for i in range(3)]
        )
        self.assertEqual(lines[0]["words"], [0x0180, 0x00F0])
        self.assertIn("COLOR00", lines[0]["text"].upper())
        self.assertIn("WAIT", lines[1]["text"].upper())

    def test_break_stops_the_emulation(self) -> None:
        breakpoint_id = self.lua.eval(f"dbg.copper_break({SECOND_MOVE})")
        info = self.lua.eval("dbg.wait(10)")
        self.assertEqual(info["reason"], "copper_breakpoint")
        self.assertEqual(info["id"], breakpoint_id)
        self.assertEqual(info["address"], SECOND_MOVE)
        # The copper gets there when the beam reaches line 100.
        line, _ = self.lua.call("return emu.beam()")
        self.assertIn(line, [100, 101])

    def test_stopped_event_is_sent(self) -> None:
        self.lua.call(f"dbg.copper_break({SECOND_MOVE})")
        event = self.lua.wait_event("stopped")
        self.assertEqual((event["reason"], event["address"]), ("copper_breakpoint", SECOND_MOVE))

    def test_break_callback_is_called_once_per_frame(self) -> None:
        # The frames are waited for in the same request, as frames go by
        # between two requests.
        hits = self.lua.eval(
            f"copper_hits = {{}} dbg.copper_break({SECOND_MOVE}, function(address) "
            "copper_hits[#copper_hits + 1] = {address, emu.beam()} end) "
            "emu.wait_frames(5) return copper_hits"
        )
        self.assertEqual(len(hits), 5)
        for address, line, _ in hits:
            self.assertEqual(address, SECOND_MOVE)
            self.assertIn(line, [100, 101])
        self.assertFalse(self.lua.eval("dbg.stopped()"))

    def test_callback_can_stop_the_emulation(self) -> None:
        self.lua.call(f"dbg.copper_break({COPPER_LIST}, function() emu.pause() end)")
        info = self.lua.eval("dbg.wait(10)")
        self.assertEqual((info["reason"], info["address"]), ("copper_breakpoint", COPPER_LIST))

    def test_watch_sees_the_writes_of_the_copper(self) -> None:
        # The copper writes COLOR00 twice per frame. The test program also
        # writes it once per frame, but that is the CPU.
        hits = self.lua.eval(
            "watch_hits = 0 dbg.copper_watch(mem.custom.COLOR00, function(register) "
            "assert(register == mem.custom.COLOR00) watch_hits = watch_hits + 1 end) "
            "emu.wait_frames(4) return watch_hits"
        )
        self.assertEqual(hits, 8)

    def test_watch_on_another_register_is_not_reached(self) -> None:
        self.lua.call("dbg.copper_watch(mem.custom.COLOR01)")
        self.assertIsNone(self.lua.eval("dbg.wait(5)"))

    def test_watch_stops_the_emulation(self) -> None:
        watch_id = self.lua.eval("dbg.copper_watch(mem.custom.COLOR00)")
        info = self.lua.eval("dbg.wait(10)")
        self.assertEqual(info["reason"], "copper_watchpoint")
        self.assertEqual((info["id"], info["address"]), (watch_id, 0xDFF180))

    def test_clear(self) -> None:
        self.lua.call(f"local id = dbg.copper_break({SECOND_MOVE}) dbg.copper_clear(id)")
        self.assertIsNone(self.lua.eval("dbg.wait(5)"))

    def test_invalid_register(self) -> None:
        with self.assertRaisesRegex(LuaError, "address of a custom chip register"):
            self.lua.call("dbg.copper_watch(0x180)")


class BeamTest(ChipTestCase):
    def test_break_after_reset(self) -> None:
        # A reset clears the events of the core, the one for the beam
        # breakpoints too.
        self.lua.call("dbg.beam_break(150, 40) emu.pause() emu.reset(false) emu.resume()")
        info = self.lua.eval("dbg.wait(10)")
        self.assertEqual(info["reason"], "beam")
        self.assertEqual(self.lua.call("return emu.beam()")[0], 150)

    def test_break_stops_the_emulation(self) -> None:
        breakpoint_id = self.lua.eval("dbg.beam_break(150, 40)")
        info = self.lua.eval("dbg.wait(10)")
        self.assertEqual(info["reason"], "beam")
        self.assertEqual(info["id"], breakpoint_id)
        self.assertEqual(info["address"], 150 << 16 | 40)
        line, position = self.lua.call("return emu.beam()")
        self.assertEqual(line, 150)
        # The CPU finishes its instruction first.
        self.assertTrue(40 <= position < 40 + 60, position)

    def test_callback_is_called_once_per_frame(self) -> None:
        hits = self.lua.eval(
            "beam_hits = {} dbg.beam_break(200, 10, function(line, position) "
            "beam_hits[#beam_hits + 1] = {line, position, emu.beam()} end) "
            "emu.wait_frames(5) return beam_hits"
        )
        self.assertEqual(len(hits), 5)
        for line, position, beam_line, beam_position in hits:
            self.assertEqual((line, position, beam_line), (200, 10, 200))
            self.assertTrue(10 <= beam_position < 10 + 60, beam_position)

    def test_position_defaults_to_the_start_of_the_line(self) -> None:
        beam_at = self.lua.eval(
            "dbg.beam_break(120, function(line, position) beam_at = {line, position} end) "
            "emu.wait_frames(2) return beam_at"
        )
        self.assertEqual(beam_at, [120, 0])

    def test_two_breakpoints(self) -> None:
        lines = self.lua.eval(
            "beam_lines = {} for _, line in ipairs({50, 250}) do "
            "dbg.beam_break(line, 0, function(line) beam_lines[#beam_lines + 1] = line end) end "
            "emu.wait_frames(3) return beam_lines"
        )
        self.assertEqual(len(lines), 6)
        self.assertEqual(sorted(set(lines)), [50, 250])

    def test_clear(self) -> None:
        self.lua.call("local id = dbg.beam_break(150) dbg.beam_clear(id)")
        self.assertIsNone(self.lua.eval("dbg.wait(5)"))

    def test_invalid_position(self) -> None:
        with self.assertRaisesRegex(LuaError, "must be 0 to 312"):
            self.lua.call("dbg.beam_break(400)")
        with self.assertRaisesRegex(LuaError, "must be 0 to 226"):
            self.lua.call("dbg.beam_break(100, 300)")


if __name__ == "__main__":
    unittest.main()

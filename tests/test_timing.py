"""Tests for emu.cycles, emu.beam, emu.timing and dbg.measure."""

import time
import unittest

import harness
from harness import EmulatorTestCase, LuaError


class TimingTest(EmulatorTestCase):
    test_disk = True

    def setUp(self) -> None:
        self.addq = self.program + harness.ADDQ_OFFSET
        self.timing = self.lua.eval("emu.timing()")

    def tearDown(self) -> None:
        self.lua.call("dbg.bpclear() emu.resume()")
        self.lua.events.clear()

    def test_timing(self) -> None:
        timing = self.timing
        # PAL has 313 lines of 227.5 colour clocks, NTSC 263.
        self.assertIn(timing["lines"], [262, 263, 312, 313])
        self.assertIn(timing["cycles_per_line"], [454, 456])
        self.assertEqual(timing["cycles_per_frame"], timing["lines"] * timing["cycles_per_line"])
        self.assertTrue(49 < timing["hz"] < 61)

    def test_cycles_per_frame(self) -> None:
        cycles = self.lua.eval("local c = emu.cycles() emu.wait_frames(20) return emu.cycles() - c")
        expected = 20 * self.timing["cycles_per_frame"]
        self.assertLess(abs(cycles - expected), expected * 0.005)

    def test_cycles_stand_still_while_paused(self) -> None:
        self.lua.call("emu.pause()")
        cycles = self.lua.eval("emu.cycles()")
        time.sleep(0.2)
        self.assertEqual(self.lua.eval("emu.cycles()"), cycles)

    def test_beam_after_a_frame(self) -> None:
        # Tasks continue right after the frame has ended, at the top.
        line, horizontal = self.lua.call("emu.wait_frames(1) return emu.beam()")
        self.assertLessEqual(line, 1)
        self.assertLess(horizontal, self.timing["cycles_per_line"] // 2)

    def test_beam_in_breakpoint(self) -> None:
        # The program waits for the vertical blank, so it runs at the top.
        self.lua.call(f"dbg.bpset({self.addq}, function() beam = {{emu.beam()}} end)")
        line, horizontal = self.lua.eval("emu.wait_frames(2) return beam")
        self.assertLess(line, 20)
        self.assertLess(horizontal, self.timing["cycles_per_line"] // 2)

    def test_measure_instruction(self) -> None:
        result = self.lua.eval(f"dbg.measure({self.addq}, {self.addq + 2}, 3)")
        self.assertEqual(result["count"], 3)
        self.assertEqual(len(result["samples"]), 3)
        self.assertEqual(result["min"], min(result["samples"]))
        self.assertEqual(result["max"], max(result["samples"]))
        self.assertAlmostEqual(result["average"], sum(result["samples"]) / 3)
        # The 68000 and the 68010 are emulated cycle by cycle.
        if self.lua.eval("emu.config_get('CPU.REVISION')") == 0:
            # ADDQ.L #1,D0 takes 8 cycles on a 68000.
            self.assertEqual(result["samples"], [8, 8, 8])
        else:
            self.assertTrue(all(0 <= sample < 100 for sample in result["samples"]))

    def test_measure_period(self) -> None:
        # The program gets to the same instruction once per frame.
        result = self.lua.eval(f"dbg.measure({self.addq}, {self.addq}, 3)")
        expected = self.timing["cycles_per_frame"]
        for sample in result["samples"]:
            self.assertLess(abs(sample - expected), expected * 0.005)

    def test_measure_removes_its_breakpoints(self) -> None:
        self.lua.call(f"dbg.measure({self.addq}, {self.addq + 2})")
        self.assertEqual(self.lua.eval("dbg.bplist()"), [])

    def test_measure_addresses_not_reached(self) -> None:
        with self.assertRaisesRegex(LuaError, "not reached in 5 frames"):
            self.lua.call("dbg.measure(0x12345678, 0x1234567a, 1, 5)")
        self.assertEqual(self.lua.eval("dbg.bplist()"), [])


if __name__ == "__main__":
    unittest.main()

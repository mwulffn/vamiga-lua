"""Tests for the window (src/window.cpp), which SDL draws and plays to nowhere here."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

import harness
from harness import Emulator, LuaError

# The drivers of SDL which need no screen and no sound device.
DUMMY_DRIVERS = {"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}

# A script which runs the machine from the start, and writes what a script
# can see of it at the end of some of the frames: first while the emulation
# runs at the speed of the Amiga, which with the window is with sound, and
# then after the test disk has been booted in warp mode.
TRACE_SCRIPT = """
local out = assert(io.open(arg_path .. "/trace.txt", "w"))
local function trace(count)
    for i = 1, count do
        emu.wait_frames(1)
        local line, position = emu.beam()
        out:write(string.format("%d %x %d %d %d\\n", emu.frame(), cpu.pc, emu.cycles(), line, position))
    end
    local pixels = assert(io.open(arg_path .. "/frame" .. emu.frame() .. ".rgb", "wb"))
    pixels:write((video.pixels()))
    pixels:close()
end
trace(25)
emu.warp(true)
emu.wait_frames(400)
emu.warp(false)
trace(10)
out:close()
emu.quit()
"""


def run_trace(directory: Path, window: bool) -> dict[str, bytes]:
    """Run TRACE_SCRIPT and return the files it wrote."""
    command = harness.base_command()
    directory.mkdir()
    harness.create_test_disk(directory / "test.adf")
    script = directory / "trace.lua"
    script.write_text(f"local arg_path = '{directory}'\n" + TRACE_SCRIPT)
    command += ["--df0", str(directory / "test.adf"), "--lua", str(script)]
    if window:
        command += ["--window"]
    result = subprocess.run(
        command,
        env={**os.environ, **DUMMY_DRIVERS},
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )
    if "built without SDL 3" in result.stderr:
        raise unittest.SkipTest("The program was built with no window")
    if result.returncode != 0:
        raise RuntimeError(result.stderr)
    return {path.name: path.read_bytes() for path in directory.glob("*.*") if path != script}


class SameWithWindowTest(unittest.TestCase):
    def test_frames_are_the_same_with_and_without_the_window(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vamiga-lua-test-") as directory:
            with_window = run_trace(Path(directory) / "window", True)
            without = run_trace(Path(directory) / "headless", False)
        self.assertEqual(len(without["trace.txt"].splitlines()), 35)
        self.assertEqual(with_window["trace.txt"].decode(), without["trace.txt"].decode())
        self.assertEqual(sorted(with_window), sorted(without))
        for name in without:
            self.assertEqual(with_window[name], without[name], name)
        # The disk was booted, and the program on it has made the screen red.
        self.assertEqual(without["frame435.rgb"][-3:], b"\xff\x00\x00")


class WindowTest(harness.EmulatorTestCase):
    test_disk = True

    @classmethod
    def setUpClass(cls) -> None:
        cls.emulator = Emulator(test_disk=True, environment=DUMMY_DRIVERS)
        cls.lua = cls.emulator.lua
        try:
            cls.lua.call("emu.window(true)")
            cls.program = cls.emulator.find_test_program()
        except LuaError as error:
            cls.emulator.stop()
            if "built without SDL 3" in str(error):
                raise unittest.SkipTest("The program was built with no window") from None
            raise
        except Exception:
            cls.emulator.stop()
            raise

    def setUp(self) -> None:
        self.addq = self.program + harness.ADDQ_OFFSET
        self.lua.call("emu.window(true)")

    def tearDown(self) -> None:
        self.lua.call("dbg.bpclear() dbg.beam_clear() emu.warp(false) emu.resume()")
        self.lua.events.clear()

    def counter(self) -> int:
        return self.lua.eval(f"mem.peek_u32({self.program + harness.COUNTER_OFFSET})")

    def test_open_and_close(self) -> None:
        self.assertTrue(self.lua.eval("emu.window()"))
        self.assertFalse(self.lua.eval("emu.window(false)"))
        self.assertFalse(self.lua.eval("emu.window()"))
        self.assertEqual(
            self.lua.eval("local f = emu.frame() emu.wait_frames(3) return emu.frame() - f"),
            3,
        )
        self.assertTrue(self.lua.eval("emu.window(true)"))
        self.assertTrue(self.lua.eval("emu.window(true)"))

    def test_one_frame_at_a_time(self) -> None:
        address = self.program + harness.COUNTER_OFFSET
        counted, cycles = self.lua.call(
            f"local counter, c = mem.peek_u32({address}), emu.cycles() emu.wait_frames(20) "
            f"return mem.peek_u32({address}) - counter, emu.cycles() - c"
        )
        self.assertEqual(counted, 20)
        expected = 20 * self.lua.eval("emu.timing().cycles_per_frame")
        self.assertLess(abs(cycles - expected), expected * 0.005)

    def test_frames_end_at_the_same_place(self) -> None:
        positions = self.lua.eval(
            "local positions = {} for i = 1, 10 do emu.wait_frames(1) "
            "positions[i] = {emu.beam()} end return positions"
        )
        for line, position in positions:
            self.assertEqual(line, 0)
            self.assertTrue(0x12 <= position < 0x12 + 60, position)

    def test_breakpoint_and_single_steps(self) -> None:
        self.lua.call(f"dbg.bpset({self.addq})")
        info = self.lua.eval("dbg.wait(10)")
        self.assertEqual(info["reason"], "breakpoint")
        self.assertEqual(self.lua.eval("cpu.pc"), self.addq)
        counter = self.counter()
        self.lua.call("dbg.bpclear() dbg.step(2)")
        self.assertEqual(self.lua.eval("cpu.pc"), self.addq + 4)
        self.assertEqual(self.counter(), counter + 1)
        self.lua.call("dbg.go()")
        self.assertFalse(self.lua.eval("dbg.stopped()"))

    def test_breakpoint_callbacks(self) -> None:
        # The breakpoint pauses the emulator in every frame, close to where
        # the frame ends. Every frame is still run, and counted, once.
        hits, cycles = self.lua.call(
            f"local hits = 0 dbg.bpset({self.addq}, function() hits = hits + 1 end) "
            f"local c = emu.cycles() emu.wait_frames(50) return hits, emu.cycles() - c"
        )
        self.assertIn(hits, (49, 50, 51))
        expected = 50 * self.lua.eval("emu.timing().cycles_per_frame")
        self.assertLess(abs(cycles - expected), expected * 0.005)

    def test_beam_breakpoint_where_the_frame_ends(self) -> None:
        # The frames are still counted once each.
        hits = self.lua.eval(
            "local hits = 0 dbg.beam_break(0, 0x12, function() hits = hits + 1 end) "
            "local f = emu.frame() emu.wait_frames(10) return {hits, emu.frame() - f}"
        )
        # The breakpoint is also reached at once, if the beam is exactly
        # there when it is set.
        self.assertIn(hits[0], (10, 11))
        self.assertEqual(hits[1], 10)

    def test_pause_and_step(self) -> None:
        self.lua.call("emu.pause()")
        counter = self.counter()
        self.lua.call("emu.step(3)")
        self.assertEqual(self.counter(), counter + 3)
        self.assertTrue(self.lua.eval("emu.paused()"))

    def test_state(self) -> None:
        path = self.emulator.path / "window.state"
        address = self.program + harness.COUNTER_OFFSET
        counters = self.lua.eval(
            f"local function counter() return mem.peek_u32({address}) end "
            f"emu.wait_frames(2) state.save('{path}') local saved = counter() "
            f"emu.wait_frames(5) state.load('{path}') local loaded = counter() "
            f"emu.wait_frames(5) return {{saved, loaded, counter()}}"
        )
        self.assertEqual(counters, [counters[0], counters[0], counters[0] + 5])

    def test_warp(self) -> None:
        address = self.program + harness.COUNTER_OFFSET
        counted = self.lua.eval(
            f"local counter = mem.peek_u32({address}) emu.warp(true) emu.wait_frames(200) "
            f"emu.warp(false) emu.wait_frames(3) return mem.peek_u32({address}) - counter"
        )
        self.assertEqual(counted, 203)

    def test_volume(self) -> None:
        self.lua.call("emu.window(true, 0) emu.wait_frames(2) emu.window(true, 100)")
        with self.assertRaisesRegex(LuaError, "volume"):
            self.lua.call("emu.window(true, 101)")


if __name__ == "__main__":
    unittest.main()

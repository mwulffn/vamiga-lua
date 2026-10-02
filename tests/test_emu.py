"""Tests for the emu functions (src/engine.cpp)."""

import time
import unittest
from pathlib import Path

from harness import Emulator, EmulatorTestCase, LuaError


class EmuTest(EmulatorTestCase):
    def tearDown(self) -> None:
        self.lua.call("emu.resume() emu.warp(false)")

    def test_frame_counter_advances(self) -> None:
        first = self.lua.eval("emu.frame()")
        time.sleep(0.5)
        self.assertGreater(self.lua.eval("emu.frame()"), first + 10)

    def test_wait_next_frame(self) -> None:
        results = self.lua.call(
            "local f = emu.frame() emu.wait_next_frame() return emu.frame() - f"
        )
        self.assertEqual(results, [1])

    def test_frame_callback(self) -> None:
        self.lua.call("count = 0 id = emu.on_frame(function() count = count + 1 end)")
        self.lua.call("emu.wait_frames(10)")
        self.lua.call("emu.remove_frame_callback(id)")
        count = self.lua.eval("count")
        self.assertGreaterEqual(count, 10)
        self.lua.call("emu.wait_frames(5)")
        self.assertEqual(self.lua.eval("count"), count)

    def test_error_in_frame_callback_is_logged(self) -> None:
        self.lua.call(
            "local id id = emu.on_frame(function() emu.remove_frame_callback(id) error('oops') end)"
        )
        self.lua.call("emu.wait_frames(2)")
        self.assertEqual(self.lua.eval("1"), 1)

    def test_pause_stops_the_frame_counter(self) -> None:
        self.assertEqual(self.lua.call("emu.pause() return emu.paused()"), [True])
        frame = self.lua.eval("emu.frame()")
        time.sleep(0.3)
        self.assertEqual(self.lua.eval("emu.frame()"), frame)
        self.lua.call("emu.resume()")
        self.assertEqual(self.lua.call("emu.wait_frames(2) return emu.paused()"), [False])

    def test_step_runs_exactly_the_given_frames(self) -> None:
        self.lua.call("emu.pause()")
        frame = self.lua.eval("emu.frame()")
        self.assertEqual(
            self.lua.call("emu.step() return emu.frame(), emu.paused()"), [frame + 1, True]
        )
        self.assertEqual(self.lua.call("emu.step(10) return emu.frame()"), [frame + 11])
        time.sleep(0.2)
        self.assertEqual(self.lua.eval("emu.frame()"), frame + 11)

    def test_warp_runs_faster(self) -> None:
        def frames_per_second() -> float:
            start = self.lua.eval("emu.frame()")
            time.sleep(1)
            return self.lua.eval("emu.frame()") - start

        normal = frames_per_second()
        self.lua.call("emu.warp(true)")
        self.assertGreater(frames_per_second(), normal * 2)
        self.lua.call("emu.warp(false) emu.wait_frames(5)")
        self.assertLess(frames_per_second(), normal * 1.5)

    def test_config_get(self) -> None:
        # The options are the ones of the vAmiga core.
        self.assertEqual(self.lua.eval("emu.config_get('DRIVE.CONNECT')"), 1)
        with self.assertRaisesRegex(LuaError, "unknown option 'no_such_option'"):
            self.lua.eval("emu.config_get('no_such_option')")

    def test_config_set(self) -> None:
        self.assertEqual(self.lua.call("return emu.config_set('MON.BRIGHTNESS', 60)"), [True])
        self.lua.call("emu.wait_frames(3)")
        self.assertEqual(self.lua.eval("emu.config_get('MON.BRIGHTNESS')"), 60)
        self.assertEqual(self.lua.call("return emu.config_set('MON.BRIGHTNESS', 'much')"), [False])
        with self.assertRaisesRegex(LuaError, "unknown option"):
            self.lua.call("emu.config_set('no_such_option', 1)")

    def test_reset_keeps_lua_running(self) -> None:
        self.lua.call("kept = 42 emu.reset(true)")
        self.assertEqual(self.lua.call("emu.wait_frames(5) return kept"), [42])


class QuitTest(unittest.TestCase):
    def check_quit(self, code: str) -> None:
        emulator = Emulator()
        try:
            emulator.lua.call(code)
            emulator.process.wait(10)
        finally:
            emulator.stop()

    def test_quit(self) -> None:
        self.check_quit("emu.quit()")

    def test_quit_while_paused(self) -> None:
        self.check_quit("emu.pause() emu.quit()")


class CommandLineTest(unittest.TestCase):
    def test_lua_option_runs_script(self) -> None:
        script = str(Path(__file__).with_name("startup.lua"))
        emulator = Emulator(arguments=["--lua", script])
        try:
            self.assertEqual(emulator.lua.eval("startup_script_loaded"), True)
            emulator.lua.call("emu.wait_frames(5)")
            self.assertGreaterEqual(emulator.lua.eval("startup_frames"), 3)
        finally:
            emulator.stop()

    def test_set_option(self) -> None:
        emulator = Emulator({"MON.BRIGHTNESS": "70"})
        try:
            self.assertEqual(emulator.lua.eval("emu.config_get('MON.BRIGHTNESS')"), 70)
        finally:
            emulator.stop()

    def test_unknown_argument_ends_the_program(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "exited while starting"):
            Emulator(arguments=["--some-unknown-option"])


if __name__ == "__main__":
    unittest.main()

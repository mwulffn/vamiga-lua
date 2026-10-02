"""Tests for the state and media tables (src/lua_state.cpp)."""

import unittest

import harness
from harness import EmulatorTestCase, LuaError


class StateFileTest(EmulatorTestCase):
    test_disk = True

    def counter(self) -> int:
        return self.lua.eval(f"mem.peek_u32({self.program + harness.COUNTER_OFFSET})")

    def check_save_and_load(self) -> None:
        path = self.emulator.path / "test.uss"
        self.lua.call(f"state.save('{path}')")
        self.assertGreater(path.stat().st_size, 1000)
        saved = self.counter()
        self.lua.call(f"mem.poke_u32({0x70000}, 0x11111111) emu.step(50)")
        self.assertEqual(self.counter(), saved + 50)
        self.lua.call(f"state.load('{path}')")
        # The state was saved at the end of a frame, and one frame has been
        # run after loading it.
        self.assertLessEqual(abs(self.counter() - (saved + 1)), 1)
        self.assertNotEqual(self.lua.eval("mem.peek_u32(0x70000)"), 0x11111111)

    def test_save_and_load_while_paused(self) -> None:
        self.lua.call("emu.pause()")
        try:
            self.check_save_and_load()
            self.assertTrue(self.lua.eval("emu.paused()"))
        finally:
            self.lua.call("emu.resume()")

    def test_save_and_load_while_running(self) -> None:
        path = self.emulator.path / "running.uss"
        self.lua.call(f"state.save('{path}')")
        saved = self.counter()
        self.lua.call("emu.wait_frames(50)")
        self.assertGreaterEqual(self.counter(), saved + 50)
        self.lua.call(f"state.load('{path}')")
        self.assertLess(self.counter(), saved + 10)
        self.assertFalse(self.lua.eval("emu.paused()"))

    def test_load_missing_file(self) -> None:
        with self.assertRaisesRegex(LuaError, "does not exist"):
            self.lua.call("state.load('/nonexistent/file.uss')")

    def test_save_to_invalid_path(self) -> None:
        with self.assertRaisesRegex(LuaError, "could not save"):
            self.lua.call("state.save('/nonexistent/directory/file.uss')")


class SnapshotTest(EmulatorTestCase):
    test_disk = True

    def setUp(self) -> None:
        self.lua.call("emu.pause()")

    def tearDown(self) -> None:
        self.lua.call("emu.resume()")

    def counter(self) -> int:
        return self.lua.eval(f"mem.peek_u32({self.program + harness.COUNTER_OFFSET})")

    def test_snapshot_and_restore(self) -> None:
        self.lua.call("mem.poke_u32(0x70000, 0xaaaaaaaa) snapshot = state.snapshot()")
        self.assertGreater(self.lua.eval("#snapshot"), 100000)
        saved = self.counter()
        self.lua.call("mem.poke_u32(0x70000, 0xbbbbbbbb) emu.step(100)")
        self.assertEqual(self.counter(), saved + 100)
        self.lua.call("state.restore(snapshot)")
        self.assertEqual(self.lua.eval("mem.peek_u32(0x70000)"), 0xAAAAAAAA)
        # One frame has been run after loading the state.
        self.assertLessEqual(abs(self.counter() - (saved + 1)), 1)
        self.assertTrue(self.lua.eval("emu.paused()"))

    def test_restore_is_repeatable(self) -> None:
        self.lua.call("snapshot = state.snapshot()")
        counters = []
        for _ in range(3):
            self.lua.call("state.restore(snapshot) emu.step(10)")
            counters.append(self.counter())
        self.assertEqual(counters, [counters[0]] * 3)

    def test_several_snapshots(self) -> None:
        # Taking a snapshot runs to the end of the current frame, so the
        # second one is 51 frames after the first.
        self.lua.call("first = state.snapshot() emu.step(50) second = state.snapshot()")
        self.lua.call("state.restore(first)")
        first = self.counter()
        self.lua.call("state.restore(second)")
        # With the CPU at maximum speed, the program can be one frame ahead
        # or behind when the frame after the restore has been run.
        self.assertLessEqual(abs(self.counter() - (first + 51)), 1)
        self.lua.call("state.restore(first)")
        self.assertLessEqual(abs(self.counter() - first), 1)

    def test_snapshot_while_running(self) -> None:
        self.lua.call("emu.resume() snapshot = state.snapshot()")
        saved = self.counter()
        self.lua.call("emu.wait_frames(30) state.restore(snapshot)")
        self.assertLess(self.counter(), saved + 10)
        self.assertFalse(self.lua.eval("emu.paused()"))

    def test_restore_needs_a_snapshot(self) -> None:
        with self.assertRaisesRegex(LuaError, "snapshot expected"):
            self.lua.call("state.restore('not a snapshot')")


class MediaTest(EmulatorTestCase):
    test_disk = True

    def test_eject_and_insert(self) -> None:
        path = str(self.emulator.path / "test.adf")
        self.assertEqual(self.lua.eval("media.path(0)"), path)
        self.lua.call("media.eject(0) emu.wait_frames(5)")
        self.assertEqual(self.lua.eval("media.path(0)"), "")
        self.lua.call(f"media.insert(0, '{path}') emu.wait_frames(100)")
        self.assertEqual(self.lua.eval("media.path(0)"), path)

    def test_insert_missing_file(self) -> None:
        with self.assertRaisesRegex(LuaError, "does not exist"):
            self.lua.call("media.insert(0, '/nonexistent/disk.adf')")

    def test_invalid_drive(self) -> None:
        with self.assertRaisesRegex(LuaError, "drive must be 0 to 3"):
            self.lua.call("media.eject(4)")


if __name__ == "__main__":
    unittest.main()

"""Tests for breakpoints, stepping and exception watches (src/lua_debug.cpp)."""

import unittest

import harness
from harness import EmulatorTestCase, LuaError


class DebugTestCase(EmulatorTestCase):
    test_disk = True

    def setUp(self) -> None:
        self.addq = self.program + harness.ADDQ_OFFSET
        self.counter_address = self.program + harness.COUNTER_OFFSET

    def tearDown(self) -> None:
        self.lua.call("dbg.bpclear() mem.tap_remove() emu.resume()")
        self.lua.events.clear()

    def counter(self) -> int:
        return self.lua.eval(f"mem.peek_u32({self.counter_address})")


class BreakpointTest(DebugTestCase):
    def test_breakpoint_stops_before_the_instruction(self) -> None:
        breakpoint_id = self.lua.eval(f"dbg.bpset({self.addq})")
        info = self.lua.eval("dbg.wait()")
        expected = {"reason": "breakpoint", "pc": self.addq, "id": breakpoint_id}
        self.assertEqual(info, expected | {"address": self.addq})
        self.assertEqual(self.lua.eval("cpu.pc"), self.addq)
        # ADDQ.L #1,D0 has not been run yet.
        self.assertEqual(self.lua.eval("cpu.d0"), self.counter())
        self.assertEqual(self.lua.eval("dbg.stopped()"), info)

    def test_stopped_event_is_sent(self) -> None:
        breakpoint_id = self.lua.eval(f"dbg.bpset({self.addq})")
        event = self.lua.wait_event("stopped")
        self.assertEqual(event["reason"], "breakpoint")
        self.assertEqual(event["pc"], self.addq)
        self.assertEqual(event["id"], breakpoint_id)

    def test_go_runs_to_the_breakpoint_again(self) -> None:
        self.lua.call(f"dbg.bpset({self.addq}) dbg.wait()")
        counter = self.counter()
        # The program reaches the breakpoint once per frame.
        for i in range(1, 4):
            info = self.lua.eval("dbg.go() return dbg.wait()")
            self.assertEqual(info["pc"], self.addq)
            self.assertEqual(self.counter(), counter + i)

    def test_breakpoint_callback(self) -> None:
        self.lua.call(
            f"hits = {{}} dbg.bpset({self.addq}, function(address) "
            "hits[#hits + 1] = {address, cpu.pc, cpu.d0} end)"
        )
        self.lua.call("emu.wait_frames(5)")
        hits = self.lua.eval("hits")
        self.assertGreaterEqual(len(hits), 4)
        self.assertFalse(self.lua.eval("dbg.stopped()"))
        for i, (address, pc, d0) in enumerate(hits):
            self.assertEqual((address, pc), (self.addq, self.addq))
            self.assertEqual(d0, hits[0][2] + i)

    def test_callback_can_stop_the_emulation(self) -> None:
        start = self.lua.eval(
            f"local start = cpu.d0 dbg.bpset({self.addq}, function() "
            "if cpu.d0 == start + 3 then emu.pause() end end) return start"
        )
        info = self.lua.eval("dbg.wait()")
        self.assertEqual(info["reason"], "breakpoint")
        self.assertEqual(self.lua.eval("cpu.d0"), start + 3)

    def test_callback_can_change_registers(self) -> None:
        self.lua.call(f"dbg.bpset({self.addq}, function() cpu.d0 = cpu.d0 + 9 end)")
        results = self.lua.call(
            f"local a = mem.peek_u32({self.counter_address}) emu.wait_frames(2) "
            f"return mem.peek_u32({self.counter_address}) - a"
        )
        self.assertEqual(results, [20])

    def test_bplist_and_bpclear(self) -> None:
        first = self.lua.eval("dbg.bpset(0x1000)")
        second = self.lua.eval("dbg.bpset(0x2000, function() end)")
        expected = [{"id": first, "address": 0x1000}, {"id": second, "address": 0x2000}]
        self.assertEqual(self.lua.eval("dbg.bplist()"), expected)
        self.lua.call(f"dbg.bpclear({first})")
        self.assertEqual(self.lua.eval("dbg.bplist()"), expected[1:])
        self.lua.call("dbg.bpclear()")
        self.assertEqual(self.lua.eval("dbg.bplist()"), [])

    def test_two_breakpoints_at_one_address(self) -> None:
        self.lua.call(
            f"calls = 0 first = dbg.bpset({self.addq}, function() calls = calls + 1 end) "
            f"dbg.bpset({self.addq}, function() calls = calls + 100 end)"
        )
        self.lua.call("emu.wait_frames(2) dbg.bpclear(first) calls = 0")
        # The address is still watched after one of the two is removed.
        calls = self.lua.eval("emu.wait_frames(3) return calls")
        self.assertGreaterEqual(calls, 200)
        self.assertEqual(calls % 100, 0)

    def test_many_breakpoints(self) -> None:
        self.lua.call("for i = 1, 2000 do dbg.bpset(0x200000 + i * 2, function() end) end")
        self.lua.call(f"hits = 0 dbg.bpset({self.addq}, function() hits = hits + 1 end)")
        self.assertGreaterEqual(self.lua.eval("emu.wait_frames(5) return hits"), 4)
        self.assertEqual(len(self.lua.eval("dbg.bplist()")), 2001)

    def test_cleared_breakpoint_does_not_stop(self) -> None:
        self.lua.call(f"dbg.bpset({self.addq}) dbg.wait() dbg.bpclear() dbg.go()")
        self.assertIsNone(self.lua.eval("dbg.wait(5)"))

    def test_wait_with_timeout(self) -> None:
        self.assertEqual(
            self.lua.call("local f = emu.frame() dbg.wait(3) return emu.frame() - f"), [3]
        )

    def test_error_in_callback_does_not_stop(self) -> None:
        self.lua.call(f"dbg.bpset({self.addq}, function() error('oops') end)")
        self.lua.call("emu.wait_frames(3)")
        self.assertFalse(self.lua.eval("dbg.stopped()"))


class StepTest(DebugTestCase):
    def test_step_runs_one_instruction(self) -> None:
        self.lua.call(f"dbg.bpset({self.addq}) dbg.wait() dbg.bpclear()")
        counter = self.counter()
        # ADDQ.L #1,D0 and then MOVE.L D0,(A0), both two bytes long.
        info = self.lua.eval("dbg.step()")
        self.assertEqual(info, {"reason": "step", "pc": self.addq + 2})
        self.assertEqual(self.lua.eval("cpu.d0"), counter + 1)
        self.assertEqual(self.counter(), counter)
        info = self.lua.eval("dbg.step()")
        self.assertEqual(info["pc"], self.addq + 4)
        self.assertEqual(self.counter(), counter + 1)

    def test_step_several_instructions(self) -> None:
        self.lua.call(f"dbg.bpset({self.addq}) dbg.wait() dbg.bpclear()")
        # ADDQ, MOVE.L, MOVE.W, MOVE.W, MOVE.W and BRA.
        info = self.lua.eval("dbg.step(6)")
        self.assertEqual(info["pc"], self.program + harness.LOOP_OFFSET)

    def test_step_from_running(self) -> None:
        info = self.lua.eval("dbg.step()")
        self.assertEqual(info["reason"], "step")
        self.assertTrue(self.lua.eval("emu.paused()"))


class TapTest(DebugTestCase):
    def test_taps_are_not_available(self) -> None:
        # Taps need a hook in the CPU emulation, which the vAmiga core does
        # not have.
        with self.assertRaisesRegex(LuaError, "memory taps are not available"):
            self.lua.call("mem.tap_write(0x70000, 0x70003, function() end)")
        with self.assertRaisesRegex(LuaError, "memory taps are not available"):
            self.lua.call("mem.tap_read(0x70000, 0x70003, function() end)")
        self.lua.call("mem.tap_remove()")


class ExceptionTest(DebugTestCase):
    """Tests of dbg.exset, which patch the test program to cause exceptions."""

    ILLEGAL = 0x4AFC
    TRAP_0 = 0x4E40

    def setUp(self) -> None:
        super().setUp()
        self.lua.call("emu.pause() exception_snapshot = state.snapshot()")

    def tearDown(self) -> None:
        self.lua.call("dbg.exclear() state.restore(exception_snapshot)")
        super().tearDown()

    def patch(self, opcode: int) -> None:
        """Replace the ADDQ instruction of the test program."""
        self.lua.call(f"mem.poke_u16({self.addq}, {opcode})")

    def test_illegal_instruction_stops(self) -> None:
        self.patch(self.ILLEGAL)
        watch = self.lua.eval("dbg.exset(4)")
        info = self.lua.eval("dbg.go() return dbg.wait(100)")
        self.assertEqual(info["reason"], "exception")
        self.assertEqual(info["vector"], 4)
        self.assertEqual(info["id"], watch)
        self.assertEqual(info["address"], self.addq)
        # The emulation stops at the first instruction of the handler.
        self.assertEqual(info["pc"], self.lua.eval("mem.peek_u32(cpu.vbr + 4 * 4)"))

    def test_stopped_event_has_the_vector(self) -> None:
        self.patch(self.ILLEGAL)
        self.lua.call("dbg.exset(4) dbg.go()")
        event = self.lua.wait_event("stopped")
        self.assertEqual((event["reason"], event["vector"]), ("exception", 4))

    def test_crash_group(self) -> None:
        self.patch(self.ILLEGAL)
        self.lua.call("dbg.exset('crash')")
        info = self.lua.eval("dbg.go() return dbg.wait(100)")
        self.assertEqual((info["reason"], info["vector"]), ("exception", 4))

    def test_division_by_zero(self) -> None:
        # MOVEQ #0,D1 and DIVU.W D1,D0
        self.lua.call(rf"mem.write_range({self.addq}, '\x72\x00\x80\xc1')")
        self.lua.call("dbg.exset('crash')")
        info = self.lua.eval("dbg.go() return dbg.wait(100)")
        self.assertEqual((info["vector"], info["address"]), (5, self.addq + 2))

    def test_callback(self) -> None:
        self.patch(self.TRAP_0)
        self.lua.call(
            "exceptions = {} dbg.exset(32, function(vector, pc) "
            "exceptions[#exceptions + 1] = {vector, pc} end)"
        )
        self.lua.call("dbg.go() emu.wait_frames(3)")
        self.assertEqual(self.lua.eval("exceptions[1]"), [32, self.addq])
        self.assertFalse(self.lua.eval("dbg.stopped()"))

    def test_callback_can_stop_the_emulation(self) -> None:
        self.patch(self.TRAP_0)
        self.lua.call("dbg.exset(32, function() emu.pause() end)")
        info = self.lua.eval("dbg.go() return dbg.wait(100)")
        self.assertEqual((info["reason"], info["vector"]), ("exception", 32))

    def test_other_vectors_do_not_match(self) -> None:
        # The "crash" group is not used here: on a 68060, the Kickstart code
        # which handles the TRAP runs into a line F exception of its own.
        self.patch(self.TRAP_0)
        self.lua.call("dbg.exset(4) dbg.exset(5) dbg.go()")
        self.assertIsNone(self.lua.eval("dbg.wait(5)"))

    def test_exclear(self) -> None:
        self.patch(self.ILLEGAL)
        self.lua.call("local id = dbg.exset(4) dbg.exclear(id) dbg.go()")
        self.assertIsNone(self.lua.eval("dbg.wait(5)"))

    def test_halt(self) -> None:
        if self.lua.eval("emu.config_get('CPU.REVISION')") != 0:
            self.skipTest("the double fault used here needs a 68000")
        # With an odd supervisor stack pointer, the CPU cannot save anything
        # on the stack, and an exception ends with the CPU halting. In the
        # vAmiga core the illegal instruction exception itself still gets
        # through (a 68000 would fail while saving the PC), and the halt
        # comes with the first instruction of the handler which uses the
        # stack. So the exception can be reported first.
        self.patch(self.ILLEGAL)
        self.lua.call("cpu.isp = 0x1001 dbg.exset('crash')")
        info = self.lua.eval("dbg.go() return dbg.wait(100)")
        if info["reason"] == "exception":
            self.assertEqual(info["vector"], 4)
            info = self.lua.eval("dbg.go() return dbg.wait(100)")
        self.assertEqual(info["reason"], "halt")
        # 2 is the reason "double fault" in the UAE core, and is used here too.
        self.assertEqual(info["vector"], 2)
        # Lua still works while the CPU is halted.
        self.assertEqual(self.lua.call("dbg.go() emu.wait_frames(3) return 1"), [1])

    def test_invalid_vector(self) -> None:
        with self.assertRaisesRegex(LuaError, "must be a vector number"):
            self.lua.call("dbg.exset('nonsense')")
        with self.assertRaisesRegex(LuaError, "must be 2 to 255"):
            self.lua.call("dbg.exset(1000)")


if __name__ == "__main__":
    unittest.main()

"""Tests for symbols of programs loaded by AmigaDOS (src/lua_symbols.cpp).

The tests need vasm, vlink and xdftool to build the program and its disk,
and are skipped if they are not installed.
"""

import unittest

from harness import Emulator, LuaError


class SymbolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.emulator = Emulator(dos_disk=True)
        cls.lua = cls.emulator.lua
        cls.executable = cls.emulator.path / "testprog"
        try:
            # Wait for AmigaDOS to boot and run the program.
            cls.lua.call(
                f"emu.warp(true) program = dbg.load_symbols('{cls.executable}', nil, 5000) "
                "emu.warp(false)"
            )
        except Exception:
            cls.emulator.stop()
            raise

    @classmethod
    def tearDownClass(cls) -> None:
        cls.emulator.stop()

    def tearDown(self) -> None:
        self.lua.call("dbg.bpclear() emu.resume()")
        self.lua.events.clear()

    def test_program(self) -> None:
        program = self.lua.eval("program")
        self.assertEqual(program["name"], "testprog")
        self.assertEqual(program["path"], str(self.executable))
        # A code segment and a data segment, in memory the CPU can run from.
        self.assertEqual(len(program["segments"]), 2)
        code, data = program["segments"]
        self.assertGreaterEqual(code["size"], 68)
        self.assertGreaterEqual(data["size"], 8)
        expected = {"start", "wait", "update", "_store_input", "counter", "joystick", "colour"}
        self.assertEqual(set(program["symbols"]), expected)

    def test_symbols_are_relocated(self) -> None:
        symbols = self.lua.eval("program.symbols")
        code, data = self.lua.eval("program.segments")
        self.assertEqual(symbols["start"], code["address"])
        self.assertEqual(symbols["counter"], data["address"])
        self.assertEqual(symbols["joystick"], data["address"] + 4)
        # The program is running its loop, which starts at wait.
        self.lua.call("emu.pause()")
        pc = self.lua.eval("cpu.pc")
        self.assertGreaterEqual(pc, symbols["wait"])
        self.assertLess(pc, code["address"] + code["size"])
        # LEA counter,A0 was relocated by AmigaDOS to the same address.
        self.assertEqual(self.lua.eval("cpu.a0"), symbols["counter"])

    def test_data_symbol(self) -> None:
        results = self.lua.call(
            "local a = mem.peek_u32(program.symbols.counter) emu.wait_frames(10) "
            "return mem.peek_u32(program.symbols.counter) - a"
        )
        self.assertEqual(results, [10])

    def test_symbol_function(self) -> None:
        self.assertEqual(
            self.lua.eval("dbg.symbol('update')"), self.lua.eval("program.symbols.update")
        )
        self.assertIsNone(self.lua.eval("dbg.symbol('no_such_symbol')"))

    def test_underscore_is_optional(self) -> None:
        address = self.lua.eval("program.symbols._store_input")
        self.assertEqual(self.lua.eval("program.symbols.store_input"), address)
        self.assertEqual(self.lua.eval("dbg.symbol('store_input')"), address)

    def test_lookup(self) -> None:
        update = self.lua.eval("program.symbols.update")
        self.assertEqual(self.lua.eval(f"dbg.lookup({update})"), "update")
        self.assertEqual(self.lua.eval(f"dbg.lookup({update + 2})"), "update+$2")
        self.assertEqual(self.lua.eval("dbg.lookup(program.symbols.colour + 1)"), "colour+$1")
        # Addresses outside the program have no name.
        self.assertIsNone(self.lua.eval("dbg.lookup(0xf80000)"))
        self.assertIsNone(self.lua.eval("dbg.lookup(program.symbols.start - 2)"))
        code = self.lua.eval("program.segments[1]")
        self.assertIsNone(self.lua.eval(f"dbg.lookup({code['address'] + code['size']})"))

    def test_breakpoint_by_name(self) -> None:
        update = self.lua.eval("program.symbols.update")
        breakpoint_id = self.lua.eval("dbg.bpset('update')")
        info = self.lua.eval("dbg.wait(100)")
        self.assertEqual(info["reason"], "breakpoint")
        self.assertEqual(info["id"], breakpoint_id)
        self.assertEqual(info["pc"], update)
        self.assertEqual(info["symbol"], "update")
        self.assertEqual(info["address_symbol"], "update")
        # ADDQ.L #1,D0 and MOVE.L D0,(A0) are two bytes each.
        info = self.lua.eval("dbg.bpclear() return dbg.step(2)")
        self.assertEqual(info["symbol"], "_store_input")

    def test_breakpoint_on_unknown_name(self) -> None:
        with self.assertRaisesRegex(LuaError, "unknown symbol 'nothing'"):
            self.lua.call("dbg.bpset('nothing')")

    def test_stopped_event_has_the_symbol(self) -> None:
        self.lua.call("dbg.bpset('update')")
        self.assertEqual(self.lua.wait_event("stopped")["symbol"], "update")

    def test_exception_reports_the_symbol(self) -> None:
        self.lua.call("emu.pause() snapshot = state.snapshot()")
        try:
            # Replace ADDQ.L #1,D0 with an illegal instruction.
            self.lua.call(
                "mem.poke_u16(program.symbols.update, 0x4afc) dbg.exset('crash') dbg.go()"
            )
            info = self.lua.eval("dbg.wait(100)")
            self.assertEqual(info["reason"], "exception")
            self.assertEqual(info["address_symbol"], "update")
            self.assertNotIn("symbol", info)
        finally:
            self.lua.call("dbg.exclear() state.restore(snapshot)")

    def test_measure_subroutine(self) -> None:
        # Without an end address, the time until the subroutine returns is
        # measured. "update" is five instructions and the RTS.
        result = self.lua.eval("dbg.measure('update', nil, 5)")
        self.assertEqual(result["count"], 5)
        # With the CPU at maximum speed (A3000, A4000), the instructions take
        # next to no emulated time.
        self.assertTrue(all(0 <= sample < 300 for sample in result["samples"]))
        if self.lua.eval("emu.config_get('CPU.REVISION')") == 0:
            self.assertTrue(all(sample > 10 for sample in result["samples"]))
        self.assertEqual(self.lua.eval("dbg.bplist()"), [])

    def test_program_not_running(self) -> None:
        with self.assertRaisesRegex(LuaError, "the program 'other' is not running"):
            self.lua.call(f"dbg.load_symbols('{self.executable}', 'other')")

    def test_stripped_executable(self) -> None:
        with self.assertRaisesRegex(LuaError, "has no symbols"):
            self.lua.call(f"dbg.load_symbols('{self.executable}.stripped', 'testprog')")

    def test_not_an_executable(self) -> None:
        with self.assertRaisesRegex(LuaError, "is not an Amiga executable"):
            self.lua.call(f"dbg.load_symbols('{self.emulator.path}/startup-sequence', 'testprog')")
        with self.assertRaisesRegex(LuaError, "cannot open"):
            self.lua.call("dbg.load_symbols('/nonexistent/program', 'testprog')")

    def test_unload_symbols(self) -> None:
        self.lua.call("dbg.unload_symbols('testprog')")
        try:
            self.assertIsNone(self.lua.eval("dbg.symbol('update')"))
            self.assertIsNone(self.lua.eval("dbg.lookup(program.symbols.update)"))
        finally:
            self.lua.call(f"program = dbg.load_symbols('{self.executable}')")
        self.assertIsNotNone(self.lua.eval("dbg.symbol('update')"))


if __name__ == "__main__":
    unittest.main()

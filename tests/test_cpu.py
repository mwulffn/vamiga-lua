"""Tests for the cpu table (src/lua_cpu.cpp)."""

import unittest

import harness
from harness import EmulatorTestCase, LuaError


class CpuTest(EmulatorTestCase):
    test_disk = True

    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        cls.lua.call("emu.pause()")

    def test_pc_is_in_the_test_program(self) -> None:
        pc = self.lua.eval("cpu.pc")
        self.assertGreaterEqual(pc, self.program + harness.LOOP_OFFSET)
        self.assertLess(pc, self.program + harness.DATA_OFFSET)

    def test_registers_set_by_the_test_program(self) -> None:
        self.assertEqual(self.lua.eval("cpu.a5"), 0xDFF000)
        self.assertEqual(self.lua.eval("cpu.a0"), self.program + harness.DATA_OFFSET)
        counter = self.lua.eval(f"mem.peek_u32({self.program + harness.COUNTER_OFFSET})")
        self.assertEqual(self.lua.eval("cpu.d0"), counter)

    def test_write_data_register(self) -> None:
        # The program stores d0 in the frame counter once per frame.
        self.lua.call("cpu.d0 = 0x1000 emu.step()")
        counter = self.lua.eval(f"mem.peek_u32({self.program + harness.COUNTER_OFFSET})")
        self.assertEqual(counter, 0x1001)

    def test_write_address_register(self) -> None:
        old = self.lua.eval("cpu.a3")
        self.lua.call("cpu.a3 = 0xcafe0000")
        self.assertEqual(self.lua.eval("cpu.a3"), 0xCAFE0000)
        self.lua.call(f"cpu.a3 = {old}")

    def test_status_register(self) -> None:
        sr = self.lua.eval("cpu.sr")
        self.assertEqual(sr & ~0xFFFF, 0)
        self.lua.call(f"cpu.sr = {sr | 0x10}")
        self.assertEqual(self.lua.eval("cpu.sr") & 0x1F, (sr | 0x10) & 0x1F)
        self.lua.call(f"cpu.sr = {sr}")

    def test_stack_pointers(self) -> None:
        supervisor = bool(self.lua.eval("cpu.sr") & 0x2000)
        active = "isp" if supervisor else "usp"
        self.assertEqual(self.lua.eval(f"cpu.{active}"), self.lua.eval("cpu.a7"))

    def test_write_pc(self) -> None:
        # Jump to the instruction which adds 1 to the counter, and run it and
        # the one which stores the counter.
        address = self.program + harness.COUNTER_OFFSET
        self.lua.call("emu.step()")
        before = self.lua.eval("cpu.d0")
        self.lua.call(f"cpu.pc = {self.program + harness.ADDQ_OFFSET} dbg.step(2)")
        self.assertEqual(self.lua.eval("cpu.pc"), self.program + harness.ADDQ_OFFSET + 4)
        self.assertEqual(self.lua.eval(f"mem.peek_u32({address})"), before + 1)

    def test_disasm(self) -> None:
        lines = self.lua.eval(f"cpu.disasm({self.program + harness.ADDQ_OFFSET}, 2)")
        self.assertEqual(lines[0]["address"], self.program + harness.ADDQ_OFFSET)
        self.assertEqual(lines[0]["size"], 2)
        self.assertRegex(lines[0]["text"].upper(), r"ADDQ?\.L\s+#\$0?1,\s*D0")
        self.assertEqual(lines[1]["address"], self.program + harness.ADDQ_OFFSET + 2)
        self.assertRegex(lines[1]["text"].upper(), r"MOVE\.L\s+D0,\s*\(A0\)")

    def test_unknown_register(self) -> None:
        with self.assertRaisesRegex(LuaError, "unknown CPU register"):
            self.lua.call("return cpu.d8")
        with self.assertRaisesRegex(LuaError, "unknown CPU register"):
            self.lua.call("cpu.x = 1")


if __name__ == "__main__":
    unittest.main()

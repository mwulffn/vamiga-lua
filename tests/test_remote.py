"""Tests for the socket protocol (src/remote.cpp)."""

import json
import socket
import unittest

from harness import EmulatorTestCase, LuaClient, LuaError, to_bytes


class RemoteTest(EmulatorTestCase):
    def test_expression(self) -> None:
        self.assertEqual(self.lua.call("1 + 1"), [2])

    def test_statements(self) -> None:
        self.assertEqual(self.lua.call("local a = 20 return a * 2, 'x'"), [40, "x"])

    def test_no_results(self) -> None:
        self.assertEqual(self.lua.call("local a = 1"), [])

    def test_value_types(self) -> None:
        results = self.lua.call("return nil, true, 1.5, {1, 2, {a = 'b'}}, {}, print")
        self.assertEqual(results[:5], [None, True, 1.5, [1, 2, {"a": "b"}], []])
        self.assertTrue(results[5].startswith("function"))

    def test_binary_string(self) -> None:
        value = self.lua.eval(r"return '\0\1\127\128\255\"\\\n'")
        self.assertEqual(to_bytes(value), b'\x00\x01\x7f\x80\xff"\\\n')

    def test_unicode_in_request(self) -> None:
        self.assertEqual(to_bytes(self.lua.eval("return 'æ€😀'")), "æ€😀".encode())

    def test_print_is_returned_as_output(self) -> None:
        reply = self.lua.request("print('a', 1) print('b') return 3")
        self.assertEqual(reply["output"], "a\t1\nb\n")
        self.assertEqual(reply["results"], [3])

    def test_globals_are_kept_between_requests(self) -> None:
        self.lua.call("test_global = 123")
        self.assertEqual(self.lua.eval("test_global"), 123)

    def test_syntax_error(self) -> None:
        with self.assertRaisesRegex(LuaError, "unexpected symbol"):
            self.lua.call("x = = 1")

    def test_runtime_error(self) -> None:
        with self.assertRaisesRegex(LuaError, "boom") as context:
            self.lua.call("print('before') error('boom')")
        self.assertEqual(context.exception.output, "before\n")

    def test_request_waiting_for_frames(self) -> None:
        results = self.lua.call("local f = emu.frame() emu.wait_frames(3) return emu.frame() - f")
        self.assertEqual(results, [3])

    def test_invalid_request(self) -> None:
        with socket.create_connection(("127.0.0.1", self.emulator.port), timeout=10) as s:
            reader = s.makefile("r")
            s.sendall(b"not json\n")
            reply = json.loads(reader.readline())
            self.assertEqual((reply["id"], reply["ok"]), (None, False))
            s.sendall(b'{"id": "abc"}\n')
            reply = json.loads(reader.readline())
            self.assertEqual((reply["id"], reply["ok"]), ("abc", False))
            s.sendall(b'{"id": "abc", "code": "2"}\n')
            self.assertEqual(json.loads(reader.readline())["results"], [2])

    def test_two_clients(self) -> None:
        with LuaClient(self.emulator.port) as other:
            self.assertEqual(other.eval("5"), 5)
            self.assertEqual(self.lua.eval("6"), 6)

    def test_client_disconnecting_before_the_reply(self) -> None:
        with socket.create_connection(("127.0.0.1", self.emulator.port), timeout=10) as s:
            s.sendall(
                b'{"id": 1, "code": "emu.wait_frames(10) return string.rep(\'x\', 100000)"}\n'
            )
        self.assertEqual(self.lua.call("emu.wait_frames(20) return 1"), [1])

    def test_print_outside_request_is_sent_as_event(self) -> None:
        self.lua.call(
            "local id id = emu.on_frame(function() "
            "print('from callback') emu.remove_frame_callback(id) end)"
        )
        self.assertEqual(self.lua.wait_event("print")["text"], "from callback")


if __name__ == "__main__":
    unittest.main()

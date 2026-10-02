#!/usr/bin/env python3
"""Run Lua code in a running vamiga-lua (started with --port).

Usage: amiga_lua.py [--port PORT] [--json] 'return cpu.pc'
       amiga_lua.py [--port PORT] [--json] --file script.lua

The results are printed one per line, after anything the code printed. With
--json the reply from vamiga-lua is printed as it is. The exit status is 1 if
the Lua code failed.
"""

import argparse
import json
import socket
import sys
from typing import Any

DEFAULT_PORT = 5600


class LuaError(Exception):
    """The Lua code failed to compile or raised an error."""

    def __init__(self, message: str, output: str = "") -> None:
        super().__init__(message)
        self.output = output


class LuaClient:
    """A connection to the Lua engine in vamiga-lua."""

    def __init__(self, port: int = DEFAULT_PORT, timeout: float | None = 30.0) -> None:
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.reader = self.socket.makefile("r", encoding="utf-8", newline="\n")
        self.next_id = 1
        # Events received while waiting for replies, oldest first.
        self.events: list[dict[str, Any]] = []

    def close(self) -> None:
        self.reader.close()
        self.socket.close()

    def __enter__(self) -> "LuaClient":
        return self

    def __exit__(self, *args: object) -> None:
        self.close()

    def read_message(self) -> dict[str, Any]:
        line = self.reader.readline()
        if not line:
            raise ConnectionError("The emulator closed the connection")
        return json.loads(line)

    def request(self, code: str) -> dict[str, Any]:
        """Run code and return the reply as it was received."""
        request_id = self.next_id
        self.next_id += 1
        message = json.dumps({"id": request_id, "code": code})
        self.socket.sendall(message.encode("utf-8") + b"\n")
        while True:
            reply = self.read_message()
            if "event" in reply:
                self.events.append(reply)
            elif reply.get("id") == request_id:
                return reply

    def call(self, code: str) -> list[Any]:
        """Run code and return the list of values it returned."""
        reply = self.request(code)
        if not reply["ok"]:
            raise LuaError(reply["error"], reply["output"])
        return reply["results"]

    def eval(self, code: str) -> Any:
        """Run code and return the first value it returned (or None)."""
        results = self.call(code)
        return results[0] if results else None

    def wait_event(self, name: str) -> dict[str, Any]:
        """Return the oldest event with the given name, waiting if needed."""
        while True:
            for i, event in enumerate(self.events):
                if event["event"] == name:
                    return self.events.pop(i)
            message = self.read_message()
            if "event" in message:
                self.events.append(message)


def to_bytes(value: str) -> bytes:
    """Convert a string received from Lua back to the bytes it was in Lua."""
    return value.encode("latin-1")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Lua code in a running vamiga-lua")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--file", help="read the Lua code from this file")
    parser.add_argument("--json", action="store_true", help="print the reply as JSON")
    parser.add_argument("--timeout", type=float, default=30.0, help="seconds, 0 for none")
    parser.add_argument("code", nargs="?")
    args = parser.parse_args()
    if (args.code is None) == (args.file is None):
        parser.error("give either Lua code or --file")
    code = args.code
    if args.file is not None:
        with open(args.file, encoding="utf-8") as f:
            code = f.read()

    with LuaClient(args.port, args.timeout or None) as client:
        reply = client.request(code)
    if args.json:
        print(json.dumps(reply))
        return 0 if reply["ok"] else 1
    sys.stdout.write(reply["output"])
    if not reply["ok"]:
        print(reply["error"], file=sys.stderr)
        return 1
    for result in reply["results"]:
        print(result if isinstance(result, str) else json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())

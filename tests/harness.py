"""Starts vamiga-lua with the Lua socket enabled, for the tests in this directory.

The tests need a Kickstart ROM, given with the environment variable
AMIGA_TEST_KICKSTART. The matching Amiga model (see MODELS) is given with
AMIGA_TEST_MODEL, and defaults to A500. AMIGA_TEST_BINARY overrides the path
to the vamiga-lua executable, which defaults to build/vamiga-lua.
AMIGA_TEST_EXT is an extension ROM, which the free AROS ROM needs.
AMIGA_TEST_OPTIONS adds options of the vAmiga core to all tests, for example
"MEM.SLOW_RAM=0,CPU.REVISION=68010".
"""

import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT_DIR / "scripts"))

from amiga_lua import LuaClient, LuaError, to_bytes  # noqa: E402, F401

# The models which AMIGA_TEST_MODEL can select.
MODELS = ("A1000", "A500", "A500-ECS", "A500+", "A1200")

# A boot block program which takes over the machine and, once per frame:
# - adds 1 to the long word at DATA_OFFSET (the frame counter)
# - copies JOY1DAT to the word at DATA_OFFSET + 4
# - copies the word at DATA_OFFSET + 6 to COLOR00 (the colour of the screen)
# It waits for the vertical blank bit in INTREQR, which is set once per
# frame also when the CPU runs at maximum speed (waiting for a beam position
# is not reliable then). The offsets are relative to the start of the code
# (see find_test_program).
TEST_PROGRAM = bytes.fromhex(
    "4bf900dff000"  # 00 lea $dff000,a5
    "3b7c7fff009a"  # 06 move.w #$7fff,$9a(a5)   (INTENA: interrupts off)
    "3b7c7fff0096"  # 0c move.w #$7fff,$96(a5)   (DMACON: DMA off)
    "41fa0028"  # 12 lea data(pc),a0
    "7000"  # 16 moveq #0,d0
    "322d001e"  # 18 wait: move.w $1e(a5),d1    (INTREQR)
    "08010005"  # 1c btst #5,d1               (vertical blank)
    "67f6"  # 20 beq.s wait
    "3b7c0020009c"  # 22 move.w #$0020,$9c(a5)   (INTREQ: clear the bit)
    "5280"  # 28 addq.l #1,d0
    "2080"  # 2a move.l d0,(a0)
    "342d000c"  # 2c move.w $c(a5),d2          (JOY1DAT)
    "31420004"  # 30 move.w d2,4(a0)
    "3b6800060180"  # 34 move.w 6(a0),$180(a5)   (COLOR00)
    "60dc"  # 3a bra.s wait
    "00000000"  # 3c data: frame counter
    "0000"  # 40 JOY1DAT copy
    "0f00"  # 42 colour (red)
)
LOOP_OFFSET = 0x18
ADDQ_OFFSET = 0x28
DATA_OFFSET = 0x3C
COUNTER_OFFSET = DATA_OFFSET
JOYSTICK_OFFSET = DATA_OFFSET + 4
COLOUR_OFFSET = DATA_OFFSET + 6


def boot_block_checksum(block: bytes) -> int:
    total = 0
    for (value,) in struct.iter_unpack(">I", block):
        total += value
        if total > 0xFFFFFFFF:
            total = (total + 1) & 0xFFFFFFFF
    return ~total & 0xFFFFFFFF


def create_test_disk(path: Path) -> None:
    """Write an ADF disk image which boots TEST_PROGRAM."""
    block = bytearray(1024)
    block[0:4] = b"DOS\0"
    block[8:12] = struct.pack(">I", 880)
    block[12 : 12 + len(TEST_PROGRAM)] = TEST_PROGRAM
    block[4:8] = struct.pack(">I", boot_block_checksum(bytes(block)))
    path.write_bytes(bytes(block) + bytes(901120 - 1024))


def run_tool(*arguments: str | Path) -> None:
    """Run one of the Amiga development tools, which must be installed."""
    if shutil.which(str(arguments[0])) is None:
        raise unittest.SkipTest(f"{arguments[0]} is not installed")
    subprocess.run([str(argument) for argument in arguments], check=True, capture_output=True)


def build_dos_disk(directory: Path) -> Path:
    """Build testprog.s and a floppy which runs it from the startup-sequence.

    This uses vasm and vlink to build the program, and xdftool (from
    amitools) to create the disk image. The files are written to directory:
    testprog (the executable with symbols), testprog.stripped (without)
    and dos.adf. Returns the path of the disk image.
    """
    source = Path(__file__).with_name("testprog.s")
    run_tool("vasmm68k_mot", "-quiet", "-Fhunk", "-o", directory / "testprog.o", source)
    run_tool("vlink", "-bamigahunk", "-o", directory / "testprog", directory / "testprog.o")
    run_tool(
        "vlink",
        "-bamigahunk",
        "-s",
        "-o",
        directory / "testprog.stripped",
        directory / "testprog.o",
    )
    (directory / "startup-sequence").write_text("testprog\n")
    disk = directory / "dos.adf"
    run_tool(
        *("xdftool", disk, "create", "+", "format", "Test", "+", "boot", "install"),
        *("+", "makedir", "s"),
        *("+", "write", directory / "startup-sequence", "s/startup-sequence"),
        *("+", "write", directory / "testprog"),
    )
    return disk


def build_program(directory: Path, name: str) -> Path:
    """Build <name>.s in this directory with vasm and vlink. Return the executable."""
    source = Path(__file__).with_name(name + ".s")
    run_tool("vasmm68k_mot", "-quiet", "-Fhunk", "-o", directory / (name + ".o"), source)
    run_tool("vlink", "-bamigahunk", "-o", directory / name, directory / (name + ".o"))
    return directory / name


def build_directory_drive(directory: Path, name: str) -> Path:
    """Create a directory to use as a hard drive, which starts the program.

    The directory holds the program built from <name>.s and a
    startup-sequence which runs it. Returns the path of the directory.
    """
    drive = directory / "drive"
    (drive / "s").mkdir(parents=True)
    shutil.copy(build_program(directory, name), drive / name)
    (drive / "s" / "startup-sequence").write_text(name + "\n")
    return drive


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Emulator:
    """A running vamiga-lua with a connected LuaClient in self.lua."""

    def __init__(
        self,
        options: dict[str, str] | None = None,
        test_disk: bool = False,
        dos_disk: bool = False,
        arguments: list[str] | None = None,
    ) -> None:
        """Start the emulator.

        The options are options of the vAmiga core ("MEM.SLOW_RAM": "0"),
        or "floppy0" to "floppy3" with the path of a disk image. The
        arguments are added to the command line as they are.
        """
        kickstart = os.environ.get("AMIGA_TEST_KICKSTART")
        if not kickstart:
            raise unittest.SkipTest("AMIGA_TEST_KICKSTART is not set")
        binary = os.environ.get("AMIGA_TEST_BINARY", str(ROOT_DIR / "build" / "vamiga-lua"))
        model = os.environ.get("AMIGA_TEST_MODEL", "A500")
        if model not in MODELS:
            raise unittest.SkipTest(f"The model {model} is not supported")

        self.directory = tempfile.TemporaryDirectory(prefix="vamiga-lua-test-")
        self.path = Path(self.directory.name)
        self.port = free_port()
        config: dict[str, str] = {}
        for option in os.environ.get("AMIGA_TEST_OPTIONS", "").split(","):
            if option:
                key, value = option.split("=", 1)
                config[key] = value
        if test_disk:
            create_test_disk(self.path / "test.adf")
            config["floppy0"] = str(self.path / "test.adf")
        if dos_disk:
            try:
                config["floppy0"] = str(build_dos_disk(self.path))
            except Exception:
                self.directory.cleanup()
                raise
        config.update(options or {})
        command = [binary, "--model", model, "--rom", kickstart, "--port", str(self.port)]
        if os.environ.get("AMIGA_TEST_EXT"):
            command += ["--ext", os.environ["AMIGA_TEST_EXT"]]
        for key, value in config.items():
            if key.startswith("floppy"):
                command += [f"--df{key[6:]}", value]
            else:
                command += ["--set", f"{key}={value}"]
        command += arguments or []

        self.lua: LuaClient | None = None
        self.log = open(self.path / "log.txt", "w")
        self.process = subprocess.Popen(command, stdout=self.log, stderr=subprocess.STDOUT)
        try:
            self.lua = self.connect()
        except Exception:
            self.stop()
            raise

    def connect(self) -> LuaClient:
        deadline = time.monotonic() + 30
        while True:
            try:
                return LuaClient(self.port)
            except ConnectionRefusedError:
                if self.process.poll() is not None:
                    raise RuntimeError("vamiga-lua exited while starting") from None
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.1)

    def stop(self) -> None:
        if self.lua is not None:
            self.lua.close()
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.log.close()
        self.directory.cleanup()

    def find_test_program(self) -> int:
        """Wait until the test disk has booted and return the address of TEST_PROGRAM."""
        self.lua.call("emu.warp(true)")
        try:
            # The program is running when the PC is inside a copy of it.
            code = "".join(f"\\x{byte:02x}" for byte in TEST_PROGRAM[:DATA_OFFSET])
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                address = self.lua.eval(
                    f"local start = cpu.pc - {DATA_OFFSET} "
                    f"local s = mem.read_range(start, {2 * DATA_OFFSET}):find('{code}', 1, true) "
                    f"return s and start + s - 1"
                )
                if address is not None:
                    return address
                self.lua.call("emu.wait_frames(25)")
            raise RuntimeError("The test disk did not boot")
        finally:
            self.lua.call("emu.warp(false)")


class EmulatorTestCase(unittest.TestCase):
    """Base class for tests which share one emulator per test class."""

    options: dict[str, str] = {}
    # Boot the test disk, and set program to the address of TEST_PROGRAM.
    test_disk = False
    emulator: Emulator
    lua: LuaClient
    program: int

    @classmethod
    def setUpClass(cls) -> None:
        cls.emulator = Emulator(cls.options, cls.test_disk)
        cls.lua = cls.emulator.lua
        if cls.test_disk:
            try:
                cls.program = cls.emulator.find_test_program()
            except Exception:
                cls.emulator.stop()
                raise

    @classmethod
    def tearDownClass(cls) -> None:
        cls.emulator.stop()

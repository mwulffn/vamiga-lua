"""Tests which boot a publicly available disk image.

The disk is the Amiga floppy version of EmuTOS (https://emutos.sourceforge.io),
a free operating system released under the GPL. It is downloaded the first
time the tests are run, and the tests are skipped if that is not possible.
"""

import hashlib
import io
import tempfile
import unittest
import urllib.request
import zipfile
from pathlib import Path

from harness import Emulator, to_bytes

EMUTOS_URL = (
    "https://sourceforge.net/projects/emutos/files/emutos/1.3/emutos-amiga-floppy-1.3.zip/download"
)
EMUTOS_SHA256 = "99c3166d9353e05939b5ddc5ffa3b6007253311cc1d05b66ce601bfb03fde344"
EMUTOS_ADF = "emutos-amiga-floppy-1.3/emutos.adf"

# The frame is two lines lower after a state has been restored (574 instead
# of 576 lines), so only the lines above that are compared.
COMPARED_LINES = 560

# The EmuTOS desktop has a dithered background, so a line across the screen
# changes between black and white about 580 times. The screen with the
# version, which is shown for a moment while EmuTOS starts, has text on the
# line, which gives about 220 changes.
DESKTOP_VISIBLE = """
local changes, last = 0, nil
for x = 100, 700 do
    local red = video.pixel(x, 300)
    if red ~= last then
        changes = changes + 1
        last = red
    end
end
return changes > 400
"""


def emutos_disk() -> Path:
    """Return the path to the EmuTOS disk image, downloading it if needed."""
    path = Path(tempfile.gettempdir()) / "amiga-lua-tests" / "emutos-1.3.adf"
    if path.exists():
        return path
    try:
        with urllib.request.urlopen(EMUTOS_URL, timeout=60) as response:
            data = response.read()
    except OSError as e:
        raise unittest.SkipTest(f"Could not download EmuTOS: {e}") from None
    if hashlib.sha256(data).hexdigest() != EMUTOS_SHA256:
        raise unittest.SkipTest("The EmuTOS download has an unexpected checksum")
    path.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        path.write_bytes(archive.read(EMUTOS_ADF))
    return path


class EmuTosTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.emulator = Emulator({"floppy0": str(emutos_disk())})
        cls.lua = cls.emulator.lua
        try:
            cls.lua.call("emu.warp(true)")
            for _ in range(60):
                cls.lua.call("emu.wait_frames(100)")
                if cls.lua.eval(DESKTOP_VISIBLE):
                    break
            else:
                raise RuntimeError("EmuTOS did not boot")
            cls.lua.call("emu.warp(false) input.port_mode(0, 'mouse') emu.wait_frames(50)")
            # The desktop is drawn in several steps, which takes a while on
            # the slower models. Wait until the screen has stopped changing.
            previous = None
            for _ in range(60):
                screen = cls.lua.eval("video.pixels()")
                if screen == previous:
                    break
                previous = screen
                cls.lua.call("emu.wait_frames(50)")
            else:
                raise RuntimeError("The EmuTOS desktop did not finish drawing")
        except Exception:
            cls.emulator.stop()
            raise

    @classmethod
    def tearDownClass(cls) -> None:
        cls.emulator.stop()

    def screen(self) -> bytes:
        """Return the pixels of the first COMPARED_LINES lines of the screen."""
        pixels, width, height = self.lua.call("return video.pixels()")
        return to_bytes(pixels)[: width * COMPARED_LINES * 3]

    def test_screen_is_black_and_white(self) -> None:
        screen = self.screen()
        white = screen.count(b"\xff\xff\xff")
        self.assertGreater(white, len(screen) // 3 // 4)
        self.assertGreater(screen.count(b"\x00\x00\x00"), len(screen) // 3 // 8)

    def test_screen_is_stable(self) -> None:
        first = self.screen()
        self.lua.call("emu.wait_frames(20)")
        self.assertEqual(self.screen(), first)

    def test_moving_the_mouse_changes_the_screen(self) -> None:
        before = self.screen()
        self.lua.call("input.mouse(60, 40) emu.wait_frames(20)")
        self.assertNotEqual(self.screen(), before)

    def test_snapshot_restores_the_screen(self) -> None:
        self.lua.call("emu.wait_frames(10) snapshot = state.snapshot() emu.wait_frames(5)")
        before = self.screen()
        self.lua.call("input.mouse(-50, -30) emu.wait_frames(20)")
        self.assertNotEqual(self.screen(), before)
        self.lua.call("state.restore(snapshot) emu.wait_frames(5)")
        self.assertEqual(self.screen(), before)

    def test_no_crash_exceptions_while_running(self) -> None:
        # Interrupts and the traps the operating system uses are exceptions
        # too, but they are not in the "crash" group.
        self.lua.call("dbg.exset('crash') input.mouse(5, 5)")
        try:
            self.assertIsNone(self.lua.eval("dbg.wait(100)"))
        finally:
            self.lua.call("dbg.exclear() emu.resume()")

    def test_program_counter_is_in_ram(self) -> None:
        # EmuTOS runs from RAM, not from the Kickstart ROM.
        self.assertLess(self.lua.eval("cpu.pc"), 0x200000)


if __name__ == "__main__":
    unittest.main()

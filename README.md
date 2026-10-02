# vamiga-lua

The [vAmiga](https://github.com/dirkwhoffmann/vAmiga) Amiga emulator core with a Lua scripting
engine and a control port, and nothing else. There is no window and no user interface. A program
(or an agent) starts it, sends Lua code over a local socket, and reads memory, registers and the
picture back.

It is a host around the core in the Unix sense: the core emulates, this program gives it a way to
be driven, and anything which needs a window or a GUI is a different program.

The Lua API and the socket protocol are the ones of the Lua engine in the
[FS-UAE fork](https://github.com/mwulffn/fs-uae) this grew out of, so scripts and tests can be
run against either emulator.

## Status

Stages 1 and 2 of 3 are done. What is here:

| Table | Functions |
| --- | --- |
| `emu` | `frame`, `cycles`, `beam`, `timing`, `wait_frames`, `wait_next_frame`, `on_frame`, `remove_frame_callback`, `pause`, `resume`, `paused`, `step`, `warp`, `reset`, `quit`, `config_get`, `config_set`, `log` |
| `cpu` | The registers as fields (`cpu.d0`, `cpu.a7`, `cpu.pc`, `cpu.sr`, ...), `disasm` |
| `mem` | `read_u8/u16/u32`, `write_u8/u16/u32`, `peek_u8/u16/u32`, `poke_u8/u16/u32`, `read_range`, `write_range`, `custom`, `tap_read`, `tap_write`, `tap_remove` |
| `video` | `size`, `pixel`, `pixels`, `screenshot` |
| `state` | `save`, `load`, `snapshot`, `restore` |
| `media` | `insert`, `eject`, `path` |
| `input` | `key`, `joy`, `mouse`, `mouse_button`, `port_mode`, `type` |
| `dbg` | `bpset`, `bpclear`, `bplist`, `exset`, `exclear`, `step`, `go`, `wait`, `stopped`, `measure`, `load_symbols`, `unload_symbols`, `symbol`, `lookup` |

Not here: `dbg.command`, which runs a command of the UAE debugger in FS-UAE.

## Building

Needs CMake, a C++20 compiler and zlib. The core is a git submodule.

```sh
git submodule update --init
cmake -B build
cmake --build build -j
```

The core comes from a fork of the [Silicium](https://github.com/dirkwhoffmann/silicium)
repository, which is where its author develops it now. The fork
([mwulffn/silicium](https://github.com/mwulffn/silicium), branch `vamiga-lua`) adds one small
hook for the memory taps; see `docs/upstream.md`. Only `Cores/` is built; Qt is not needed.
Lua 5.4 is in `extern/lua`.

## Running

```sh
build/vamiga-lua --model A500 --rom kick13.rom --df0 game.adf --port 5600 --warp
scripts/amiga_lua.py --port 5600 'emu.wait_frames(500) return cpu.pc, video.screenshot("/tmp/shot.png")'
```

| Option | Meaning |
| --- | --- |
| `--model <name>` | `A1000`, `A500` (default), `A500-ECS`, `A500+` or `A1200` |
| `--rom <file>` | Kickstart ROM (required) |
| `--df0 <file>` to `--df3` | Disk images (ADF, DMS and the other formats the core reads) |
| `--set <option>=<value>` | An option of the vAmiga core, for example `MEM.SLOW_RAM=512` |
| `--lua <file>` | Run a Lua script at start; can be given several times |
| `--port <number>` | Listen on 127.0.0.1 for Lua code |
| `--warp` | Run as fast as possible from the start |

Lua code is not sandboxed. Anything which can connect to the port can run code with the access
of the process.

The socket protocol is one line of JSON per request and reply:

```
-> {"id": 7, "code": "return cpu.pc, mem.read_u16(0xdff004)"}
<- {"id": 7, "ok": true, "results": [16515298, 8236], "output": ""}
```

## How it works

The core runs the emulation on its own thread. This program keeps the emulator paused, and lets
it run to the end of the current frame when a frame is wanted. All Lua code runs on the main
thread while the emulator is paused, so it always sees the machine between two instructions and
between two frames. Two runs of the same script give the same result, frame for frame.

States are taken and loaded at once, at that same point. In 1,600 snapshot and restore cycles
with a program reading a file in a loop (A500 and A1200 configurations) none failed.

Breakpoints and exception watches use the breakpoints and catchpoints of the core, which pause
the emulator before the instruction at the address (or before the first instruction of the
exception handler). A Lua callback is run on the main thread at that point, and the emulation is
then continued. A callback costs about 13 microseconds.

Memory taps are different: the core calls a function for every data access of the CPU, in the
middle of the instruction, and the Lua callback is run there, on the emulator thread, while the
main thread waits. It can give the CPU another value. A tap callback costs about 5 microseconds,
and cannot use the functions which need the emulator to be paused (`state`, `media`, `input`).

In warp mode, with every frame drawn, a demo runs at about 720 frames per second on an Apple
Silicon Mac.

## Differences from the FS-UAE engine

- No `dbg.command`.
- Memory taps do not report instruction fetches or reads with PC-relative addressing, and a tap
  callback cannot save or load states, change disks or send input.
- The exception watches (`dbg.exset`) give the address of the instruction which caused the
  exception. The core does not keep it, so it is worked out from what the CPU saved on the stack.
  For a division by zero or CHK on a 68000 or 68010 the instruction is looked for in the words
  before the saved PC, and for bus and address errors on a 68000 the saved PC is given, which is
  some words past the instruction.
- A halted CPU (`dbg.exset("halt")`) is noticed at the end of the frame, not at once.
- With an interlaced screen, the frame holds both fields (the last two frames of the core).
- `emu.config_get` and `emu.config_set` use the option names of the vAmiga core
  (`CPU.REVISION`, `MEM.CHIP_RAM`, ...), and numbers as values.
- `state.save`, `state.snapshot`, `state.load` and `state.restore` take effect at once instead of
  at the end of the frame.
- State files are snapshots of the vAmiga core, and cannot be exchanged with FS-UAE.
- The frame is 840 x 574 pixels: everything outside the blanking areas, with each raster line
  twice.
- `input.port_mode` is accepted and does nothing: both ports always have a mouse and a joystick.
  Releasing one joystick direction centres the stick on that axis.
- Machines: what the core has. A1000, A500, A500+ and A1200 (AGA and the 68EC020 are marked as
  partially supported by the core). No A600, A3000, A4000, 68030 or graphics cards.

## Tests

```sh
cd tests
AMIGA_TEST_KICKSTART=/path/to/kick13.rom AMIGA_TEST_MODEL=A500 ./run_tests.py
```

140 tests in eleven modules, taken from the FS-UAE engine with few changes. They pass on the
A1000, A500, A500-ECS, A500+ and A1200 configurations (Kickstart 1.3, 2.04 and 3.1). Most boot a
small disk image which they create themselves. `test_symbols` builds a program with vasm and
vlink and puts it on a disk with xdftool (from amitools), and is skipped without them.
`test_public_disk` downloads the free operating system EmuTOS and boots that.

## Licence

The code in `src`, `scripts` and `tests` is under the MIT licence (see `LICENSE`).

The parts it is built with have their own:

- The vAmiga core (`extern/silicium`, with the change described in `docs/upstream.md`) is under
  the Mozilla Public License 2.0 and its CPU core
  (Moira) under the MIT licence, according to the licence file of Silicium. The Silicium
  applications, which are not built or used here, are under the GNU General Public License 3.
- Lua (`extern/lua`) is under the MIT licence.

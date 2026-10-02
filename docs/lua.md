# Lua scripting

vamiga-lua runs Lua scripts which inspect and control the emulated Amiga: read and write memory
and CPU registers, set breakpoints, send input, take screenshots and save states. Scripts are
either loaded when the emulation starts (`--lua`), or sent to the running emulator over a local
socket (`--port`), which makes it possible for another program to drive the emulation step by
step.

The API is the one of the Lua engine in the [FS-UAE fork](https://github.com/mwulffn/fs-uae) this
grew out of. The differences are listed at the end.

## Running scripts

```sh
build/vamiga-lua --rom kick13.rom --df0 game.adf --lua script.lua --port 5600
```

- `--lua <file>` loads a script when the emulation starts. It can be given several times.
- `--port <number>` makes the emulator listen on this TCP port, on 127.0.0.1 only, for Lua code
  to run.

All scripts and all code received on the socket share one Lua state, so a global variable set by
one request can be used by the next. The standard Lua 5.4 libraries are available, including `io`
and `os`.

**Lua code is not sandboxed.** Anything which can connect to the port can run code with the access
of the vamiga-lua process, so only use `--port` on a machine where you trust the local users.

### The socket protocol

A request is one line of JSON, and so is the reply:

```
-> {"id": 7, "code": "return cpu.pc, mem.read_u16(0xdff004)"}
<- {"id": 7, "ok": true, "results": [16515298, 8236], "output": ""}
-> {"id": 8, "code": "error('no')"}
<- {"id": 8, "ok": false, "error": "remote:1: no", "output": ""}
```

- `code` is first tried as an expression, so `"cpu.pc"` works like `"return cpu.pc"`.
- `results` holds the returned values. Tables become JSON arrays or objects, and functions and
  other values which JSON cannot hold become strings describing them.
- `output` is what the code printed with `print`.
- Lua strings are byte strings. Bytes above 127 are sent as the characters U+0080 to U+00FF, so
  the receiver gets the original bytes by encoding the string as Latin-1.
- The reply is sent when the code has finished. Code which waits (for example with
  `emu.wait_frames`) is replied to later, and other requests are handled in the meantime.

The emulator also sends lines which are not replies. They have an `event` key instead of `id`:

- `{"event": "stopped", "reason": "breakpoint", "pc": ..., "id": ..., "address": ...}` when the
  emulation stops at a breakpoint, after a callback paused it, or after `dbg.step`.
- `{"event": "print", "text": "..."}` for `print` called outside a request, for example in a
  breakpoint callback.

A client in any language is a socket, a JSON encoder and a loop.

### The client script

`scripts/amiga_lua.py` sends code to the emulator and prints the result. It only needs Python 3.

```sh
scripts/amiga_lua.py 'return string.format("%08x", cpu.pc)'
scripts/amiga_lua.py --file script.lua
scripts/amiga_lua.py --json 'cpu.disasm(cpu.pc, 5)'
```

The port defaults to 5600 and is changed with `--port`. The exit status is 1 if the code failed.
The file can also be imported: `LuaClient(port)` has `call(code)` returning the list of results,
`eval(code)` returning the first one, and `wait_event(name)`.

## Tasks, frames and pausing

Every script and every request runs as a *task*, which can wait while the emulation continues:

```lua
emu.wait_frames(50)        -- continue here 50 frames later
local info = dbg.wait()    -- continue here when the emulation stops
```

The emulator is paused whenever Lua code runs. Tasks, frame callbacks and breakpoint callbacks
run between two emulated instructions, so the CPU registers are always consistent when Lua code
reads or changes them. Tasks are continued at the end of a frame. Callbacks are plain function
calls and cannot wait.

Because of this, a script gives the same result every time it is run: the same frames, the same
register values, the same cycle counts.

The emulation is either running or stopped. It stops when `emu.pause` is called, when a breakpoint
without a callback is reached, and when a frame or instruction step has finished. While it is
stopped, requests from the socket are still handled, so everything can be inspected and changed.
A request which waits for frames while the emulation is stopped is not answered until the
emulation runs again.

## API

Addresses and values are integers. Functions raise a Lua error when given invalid arguments.

### emu

| Function | Description |
| --- | --- |
| `emu.frame()` | Number of frames emulated since the emulator was started. |
| `emu.cycles()` | The emulated time in cycles (see below). |
| `emu.beam()` | The line and the horizontal position (in colour clocks) the display has reached. |
| `emu.timing()` | `{lines = ..., cycles_per_line = ..., cycles_per_frame = ..., hz = ...}` for the current display mode. |
| `emu.wait_frames(n)` | Wait until `n` frames (default 1) have been emulated. |
| `emu.wait_next_frame()` | The same as `emu.wait_frames(1)`. |
| `emu.on_frame(f)` | Call `f()` after every frame. Returns an id. |
| `emu.remove_frame_callback(id)` | Remove a frame callback. |
| `emu.pause()` | Stop the emulation before the next instruction. |
| `emu.resume()` | Continue the emulation. |
| `emu.paused()` | True if the emulation is stopped. |
| `emu.step(n)` | Run `n` frames (default 1) and stop again. Returns when that is done. |
| `emu.warp(on)` | Turn warp mode (running as fast as possible) on or off. |
| `emu.reset(hard)` | Reset the Amiga. A hard reset also clears memory. |
| `emu.quit()` | End the program. |
| `emu.config_get(name)` | The value of an option of the vAmiga core, as a number. |
| `emu.config_set(name, value)` | Change an option. Returns false if the value was not accepted. |
| `emu.log(text)` | Write a line to the log (standard error). |

`print` writes to the output of the request, or to the log when called outside a request.

A cycle in `emu.cycles` is a cycle of the 7.09 MHz clock of a PAL Amiga: there are two per colour
clock, 454 per line and 142102 per PAL frame, and it is the unit in which the instruction times
of the 68000 are given. How much of a frame something takes is its cycles divided by
`emu.timing().cycles_per_frame`, and in raster lines by `cycles_per_line`.

The options are those of the vAmiga core, with the names it uses in its own shell, for example
`CPU.REVISION`, `CPU.OVERCLOCKING`, `MEM.CHIP_RAM`, `MEM.SLOW_RAM`, `MEM.FAST_RAM`,
`AGNUS.REVISION`, `DENISE.REVISION` and `BLITTER.ACCURACY`. `emu.config_set` takes a number, or a
name the core knows for the value. An unknown option name is an error. The same options can be
given on the command line with `--set <name>=<value>`.

### mem

| Function | Description |
| --- | --- |
| `mem.read_u8(a)`, `read_u16`, `read_u32` | Read like the CPU does. Reading a hardware register has the same side effects as on the Amiga. |
| `mem.write_u8(a, v)`, `write_u16`, `write_u32` | Write like the CPU does. |
| `mem.peek_u8(a)`, `peek_u16`, `peek_u32` | Read RAM or ROM without side effects. Fails for other addresses. |
| `mem.poke_u8(a, v)`, `poke_u16`, `poke_u32` | Write RAM or ROM directly. |
| `mem.read_range(a, length)` | The bytes as a string. Bytes which are not RAM or ROM are 0. |
| `mem.write_range(a, string)` | Write the bytes of the string to RAM or ROM. |
| `mem.custom.NAME` | The address of a custom chip register, for example `mem.custom.COLOR00`. |
| `mem.tap_read(first, last, f)` | Call `f(address, value, size, pc)` when the CPU reads from the range. Returns an id. |
| `mem.tap_write(first, last, f)` | The same when the CPU writes to the range. |
| `mem.tap_remove(id)` | Remove a tap, or all taps when called without an id. |

Writing memory from Lua also clears the instruction cache of the emulated CPU (68020), so changed
code is run. The instruction at `cpu.pc` has already been fetched; assign `cpu.pc = cpu.pc` to
fetch it again after changing it.

Notes on taps:

- If the callback returns an integer, that value is read or written instead.
- `pc` is the address of the instruction making the access.
- The callback is called for each bus access. A 68000 accesses a long word as two words.
- The callback runs in the middle of an instruction. It can read and write memory and registers
  and call `emu.pause()`, which stops the emulation after the instruction.
- A tap callback cannot use the `state`, `media` and `input` functions, `emu.reset` or
  `emu.config_set`. They raise an error there.
- Instruction fetches are not reported, and neither are reads which use PC-relative addressing
  (`move.w table(pc,d0.w),d1`), as the CPU makes those in its program space.
- Memory accesses made from Lua do not run taps, and neither do accesses by the custom chips
  (DMA).
- There can be 20 taps. Only the first tap which matches an access is run.

### cpu

The registers are fields which can be read and assigned: `cpu.d0` to `cpu.d7`, `cpu.a0` to
`cpu.a7`, `cpu.pc`, `cpu.sr`, `cpu.usp`, `cpu.isp`, `cpu.msp` and `cpu.vbr`.

`cpu.pc` is the address of the instruction which is run next.

`cpu.disasm(address, count)` returns a list of `count` (default 1) instructions, each a table
`{address = ..., size = ..., text = "..."}`.

### dbg

| Function | Description |
| --- | --- |
| `dbg.bpset(address, f)` | Set a breakpoint and return its id. Without `f` the emulation stops there. With `f`, `f(address)` is called and the emulation continues, unless `f` calls `emu.pause()`. |
| `dbg.bpclear(id)` | Remove a breakpoint, or all breakpoints when called without an id. |
| `dbg.bplist()` | A list of `{id = ..., address = ...}`. |
| `dbg.exset(vector, f)` | Watch for a CPU exception and return an id. `vector` is the vector number, `"crash"` or `"halt"` (see below). Without `f` the emulation stops; with `f`, `f(vector, pc)` is called and the emulation continues, unless `f` calls `emu.pause()`. |
| `dbg.exclear(id)` | Remove an exception watch, or all of them when called without an id. |
| `dbg.go()` | Continue the emulation (the same as `emu.resume`). |
| `dbg.wait(frames)` | Wait until the emulation stops and return a table saying why. With `frames`, give up after that many frames and return nothing. |
| `dbg.step(n)` | Run `n` instructions (default 1) and stop. Returns the same as `dbg.wait`. |
| `dbg.stopped()` | The same table as `dbg.wait` returns if the emulation is stopped, otherwise false. |
| `dbg.measure(from, to, count, frames)` | Measure the time from one address to another (see below). |
| `dbg.load_symbols(path, name, frames)` | Load the symbols of a running program (see below). |
| `dbg.symbol(name)` | The address of a symbol, or nil. |
| `dbg.lookup(address)` | The name for an address inside a loaded program, such as `"update"` or `"update+$1a"`, or nil. |
| `dbg.unload_symbols(name)` | Forget the symbols of a program, or of all programs when called without a name. |

The table from `dbg.wait` has `reason` (`"breakpoint"`, `"tap"`, `"step"`, `"exception"`, `"halt"`
or `"pause"`) and `pc`. For breakpoints, taps and exceptions it also has `id` and `address`, and
for exceptions and halts `vector`.

A breakpoint stops the emulation *before* the instruction at its address is run. A breakpoint
callback costs about 13 microseconds, so a breakpoint in a loop which runs a thousand times per
frame is no problem, and one in the innermost loop of a program is noticeable.

#### Measuring time

`dbg.measure` uses breakpoints and `emu.cycles` to time a part of the program:

```lua
dbg.measure("update_player")                 -- a subroutine, from its start until it returns
dbg.measure(0x24a6c, 0x24b10)                -- from one address to another
dbg.measure("main_loop", "main_loop", 50)    -- between passes of one address, 50 times
```

- `from` and `to` are addresses, or symbol names when symbols are loaded. Without `to`, `from`
  must be the start of a subroutine entered with JSR or BSR.
- `count` is the number of measurements (default 1), and `frames` how many frames to wait for
  them (default 500). The emulation must be running.
- The result is `{count = ..., min = ..., max = ..., average = ..., samples = {...}}`, in the
  cycles of `emu.cycles`. Interrupts taken in between are part of the time.
- The 68000 and the 68010 are emulated cycle by cycle, so the times are exact: `ADDQ.L #1,D0`
  measures 8 cycles. The 68EC020 of the A1200 configuration is not, and its times are
  approximate.

#### Symbols

When a program is started by AmigaDOS, it is loaded wherever there is free memory, so its
addresses differ from run to run. `dbg.load_symbols` works out the addresses of the names in the
program:

```lua
local program = dbg.load_symbols("/path/to/build/game", "game")
dbg.bpset("update_player")                  -- dbg.bpset takes symbol names
print(mem.peek_u16(program.symbols.lives))
print(dbg.lookup(cpu.pc))                   --> update_player+$1a
```

- `path` is the executable on the host: the same file which was put on the Amiga disk. It must be
  an Amiga executable (hunk format) which still has its symbols. vasm and vlink keep them by
  default; do not link with `-s`. Local labels are only there if the assembler was told to keep
  them.
- `name` is the name of the program on the Amiga, and defaults to the file name of `path`. The
  program must be running: it is found among the tasks of the Amiga, either as the command of a
  CLI or as a process with that name.
- `frames` makes the function wait up to that many frames for the program to start. Without it,
  the function fails if the program is not running.
- The result has `name`, `path`, `segments` (a list of `{address = ..., size = ...}`, one for each
  hunk of the executable) and `symbols` (name to address).
- C compilers put an underscore before the names from the source code. `program.symbols.main` and
  `dbg.symbol("main")` also find `_main`.
- When symbols are loaded, the table from `dbg.wait` and the `stopped` event also have `symbol`
  (the name for `pc`) and `address_symbol` (the name for `address`) where the addresses are inside
  a loaded program.

The symbols are those in the executable, so this does not help with programs which unpack or
move themselves after they are loaded, and overlays are not supported. Source files and line
numbers are not read. Load the symbols again after the program has been started again, as it will
be at another address.

#### Exceptions

Exception watches catch a program going wrong:

- `dbg.exset("crash")` matches the exceptions which normally mean that a program has crashed: bus
  error (vector 2), address error (3), illegal instruction (4), division by zero (5) and the
  unimplemented line A and line F instructions (10 and 11). It also matches the CPU halting,
  which is what happens after a double fault. Interrupts, TRAP instructions and privilege
  violations are not included, as the operating system uses them.
- The emulation stops at the first instruction of the exception handler, so the exception stack
  frame can be inspected. `address` in the stop information (and `pc` given to the callback) is
  the instruction which caused the exception.
- That address is worked out from what the CPU saved on the stack. For a division by zero or a
  CHK on a 68000 or 68010, the instruction is looked for in the words before the saved PC. For
  bus and address errors on a 68000, the address is the saved PC, which is some words past the
  instruction.
- For a halt, `reason` is `"halt"` and `vector` is 2. A halt is noticed at the end of the frame.
  Lua keeps working while the CPU is halted; `emu.reset()` or restoring a state gets it going
  again.
- A guru meditation which the operating system raises itself (by calling `Alert`) is not an
  exception, and is not caught unless it follows one of the exceptions above.

### input

| Function | Description |
| --- | --- |
| `input.key(name, down)` | Press (`true`) or release (`false`) a key. |
| `input.type(text, frames)` | Type text on a US keyboard, holding each key for `frames` frames (default 2). |
| `input.joy(port, button, down)` | Press or release `"left"`, `"right"`, `"up"`, `"down"`, `"fire"`, `"fire2"` or `"fire3"`. Port 1 is the normal joystick port. |
| `input.mouse(dx, dy)` | Move the mouse in port 0. |
| `input.mouse_button(button, down)` | Press or release mouse button 1 (left), 2 (right) or 3 (middle). |
| `input.port_mode(port, mode)` | Accepted for scripts written for FS-UAE; does nothing (see below). |

Key names, in any case: `a` to `z`, `0` to `9`, `f1` to `f10`, `return`, `enter`, `space`, `esc`,
`tab`, `backspace`, `del`, `help`, `cursor_up`, `cursor_down`, `cursor_left`, `cursor_right`,
`shift_left`, `shift_right`, `caps_lock`, `ctrl`, `alt_left`, `alt_right`, `amiga_left`,
`amiga_right`, `backquote`, `sub`, `equals`, `backslash`, `leftbracket`, `rightbracket`,
`semicolon`, `singlequote`, `comma`, `period`, `div`, `np_0` to `np_9`, `np_period`, `np_add`,
`np_sub`, `np_mul`, `np_div`, `np_lparen` and `np_rparen`.

Both ports always have a mouse and a joystick connected, and the one which is moved is the one
the Amiga sees. Releasing a joystick direction puts the stick back in the middle on that axis.

### video

| Function | Description |
| --- | --- |
| `video.size()` | The width and height of the frame in pixels: 840 and 574. |
| `video.pixel(x, y)` | The red, green and blue values (0 to 255) of a pixel. |
| `video.pixels()` | The frame as a string with three bytes per pixel, and the width and height. |
| `video.screenshot(path)` | Save the frame as a PNG file. Returns the width and height. |

The frame is the last one the emulator finished. It has one pixel per hires pixel and two lines
per raster line, and leaves out the blanking areas to the left and at the top. Every frame is
drawn, also in warp mode. With an interlaced screen, the frame holds both fields.

The colours are the ones the Amiga program set, not adjusted to look like they do on a monitor: a
colour register holding `$F80` gives the pixel 255, 136, 0.

### state

| Function | Description |
| --- | --- |
| `state.save(path)` | Save a state file. |
| `state.load(path)` | Load a state file. |
| `state.snapshot()` | Save a state in memory and return it. `#snapshot` is its size in bytes. |
| `state.restore(snapshot)` | Load a state returned by `state.snapshot`. It can be loaded many times. |

A state is saved and loaded at once, between two instructions. Lua variables, breakpoints, taps
and tasks are not part of the state and are kept.

Taking a snapshot takes about 15 ms and restoring one about 35 ms, and a snapshot is 7 to 8 MB.
State files are snapshots of the vAmiga core, and are tied to the version of the core.

### media

| Function | Description |
| --- | --- |
| `media.insert(drive, path)` | Insert a disk image in drive 0 to 3. |
| `media.eject(drive)` | Eject the disk. |
| `media.path(drive)` | The path of the disk image in the drive, or an empty string. |

The disk images are the ones the core reads, among them ADF, extended ADF, DMS and Amiga
executables.

## Examples

Find out which instruction writes to an address:

```lua
mem.tap_write(0x7f000, 0x7f001, function(address, value, size, pc)
    print(string.format("%08x: write %04x to %08x", pc, value, address))
    print(cpu.disasm(pc)[1].text)
end)
```

Run to an address, and try the next frames twice with different input:

```lua
dbg.bpset(0x24a6c)
local info = dbg.wait()
dbg.bpclear()

local before = state.snapshot()
input.joy(1, "fire", true)
emu.step(50)
video.screenshot("/tmp/with-fire.png")

state.restore(before)
input.joy(1, "fire", false)
emu.step(50)
video.screenshot("/tmp/without-fire.png")
```

Patch a game while it runs: give the player more lives whenever the counter is written.

```lua
mem.tap_write(0x3c012, 0x3c013, function() return 9 end)
```

Stop when the program crashes, and say where:

```lua
dbg.exset("crash")
local info = dbg.wait()
print(info.reason, info.vector, string.format("%08x", info.address), info.address_symbol)
```

## Running fast

`emu.warp(true)` (or `--warp`) runs the emulation as fast as the host allows. On an Apple Silicon
Mac a demo on the A500 configuration runs at about 720 frames per second, 14 times the speed of
the Amiga. Other ways to save time:

- Take a snapshot (`state.snapshot`) when the program has started, and restore it for each test
  instead of booting again.
- Remove breakpoints and taps when they are no longer needed. A breakpoint only costs when it is
  reached. A tap costs about 5 microseconds for every access in its range.

## Differences from the FS-UAE engine

- There is no window. The emulator is only controlled through Lua.
- `dbg.command` (commands of the UAE debugger) does not exist.
- `emu.config_get` and `emu.config_set` use the option names of the vAmiga core and numbers as
  values.
- The `state` functions take effect at once instead of at the end of the frame, and state files
  cannot be exchanged with FS-UAE.
- The frame is 840 x 574 pixels (FS-UAE: 756 x 574 or 576).
- `input.port_mode` does nothing, and there is no CD32 pad.
- Taps do not report reads with PC-relative addressing, and tap callbacks cannot use the
  functions listed under `mem`.
- A halted CPU is noticed at the end of the frame, and the address of the instruction which
  caused an exception is worked out from the stack (see Exceptions).
- The machines are those of the core: A1000, A500, A500+ and A1200. AGA and the 68EC020 are
  marked as partially supported by the core. No A600, A3000, A4000, 68030 or graphics cards, and
  no hard drive from a directory on the host.

## Tests

`tests/` has a Python test suite which starts the emulator and talks to it over the socket.

```sh
cd tests
AMIGA_TEST_KICKSTART=/path/to/kick13.rom AMIGA_TEST_MODEL=A500 ./run_tests.py
./run_tests.py test_debug test_state         # only these modules
python3 -m unittest test_debug.TapTest       # one class
```

- `AMIGA_TEST_KICKSTART` is the Kickstart ROM to use; without it the tests are skipped.
- `AMIGA_TEST_MODEL` is `A1000`, `A500` (the default), `A500-ECS`, `A500+` or `A1200`.
- `AMIGA_TEST_BINARY` is the executable to test (default `build/vamiga-lua`).
- `AMIGA_TEST_OPTIONS` adds options of the core to all tests, for example
  `CPU.REVISION=68010,MEM.SLOW_RAM=0`.

Most tests boot a small disk image which they create themselves. `test_symbols.py` builds a
program with vasm and vlink and puts it on a disk with xdftool (from amitools); it is skipped if
those tools are not installed. `test_public_disk.py` downloads the free operating system EmuTOS
and boots that.

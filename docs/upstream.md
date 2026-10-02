# Notes for the vAmiga core

The core is used from a fork (`mwulffn/silicium`, branch `vamiga-lua`) of
[Silicium](https://github.com/dirkwhoffmann/silicium), pinned as the submodule `extern/silicium`.
The fork should stay as close to the original as possible. This file lists what the fork changes,
and things found in the core which could be reported or contributed. Nothing here has been sent
to the author yet.

## Changes in the fork

### An observer of the data accesses of the CPU

Commit `b0b9991`, 37 lines in `Cores/VACore/Components/CPU/CPU.h` and `CPU.cpp`.

`CPU::memoryObserver` is an optional function which is called for every byte, word or long word
the CPU reads from or writes to memory, with the address, the value, the size and the direction.
It returns the value to use. The memory taps (`mem.tap_read`, `mem.tap_write`) are built on it.
The watchpoints of the core only tell that an address was touched, after the instruction has
finished, and not what was read or written.

Accesses to the program space (instruction fetches and PC-relative operands) are not reported.
With no observer set, the cost is one test of a pointer per access; no difference in speed could
be measured.

Worth offering upstream as it is. To update the core, merge the `main` branch of Silicium into
the `vamiga-lua` branch of the fork, push it, and move the submodule to the merge.

## Found in the core, not changed

### Resuming the emulator computes the checksum of the ROM

`Thread::run()` calls `isReady()`, and `Memory::_isReady()` calls `getRomTraits()`, which
computes the CRC-32 of the whole Kickstart ROM. So every `run()`, `stepInto()`, `finishFrame()`
and so on costs about 0.45 ms with a 256 KB ROM, and `VAmiga::run()` does it twice (once itself,
once in the command it queues). With a breakpoint in a loop, or when stepping frame by frame,
this was most of the time spent: a breakpoint took 0.9 ms to pass.

Here it is avoided by calling `Emulator::switchState(ExecState::RUNNING)` directly (see
`host_run` in `src/main.cpp`).

This is not a mistake in the core, and there are reasons for doing it this way:

- The check is a real one: the emulator must not run without a ROM, and the AROS ROM needs its
  extension ROM and 1 MB of RAM. Which ROM it is can only be told from its contents.
- Computing the checksum when it is asked for is always right. The contents of the ROM can
  change in many places: loading, erasing or deleting a ROM, loading a snapshot or a workspace
  with the ROMs in it (`MEM.SAVE_ROMS`), the patch the core applies to Kickstart 1.2 for its
  debugger board, and `Memory::patch` (which the Lua `poke` functions use). A checksum which is
  kept would have to be thrown away in every one of them.
- For a person at the keyboard, 0.45 ms per press of "run" cannot be noticed. It only shows when
  a program resumes the emulator thousands of times per second, as this one does.

So what to tell the author is how the core is used here and what it costs then, and to ask
whether he would take a change which keeps the checksum (cleared wherever the ROM changes) or
which makes the check cheaper, for example by only looking for a ROM when running and leaving
the AROS checks to when a ROM is loaded. If he would rather keep it as it is, nothing is lost:
the way around it above works.

### An exception gets through with an odd supervisor stack pointer (68000)

On a 68000, when the supervisor stack pointer is odd, the CPU cannot save the PC and SR for an
exception: the write to the stack is itself an address error, and as that one cannot be saved
either, the CPU halts (double fault). WinUAE does this.

In the core (Moira), the exception goes through: the frame is written at the odd address and the
handler is entered. The halt then comes with the first instruction of the handler which uses the
stack. Seen with an illegal instruction (vector 4) and the supervisor stack pointer set to
$1001: a catchpoint on vector 4 is reached with A7 = $FFB, and `CPU_HALT` follows later.

The cause is in `writeStackFrame0000` (and its siblings) in
`Cores/VACore/Components/CPU/Moira/MoiraExceptions_cpp.h`: the frame is written with the address
masked to an even one (`U32_ADD(reg.sp, 4) & ~1`), so the alignment check in the write path never
sees an odd address. The mask is written out on purpose, so this may be a decision of the author
rather than an oversight, perhaps from measurements on a real machine. It should be raised as a
question first, with the test case, and not sent as a fix.

It only matters to a program which has already crashed, but it changes what a debugger reports:
an illegal instruction exception first, where WinUAE just halts. `test_halt` in
`tests/test_debug.py` reproduces it and allows for both behaviours.

Not verified on real hardware here. That a 68000 halts in this case is from its documentation
and from WinUAE's behaviour. Moira is a project of its own
(https://github.com/dirkwhoffmann/Moira), so that is where it belongs.

### One of the three self-tests of the core fails

`VAHeadless --smoke` stops at `hd0 geometry cylinders=320 heads=2 sectors=32` with "The drive
geometry doesn't match the hard drive capacity." (vAmiga repository, commit `899da5d`, the same
core as here). Not looked into.

// The emulator, as seen by the rest of the program.
//
// The vAmiga core runs the emulation on a thread of its own. This program
// keeps the emulator paused, and lets it run to the end of the current frame
// when a frame is wanted (host_run_frame). Everything else, including all Lua
// code, runs on the main thread while the emulator is paused, so the Lua code
// always sees the machine between two instructions and between two frames.

#ifndef VAMIGA_LUA_HOST_H
#define VAMIGA_LUA_HOST_H

#include "VAmiga.h"

#include <cstdint>
#include <vector>

// The internals of the core. The public API in VAmiga.h does not offer
// everything the Lua functions need (writing registers and memory, for
// example), so they also use the components directly.
#include "vaconfig.h"
#include "Amiga.h"
#include "Emulator.h"
#include "Option.h"

extern vamiga::VAmiga *g_vamiga;

// The components of the emulated Amiga.
vamiga::Amiga &core(void);

// Something which made the emulator pause.
enum host_event_type {
    // The frame has ended (see "Where a frame ends" in main.cpp).
    HOST_FRAME_END,
    // The next instruction is at a breakpoint address.
    HOST_BREAKPOINT,
    // An exception was taken which has a catchpoint. The next instruction
    // is the first one of the exception handler.
    HOST_EXCEPTION,
    // One instruction was run (when single stepping).
    HOST_STEP,
    // The CPU has halted (after a double fault).
    HOST_HALT,
    // The copper has reached an instruction which has a breakpoint in the
    // core, has written a register which has a watchpoint, or the beam has
    // reached a position which has a trap. The CPU has finished the
    // instruction it was running when that happened.
    HOST_COPPER_BREAKPOINT,
    HOST_COPPER_WATCHPOINT,
    HOST_BEAM,
};

// The beam trap of the core which the frame loop uses (line 0, in the form
// the core has its beam traps in).
#define HOST_FRAME_END_TRAP 0x12

struct host_event {
    host_event_type type;
    uint32_t address;
    int vector;
};

// Runs the emulation until the end of the current frame, or until something
// else makes it pause: a breakpoint or a catchpoint set in the core, or, with
// single_step, the end of the next instruction. Returns what happened, which
// can be several things at once.
std::vector<host_event> host_run(bool single_step);

// Changes an option of the core. Throws an exception if the value is not
// accepted. The emulator must be paused (it is whenever Lua code runs).
void host_set_option(vamiga::Opt option, vamiga::i64 value);

// While warp is off, frames are run at the speed of the Amiga.
void host_set_warp(bool warp);

void host_request_quit(void);
bool host_quit_requested(void);

// Writes a line to the log (standard error), with printf formatting.
void host_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif

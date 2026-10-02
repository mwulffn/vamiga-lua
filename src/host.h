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

// The internals of the core. The public API in VAmiga.h does not offer
// everything the Lua functions need (writing registers and memory, for
// example), so they also use the components directly.
#include "vaconfig.h"
#include "Amiga.h"
#include "Option.h"

extern vamiga::VAmiga *g_vamiga;

// The components of the emulated Amiga.
vamiga::Amiga &core(void);

// Runs the emulation to the end of the current frame.
void host_run_frame(void);

// While warp is off, frames are run at the speed of the Amiga.
void host_set_warp(bool warp);

void host_request_quit(void);
bool host_quit_requested(void);

// Writes a line to the log (standard error), with printf formatting.
void host_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif

// An optional window which shows the picture and plays the sound of the
// emulated Amiga, for a person to watch what a script is doing. It takes no
// input: the emulator is controlled through Lua, with or without the window.
//
// The window needs SDL 3. Without it, the program is built with no window,
// and window_open fails.

#ifndef VAMIGA_LUA_WINDOW_H
#define VAMIGA_LUA_WINDOW_H

#include <string>

// Opens the window. Returns false, with a message in error, if that is not
// possible. Must be called on the main thread.
bool window_open(std::string &error);
void window_close(void);
bool window_is_open(void);

// Shows the frame the emulator has just finished. In warp mode only some of
// the frames are shown. Also handles the events of the window.
void window_frame(void);
// Handles the events of the window while no frames are being emulated.
void window_idle(void);

// Sets how loud the sound is played, from 0 (silent) to 100.
void window_set_volume(int percent);

// True while sound is being played: the window is open, has a sound device,
// and the emulation runs at the speed of the Amiga.
bool window_sound_active(void);

// Tells the window whether the emulation is running at the speed of the
// Amiga. Sound is only played when it is.
void window_set_playing(bool playing);

#endif

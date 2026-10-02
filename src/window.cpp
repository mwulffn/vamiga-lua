#include "window.h"

#include "engine.h"

#ifdef VAMIGA_LUA_WINDOW

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <vector>

using namespace vamiga;

// The sound is asked for at this rate, and SDL converts it to what the
// sound device wants.
#define SAMPLE_RATE 44100

// In warp mode, the window is drawn this many times per second at most.
#define WARP_FRAMES_PER_SECOND 50

static bool g_sdl_started;
static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static SDL_AudioStream *g_audio;
static int g_width, g_height;
// Read by the sound callback, which runs on a thread of SDL: whether the
// emulation runs at the speed of the Amiga, and whether it is stopped.
static std::atomic<bool> g_playing;
static std::atomic<bool> g_idle;
static int g_volume = 100;
static std::chrono::steady_clock::time_point g_last_draw;
static int g_frames_since_title;

bool window_is_open(void)
{
    return g_window != NULL;
}

void window_set_volume(int percent)
{
    g_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (g_audio != NULL) {
        SDL_SetAudioStreamGain(g_audio, g_volume / 100.0f);
    }
}

bool window_sound_active(void)
{
    return g_window != NULL && g_audio != NULL && g_playing;
}

// Called by SDL, on its sound thread, when it needs more samples. While the
// emulation runs at the speed of the Amiga, they are taken from the core,
// which adjusts how fast it produces samples to how fast they are taken.
// Otherwise there is silence.
static void SDLCALL sound_callback(void *userdata, SDL_AudioStream *stream, int needed, int total)
{
    static std::vector<float> buffer;
    const int frame_size = 2 * (int) sizeof(float);
    int frames = needed / frame_size;
    if (frames <= 0) {
        return;
    }
    buffer.assign((size_t) frames * 2, 0.0f);
    if (g_playing && !g_idle) {
        core().audioPort.copyInterleaved(buffer.data(), frames);
    }
    SDL_PutAudioStreamData(stream, buffer.data(), frames * frame_size);
}

// With sound, the core must run at its normal speed: in its warp mode it
// produces no sound. The core is told that it gets one signal per frame
// (which is how this program runs it anyway), as it would otherwise decide
// from the clock of the host how many frames to run.
static void update_core_speed(void)
{
    host_set_option(Opt::AMIGA_VSYNC, 1);
    host_set_option(Opt::AMIGA_WARP_MODE, (i64) (window_sound_active() ? Warp::NEVER : Warp::ALWAYS));
}

void window_set_playing(bool playing)
{
    if (g_playing != playing) {
        g_playing = playing;
        if (g_window != NULL) {
            update_core_speed();
        }
    }
}

bool window_open(std::string &error)
{
    if (g_window != NULL) {
        return true;
    }
    if (!g_sdl_started) {
        // The window being closed must not be taken as a request to end the
        // program, which carries on without it.
        SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
            error = SDL_GetError();
            return false;
        }
        g_sdl_started = true;
    }
    engine_video_size(g_width, g_height);

    if (!SDL_CreateWindowAndRenderer(
            "vamiga-lua", g_width, g_height, SDL_WINDOW_RESIZABLE, &g_window, &g_renderer)) {
        error = SDL_GetError();
        g_window = NULL;
        return false;
    }
    // The picture keeps its proportions when the window is resized.
    SDL_SetRenderLogicalPresentation(
        g_renderer, g_width, g_height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    // The pace is set by the frame loop of this program, not by the screen.
    SDL_SetRenderVSync(g_renderer, 0);
    g_texture = SDL_CreateTexture(
        g_renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, g_width, g_height);
    if (g_texture == NULL) {
        error = SDL_GetError();
        window_close();
        return false;
    }
    SDL_SetTextureScaleMode(g_texture, SDL_SCALEMODE_NEAREST);

    // No sound is not a reason to have no window.
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, SAMPLE_RATE};
    g_audio = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, sound_callback, NULL);
    if (g_audio == NULL) {
        host_log("No sound: %s", SDL_GetError());
    } else {
        host_set_option(Opt::HOST_SAMPLE_RATE, SAMPLE_RATE);
        SDL_SetAudioStreamGain(g_audio, g_volume / 100.0f);
        SDL_ResumeAudioStreamDevice(g_audio);
    }
    update_core_speed();
    g_frames_since_title = 1000;
    window_frame();
    return true;
}

void window_close(void)
{
    if (g_audio != NULL) {
        SDL_DestroyAudioStream(g_audio);
        g_audio = NULL;
    }
    if (g_texture != NULL) {
        SDL_DestroyTexture(g_texture);
        g_texture = NULL;
    }
    if (g_renderer != NULL) {
        SDL_DestroyRenderer(g_renderer);
        g_renderer = NULL;
    }
    if (g_window != NULL) {
        SDL_DestroyWindow(g_window);
        g_window = NULL;
        update_core_speed();
    }
}

static void handle_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            window_close();
            return;
        }
        if (event.type == SDL_EVENT_QUIT) {
            // Quitting the application (Cmd+Q on macOS) ends the program.
            host_request_quit();
        }
    }
}

static void draw(void)
{
    // The frame is the one the Lua video functions give.
    std::vector<uint8_t> pixels = engine_video_frame();
    SDL_UpdateTexture(g_texture, NULL, pixels.data(), g_width * 3);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderTexture(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
    g_last_draw = std::chrono::steady_clock::now();
}

static void update_title(void)
{
    long long frame = engine_frame_number();
    const char *state = engine_stop_requested() ? "stopped" : g_playing ? "running" : "warp";
    char title[100];
    snprintf(title, sizeof title, "vamiga-lua - frame %lld - %s", frame, state);
    SDL_SetWindowTitle(g_window, title);
}

void window_frame(void)
{
    if (g_window == NULL) {
        return;
    }
    g_idle = false;
    handle_events();
    if (g_window == NULL) {
        return;
    }
    auto since_draw = std::chrono::steady_clock::now() - g_last_draw;
    if (g_playing || since_draw >= std::chrono::milliseconds(1000 / WARP_FRAMES_PER_SECOND)) {
        draw();
    }
    if (++g_frames_since_title >= 25) {
        g_frames_since_title = 0;
        update_title();
    }
}

void window_idle(void)
{
    if (g_window == NULL) {
        return;
    }
    g_idle = true;
    handle_events();
    if (g_window == NULL) {
        return;
    }
    // Lua code can change what is on the screen while the emulation is
    // stopped (by loading a state), so the picture is drawn again now and
    // then.
    auto since_draw = std::chrono::steady_clock::now() - g_last_draw;
    if (since_draw >= std::chrono::milliseconds(100)) {
        draw();
        update_title();
    }
}

#else  // VAMIGA_LUA_WINDOW

bool window_open(std::string &error)
{
    error = "this program was built without SDL 3, and has no window";
    return false;
}

void window_close(void)
{
}

bool window_is_open(void)
{
    return false;
}

void window_frame(void)
{
}

void window_idle(void)
{
}

void window_set_playing(bool playing)
{
}

void window_set_volume(int percent)
{
}

bool window_sound_active(void)
{
    return false;
}

#endif

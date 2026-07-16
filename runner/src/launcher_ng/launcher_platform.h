// launcher_platform.h — shared SDL3 window + OpenGL + high-DPI plumbing.
//
// This layer is where the launcher's #1 requirement lives: crisp fractional /
// per-monitor DPI on Windows, macOS, and Linux (Wayland + X11). It is the SAME
// for both render backends. The key move (vs the old SDL2 launcher) is
// SDL_WINDOW_HIGH_PIXEL_DENSITY + treating window size (logical) and drawable
// size (physical pixels) as distinct, and tracking SDL's display scale so the
// backend can render at native resolution instead of letting the compositor
// upscale a low-res buffer (the root cause of the Wayland blur).

#ifndef LAUNCHER_NG_PLATFORM_H
#define LAUNCHER_NG_PLATFORM_H

// One switch selects the windowing backend for the whole launcher. SDL2 is the
// shipping default (it matches the game runtime, so the launcher links
// in-process with no migration); SDL3 is the follow-up that adds Wayland
// fractional scaling. Everything above this header is identical either way.
#if defined(LNG_SDL3)
  #include <SDL3/SDL.h>
#else
  #include <SDL.h>
#endif

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LauncherPlatform {
    SDL_Window*   window;
    SDL_GLContext gl;

    // Logical (DPI-independent) UI size — what layout & hit-testing use.
    int   logical_w, logical_h;
    // Physical framebuffer size in pixels — what glViewport uses.
    int   pixel_w, pixel_h;
    // Content scale from SDL (1.0, 1.25, 1.5, 1.75, 2.0, ...). Backends multiply
    // font raster sizes and any physical dimensions by this.
    float display_scale;

    bool  scale_changed;   // set for one frame after a DPI/monitor change
    bool  size_changed;    // set for one frame after a resize
    bool  should_quit;     // window close requested
} LauncherPlatform;

// Create the window (RESIZABLE + HIGH_PIXEL_DENSITY) and a GL 3.3 core context.
// Returns false if SDL/GL initialization fails (caller boots as if skipped).
bool launcher_platform_open(LauncherPlatform* p, const char* title,
                            int logical_w, int logical_h);

// Refresh logical/pixel sizes and display scale from the live window. Called
// once per frame (and after resize / scale-change events).
void launcher_platform_refresh_metrics(LauncherPlatform* p);

// Present the current GL framebuffer.
void launcher_platform_present(LauncherPlatform* p);

// Tear down GL context + window and reset GL attributes, so the game runtime's
// SDL layer starts from a clean slate afterward.
void launcher_platform_close(LauncherPlatform* p);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_PLATFORM_H

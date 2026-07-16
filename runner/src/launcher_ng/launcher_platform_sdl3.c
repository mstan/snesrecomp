// launcher_platform_sdl3.c — SDL3 implementation of the shared platform layer.

#include "launcher_platform.h"

#include <stdio.h>

bool launcher_platform_open(LauncherPlatform* p, const char* title,
                            int logical_w, int logical_h) {
    if (!p) return false;
    SDL_zerop(p);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "[launcher] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    // HIGH_PIXEL_DENSITY is the linchpin: it asks SDL for a native-resolution
    // backbuffer on fractional-scale displays (esp. Wayland) instead of a
    // logical-size buffer the compositor blurs up. RESIZABLE lets us exercise
    // the live-resize requirement.
    const SDL_WindowFlags flags =
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;

    p->window = SDL_CreateWindow(title ? title : "Launcher",
                                 logical_w, logical_h, flags);
    if (!p->window) {
        fprintf(stderr, "[launcher] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    p->gl = SDL_GL_CreateContext(p->window);
    if (!p->gl) {
        fprintf(stderr, "[launcher] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(p->window);
        p->window = NULL;
        SDL_Quit();
        return false;
    }

    SDL_GL_MakeCurrent(p->window, p->gl);
    SDL_GL_SetSwapInterval(1);   // vsync — a launcher has no reason to spin

    launcher_platform_refresh_metrics(p);
    return true;
}

void launcher_platform_refresh_metrics(LauncherPlatform* p) {
    if (!p || !p->window) return;

    SDL_GetWindowSize(p->window, &p->logical_w, &p->logical_h);
    SDL_GetWindowSizeInPixels(p->window, &p->pixel_w, &p->pixel_h);

    float s = SDL_GetWindowDisplayScale(p->window);
    if (s <= 0.0f) s = 1.0f;
    p->display_scale = s;
}

void launcher_platform_present(LauncherPlatform* p) {
    if (p && p->window) SDL_GL_SwapWindow(p->window);
}

void launcher_platform_close(LauncherPlatform* p) {
    if (!p) return;
    if (p->gl)     { SDL_GL_DestroyContext(p->gl); p->gl = NULL; }
    if (p->window) { SDL_DestroyWindow(p->window); p->window = NULL; }
    SDL_Quit();
}

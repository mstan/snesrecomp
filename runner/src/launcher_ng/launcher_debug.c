// launcher_debug.c — LNG_SCRIPT interpreter + framebuffer capture.

#include "launcher_debug.h"
#include "launcher_sdlcompat.h"   // SDL2/SDL3 event-symbol shim + GL header

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LNG_MAX_CMDS 64

static char  g_script[2048];
static char* g_cmds[LNG_MAX_CMDS];
static int   g_cmd_count = 0;
static int   g_cmd_index = 0;
static int   g_wait_frames = 0;
static bool  g_active = false;

bool launcher_debug_active(void) { return g_active; }

void launcher_debug_init(void) {
    const char* s = SDL_getenv("LNG_SCRIPT");
    if (!s || !s[0]) return;

    snprintf(g_script, sizeof(g_script), "%s", s);
    g_cmd_count = 0;
    char* tok = strtok(g_script, ";");
    while (tok && g_cmd_count < LNG_MAX_CMDS) {
        while (*tok == ' ') ++tok;          // trim leading spaces
        if (*tok) g_cmds[g_cmd_count++] = tok;
        tok = strtok(NULL, ";");
    }
    g_active = g_cmd_count > 0;
    if (g_active) fprintf(stderr, "[dbg] script: %d commands\n", g_cmd_count);
}

bool launcher_capture_png(const char* path, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    unsigned char* px = (unsigned char*)malloc((size_t)w * h * 3);
    if (!px) return false;

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);

    // GL origin is bottom-left; PNG wants top-down. Flip rows in place.
    const size_t stride = (size_t)w * 3;
    unsigned char* row = (unsigned char*)malloc(stride);
    if (row) {
        for (int y = 0; y < h / 2; ++y) {
            unsigned char* a = px + (size_t)y * stride;
            unsigned char* b = px + (size_t)(h - 1 - y) * stride;
            memcpy(row, a, stride); memcpy(a, b, stride); memcpy(b, row, stride);
        }
        free(row);
    }

    int ok = stbi_write_png(path, w, h, 3, px, (int)stride);
    free(px);
    if (ok) fprintf(stderr, "[dbg] shot -> %s (%dx%d)\n", path, w, h);
    else    fprintf(stderr, "[dbg] shot FAILED -> %s\n", path);
    return ok != 0;
}

// Synthesize a click at logical window coords: warp the cursor (so backends
// that sample SDL_GetMouseState see it) and push button events (so backends
// that consume the event queue see it). Covers both ImGui and Clay.
static void synth_click(LauncherPlatform* p, float x, float y) {
    SDL_WarpMouseInWindow(p->window, x, y);

    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = SDL_GetWindowID(p->window);
    e.motion.x = x; e.motion.y = y;
    SDL_PushEvent(&e);

    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.windowID = SDL_GetWindowID(p->window);
    e.button.button = SDL_BUTTON_LEFT;
    e.button.clicks = 1;
    e.button.x = x; e.button.y = y;
#if defined(LNG_SDL3)
    e.button.down = true;
#else
    e.button.state = SDL_PRESSED;
#endif
    SDL_PushEvent(&e);

    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
#if defined(LNG_SDL3)
    e.button.down = false;
#else
    e.button.state = SDL_RELEASED;
#endif
    SDL_PushEvent(&e);
}

static void synth_key(SDL_Keycode key) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_KEY_DOWN;
#if defined(LNG_SDL3)
    e.key.key = key;
    e.key.down = true;
#else
    e.key.keysym.sym = key;
    e.key.state = SDL_PRESSED;
#endif
    SDL_PushEvent(&e);
    e.type = SDL_EVENT_KEY_UP;
#if defined(LNG_SDL3)
    e.key.down = false;
#else
    e.key.state = SDL_RELEASED;
#endif
    SDL_PushEvent(&e);
}

void launcher_debug_step(LauncherPlatform* p, LauncherModel* m) {
    if (!g_active) return;

    // Never clobber an action the UI already set this frame (e.g. PLAY -> LAUNCH);
    // otherwise a script that clicks PLAY and then ends would overwrite LAUNCH
    // with the script-exhausted QUIT below.
    if (m->action != LNG_ACTION_NONE) return;

    if (g_wait_frames > 0) { --g_wait_frames; return; }

    if (g_cmd_index >= g_cmd_count) {   // script exhausted -> exit
        m->action = LNG_ACTION_QUIT;
        return;
    }

    const char* c = g_cmds[g_cmd_index++];

    if (strncmp(c, "view:", 5) == 0) {
        const char* v = c + 5;
        if      (strcmp(v, "dashboard")  == 0) launcher_model_set_view(m, LNG_VIEW_DASHBOARD);
        else if (strcmp(v, "settings")   == 0) launcher_model_set_view(m, LNG_VIEW_SETTINGS);
        else if (strcmp(v, "controller") == 0) launcher_model_set_view(m, LNG_VIEW_CONTROLLER);
    } else if (strncmp(c, "player:", 7) == 0) {
        m->cfg_player = atoi(c + 7) ? 1 : 0;
    } else if (strncmp(c, "size:", 5) == 0) {
        int w = 0, h = 0;
        if (sscanf(c + 5, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            SDL_SetWindowSize(p->window, w, h);
            launcher_platform_refresh_metrics(p);
        }
    } else if (strncmp(c, "click:", 6) == 0) {
        float x = 0, y = 0;
        if (sscanf(c + 6, "%f,%f", &x, &y) == 2) synth_click(p, x, y);
    } else if (strncmp(c, "key:", 4) == 0) {
        if (strcmp(c + 4, "escape") == 0) synth_key(SDLK_ESCAPE);
    } else if (strncmp(c, "wait:", 5) == 0) {
        g_wait_frames = atoi(c + 5);
    } else if (strncmp(c, "shot:", 5) == 0) {
        launcher_capture_png(c + 5, p->pixel_w, p->pixel_h);
    } else if (strcmp(c, "quit") == 0) {
        m->action = LNG_ACTION_QUIT;
    } else {
        fprintf(stderr, "[dbg] unknown command: %s\n", c);
    }
}

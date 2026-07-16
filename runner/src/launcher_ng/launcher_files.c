// launcher_files.c — SDL3 native file dialog + process handoff.

#include "launcher_files.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char*  out;
    size_t cap;
    bool*  done;
} PickCtx;

static PickCtx g_pick;   // one dialog at a time is plenty for a launcher

static void SDLCALL pick_cb(void* userdata, const char* const* filelist, int filter) {
    (void)userdata; (void)filter;
    if (!filelist) {                       // error
        fprintf(stderr, "[launcher] file dialog error: %s\n", SDL_GetError());
        return;
    }
    if (!filelist[0]) return;              // user cancelled
    if (g_pick.out && g_pick.cap) {
        snprintf(g_pick.out, g_pick.cap, "%s", filelist[0]);
        if (g_pick.done) *g_pick.done = true;
    }
}

void launcher_pick_rom(SDL_Window* parent, char* out_path, size_t out_cap, bool* done) {
    static const SDL_DialogFileFilter filters[] = {
        { "SNES ROM", "sfc;smc;fig;swc" },
        { "All files", "*" },
    };
    g_pick.out = out_path; g_pick.cap = out_cap; g_pick.done = done;
    SDL_ShowOpenFileDialog(pick_cb, NULL, parent, filters, SDL_arraysize(filters),
                           NULL, false);
}

bool launcher_launch_game(const char* exe_path, const char* rom_path) {
    if (!exe_path || !exe_path[0]) return false;

    const char* argv[4];
    int n = 0;
    argv[n++] = exe_path;
    if (rom_path && rom_path[0]) argv[n++] = rom_path;
    argv[n] = NULL;

    SDL_Process* proc = SDL_CreateProcess(argv, false);
    if (!proc) {
        fprintf(stderr, "[launcher] failed to launch %s: %s\n", exe_path, SDL_GetError());
        return false;
    }
    // Detach: the launcher exits and the game keeps running.
    SDL_DestroyProcess(proc);
    return true;
}

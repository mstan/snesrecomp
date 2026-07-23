#include "snes_host_session.h"

#include <stdio.h>

#include "common_cpu_infra.h"
#include "snes_netplay.h"

#include <SDL.h>

int snes_host_ensure_sdl(void)
{
    if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_WasInit(SDL_INIT_AUDIO) &&
        SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        return 0;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) !=
        0) {
        fprintf(stderr, "snes_host: SDL_Init (session) FAILED: %s\n",
                SDL_GetError());
        return -1;
    }
    fprintf(stderr, "snes_host: SDL session init ok: video=%s audio=%s\n",
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver()
                                        : "(none)",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver()
                                        : "(none)");
    return 0;
}

void snes_host_session_reset(void)
{
    if (g_rtl_game_info && g_rtl_game_info->session_reset)
        g_rtl_game_info->session_reset();
}

void snes_netplay_soft_exit_to_lobby(const char *origin, int from_lobby)
{
    snes_netplay_shutdown();
    if (!from_lobby)
        return;
    fprintf(stderr, "snes_netplay: ended (%s) — returning to lobby\n",
            origin && origin[0] ? origin : "?");
    snes_netplay_request_return_to_lobby();
}

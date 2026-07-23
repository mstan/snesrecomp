/*
 * Shared host helpers for MotK soft-return rematch and lobby netplay UX.
 *
 * Prefer these (and snes_lobby_* / snes_netplay_*) over copying SDL / soft-exit
 * / rematch logic into each game main.c. Game-specific sticky clears go in
 * RtlGameInfo.session_reset (see docs/RECOMP_NET.md).
 */
#ifndef SNES_HOST_SESSION_H
#define SNES_HOST_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Re-init SDL video/audio/gamecontroller when recomp-ui's
 * launcher_platform_close() called SDL_Quit() before rematch.
 * Returns 0 on success, -1 if SDL_Init fails (error already printed).
 * No-op when subsystems are already live.
 */
int snes_host_ensure_sdl(void);

/*
 * Invoke RtlGameInfo.session_reset when registered. Call at session_reboot
 * after snes_host_ensure_sdl() and before SnesInit.
 */
void snes_host_session_reset(void);

/*
 * Tear down the netplay session and, when from_lobby != 0, request soft-return
 * to the waiting room (same path as Escape / SDL_QUIT). origin is for logs.
 */
void snes_netplay_soft_exit_to_lobby(const char *origin, int from_lobby);

#ifdef __cplusplus
}
#endif

#endif /* SNES_HOST_SESSION_H */

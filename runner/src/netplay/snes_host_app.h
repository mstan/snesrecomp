/*
 * Shared rematch / admit helpers for MotK-style SNES hosts.
 * Pair with snes_host_lobby_* for the launcher callback table.
 */
#ifndef SNES_HOST_APP_H
#define SNES_HOST_APP_H

#include <stdint.h>

#include "snes_host_session.h"
#include "snes_host_lobby.h"
#include "snes_netplay.h"

#if defined(RECOMP_LAUNCHER) || defined(SNES_HOST_HAS_RECOMP_UI)
#include "recomp_launcher.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(RECOMP_LAUNCHER) || defined(SNES_HOST_HAS_RECOMP_UI)

typedef struct SnesHostLaunchResult {
  int launched;       /* recomp_launcher_run_window returned 0 */
  int quit;           /* returned 1 */
  int netplay_enabled;
  int from_lobby;
  SnesNetplayConfig net_cfg;
  int caps_ws_extra;  /* from match_caps, or -1 if unknown */
} SnesHostLaunchResult;

/* Map launcher netplay_launch into SnesNetplayConfig (+ apply_env). */
void snes_host_app_apply_launch(const RecompLauncherCNetplayLaunch *net,
                                SnesHostLaunchResult *out);

/*
 * Soft-return prep before reopening the waiting room:
 * prepare_rematch + optional resume_netplay_room on *gi.
 */
void snes_host_app_begin_soft_return(RecompLauncherCGameInfo *gi,
                                     int set_resume_room);

#endif

typedef struct SnesHostBarrierHooks {
  uint16_t (*capture_local_pad)(void *ctx);
  /* Called each stall iteration; set *want_soft_exit nonzero to soft-exit. */
  void (*poll_events)(void *ctx, int *want_soft_exit);
  void *ctx;
  uint32_t peer_timeout_ms;    /* default 1500 if 0 */
  uint32_t connect_timeout_ms; /* 0 = disabled */
  void (*on_connect_timeout)(void *ctx);
} SnesHostBarrierHooks;

/*
 * MotK admit loop. Returns 1 if admitted (caller RtlRunFrame + finish_frame).
 * Returns 0 to skip the tick; *running may be cleared on soft-exit.
 */
int snes_host_barrier_admit(int from_lobby, int *running,
                            const SnesHostBarrierHooks *hooks);

#ifdef __cplusplus
}
#endif

#endif /* SNES_HOST_APP_H */

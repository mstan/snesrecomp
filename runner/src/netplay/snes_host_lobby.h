/*
 * Engine-owned MotK + LAN file-registry lobby adapter for recomp-ui.
 *
 * Games register identity + optional match_caps / rematch policy, then wire
 * snes_host_lobby_callbacks() into RecompLauncherCGameInfo.netplay. Do not
 * copy create/join/fill_launch glue into each title.
 *
 * Requires RECOMP_LAUNCHER (recomp-ui include path on the game target).
 */
#ifndef SNES_HOST_LOBBY_H
#define SNES_HOST_LOBBY_H

#include "snes_lobby_client.h"

#if defined(RECOMP_LAUNCHER) || defined(SNES_HOST_HAS_RECOMP_UI)
#include "recomp_launcher.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnesHostLobbyIdentity {
  const char *game_name;         /* e.g. "Metal Warriors" */
  const char *game_version;      /* e.g. SNES_GAME_VERSION */
  const char *lan_registry_path; /* e.g. "netplay_lan_lobby.txt" */
  const char *default_lobby_name;
} SnesHostLobbyIdentity;

typedef void (*SnesHostFillMatchCapsFn)(void *ctx,
                                        const void *settings /* RecompLauncherCSettings* */,
                                        SnesLobbyMatchCaps *out);

typedef struct SnesHostLobbyOpts {
  int auto_ready_guests;  /* 1: set_ready(1) for non-hosts in Pump (SMW) */
  int rematch_set_ready;  /* 1: set_ready(1) on soft-return prepare (MW) */
  SnesHostFillMatchCapsFn fill_match_caps; /* NULL → delay=2, no ws */
  void *caps_ctx;
} SnesHostLobbyOpts;

/* Init once before first launcher open. Returns 0 on success. */
int snes_host_lobby_init(const SnesHostLobbyIdentity *id,
                         const SnesHostLobbyOpts *opts);
void snes_host_lobby_shutdown(void);

#if defined(RECOMP_LAUNCHER) || defined(SNES_HOST_HAS_RECOMP_UI)
/* Stable callback table for RecompLauncherCGameInfo.netplay. */
const RecompLauncherCNetplayCallbacks *snes_host_lobby_callbacks(void);
#endif

/* Soft-return: un-start LAN, clear launch pending, apply rematch ready policy. */
void snes_host_lobby_prepare_rematch(void);

/* Leave LAN + MotK seats (does not disconnect WebSocket). */
int snes_host_lobby_leave(void);

/* Full disconnect (leave + snes_lobby_disconnect). */
void snes_host_lobby_disconnect(void);

/* Optional LAN resume endpoint for gi.resume_netplay_endpoint (may be empty). */
const char *snes_host_lobby_resume_endpoint(void);

int snes_host_lobby_in_lan(void);

#ifdef __cplusplus
}
#endif

#endif /* SNES_HOST_LOBBY_H */

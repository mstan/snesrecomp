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

/*
 * "Is a SIM-AFFECTING mod feature on locally, beyond what `ruleset_id` itself
 * imposes?" Answered by the GAME, because only the game knows which of its
 * features touch the sim and which are presentation.
 *
 * It becomes the ticket's `mods_enabled` assertion. A true is refused by the
 * server with mods_not_pooled, which is correct: a player with an extra
 * sim-affecting feature on boots a different sim from a vanilla opponent, and
 * that is a desync rather than a fairness complaint.
 *
 * Note the "beyond what the ruleset imposes". A ruleset that pins, say, a
 * widescreen margin has BOTH peers running that patched sim on the server's
 * instruction -- it is the ruleset, not a divergence, and answering 1 for it
 * would make the queue permanently unusable for the very feature it exists to
 * standardize. `ruleset_id` is passed so the game can make that distinction;
 * NULL/"" means the first ruleset.
 *
 * NULL leaves the assertion at 0, which is right for a game with no
 * sim-affecting mods at all.
 */
/* `why` receives a player-facing reason when the answer is 1 -- the FEATURE in
 * the way, not the fact that something is. "Turn off sim-affecting mods" sends
 * a player to a Mods page with several toggles and no indication which one
 * matters; naming it is the difference between a fixable message and a
 * mystery. May be left empty. */
typedef int (*SnesHostModsEnabledFn)(void *ctx, const char *ruleset_id,
                                     char *why, size_t why_cap);

typedef struct SnesHostLobbyOpts {
  int auto_ready_guests;  /* 1: set_ready(1) for non-hosts in Pump (SMW) */
  int rematch_set_ready;  /* 1: set_ready(1) on soft-return prepare (MW) */
  SnesHostFillMatchCapsFn fill_match_caps; /* NULL → delay=2, no ws */
  void *caps_ctx;
  /* Appended: a runner built before this field zero-fills it and asserts 0. */
  SnesHostModsEnabledFn mods_enabled;
  void *mods_ctx;
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

/* Surface a game-session failure when recomp-ui resumes the waiting room
 * (prefer over inventing a second error channel in each title). */
void snes_host_lobby_set_runtime_error(const char *error_code);

#if defined(RECOMP_LAUNCHER) || defined(SNES_HOST_HAS_RECOMP_UI)
/*
 * Headless MotK self-test (no ImGui). Leaves the lobby WebSocket open after
 * success so ICE signaling can continue. Requires snes_host_lobby_init first.
 * Returns 0 on launch filled into *out; negative stage codes on failure.
 */
int snes_host_lobby_auto_launch(const char *role, const char *player_name,
                                const char *lobby_name, unsigned timeout_ms,
                                RecompLauncherCNetplayLaunch *out);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SNES_HOST_LOBBY_H */

#ifndef SNES_LOBBY_CLIENT_H
#define SNES_LOBBY_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNES_LOBBY_ID_LEN 40
#define SNES_LOBBY_NAME_LEN 64
#define SNES_LOBBY_VERSION_LEN 32
#define SNES_LOBBY_ENDPOINT_LEN 64
#define SNES_LOBBY_MAX_LIST 32
#define SNES_LOBBY_MAX_MEMBERS 4

#ifndef SNES_GAME_VERSION
#ifdef SNESRECOMP_BUILD_VERSION
#define SNES_GAME_VERSION SNESRECOMP_BUILD_VERSION
#else
#define SNES_GAME_VERSION "dev"
#endif
#endif

typedef struct SnesLobbyRow {
    char     lobby_id[SNES_LOBBY_ID_LEN];
    char     name[SNES_LOBBY_NAME_LEN];
    char     game_name[SNES_LOBBY_NAME_LEN];
    char     game_version[SNES_LOBBY_VERSION_LEN];
    int      player_count;
    int      max_slots;
    int      has_password;
} SnesLobbyRow;

typedef struct SnesLobbyMember {
    int  slot;
    char player_id[SNES_LOBBY_ID_LEN];
    char display_name[SNES_LOBBY_NAME_LEN];
    int  ready;
} SnesLobbyMember;

/*
 * Host-authoritative sim settings negotiated over the lobby.
 * Guests apply these on launch so both peers boot with matching caps.
 */
typedef struct SnesLobbyMatchCaps {
    int  valid;            /* 1 when a host blob was received / set */
    int  widescreen;       /* 0/1 */
    int  widescreen_hud;   /* 0/1 */
    int  ignore_aspect;    /* 0/1 */
    int  input_delay;      /* recomp-net delay frames (0-16, default 2) */
    int  ws_extra;         /* widescreen margin; 0 = game default / env force */
} SnesLobbyMatchCaps;

typedef struct SnesLobbyJoinInfo {
    int      ok;
    char     lobby_id[SNES_LOBBY_ID_LEN];
    uint32_t session_id;
    int      local_slot;
    char     host_endpoint[SNES_LOBBY_ENDPOINT_LEN];
    char     guest_endpoint[SNES_LOBBY_ENDPOINT_LEN];
    char     bind_hostport[SNES_LOBBY_ENDPOINT_LEN];
    char     peer_hostport[SNES_LOBBY_ENDPOINT_LEN];
    int      player_count;
    int      max_slots;
    char     last_error[64]; /* need_password | bad_password | … */
} SnesLobbyJoinInfo;

/* Default URL when SNES_NET_LOBBY_URL unset:
 * ws://netplay.technicallycomputers.ca:8765 */
const char *snes_lobby_default_url(void);

int  snes_lobby_connect(const char *ws_url); /* 0 ok */
void snes_lobby_disconnect(void);
int  snes_lobby_connected(void);

void snes_lobby_set_display_name(const char *name);
const char *snes_lobby_display_name(void);
const char *snes_lobby_player_id(void);

/* Non-blocking pump — call every frame from the launcher. */
void snes_lobby_pump(void);

/* Title + release pin used for create/join matching and list filters. */
void snes_lobby_set_game_identity(const char *game_name,
                                  const char *game_version);
const char *snes_lobby_game_version(void);

void snes_lobby_request_list(void);
int  snes_lobby_list_count(void);
int  snes_lobby_list_get(int index, SnesLobbyRow *out);

/*
 * Create lobby. host_bind e.g. "0.0.0.0:7777". password may be NULL/empty.
 * match_caps may be NULL (legacy); when non-NULL and valid, sent to the server
 * so guests join with the host's sim settings.
 * Returns 0 if request sent; poll snes_lobby_join_info() / in_lobby().
 */
int  snes_lobby_create(const char *name, const char *game_name,
                      const char *game_version, const char *password,
                      const char *host_bind,
                      const SnesLobbyMatchCaps *match_caps);

/* Join lobby. guest_bind may be NULL/empty/"host:0" — the client always
 * advertises a concrete UDP bind (prefers 7778..) so server-hosted launches
 * never hand the host peer_ip:0 (rnet_session_start_lan rejects port 0). */
int  snes_lobby_join(const char *lobby_id, const char *password,
                    const char *guest_bind);

int  snes_lobby_leave(void);

/* Host: remove the player seated in `slot` (not the host). Returns 0 if sent. */
int  snes_lobby_kick(int slot);

/* Host: swap (or move into empty) seats. Returns 0 if sent; server broadcasts
 * lobby_update. LAN file-registry hosts handle seat flips in the game callback. */
int  snes_lobby_move(int from_slot, int to_slot);

int  snes_lobby_in_lobby(void);
int  snes_lobby_is_host(void);
/* Lobby host's player_id (stable across seat swaps); empty if unknown. */
const char *snes_lobby_host_player_id(void);
/* Filled after create/join/lobby_update; peer endpoints for PsxNetplayConfig. */
const SnesLobbyJoinInfo *snes_lobby_join_info(void);

/* Latest host match_caps (valid==0 until create/join/launch delivers one). */
const SnesLobbyMatchCaps *snes_lobby_match_caps(void);

/* Host: push updated caps while in lobby (clears ready via lobby_update). */
int  snes_lobby_set_match_caps(const SnesLobbyMatchCaps *caps);

/* Live member table from lobby_update (and create/join). */
int  snes_lobby_member_count(void);
int  snes_lobby_member_get(int index, SnesLobbyMember *out);

/* True when member.player_id matches snes_lobby_host_player_id().
 * Prefer this over `slot == 0` — seats can move. */
int  snes_lobby_member_is_host(const SnesLobbyMember *member);

/* Local ready flag (from last lobby_update matching our player_id). */
int  snes_lobby_local_ready(void);
/* True when every seated player is ready and player_count >= 2. */
int  snes_lobby_all_ready(void);

/* Toggle ready in the current lobby. */
int  snes_lobby_set_ready(int ready);

/*
 * Host: ask server to broadcast launch. When match_caps is non-NULL and valid,
 * it is attached to start so launch freezes the latest host settings.
 */
int  snes_lobby_request_start(const SnesLobbyMatchCaps *match_caps);

/*
 * Set when server sends op:launch. Both host and guests should boot netplay.
 * Cleared by snes_lobby_clear_launch_pending() after consuming.
 */
int  snes_lobby_launch_pending(void);
void snes_lobby_clear_launch_pending(void);
void snes_lobby_clear_last_error(void);

/* After op:launch: copy seating endpoints into *out when launch_pending and
 * bind/peer are usable. Returns 1 if filled. Does not clear launch_pending.
 * Games wire this from RecompLauncherCNetplayCallbacks.fill_launch. */
int  snes_lobby_try_fill_launch(SnesLobbyJoinInfo *out);

/*
 * ICE signaling relay (MotK WS op:signal). text is SDP/candidate (max 2047).
 * send returns 0 if queued/written; poll returns 1 when an inbound signal was
 * copied out (LOCAL_* types as emitted by the peer — remap to REMOTE_* before
 * rnet_session_push_signal).
 */
int  snes_lobby_send_signal(int type, int flag, const char *text);
int  snes_lobby_poll_signal(int *type, int *flag, char *text, size_t text_cap);

#ifdef __cplusplus
}
#endif

#endif /* SNES_LOBBY_CLIENT_H */

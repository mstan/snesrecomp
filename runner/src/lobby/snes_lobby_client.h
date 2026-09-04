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

/* One package on the lobby wire -- a row of the host's required plan, or of a
 * peer's offer of what it already has. Deliberately independent of the mod
 * runtime's own types: the lobby transports package identity and knows nothing
 * about what a package contains. */
#define SNES_LOBBY_MAX_MODS      16
#define SNES_LOBBY_MOD_ID_LEN    96
#define SNES_LOBBY_MOD_VER_LEN   32
#define SNES_LOBBY_MOD_NAME_LEN  64
#define SNES_LOBBY_MOD_FEATS_LEN 192

typedef struct SnesLobbyModPkg {
    char id[SNES_LOBBY_MOD_ID_LEN];
    char ver[SNES_LOBBY_MOD_VER_LEN];
    /* Display name, for a player reading the list. Empty on an offer row. */
    char name[SNES_LOBBY_MOD_NAME_LEN];
    /* Comma-separated ids of the features the host enabled. Empty on an offer
     * row, which claims possession only. Never part of the seat decision --
     * the server matches on (id, ver). */
    char feats[SNES_LOBBY_MOD_FEATS_LEN];
} SnesLobbyModPkg;

/* Fills `out` with the packages this peer already has, returning the count.
 *
 * Installed by the host layer, because the lobby transports package identity
 * and deliberately knows nothing about what a package is or where it lives. */
typedef int (*SnesLobbyModOfferFn)(SnesLobbyModPkg *out, int max, void *ctx);

/* Without a supplier this peer announces nothing, and every other peer reads
 * that as "has no mods" -- so the host's launch gate holds the match. That is
 * the safe direction (held rather than started into a desync), but it is only
 * the TRUE answer if the peer genuinely has nothing. Any build with a mod
 * runtime must install one. */
void snes_lobby_set_mod_offer_supplier(SnesLobbyModOfferFn fn, void *ctx);

/* The last need_mods refusal: how many packages the server said were missing,
 * and each row. Valid until the next join attempt. `can_transfer` is the
 * server's claim that a transfer channel is available -- it is the server's
 * opinion about ITSELF, not a promise that this build can drive one. */
int snes_lobby_need_mods_count(void);
const SnesLobbyModPkg *snes_lobby_need_mods_get(int index);
int snes_lobby_need_mods_can_transfer(void);

/* The launch gate. Returns how many plan packages the OTHER seated peers are
 * missing (0 = the match may start), naming the first offender and the first
 * package they lack. Joining is deliberately not gated on this -- a peer
 * without the mods belongs in the lobby, where they can get them. */
int snes_lobby_match_blocked_by_mods(char *who, size_t who_cap,
                                     char *what, size_t what_cap);

/* How many plan packages THIS peer is missing, as the host's published plan
 * and our own announced offer see it. */
int snes_lobby_local_missing_mods(void);

/*
 * Host-authoritative sim settings negotiated over the lobby.
 * Guests apply these on launch so both peers boot with matching caps.
 */
typedef struct SnesLobbyMatchCaps {
    int  valid;            /* 1 when a host blob was received / set */
    int  widescreen;       /* 0/1 */
    int  widescreen_hud;   /* 0/1 */
    int  ignore_aspect;    /* 0/1 */
    int  input_delay;      /* recomp-net delay frames (2-20 typical; default 2) */
    int  ws_extra;         /* widescreen margin; 0 = game default / env force */
    int  force_turn;       /* 0/1 — host: ICE relay-only (TURN) for all peers */
    int  force_input_relay; /* 0/1 — lobby-server UDP input relay */
    int  rollback;         /* 0/1 — session mode; lobby default ON */
    /* The host's required mod plan, one row per PACKAGE.
     *
     * Host-authoritative like everything else here: mods patch guest memory,
     * so a peer running a different set is running a different game.
     *
     * Published as `match_caps.mod_plan`, deliberately NOT as `match_caps.mods`.
     *
     * `mods` is the key the lobby server enforces: required_mod_rows() reads it
     * and REFUSES TO SEAT a joiner whose mod_offer does not cover it. That is
     * the wrong door for this title. A player who lacks a mod should still get
     * into the room -- to see what is needed, to talk to the host, and above
     * all to download it from them. What must not happen is the MATCH starting
     * while the peers would simulate differently, and that gate belongs at
     * launch, where it can be enforced against every seated peer rather than
     * against one joiner at the moment they knock.
     *
     * The server passes unknown match_caps keys through untouched and echoes
     * them to members, so `mod_plan` reaches every peer while leaving the seat
     * gate inert. It is a policy difference, not a workaround: this title says
     * who may PLAY, the server says who may SIT.
     *
     * Encoded as an array of objects either way. An older encoding put the plan
     * in a ';'-separated STRING, which is valid JSON but not an array, so
     * anything reading it with as_array() got nothing -- and read a host with a
     * full plan as a host requiring nothing. */
    int mod_count;
    SnesLobbyModPkg mods[SNES_LOBBY_MAX_MODS];
    /* The host's EFFECTIVE set: one entry per enabled feature with its
     * resolved option values, ';' where the canonical text has newlines
     * (json_get_str does not unescape, so the text cannot carry them).
     *
     * The plan above says which packages a peer must HAVE. This says how the
     * host has them configured, which is a different and stricter question --
     * and the one the match actually enforces. A guest that owns both mods but
     * has enabled neither passes the plan and is still refused at launch,
     * because the two would simulate differently.
     *
     * Carried so a guest can match the host BEFORE launching rather than
     * discovering the difference from a refused match. */
    char mod_set[512];
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
 * ws://netplay.retcomm.net:8765 */
const char *snes_lobby_default_url(void);

int  snes_lobby_connect(const char *ws_url); /* 0 ok */
void snes_lobby_disconnect(void);
int  snes_lobby_connected(void);
/* Current WS URL when connected (else empty string). */
const char *snes_lobby_url(void);

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
                      const SnesLobbyMatchCaps *match_caps,
                      int max_slots);

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

/* Waiting-room RTT to the lobby host in ms for `slot`, or -1 if unknown.
 * Host's own seat is always -1. Guests measure via signal ping; hosts learn
 * guest RTT from peer reports. */
int  snes_lobby_member_latency_ms(int slot);

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
/* Addressed variant. An empty/NULL to_player_id broadcasts to the other
 * seated members, which is what the game's own ICE wants; a point-to-point
 * exchange must name its peer. */
int  snes_lobby_send_signal_to(const char *to_player_id, int type, int flag,
                              const char *text);
int  snes_lobby_send_signal(int type, int flag, const char *text);

/* ---- peer-to-peer mod transfer -----------------------------------------
 *
 * The package travels over its own ICE agent, directly between the two
 * players. The lobby server relays only SDP and candidate lines, on the same
 * seated `signal` channel the room already uses -- no file byte passes
 * through it, and it needs no support for any of this.
 *
 * Hooks rather than direct calls into the mod runtime: the lobby transports
 * package identity and bytes, and knows nothing about what a package is.
 * export writes a self-contained archive plus its digest; install is handed
 * that digest and must verify BEFORE unpacking. */
typedef int (*SnesLobbyModExportFn)(const char *package_id, const char *version,
                                    uint8_t **out, uint32_t *out_len,
                                    char *sha256_hex, uint32_t sha_cap,
                                    char *err, uint32_t err_cap, void *ctx);
typedef void (*SnesLobbyModFreeFn)(uint8_t *blob);
typedef int (*SnesLobbyModInstallFn)(const uint8_t *data, uint32_t len,
                                     const char *expect_sha256,
                                     char *installed_id, uint32_t id_cap,
                                     char *installed_ver, uint32_t ver_cap,
                                     char *err, uint32_t err_cap, void *ctx);
void snes_lobby_set_mod_transfer_hooks(SnesLobbyModExportFn export_fn,
                                       SnesLobbyModFreeFn free_fn,
                                       SnesLobbyModInstallFn install_fn,
                                       void *ctx);

/* Guest: ask the host for one package. 0 = asked. */
int  snes_lobby_mod_request(const char *package_id, const char *version);
void snes_lobby_mod_cancel(void);
/* -1 idle, -2 failed, 0..100 in flight. */
int  snes_lobby_mod_progress(void);
int  snes_lobby_mod_failed(char *err, size_t err_cap);
/* Package id currently moving, or "" when idle. */
const char *snes_lobby_mod_in_flight(void);
/* Pumped from snes_lobby_pump; exposed for hosts that drive their own loop. */
void snes_lobby_mod_xfer_pump(void);
int  snes_lobby_poll_signal(int *type, int *flag, char *text, size_t text_cap);

/*
 * Coturn / ICE credentials minted by the WS lobby
 * (`get_turn_credentials` → `turn_credentials`). Valid until disconnect or TTL.
 * Strings are stable until the next successful mint or disconnect — safe to
 * pass into RNetIceConfig for juice_create.
 */
typedef struct SnesLobbyTurnCredentials {
    int      valid; /* 1 when ok mint cached and not expired */
    char     stun_host[128];
    int      stun_port;
    char     turn_host[128];
    int      turn_port;
    char     username[192];
    char     password[128];
    uint32_t ttl_secs;
} SnesLobbyTurnCredentials;

/* Queue WS get_turn_credentials. Returns 0 if sent/queued. */
int  snes_lobby_request_turn_credentials(void);
/* Non-NULL; valid==0 when unavailable / expired / STUN-only. */
const SnesLobbyTurnCredentials *snes_lobby_turn_credentials(void);

#ifdef __cplusplus
}
#endif

#endif /* SNES_LOBBY_CLIENT_H */

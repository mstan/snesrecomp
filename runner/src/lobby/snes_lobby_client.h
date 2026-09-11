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

/* Player seats this title can seat. Was what SNES_LOBBY_MAX_MEMBERS meant
 * before there was anything in a lobby that was not a player. */
#define SNES_LOBBY_MAX_PLAYERS 4
/* Spectator seats a host may open, on top of the players. */
#define SNES_LOBBY_MAX_SPECTATORS 4
/* Rows in the membership table: both tables land in it, tagged by role. */
#define SNES_LOBBY_MAX_MEMBERS \
    (SNES_LOBBY_MAX_PLAYERS + SNES_LOBBY_MAX_SPECTATORS)

/* Seat indices are one namespace, matching the server: below the base is a
 * player seat, at or above it is `index - base` in the gallery. The server
 * republishes its own base in every lobby_update; this is the compiled-in
 * default for the case where it does not. */
#define SNES_LOBBY_SPECTATOR_SLOT_BASE 64

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
    /* Host's country (alpha-2) from the server's GeoIP; "" unknown. */
    char     host_country[4];
    /* Gallery: 0 = none; else how many watch out of how many seats. */
    int      allow_spectators;
    int      max_spectators;
    int      spectator_count;
} SnesLobbyRow;

/* One player connected to the hub (the lobby_list `players` array). */
typedef struct SnesLobbyOnlinePlayer {
    char display_name[SNES_LOBBY_NAME_LEN];
    char country[4];
    char lobby_id[SNES_LOBBY_ID_LEN];
    char lobby_name[SNES_LOBBY_NAME_LEN];
    int  hosting;
    /* First 8 characters of the player's connection id: enough to tell
     * "which row is me" without publishing whole ids to browsers. */
    char tag[12];
    /* The title that player is browsing for; rows of other titles are
     * dropped at parse time when this client has a game identity. */
    char game_name[SNES_LOBBY_NAME_LEN];
} SnesLobbyOnlinePlayer;
#define SNES_LOBBY_MAX_ONLINE 64

/* One lobby chat line.
 *
 * The server echoes every line to everyone in the room INCLUDING the sender,
 * so this ring is the room's order rather than ours -- the client never
 * appends its own send locally, or two peers would disagree about what was
 * said when. */
#define SNES_LOBBY_CHAT_TEXT_LEN 256
#define SNES_LOBBY_CHAT_RING 64
typedef struct SnesLobbyChatMsg {
    char     player_id[SNES_LOBBY_ID_LEN];
    char     from[SNES_LOBBY_NAME_LEN];
    char     text[SNES_LOBBY_CHAT_TEXT_LEN];
    int      is_local;
    int      is_system;
    uint32_t seq;
} SnesLobbyChatMsg;

typedef struct SnesLobbyMember {
    /* Seat index in the shared namespace: a player seat, or
     * spectator_slot_base + gallery index. Pass it back to kick / move as-is. */
    int  slot;
    char player_id[SNES_LOBBY_ID_LEN];
    char display_name[SNES_LOBBY_NAME_LEN];
    int  ready;
    /* 1 when this row is in the gallery. Read this rather than comparing
     * `slot` against a base: the base is the server's to choose. */
    int  is_spectator;
    /* Country (alpha-2) from the server's GeoIP; "" unknown. */
    char country[4];
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
    /* Which packages this match's AUTHORITY grants the presentation-only
     * exemption to: ';'-separated `id@version` or `id@version#sha256`.
     *
     * A mod declaring `presentation_only` in its own manifest is only asking.
     * This is the grant, and it comes from the server (in an automatch
     * ruleset) or from the host (in a lobby) -- never from the machine that
     * wants the exemption, because a cheat author writes manifests too.
     *
     * EMPTY MEANS NOTHING IS EXEMPT, which is what an older host and a
     * ruleset without the key both produce, and is the answer that fails
     * safe: an ungranted claim is treated as an ordinary simulation mod, so
     * it must match or be switched off.
     *
     * Names bytes, not just a string, when the `#sha256` form is used --
     * otherwise the list is keyed on an id the client picks for itself, and a
     * mod gets exempted by calling itself something allowlisted. */
    char mod_cosmetic_allow[512];
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
    /* 1 when this client holds a gallery seat: it runs the match and shows it,
     * and contributes no input to anyone. */
    int      local_is_spectator;
    /* Host opt-in, echoed by the server. 0 when the server predates
     * spectators, which is why every spectator control is gated on it. */
    int      allow_spectators;
    int      max_spectators;
    int      spectator_count;
    int      spectator_slot_base;
    /* Where the gallery starts in the RELAY's slot space -- a different
     * namespace from the lobby seat index above, and the one the engine needs.
     * The relay forwards nothing from a slot at or beyond its player count, so
     * this is the number that makes a spectator unable to send. */
    int      spectator_relay_base;
    /* This match's transport, as the LAUNCH stated it: 1 = the lobby server
     * allocated its UDP input relay and everyone dials it, 0 = peer-to-peer.
     *
     * Deliberately not read back out of match_caps at start time. The caps
     * copy of this flag is also written by the HOST's published caps, where it
     * means the host's UI toggle, so an unrelated republish landing between
     * the launch and the start silently reverted a relayed match to p2p. It is
     * recorded here, once per launch, by the only message that knows. */
    int      force_input_relay;
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

/* Title + release pin used for create/join matching and list filters.
 *
 * SNES_NET_GAME_VERSION overrides the pin for one run, for TESTING two
 * development builds against each other without reconfiguring and rebuilding
 * both. Set the same string on both machines. It is announced on stderr with
 * the real pin beside it every time the identity is set, because a build
 * lying about which build it is has to be visible in the log a desync report
 * is written from. Unset (the normal case) keeps the derived pin, which
 * matches only builds that really are the same build. */
void snes_lobby_set_game_identity(const char *game_name,
                                  const char *game_version);
/* The pin actually presented on the wire -- the override when one is set. */
const char *snes_lobby_game_version(void);

void snes_lobby_request_list(void);
int  snes_lobby_list_count(void);
int  snes_lobby_list_get(int index, SnesLobbyRow *out);
/* Players connected to the hub, refreshed with every lobby_list. */
int  snes_lobby_online_count(void);
int  snes_lobby_online_get(int index, SnesLobbyOnlinePlayer *out);

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

/* ---- spectators --------------------------------------------------------
 *
 * A gallery seat runs the match locally, in sync, and contributes nothing.
 * The server enforces the "contributes nothing" half at the relay; this side
 * enforces the rest -- no Ready vote, no controller reaching the sim.
 *
 * Every one of these reads 0 against a server that predates spectators, so a
 * caller that gates its UI on allow_spectators degrades to the old lobby. */

/* Host: open a gallery on the NEXT create. Sticky, like the max_slots
 * default -- create carries whatever this was last set to. */
void snes_lobby_set_allow_spectators(int allow);
/* What the toggle is set to, for the UI to render. Distinct from the next
 * call: this is what the host ASKED for and has not created yet. */
int  snes_lobby_allow_spectators_pref(void);
/* What the current lobby actually has (server-echoed), not what was asked.
 * These disagree whenever the server predates spectators -- which is exactly
 * the case the UI must not offer a gallery in. */
int  snes_lobby_allow_spectators(void);
int  snes_lobby_max_spectators(void);
int  snes_lobby_spectator_count(void);
/* 1 when THIS client is in the gallery. The one call the engine needs. */
int  snes_lobby_local_is_spectator(void);
/* Base of the gallery half of the seat namespace, as the server reports it. */
int  snes_lobby_spectator_slot_base(void);
/* 1 when `slot` is a seat the server would accept in move / kick. */
int  snes_lobby_seat_valid(int slot);
/* Seat index for gallery position `index`, for move / kick. */
int  snes_lobby_spectator_slot(int index);
/* This client's slot in the RELAY's namespace, for RNetConfig.wire_slot.
 * -1 when this client is not a spectator or the server published no relay
 * base -- and a spectator without one must not launch, because sending as a
 * player slot is exactly what it must not do. */
int  snes_lobby_local_wire_slot(void);

/* ---- lobby chat --------------------------------------------------------
 * Spectators take part: the server fans a line out to everyone in the room,
 * and talking is not affecting the match. */
int  snes_lobby_send_chat(const char *text);

/* Server chat: per-game, outside any room (op server_chat). Its own ring,
 * cleared on disconnect; lines are masked on arrival like lobby chat. */
int  snes_lobby_send_server_chat(const char *text);
int  snes_lobby_server_chat_count(void);
int  snes_lobby_server_chat_get(int index, SnesLobbyChatMsg *out);

/* Seat self-service (server ops seat_move / seat_swap_request /
 * seat_swap_answer): move yourself to a FREE player seat, or ask the
 * occupant of a taken one to trade. incoming: 1 while somebody is asking
 * this player (who + their seat); respond answers it. outgoing: 0 idle,
 * 1 waiting, 2 accepted, -1 declined; clear returns a finished result to 0. */
int  snes_lobby_seat_move_self(int to_slot);
int  snes_lobby_seat_swap_request(int target_slot);
int  snes_lobby_seat_swap_incoming(char *who, size_t who_cap, int *from_slot);
int  snes_lobby_seat_swap_respond(int accept);
int  snes_lobby_seat_swap_outgoing(void);
void snes_lobby_seat_swap_clear(void);
int  snes_lobby_chat_count(void);
int  snes_lobby_chat_get(int index, SnesLobbyChatMsg *out);
void snes_lobby_chat_clear(void);

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

/*
 * A relayed transfer spends the TURN server's bandwidth, not the two players'.
 * A direct pair (host/srflx/prflx) costs nobody anything and is left uncapped;
 * a relayed pair is capped, and a package over the cap is refused with a
 * reason that tells the player to get it from its source instead.
 *
 * Bytes, not megabytes, so the check needs no unit conversion at the call
 * site: 5 MiB.
 */
#define SNES_LOBBY_MOD_RELAY_MAX_BYTES (5u * 1024u * 1024u)

/*
 * May a transfer of `bytes` proceed over an ICE pair of type `path`?
 *
 * `path` is what rnet_ice_xfer_path reports: "host", "srflx", "prflx",
 * "relay", "unknown" or "none". Only "relay" is capped. Returns 1 to allow,
 * 0 to refuse -- and on a refusal writes a player-facing sentence naming the
 * package, its size and the cap.
 *
 * An unknown path ALLOWS. The alternative is refusing transfers on a direct
 * pair we merely failed to name, which breaks the case this cap exists to
 * protect. Callers gate on knowing the path first; see the pump.
 *
 * Pure: no globals, no I/O. Both ends call it -- the host before it queues
 * anything, the guest when the header says how big the payload will be -- so
 * a peer that skips the check on its side still cannot land an oversized
 * relayed package on the other.
 */
int  snes_lobby_mod_relay_size_allows(const char *path, uint64_t bytes,
                                      const char *package_id,
                                      char *reason, size_t reason_cap);

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

/* ── ROM fingerprint ────────────────────────────────────────────────────────
 *
 * Lower-case hex SHA-256 of the guest image this build is actually running,
 * 64 characters. The host sets it once at startup; "" means unknown.
 *
 * In `join` an empty fingerprint means "legacy host, no check" -- a wildcard.
 * That is tolerable between two people who chose each other's room and can
 * talk about it. It is NOT tolerable in a queue: a wildcard there silently
 * pairs a Track-01-only dump against a full one, which is exactly what the
 * fingerprint was added to catch, so automatch refuses to queue without a
 * real one.
 */
void snes_lobby_set_disc_fp(const char *hex64);
const char *snes_lobby_disc_fp(void);

/* ── Automatch ──────────────────────────────────────────────────────────────
 *
 * Server-run pairing: the player asks for a match and the SERVER creates the
 * room, is its host, and owns its match_caps from a named ruleset. Protocol:
 * recomp-net-server docs/AUTOMATCH.md. Everything downstream of the pairing is
 * the ordinary `joined` / `lobby_update` / `launch` path already in this file,
 * so this adds a queue and an accept gate and nothing else.
 *
 * Requires a signed-in account: the accept gate charges a dodge cooldown, and
 * a cost that reconnecting erases is not a cost.
 */
#define SNES_LOBBY_MAX_RULESETS 8
#define SNES_LOBBY_RULESET_ID_LEN 48
#define SNES_LOBBY_RULESET_LABEL_LEN 64
#define SNES_LOBBY_CAPS_SUMMARY_LEN 96

/* One queue type this server offers for this title. */
typedef struct SnesLobbyRuleset {
    char id[SNES_LOBBY_RULESET_ID_LEN];
    char label[SNES_LOBBY_RULESET_LABEL_LEN];
    /* Server-derived one-liner ("Delay 2 - Rollback on"). Derived from the
     * caps rather than authored, so it cannot drift from what the match runs. */
    char caps_summary[SNES_LOBBY_CAPS_SUMMARY_LEN];
    /* The caps the match will run. Parsed like a host's blob, because that is
     * exactly what it is -- the server is the host of an automatch room. */
    SnesLobbyMatchCaps caps;
    /* Empty = any release may queue (still only pooling with its own). */
    char game_version[SNES_LOBBY_VERSION_LEN];
} SnesLobbyRuleset;

/* Matches RECOMP_LAUNCHER_AUTOMATCH_* so the host layer can hand these
 * straight to the launcher without a translation table that could drift. */
enum {
    SNES_LOBBY_AUTOMATCH_IDLE = 0,
    SNES_LOBBY_AUTOMATCH_QUEUED = 1,
    SNES_LOBBY_AUTOMATCH_FOUND = 2,
    SNES_LOBBY_AUTOMATCH_ACCEPTED = 3,
    SNES_LOBBY_AUTOMATCH_FAILED = 4
};

/* The offer on the table while state is FOUND. */
typedef struct SnesLobbyAutomatchFound {
    char opponent[SNES_LOBBY_NAME_LEN];
    char opponent_country[8];
    char ruleset_id[SNES_LOBBY_RULESET_ID_LEN];
    char ruleset_label[SNES_LOBBY_RULESET_LABEL_LEN];
    int  est_rtt_ms;      /* the pair's estimate: rtt_a + rtt_b */
    int  accept_secs;     /* seconds left to answer */
} SnesLobbyAutomatchFound;

/* Ask the server what queues it offers for this title. Answered into
 * snes_lobby_ruleset_count/get; 0 of them means automatch is off here. */
/*
 * One simulation-state fork, as reported to the server.
 *
 * Evidence, not a verdict: both digests, from both peers independently. See
 * snes_lobby_report_desync for why a fork is never an accusation on its own.
 */
typedef struct SnesLobbyDesyncReport {
    uint32_t tick;
    const char *partition;   /* "wram", "apu", "ppu", "post", "other" */
    uint32_t mine;           /* local master digest */
    uint32_t theirs;         /* the peer's, as received */
    int is_host;
    /* The cosmetic exemptions this peer was running, so a report can be read
     * beside what the match approved. ';'-separated `id@version#sha256`. */
    const char *mod_exempt;
} SnesLobbyDesyncReport;

int  snes_lobby_report_desync(const SnesLobbyDesyncReport *r);

int  snes_lobby_automatch_request_rulesets(void);
int  snes_lobby_automatch_available(void);
int  snes_lobby_automatch_ruleset_count(void);
int  snes_lobby_automatch_ruleset_get(int index, SnesLobbyRuleset *out);

/*
 * Join the queue. `ruleset_id` NULL/"" takes the first one.
 *
 * `mods_enabled` is the caller's assertion that a SIM-AFFECTING mod feature is
 * on locally, beyond whatever the ruleset itself imposes. The server refuses a
 * true with mods_not_pooled and cannot check it -- the assertion exists so a
 * modified client makes a deliberate false statement rather than exploiting an
 * omission (AUTOMATCH.md §5). The host layer decides it; this file only
 * carries it.
 *
 * 0 = sent. <0 = refused locally (not connected, no account, already queued).
 * A server refusal arrives asynchronously as state FAILED with a reason in
 * snes_lobby_automatch_error().
 */
/*
 * `mod_exempt` is the EVIDENCE for every cosmetic exemption this build is
 * taking: ';'-separated `id@version#sha256`, from
 * snes_mod_runtime_exempted_packages_c. NULL or "" means none.
 *
 * It is sent so the SERVER can check those exemptions against its own
 * allowlist. `mods_enabled` beside it is still this client's own verdict and
 * still refused when true -- the two are layers, not alternatives: an old
 * server ignores the evidence and the boolean keeps working, a new server
 * checks the evidence and stops trusting the boolean to be the whole story.
 */
int  snes_lobby_automatch_queue(const char *ruleset_id, int mods_enabled,
                                const char *mod_exempt);
int  snes_lobby_automatch_cancel(void);
int  snes_lobby_automatch_state(void);
int  snes_lobby_automatch_queued_secs(void);
int  snes_lobby_automatch_pool(void);
int  snes_lobby_automatch_found_get(SnesLobbyAutomatchFound *out);
/* accept != 0 accepts; 0 declines and takes the dodge strike. */
int  snes_lobby_automatch_accept(int accept);
/* Refuse a queue locally, with the reason the player sees. Used when the HOST
 * already knows the server would bounce the ticket -- there is no value in a
 * round trip that ends in a generic code when the client can name the actual
 * feature that is in the way. */
/*
 * Non-zero while the room this client is seated in was created by AUTOMATCH.
 *
 * The distinction matters on the way OUT of a match. A human-hosted room is
 * somebody's room: it survives the match, and staying seated is how a rematch
 * happens. An automatch room is the server's, it is created at both-accept
 * and is not joinable by anyone, and there is no host to rematch with -- so
 * staying in it leaves the player parked in a room that can never fill, and
 * the server refuses their next ticket with `already_in_lobby`.
 *
 * Cleared by leaving, and by anything else that ends the seating.
 */
int snes_lobby_automatch_room(void);
void snes_lobby_automatch_refuse_local(const char *why);
const char *snes_lobby_automatch_error(void);

#ifdef __cplusplus
}
#endif

#endif /* SNES_LOBBY_CLIENT_H */

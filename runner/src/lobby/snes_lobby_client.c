#include "snes_lobby_client.h"

#include "recomp_net/ice_xfer.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(SNES_HAS_LOBBY_CLIENT)

const char *snes_lobby_default_url(void)
{
    return "ws://netplay.retcomm.net:8765";
}
int  snes_lobby_connect(const char *ws_url) { (void)ws_url; return -1; }
void snes_lobby_disconnect(void) {}
int  snes_lobby_connected(void) { return 0; }
const char *snes_lobby_url(void) { return ""; }
void snes_lobby_set_display_name(const char *name) { (void)name; }
const char *snes_lobby_display_name(void) { return ""; }
const char *snes_lobby_player_id(void) { return ""; }
void snes_lobby_pump(void) {}
void snes_lobby_request_list(void) {}
int  snes_lobby_list_count(void) { return 0; }
int  snes_lobby_list_get(int index, SnesLobbyRow *out) { (void)index; (void)out; return 0; }
void snes_lobby_set_game_identity(const char *a, const char *b) { (void)a; (void)b; }
const char *snes_lobby_game_version(void) { return SNES_GAME_VERSION; }
int  snes_lobby_create(const char *a, const char *b, const char *c, const char *d,
                       const char *e, const SnesLobbyMatchCaps *f, int max_slots)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)max_slots; return -1; }
int  snes_lobby_join(const char *a, const char *b, const char *c)
{ (void)a; (void)b; (void)c; return -1; }
int  snes_lobby_leave(void) { return -1; }
int  snes_lobby_kick(int slot) { (void)slot; return -1; }
int  snes_lobby_move(int from_slot, int to_slot)
{ (void)from_slot; (void)to_slot; return -1; }
int  snes_lobby_in_lobby(void) { return 0; }
int  snes_lobby_is_host(void) { return 0; }
const char *snes_lobby_host_player_id(void) { return ""; }
const SnesLobbyJoinInfo *snes_lobby_join_info(void)
{
    static SnesLobbyJoinInfo z;
    return &z;
}
const SnesLobbyMatchCaps *snes_lobby_match_caps(void)
{
    static SnesLobbyMatchCaps z;
    return &z;
}
void snes_lobby_set_mod_offer_supplier(SnesLobbyModOfferFn fn, void *ctx)
{
    (void)fn; (void)ctx;
}
int  snes_lobby_need_mods_count(void) { return 0; }
const SnesLobbyModPkg *snes_lobby_need_mods_get(int i) { (void)i; return NULL; }
int  snes_lobby_need_mods_can_transfer(void) { return 0; }
int  snes_lobby_send_signal_to(const char *t, int a2, int b, const char *c)
{
    (void)t; (void)a2; (void)b; (void)c; return -1;
}
void snes_lobby_set_mod_transfer_hooks(SnesLobbyModExportFn e,
                                       SnesLobbyModFreeFn f,
                                       SnesLobbyModInstallFn i, void *c)
{
    (void)e; (void)f; (void)i; (void)c;
}
int  snes_lobby_mod_request(const char *a2, const char *b)
{
    (void)a2; (void)b; return -1;
}
void snes_lobby_mod_cancel(void) {}
int  snes_lobby_mod_progress(void) { return -1; }
int  snes_lobby_mod_failed(char *e, size_t c) { (void)e; (void)c; return 0; }
const char *snes_lobby_mod_in_flight(void) { return ""; }
void snes_lobby_mod_xfer_pump(void) {}
int  snes_lobby_match_blocked_by_mods(char *w, size_t wc, char *t, size_t tc)
{
    (void)w; (void)wc; (void)t; (void)tc; return 0;
}
int  snes_lobby_local_missing_mods(void) { return 0; }
int  snes_lobby_set_match_caps(const SnesLobbyMatchCaps *c) { (void)c; return -1; }
int  snes_lobby_member_count(void) { return 0; }
int  snes_lobby_member_get(int index, SnesLobbyMember *out) { (void)index; (void)out; return 0; }
int  snes_lobby_member_latency_ms(int slot) { (void)slot; return -1; }
int  snes_lobby_member_is_host(const SnesLobbyMember *member)
{
    (void)member;
    return 0;
}
int  snes_lobby_local_ready(void) { return 0; }
int  snes_lobby_all_ready(void) { return 0; }
int  snes_lobby_set_ready(int ready) { (void)ready; return -1; }
int  snes_lobby_request_start(const SnesLobbyMatchCaps *c) { (void)c; return -1; }
int  snes_lobby_launch_pending(void) { return 0; }
void snes_lobby_clear_launch_pending(void) {}
void snes_lobby_clear_last_error(void) {}
int  snes_lobby_try_fill_launch(SnesLobbyJoinInfo *out)
{
    (void)out;
    return 0;
}
int  snes_lobby_send_signal(int type, int flag, const char *text)
{
    (void)type;
    (void)flag;
    (void)text;
    return -1;
}
int  snes_lobby_poll_signal(int *type, int *flag, char *text, size_t text_cap)
{
    (void)type;
    (void)flag;
    (void)text;
    (void)text_cap;
    return 0;
}
int  snes_lobby_request_turn_credentials(void) { return -1; }
const SnesLobbyTurnCredentials *snes_lobby_turn_credentials(void)
{
    static SnesLobbyTurnCredentials z;
    return &z;
}

#else /* SNES_HAS_LOBBY_CLIENT */

#include "rnet_ws.h"
#include "rnet_sha1.h"
#include "recomp_net/address.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static int socket_would_block(void)
{
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

typedef struct {
    int fd;
    int connected;
    int handshake_done;
    char player_id[SNES_LOBBY_ID_LEN];
    char display_name[SNES_LOBBY_NAME_LEN];
    char host[128];
    int port;
    char path[128];
    char url[256]; /* full WS URL passed to connect */
    char rx_http[4096];
    size_t rx_http_len;
    /* Bytes that arrived with the HTTP 101 response after the header end. */
    uint8_t ws_pending[4096];
    size_t ws_pending_len;
    SnesLobbyRow list[SNES_LOBBY_MAX_LIST];
    int list_count;
    int in_lobby;
    int is_host;
    char host_player_id[SNES_LOBBY_ID_LEN];
    char my_bind[SNES_LOBBY_ENDPOINT_LEN];
    char filter_game_name[SNES_LOBBY_NAME_LEN];
    char filter_game_version[SNES_LOBBY_VERSION_LEN];
    SnesLobbyJoinInfo join;
    SnesLobbyMember members[SNES_LOBBY_MAX_MEMBERS];
    int member_count;
    int local_ready;
    int all_ready;
    int launch_pending;
    SnesLobbyMatchCaps match_caps;
    /* The server's need_mods refusal: what it says we are missing, and the
     * identity of the peer that could supply it. Kept rather than reduced to
     * an error code because "you cannot join" is not actionable without the
     * list. */
    /* What each seated peer says it has, from the server's per-slot echo of
     * their set_ready offer. The input to the launch gate: the host compares
     * every peer against the plan and holds the match until they can run it. */
    SnesLobbyModPkg member_offer[SNES_LOBBY_MAX_MEMBERS][SNES_LOBBY_MAX_MODS];
    int member_offer_count[SNES_LOBBY_MAX_MEMBERS];
    /* Mod transfer: one at a time, in one direction. A second request while
     * one is in flight is refused rather than queued -- two agents on one
     * relay is a harder thing to get right than making the player click
     * again, and the panel shows which row is moving. */
    RNetIceXfer *xfer;
    int   xfer_busy;
    int   xfer_sending;              /* 1 host side, 0 guest side */
    int   xfer_progress;             /* -1 idle, -2 failed, 0..100 */
    char  xfer_peer[SNES_LOBBY_ID_LEN];
    char  xfer_id[SNES_LOBBY_MOD_ID_LEN];
    char  xfer_ver[SNES_LOBBY_MOD_VER_LEN];
    char  xfer_sha[65];
    uint32_t xfer_expect;
    char  xfer_err[192];
    char  ice_stun[128], ice_turn[128], ice_user[192], ice_pass[128];
    /* ICE signals that arrived before the agent existed.
     *
     * The requester opens its agent and starts gathering the moment it asks,
     * so its offer and first candidates are already crossing the relay while
     * the sender is still packing the archive. Dropped, they are simply gone
     * -- libjuice will not re-send them -- and the handshake stalls until it
     * times out. Held here and replayed in arrival order once the agent
     * exists. */
    RNetSignal sig_hold[24];
    int   sig_hold_n;
    char  sig_hold_from[SNES_LOBBY_ID_LEN];
    int      xfer_last_state;    /* last RNetIceState logged */
    uint64_t xfer_started_ms;    /* for the stall watchdog */
    uint64_t xfer_connected_ms;
    SnesLobbyModPkg need_mods[SNES_LOBBY_MAX_MODS];
    int need_mods_count;
    int need_mods_can_transfer;
    char need_mods_lobby_id[SNES_LOBBY_ID_LEN];
    char need_mods_host_player_id[SNES_LOBBY_ID_LEN];
    char pending_tx[8][2048];
    int pending_n;
    /* Inbound ICE signals (WS op:signal). */
    struct {
        int type;
        int flag;
        char text[2048];
    } sig_q[32];
    int sig_head;
    int sig_tail;
    int sig_count;
    /* Coturn mint from WS get_turn_credentials. */
    SnesLobbyTurnCredentials turn;
    time_t turn_received_at;
    int turn_request_pending;
    /* Waiting-room latency (ms) keyed by pad slot; -1 = unknown. */
    int member_rtt_ms[SNES_LOBBY_MAX_MEMBERS];
    uint64_t rtt_next_ping_ms;
} LobbyClient;

enum {
    SNES_LOBBY_SIG_RTT_PING = 100,
    SNES_LOBBY_SIG_RTT_PONG = 101,
    SNES_LOBBY_SIG_RTT_REPORT = 102,

    /* Mod transfer.
     *
     * These ride the ordinary seated `signal` relay, which the lobby server
     * forwards between seated members verbatim -- so a transfer needs no
     * server support at all. The server's own mod_xfer_* ops cannot serve
     * this: it grants that channel only to a peer it REFUSED to seat, and
     * this title seats everyone on purpose.
     *
     * Only SDP and ICE candidates travel here. The package itself goes over
     * the direct peer-to-peer connection those negotiate, so no file byte
     * ever reaches the lobby server. */
    SNES_LOBBY_SIG_MOD_REQ = 110,   /* guest -> host: "<id>@<ver>"           */
    SNES_LOBBY_SIG_MOD_NAK = 111,   /* host -> guest: refusal, text = reason */
    /* 120 + RNetSignalType(1..6): the ICE handshake for the transfer agent.
     * Offset so the game's own netplay ICE, which uses the bare types on the
     * same relay, can never ingest one of ours nor we one of its. */
    SNES_LOBBY_SIG_MOD_ICE_BASE = 120
};

static uint64_t lobby_mono_ms(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000ull +
               (uint64_t)ts.tv_nsec / 1000000ull;
#endif
    return (uint64_t)time(NULL) * 1000ull;
}

/* Defined later; used by waiting-room RTT signal handling. */
int snes_lobby_send_signal(int type, int flag, const char *text);

static LobbyClient g_lc = {
    .fd = -1,
    .filter_game_version = SNES_GAME_VERSION,
};

static void member_rtt_clear(void)
{
    int i;
    for (i = 0; i < SNES_LOBBY_MAX_MEMBERS; ++i)
        g_lc.member_rtt_ms[i] = -1;
    g_lc.rtt_next_ping_ms = 0;
}

static int member_slot_for_player(const char *player_id)
{
    int i;
    if (!player_id || !player_id[0])
        return -1;
    for (i = 0; i < g_lc.member_count; ++i) {
        if (strcmp(g_lc.members[i].player_id, player_id) == 0)
            return g_lc.members[i].slot;
    }
    return -1;
}

static int local_member_slot(void)
{
    return member_slot_for_player(g_lc.player_id);
}

static const char *effective_game_version(const char *override_ver)
{
    if (override_ver && override_ver[0]) return override_ver;
    if (g_lc.filter_game_version[0]) return g_lc.filter_game_version;
    return SNES_GAME_VERSION;
}

static int list_filter_version_strict(void)
{
    /* Any dev build lists UNFILTERED, including the "dev+<sha>" ones.
     *
     * The version pin is still enforced -- the server refuses the join with
     * version_mismatch -- but a filtered list would hide the mismatched lobby
     * instead of explaining it, and "my friend's lobby isn't showing up" is a
     * much worse thing to debug than "this lobby is a different build". Prefix,
     * not equality: dev+abc12345 and dev+abc12345-dirty are both dev. */
    const char *gv = effective_game_version(NULL);
    return gv && gv[0] && strncmp(gv, "dev", 3) != 0;
}

static void queue_send(const char *json);
static void clear_turn_credentials(void);
static int queue_turn_credentials_request(void);

static void clear_turn_credentials(void)
{
    memset(&g_lc.turn, 0, sizeof(g_lc.turn));
    g_lc.turn_received_at = 0;
    g_lc.turn_request_pending = 0;
}

static int queue_turn_credentials_request(void)
{
    if (!snes_lobby_connected())
        return -1;
    queue_send("{\"op\":\"get_turn_credentials\"}");
    g_lc.turn_request_pending = 1;
    return 0;
}

static void queue_list_request(void)
{
    char msg[384];
    const char *gn = g_lc.filter_game_name;
    const char *gv = effective_game_version(NULL);
    if (list_filter_version_strict() && (gn[0] || (gv && gv[0]))) {
        snprintf(msg, sizeof(msg),
                 "{\"op\":\"list\",\"game_name\":\"%s\",\"game_version\":\"%s\"}",
                 gn, gv ? gv : "dev");
        queue_send(msg);
    } else if (gn[0]) {
        snprintf(msg, sizeof(msg), "{\"op\":\"list\",\"game_name\":\"%s\"}", gn);
        queue_send(msg);
    } else {
        queue_send("{\"op\":\"list\"}");
    }
}

static void match_caps_clear(SnesLobbyMatchCaps *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->input_delay = 2;
}

static int json_extract_object(const char *json, const char *key, char *out, size_t out_cap);
static void send_set_ready(int ready);
static void mod_xfer_on_request(const char *from, const char *text);
static void mod_xfer_fail(const char *why);
static int mod_ice_type_for_push(int emitted_type);
static void parse_match_caps_object(const char *obj, SnesLobbyMatchCaps *out);
static void ingest_match_caps_from_json(const char *json);
static int append_match_caps_json(char *dst, size_t dst_cap, const SnesLobbyMatchCaps *caps);

const char *snes_lobby_default_url(void)
{
    const char *e = getenv("SNES_NET_LOBBY_URL");
    return (e && e[0]) ? e : "ws://netplay.retcomm.net:8765";
}

static int parse_ws_url(const char *url, char *host, size_t hcap, int *port, char *path, size_t pcap)
{
    const char *p = url;
    const char *slash;
    char hostport[192];
    char *colon;
    if (!url) {
        return -1;
    }
    if (strncmp(p, "ws://", 5) == 0) {
        p += 5;
    } else if (strncmp(p, "wss://", 6) == 0) {
        return -1; /* TLS not in this phase */
    }
    slash = strchr(p, '/');
    if (slash) {
        size_t n = (size_t)(slash - p);
        if (n >= sizeof(hostport)) {
            n = sizeof(hostport) - 1;
        }
        memcpy(hostport, p, n);
        hostport[n] = '\0';
        strncpy(path, slash, pcap - 1);
        path[pcap - 1] = '\0';
    } else {
        strncpy(hostport, p, sizeof(hostport) - 1);
        hostport[sizeof(hostport) - 1] = '\0';
        strncpy(path, "/", pcap - 1);
    }
    colon = strrchr(hostport, ':');
    if (colon && strchr(hostport, ']') == NULL) {
        *colon = '\0';
        *port = atoi(colon + 1);
        strncpy(host, hostport, hcap - 1);
    } else {
        strncpy(host, hostport, hcap - 1);
        *port = 8765;
    }
    host[hcap - 1] = '\0';
    return 0;
}

static const char *json_get_str(const char *json, const char *key, char *out, size_t cap)
{
    char pat[80];
    const char *p;
    size_t o = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) {
        if (out && cap) {
            out[0] = '\0';
        }
        return NULL;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return NULL;
    }
    ++p;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p != '"') {
        return NULL;
    }
    ++p;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) {
            ++p;
            switch (*p) {
            case 'n': out[o++] = '\n'; break;
            case 'r': out[o++] = '\r'; break;
            case 't': out[o++] = '\t'; break;
            case '"': out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; break;
            case '/': out[o++] = '/'; break;
            default: out[o++] = *p; break;
            }
            ++p;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return out;
}

static size_t json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    if (!in || !out || cap == 0) return 0;
    while (*in && o + 2 < cap) {
        unsigned char c = (unsigned char)*in++;
        if (c == '"' || c == '\\') {
            if (o + 3 >= cap) break;
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            if (o + 3 >= cap) break;
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (c == '\r') {
            if (o + 3 >= cap) break;
            out[o++] = '\\';
            out[o++] = 'r';
        } else if (c == '\t') {
            if (o + 3 >= cap) break;
            out[o++] = '\\';
            out[o++] = 't';
        } else if (c < 0x20) {
            continue;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return o;
}

static void enqueue_signal(int type, int flag, const char *text)
{
    int i;
    if (g_lc.sig_count >= (int)(sizeof(g_lc.sig_q) / sizeof(g_lc.sig_q[0]))) {
        /* Drop oldest. */
        g_lc.sig_head = (g_lc.sig_head + 1) % (int)(sizeof(g_lc.sig_q) / sizeof(g_lc.sig_q[0]));
        g_lc.sig_count--;
    }
    i = g_lc.sig_tail;
    g_lc.sig_q[i].type = type;
    g_lc.sig_q[i].flag = flag;
    g_lc.sig_q[i].text[0] = '\0';
    if (text)
        strncpy(g_lc.sig_q[i].text, text, sizeof(g_lc.sig_q[i].text) - 1);
    g_lc.sig_tail = (g_lc.sig_tail + 1) % (int)(sizeof(g_lc.sig_q) / sizeof(g_lc.sig_q[0]));
    g_lc.sig_count++;
}

static int json_get_int(const char *json, const char *key, int def)
{
    char pat[80];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) {
        return def;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return def;
    }
    return (int)strtol(p + 1, NULL, 10);
}

static int json_get_bool(const char *json, const char *key, int def)
{
    char pat[80];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) {
        return def;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return def;
    }
    ++p;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    if (strncmp(p, "true", 4) == 0) {
        return 1;
    }
    if (strncmp(p, "false", 5) == 0) {
        return 0;
    }
    return def;
}

static int json_extract_object(const char *json, const char *key, char *out, size_t out_cap)
{
    char pat[80];
    const char *p;
    int depth;
    size_t n;
    if (!json || !key || !out || out_cap < 3) return 0;
    out[0] = '\0';
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    ++p;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != '{') return 0;
    depth = 0;
    n = 0;
    do {
        if (*p == '{') ++depth;
        else if (*p == '}') --depth;
        if (n + 1 >= out_cap) return 0;
        out[n++] = *p++;
    } while (*p && depth > 0);
    out[n] = '\0';
    return depth == 0 && n > 1;
}

/* Parse `"<key>":[ {..}, {..} ]` into package rows.
 *
 * Returns the number of rows filled. A row missing an id or a version is
 * skipped: the pair is the whole identity the seat gate matches on, so half of
 * it is not a lesser row, it is a different package. */
static int parse_mod_pkg_array(const char *json, const char *key,
                               SnesLobbyModPkg *out, int max)
{
    char pattern[64];
    const char *p;
    int n = 0;

    if (!json || !key || !out || max <= 0) return 0;
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p && *p != ':') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '[') return 0;          /* a string here is the old encoding */
    p++;

    while (*p && n < max) {
        char obj[512];
        const char *start;
        int depth = 0;
        int in_str = 0;
        size_t len;
        SnesLobbyModPkg row;

        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p == ']' || !*p) break;
        if (*p != '{') break;
        start = p;
        for (; *p; ++p) {
            if (in_str) {
                if (*p == '\\' && p[1]) { ++p; continue; }
                if (*p == '"') in_str = 0;
                continue;
            }
            if (*p == '"') { in_str = 1; continue; }
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { ++p; break; } }
        }
        if (depth != 0) break;                    /* truncated array */
        len = (size_t)(p - start);
        if (len >= sizeof(obj)) continue;         /* absurd row; skip it */
        memcpy(obj, start, len);
        obj[len] = '\0';

        memset(&row, 0, sizeof(row));
        json_get_str(obj, "id", row.id, sizeof(row.id));
        json_get_str(obj, "ver", row.ver, sizeof(row.ver));
        json_get_str(obj, "n", row.name, sizeof(row.name));
        json_get_str(obj, "f", row.feats, sizeof(row.feats));
        if (!row.id[0] || !row.ver[0])
            continue;
        out[n++] = row;
    }
    return n;
}

/* Emit `"<key>":[ … ]`. Returns bytes written, or 0 if the whole array did not
 * fit -- a half-written array is invalid JSON, and a SHORT one is worse: it
 * would name fewer requirements than the host actually has. */
static int append_mod_pkg_array(char *dst, size_t cap, const char *key,
                                const SnesLobbyModPkg *pkgs, int count)
{
    size_t used = 0;
    int i;
    int n;
    int wrote = 0;

    if (!dst || cap < 8 || !key || (!pkgs && count > 0)) return 0;
    n = snprintf(dst, cap, "\"%s\":[", key);
    if (n < 0 || (size_t)n >= cap) return 0;
    used = (size_t)n;
    for (i = 0; i < count; ++i) {
        char name_esc[SNES_LOBBY_MOD_NAME_LEN * 2 + 4];
        char feats_esc[SNES_LOBBY_MOD_FEATS_LEN * 2 + 4];
        if (!pkgs[i].id[0] || !pkgs[i].ver[0]) continue;
        json_escape(pkgs[i].name, name_esc, sizeof(name_esc));
        json_escape(pkgs[i].feats, feats_esc, sizeof(feats_esc));
        n = snprintf(dst + used, cap - used,
                     "%s{\"id\":\"%s\",\"ver\":\"%s\",\"n\":\"%s\",\"f\":\"%s\"}",
                     wrote ? "," : "",
                     pkgs[i].id, pkgs[i].ver, name_esc, feats_esc);
        if (n < 0 || (size_t)n >= cap - used) return 0;
        used += (size_t)n;
        wrote++;
    }
    n = snprintf(dst + used, cap - used, "]");
    if (n < 0 || (size_t)n >= cap - used) return 0;
    return (int)(used + (size_t)n);
}

/* Installed once at start-up and kept OUTSIDE LobbyClient on purpose.
 *
 * snes_lobby_disconnect() memsets the whole LobbyClient -- and connect()
 * calls disconnect() first -- so anything living in there is per-connection
 * state by definition. These are configuration: the build either can pack and
 * install a package or it cannot, and connecting to a lobby does not change
 * the answer. Held in g_lc, the export hook was installed at init and NULL by
 * the time any peer could ask, and every request came back "could not pack
 * the mod" from a host that was perfectly able to. */
static SnesLobbyModOfferFn g_mod_offer_fn;
static void *g_mod_offer_ctx;
static SnesLobbyModExportFn  g_mod_export_fn;
static SnesLobbyModFreeFn    g_mod_free_fn;
static SnesLobbyModInstallFn g_mod_install_fn;
static void *g_mod_hook_ctx;

void snes_lobby_set_mod_offer_supplier(SnesLobbyModOfferFn fn, void *ctx)
{
    g_mod_offer_fn = fn;
    g_mod_offer_ctx = ctx;
}

/* `"mod_offer":{"pkgs":[…]}` -- what this peer already has. The server matches
 * each row of the host's plan against it and refuses to seat on any miss, so
 * an offer that is short in EITHER direction is wrong: too few rows and the
 * peer is turned away holding the mod, too many and it is seated without one.
 * Returns bytes written, or 0 if the whole offer did not fit. */
static int append_mod_offer(char *dst, size_t cap)
{
    SnesLobbyModPkg rows[SNES_LOBBY_MAX_MODS];
    int n;
    int used;

    if (!dst || cap < 8) return 0;
    if (!g_mod_offer_fn) { dst[0] = '\0'; return 0; }
    memset(rows, 0, sizeof(rows));
    n = g_mod_offer_fn(rows, SNES_LOBBY_MAX_MODS, g_mod_offer_ctx);
    if (n < 0) n = 0;
    if (n > SNES_LOBBY_MAX_MODS) n = SNES_LOBBY_MAX_MODS;
    used = snprintf(dst, cap, ",\"mod_offer\":{");
    if (used < 0 || (size_t)used >= cap) return 0;
    {
        int m = append_mod_pkg_array(dst + used, cap - (size_t)used, "pkgs",
                                     rows, n);
        if (m <= 0) return 0;
        used += m;
    }
    if ((size_t)used + 2 >= cap) return 0;
    dst[used++] = '}';
    dst[used] = '\0';
    /* The lobby server discards a mod_offer object larger than 2048 bytes
     * (sanitize_mod_offer), and a discarded offer is indistinguishable from
     * "this peer has nothing" -- which reads as missing every mod and holds
     * the match. Refuse here, where the reason can be said out loud. The
     * measured field is the object itself, not the `,"mod_offer":` prefix the
     * server never sees. */
    {
        const char *obj = strchr(dst, '{');
        const size_t obj_len = obj ? strlen(obj) : (size_t)used;
        if (obj_len > 2000) {
            fprintf(stderr,
                    "snes_lobby: the installed mod set is %zu bytes, over the "
                    "lobby server's 2048-byte limit; it was not announced\n",
                    obj_len);
            dst[0] = '\0';
            return 0;
        }
    }
    return used;
}

static void parse_match_caps_object(const char *obj, SnesLobbyMatchCaps *out)
{
    if (!obj || !out || obj[0] != '{') return;
    match_caps_clear(out);
    out->widescreen = json_get_bool(obj, "widescreen", 0);
    out->widescreen_hud = json_get_bool(obj, "widescreen_hud", 1);
    /* Absent: an empty plan is "no mods required". A plan encoded as a STRING
     * rather than an array parses to zero rows here on purpose -- that is the
     * superseded encoding, and reading it would revive a plan the lobby server
     * has already ignored, leaving this peer and the server disagreeing about
     * what the match requires. */
    out->mod_count = parse_mod_pkg_array(obj, "mod_plan", out->mods,
                                         SNES_LOBBY_MAX_MODS);
    json_get_str(obj, "mod_set", out->mod_set, sizeof(out->mod_set));
    out->ignore_aspect = json_get_bool(obj, "ignore_aspect", 0);
    out->input_delay = json_get_int(obj, "input_delay", 2);
    if (out->input_delay < 2) out->input_delay = 2;
    if (out->input_delay > 20) out->input_delay = 20;
    out->ws_extra = json_get_int(obj, "ws_extra", 0);
    if (out->ws_extra < 0) out->ws_extra = 0;
    out->force_turn = json_get_bool(obj, "force_turn", 0) ? 1 : 0;
    out->force_input_relay = json_get_bool(obj, "force_input_relay", 0) ? 1 : 0;
    out->rollback = json_get_bool(obj, "rollback", 1) ? 1 : 0;
    out->valid = 1;
}

static void ingest_match_caps_from_json(const char *json)
{
    /* Sized for the whole caps object INCLUDING a full mod array. It used to
     * be 512 bytes, which a plan of any size overflows -- and a truncated
     * extract parses as a caps blob with no plan, i.e. it fails open. */
    char obj[SNES_LOBBY_MAX_MODS * 256 + 512];
    if (json_extract_object(json, "match_caps", obj, sizeof(obj)))
        parse_match_caps_object(obj, &g_lc.match_caps);
}

static int append_match_caps_json(char *dst, size_t dst_cap, const SnesLobbyMatchCaps *caps)
{
    char mods[SNES_LOBBY_MAX_MODS * 256 + 16];
    char set_esc[sizeof(caps->mod_set) * 2 + 4];
    int n;

    if (!dst || dst_cap < 8 || !caps || !caps->valid) return 0;
    /* Build the plan first. If it does not fit, publish NOTHING rather than a
     * caps blob carrying a short plan: the server seats on what it reads, so
     * an under-reported requirement seats a peer that cannot play. */
    if (!append_mod_pkg_array(mods, sizeof(mods), "mod_plan", caps->mods,
                              caps->mod_count))
        return 0;
    json_escape(caps->mod_set, set_esc, sizeof(set_esc));
    n = snprintf(dst, dst_cap,
                 ",\"match_caps\":{\"v\":1,\"widescreen\":%s,\"widescreen_hud\":%s,"
                 "\"ignore_aspect\":%s,\"input_delay\":%d,\"ws_extra\":%d,"
                 "\"force_turn\":%s,\"force_input_relay\":%s,"
                 "\"rollback\":%s,%s,\"mod_set\":\"%s\"}",
                 caps->widescreen ? "true" : "false",
                 caps->widescreen_hud ? "true" : "false",
                 caps->ignore_aspect ? "true" : "false",
                 caps->input_delay, caps->ws_extra,
                 caps->force_turn ? "true" : "false",
                 caps->force_input_relay ? "true" : "false",
                 caps->rollback ? "true" : "false",
                 mods, set_esc);
    if (n < 0 || (size_t)n >= dst_cap) return 0;
    /* The lobby server drops a match_caps object over 4096 bytes ENTIRELY
     * (sanitize_match_caps returns None), which would take widescreen,
     * rollback and input_delay down with the plan and hand guests a blank
     * caps blob. Refuse here instead, where we can say why. */
    if ((size_t)n > 4000) {
        fprintf(stderr,
                "snes_lobby: match caps are %d bytes with %d mod(s); the lobby "
                "server discards anything over 4096, so nothing was published"
                " -- reduce the enabled mod set\n",
                n, caps->mod_count);
        return 0;
    }
    return n;
}

static void queue_send(const char *json)
{
    if (g_lc.pending_n >= 8) {
        return;
    }
    strncpy(g_lc.pending_tx[g_lc.pending_n], json, sizeof(g_lc.pending_tx[0]) - 1);
    g_lc.pending_tx[g_lc.pending_n][sizeof(g_lc.pending_tx[0]) - 1] = '\0';
    g_lc.pending_n++;
}

static void flush_pending(void)
{
    int i;
    if (!g_lc.handshake_done) {
        return;
    }
    for (i = 0; i < g_lc.pending_n; ++i) {
        rnet_ws_write_text(g_lc.fd, g_lc.pending_tx[i], 1);
    }
    g_lc.pending_n = 0;
}

static int endpoint_has_usable_port(const char *endpoint)
{
    const char *colon;
    unsigned port = 0;
    if (!endpoint || !endpoint[0]) return 0;
    colon = strrchr(endpoint, ':');
    if (!colon || !colon[1]) return 0;
    for (colon++; *colon; ++colon) {
        if (*colon < '0' || *colon > '9') return 0;
        port = port * 10u + (unsigned)(*colon - '0');
        if (port > 65535u) return 0;
    }
    return port != 0;
}

static int using_server_input_relay(const SnesLobbyJoinInfo *j)
{
    if (g_lc.match_caps.valid && g_lc.match_caps.force_input_relay)
        return 1;
    /* Server rewrote both endpoints to the same relay advertise address. */
    if (j && j->host_endpoint[0] && j->guest_endpoint[0] &&
        endpoint_has_usable_port(j->host_endpoint) &&
        endpoint_has_usable_port(j->guest_endpoint) &&
        strcmp(j->host_endpoint, j->guest_endpoint) == 0 &&
        (!g_lc.my_bind[0] || strcmp(j->host_endpoint, g_lc.my_bind) != 0))
        return 1;
    return 0;
}

static void fill_peer_bind_from_join(void)
{
    SnesLobbyJoinInfo *j = &g_lc.join;
    const char *port;
    const int force_relay = using_server_input_relay(j);
    memset(j->bind_hostport, 0, sizeof(j->bind_hostport));
    memset(j->peer_hostport, 0, sizeof(j->peer_hostport));
    if (force_relay) {
        /* Everyone dials the lobby-server UDP relay — ephemeral local bind. */
        strncpy(j->bind_hostport, "0.0.0.0:0", sizeof(j->bind_hostport) - 1);
        strncpy(j->peer_hostport, j->host_endpoint, sizeof(j->peer_hostport) - 1);
    } else if (g_lc.is_host) {
        /* host_endpoint is the address advertised to peers. It may be the
         * router's public/NAT address and therefore cannot be bound on this
         * machine. Listen on every local interface using the advertised port. */
        port = strrchr(g_lc.my_bind, ':');
        if (port && port[1]) {
            snprintf(j->bind_hostport, sizeof(j->bind_hostport),
                     "0.0.0.0:%s", port + 1);
        } else {
            strncpy(j->bind_hostport, g_lc.my_bind,
                    sizeof(j->bind_hostport) - 1);
        }
        /* guest_endpoint "ip:0" is unusable — leave peer empty so transport
         * accept_first_peer learns the real source from the first UDP packet. */
        if (endpoint_has_usable_port(j->guest_endpoint))
            strncpy(j->peer_hostport, j->guest_endpoint,
                    sizeof(j->peer_hostport) - 1);
    } else {
        strncpy(j->bind_hostport, g_lc.my_bind, sizeof(j->bind_hostport) - 1);
        strncpy(j->peer_hostport, j->host_endpoint, sizeof(j->peer_hostport) - 1);
    }
    j->bind_hostport[sizeof(j->bind_hostport) - 1] = '\0';
    j->peer_hostport[sizeof(j->peer_hostport) - 1] = '\0';
}

static void parse_slots_array(const char *json)
{
    const char *p = strstr(json, "\"slots\"");
    int n = 0;
    g_lc.member_count = 0;
    g_lc.local_ready = 0;
    if (!p) {
        return;
    }
    p = strchr(p, '[');
    if (!p) {
        return;
    }
    ++p;
    while (*p && n < SNES_LOBBY_MAX_MEMBERS) {
        const char *obj;
        while (*p && *p != '{') {
            if (*p == ']') {
                g_lc.member_count = n;
                return;
            }
            ++p;
        }
        if (*p != '{') {
            break;
        }
        obj = p;
        {
            int depth = 0;
            const char *end = p;
            do {
                if (*end == '{') {
                    ++depth;
                } else if (*end == '}') {
                    --depth;
                }
                ++end;
            } while (*end && depth > 0);
            {
                /* Wide enough for a slot row carrying a full mod_offer. At
                 * 512 the row was silently clipped and the peer's set parsed
                 * short -- which reads as "missing", so the gate would hold a
                 * match that should have started. */
                char chunk[SNES_LOBBY_MAX_MODS * 256 + 512];
                size_t len = (size_t)(end - obj);
                int clipped = 0;
                if (len >= sizeof(chunk)) {
                    len = sizeof(chunk) - 1;
                    clipped = 1;
                }
                memcpy(chunk, obj, len);
                chunk[len] = '\0';
                g_lc.members[n].slot = json_get_int(chunk, "slot", n);
                json_get_str(chunk, "player_id", g_lc.members[n].player_id,
                             sizeof(g_lc.members[n].player_id));
                /* mod_offer is an OBJECT -- {"pkgs":[...]} -- not a bare
                 * array. The server requires an object (sanitize_mod_offer
                 * rejects anything else) and echoes it back verbatim, so the
                 * array has to be reached through it.
                 *
                 * Read as a bare array this returned zero rows every time, so
                 * the host saw every peer as owning nothing: a guest with both
                 * mods installed and showing OK on its own screen was still
                 * refused at Play, and the message named a mod it had. */
                g_lc.member_offer_count[n] = 0;
                if (!clipped) {
                    char offer_obj[SNES_LOBBY_MAX_MODS * 256 + 64];
                    if (json_extract_object(chunk, "mod_offer", offer_obj,
                                            sizeof(offer_obj)))
                        g_lc.member_offer_count[n] =
                            parse_mod_pkg_array(offer_obj, "pkgs",
                                                g_lc.member_offer[n],
                                                SNES_LOBBY_MAX_MODS);
                }
                if (clipped)
                    fprintf(stderr,
                            "snes_lobby: slot %d row did not fit; treating its "
                            "mod set as unknown\n", n);
                json_get_str(chunk, "display_name", g_lc.members[n].display_name,
                             sizeof(g_lc.members[n].display_name));
                g_lc.members[n].ready = json_get_bool(chunk, "ready", 0);
                if (g_lc.player_id[0] &&
                    strcmp(g_lc.members[n].player_id, g_lc.player_id) == 0) {
                    g_lc.local_ready = g_lc.members[n].ready;
                    /* Seat swaps only arrive via lobby_update slots — keep
                     * join.local_slot in sync for launch / netplay_cfg. */
                    g_lc.join.local_slot = g_lc.members[n].slot;
                }
                ++n;
                p = end;
            }
        }
    }
    g_lc.member_count = n;
}

static void handle_server_json(const char *json);

/* Parse complete unmasked server text frames from ws_pending; leave remainder. */
static void drain_ws_pending(void)
{
    while (g_lc.ws_pending_len >= 2) {
        size_t i = 0;
        uint8_t b0 = g_lc.ws_pending[i++];
        uint8_t b1 = g_lc.ws_pending[i++];
        int opcode = b0 & 0x0f;
        size_t plen = b1 & 0x7f;
        if (b1 & 0x80) {
            /* Server frames must not be masked. */
            g_lc.ws_pending_len = 0;
            return;
        }
        if (plen == 126) {
            if (g_lc.ws_pending_len < i + 2) {
                return;
            }
            plen = ((size_t)g_lc.ws_pending[i] << 8) | g_lc.ws_pending[i + 1];
            i += 2;
        } else if (plen == 127) {
            g_lc.ws_pending_len = 0;
            return;
        }
        if (g_lc.ws_pending_len < i + plen) {
            return;
        }
        if (opcode == 0x1 && plen + 1 < sizeof(g_lc.rx_http)) {
            char text[4096];
            memcpy(text, g_lc.ws_pending + i, plen);
            text[plen] = '\0';
            handle_server_json(text);
        }
        i += plen;
        memmove(g_lc.ws_pending, g_lc.ws_pending + i, g_lc.ws_pending_len - i);
        g_lc.ws_pending_len -= i;
        if (opcode == 0x8) {
            snes_lobby_disconnect();
            return;
        }
    }
}

static void handle_server_json(const char *json)
{
    char op[32];
    json_get_str(json, "op", op, sizeof(op));
    if (strcmp(op, "welcome") == 0) {
        json_get_str(json, "player_id", g_lc.player_id, sizeof(g_lc.player_id));
        if (g_lc.display_name[0]) {
            char msg[256];
            snprintf(msg, sizeof(msg), "{\"op\":\"hello\",\"display_name\":\"%s\"}", g_lc.display_name);
            queue_send(msg);
        }
        queue_send("{\"op\":\"list\"}");
        /* Prefetch Coturn creds for ICE (no-op reply if server lacks COTURN_*). */
        (void)queue_turn_credentials_request();
        return;
    }
    if (strcmp(op, "turn_credentials") == 0) {
        /* Server sends JSON boolean "ok": true — not an integer. */
        int ok = json_get_bool(json, "ok", 0);
        g_lc.turn_request_pending = 0;
        memset(&g_lc.turn, 0, sizeof(g_lc.turn));
        g_lc.turn_received_at = 0;
        if (!ok) {
            char err[64];
            json_get_str(json, "error", err, sizeof(err));
            fprintf(stderr,
                    "snes_lobby: turn_credentials failed (%s) — ICE will be "
                    "STUN-only unless SNES_NET_TURN_* is set\n",
                    err[0] ? err : "unknown");
            return;
        }
        json_get_str(json, "stun_host", g_lc.turn.stun_host,
                     sizeof(g_lc.turn.stun_host));
        json_get_str(json, "turn_host", g_lc.turn.turn_host,
                     sizeof(g_lc.turn.turn_host));
        json_get_str(json, "username", g_lc.turn.username,
                     sizeof(g_lc.turn.username));
        json_get_str(json, "password", g_lc.turn.password,
                     sizeof(g_lc.turn.password));
        g_lc.turn.stun_port = json_get_int(json, "stun_port", 3478);
        g_lc.turn.turn_port = json_get_int(json, "turn_port", 3478);
        g_lc.turn.ttl_secs = (uint32_t)json_get_int(json, "ttl_secs", 86400);
        if (g_lc.turn.turn_host[0] && g_lc.turn.username[0] &&
            g_lc.turn.password[0]) {
            g_lc.turn.valid = 1;
            g_lc.turn_received_at = time(NULL);
            fprintf(stderr,
                    "snes_lobby: turn_credentials ok stun=%s:%d turn=%s:%d "
                    "user=%s ttl=%us\n",
                    g_lc.turn.stun_host[0] ? g_lc.turn.stun_host : "(none)",
                    g_lc.turn.stun_port,
                    g_lc.turn.turn_host, g_lc.turn.turn_port,
                    g_lc.turn.username, (unsigned)g_lc.turn.ttl_secs);
        } else {
            fprintf(stderr,
                    "snes_lobby: turn_credentials ok but incomplete fields\n");
        }
        return;
    }
    if (strcmp(op, "lobby_list") == 0) {
        const char *p = strstr(json, "\"lobbies\"");
        int n = 0;
        g_lc.list_count = 0;
        if (!p) {
            return;
        }
        p = strchr(p, '[');
        if (!p) {
            return;
        }
        ++p;
        while (*p && n < SNES_LOBBY_MAX_LIST) {
            const char *obj;
            while (*p && *p != '{') {
                if (*p == ']') {
                    g_lc.list_count = n;
                    return;
                }
                ++p;
            }
            if (*p != '{') {
                break;
            }
            obj = p;
            {
                int depth = 0;
                const char *end = p;
                do {
                    if (*end == '{') {
                        ++depth;
                    } else if (*end == '}') {
                        --depth;
                    }
                    ++end;
                } while (*end && depth > 0);
                {
                    char chunk[1024];
                    size_t len = (size_t)(end - obj);
                    if (len >= sizeof(chunk)) {
                        len = sizeof(chunk) - 1;
                    }
                    memcpy(chunk, obj, len);
                    chunk[len] = '\0';
                    json_get_str(chunk, "lobby_id", g_lc.list[n].lobby_id, sizeof(g_lc.list[n].lobby_id));
                    json_get_str(chunk, "name", g_lc.list[n].name, sizeof(g_lc.list[n].name));
                    json_get_str(chunk, "game_name", g_lc.list[n].game_name, sizeof(g_lc.list[n].game_name));
                    json_get_str(chunk, "game_version", g_lc.list[n].game_version,
                                 sizeof(g_lc.list[n].game_version));
                    if (!g_lc.list[n].game_version[0])
                        strncpy(g_lc.list[n].game_version, "dev",
                                sizeof(g_lc.list[n].game_version) - 1);
                    if (g_lc.filter_game_name[0] &&
                        strcmp(g_lc.list[n].game_name, g_lc.filter_game_name) != 0) {
                        p = end;
                        continue;
                    }
                    if (list_filter_version_strict()) {
                        const char *want_ver = effective_game_version(NULL);
                        if (want_ver && want_ver[0] &&
                            strcmp(g_lc.list[n].game_version, want_ver) != 0) {
                            p = end;
                            continue;
                        }
                    }
                    g_lc.list[n].player_count = json_get_int(chunk, "player_count", 0);
                    g_lc.list[n].max_slots = json_get_int(chunk, "max_slots", 2);
                    g_lc.list[n].has_password = json_get_bool(chunk, "has_password", 0);
                    ++n;
                    p = end;
                }
            }
        }
        g_lc.list_count = n;
        return;
    }
    if (strcmp(op, "created") == 0) {
        g_lc.in_lobby = 1;
        g_lc.is_host = 1;
        g_lc.join.ok = 1;
        g_lc.launch_pending = 0;
        g_lc.all_ready = 0;
        member_rtt_clear();
        json_get_str(json, "lobby_id", g_lc.join.lobby_id, sizeof(g_lc.join.lobby_id));
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", 1);
        g_lc.join.local_slot = json_get_int(json, "local_slot", 0);
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        json_get_str(json, "host_player_id", g_lc.host_player_id,
                     sizeof(g_lc.host_player_id));
        if (!g_lc.host_player_id[0])
            strncpy(g_lc.host_player_id, g_lc.player_id,
                    sizeof(g_lc.host_player_id) - 1);
        g_lc.join.player_count = 1;
        g_lc.join.max_slots = 2;
        g_lc.join.last_error[0] = '\0';
        ingest_match_caps_from_json(json);
        fill_peer_bind_from_join();
        parse_slots_array(json);
        if (g_lc.member_count == 0) {
            g_lc.members[0].slot = 0;
            strncpy(g_lc.members[0].player_id, g_lc.player_id, sizeof(g_lc.members[0].player_id) - 1);
            strncpy(g_lc.members[0].display_name, g_lc.display_name,
                    sizeof(g_lc.members[0].display_name) - 1);
            g_lc.members[0].ready = 0;
            g_lc.member_count = 1;
            g_lc.local_ready = 0;
        }
        /* Ready UI is gone; auto-ready so older lobby servers that still gate
         * start on all_ready accept host Play. */
        send_set_ready(1);
        flush_pending();
        return;
    }
    if (strcmp(op, "joined") == 0) {
        g_lc.in_lobby = 1;
        g_lc.is_host = 0;
        g_lc.join.ok = 1;
        g_lc.launch_pending = 0;
        g_lc.all_ready = 0;
        member_rtt_clear();
        json_get_str(json, "lobby_id", g_lc.join.lobby_id, sizeof(g_lc.join.lobby_id));
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", 1);
        g_lc.join.local_slot = json_get_int(json, "local_slot", 1);
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        json_get_str(json, "host_player_id", g_lc.host_player_id,
                     sizeof(g_lc.host_player_id));
        g_lc.join.player_count = 2;
        g_lc.join.max_slots = 2;
        g_lc.join.last_error[0] = '\0';
        ingest_match_caps_from_json(json);
        fill_peer_bind_from_join();
        parse_slots_array(json);
        send_set_ready(1);
        flush_pending();
        return;
    }
    if (strcmp(op, "lobby_update") == 0) {
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        json_get_str(json, "host_player_id", g_lc.host_player_id,
                     sizeof(g_lc.host_player_id));
        g_lc.join.player_count = json_get_int(json, "player_count", g_lc.join.player_count);
        g_lc.join.max_slots = json_get_int(json, "max_slots", g_lc.join.max_slots);
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", (int)g_lc.join.session_id);
        g_lc.all_ready = json_get_bool(json, "all_ready", 0);
        ingest_match_caps_from_json(json);
        fill_peer_bind_from_join();
        parse_slots_array(json);
        /* Kick/move/start clear ready; re-arm so host Play keeps working on
         * servers that still require all_ready. */
        if (g_lc.in_lobby && !g_lc.local_ready) {
            send_set_ready(1);
            flush_pending();
        }
        return;
    }
    if (strcmp(op, "launch") == 0) {
        char relay_endpoint[SNES_LOBBY_ENDPOINT_LEN];
        json_get_str(json, "host_endpoint", g_lc.join.host_endpoint, sizeof(g_lc.join.host_endpoint));
        json_get_str(json, "guest_endpoint", g_lc.join.guest_endpoint, sizeof(g_lc.join.guest_endpoint));
        relay_endpoint[0] = '\0';
        json_get_str(json, "relay_endpoint", relay_endpoint, sizeof(relay_endpoint));
        g_lc.join.player_count = json_get_int(json, "player_count", g_lc.join.player_count);
        g_lc.join.max_slots = json_get_int(json, "max_slots", g_lc.join.max_slots);
        g_lc.join.session_id = (uint32_t)json_get_int(json, "session_id", (int)g_lc.join.session_id);
        ingest_match_caps_from_json(json);
        if (relay_endpoint[0] && endpoint_has_usable_port(relay_endpoint)) {
            strncpy(g_lc.join.host_endpoint, relay_endpoint,
                    sizeof(g_lc.join.host_endpoint) - 1);
            g_lc.join.host_endpoint[sizeof(g_lc.join.host_endpoint) - 1] = '\0';
            strncpy(g_lc.join.guest_endpoint, relay_endpoint,
                    sizeof(g_lc.join.guest_endpoint) - 1);
            g_lc.join.guest_endpoint[sizeof(g_lc.join.guest_endpoint) - 1] = '\0';
            if (!g_lc.match_caps.valid)
                g_lc.match_caps.valid = 1;
            g_lc.match_caps.force_input_relay = 1;
        }
        fill_peer_bind_from_join();
        parse_slots_array(json);
        /* Guest must know the host. Host may leave peer empty to learn the
         * guest from the first UDP packet (LAN / legacy guest_bind :0). */
        if (!g_lc.join.host_endpoint[0] || !g_lc.join.bind_hostport[0] ||
            (!g_lc.is_host && !g_lc.join.peer_hostport[0] &&
             !using_server_input_relay(&g_lc.join))) {
            strncpy(g_lc.join.last_error, "missing_endpoints",
                    sizeof(g_lc.join.last_error) - 1);
            g_lc.launch_pending = 0;
            return;
        }
        /* A prior lobby error must not leave join.ok=0 or fill_launch will
         * refuse the match forever while launch_pending stays sticky. */
        g_lc.join.ok = 1;
        g_lc.join.last_error[0] = '\0';
        g_lc.launch_pending = 1;
        return;
    }
    if (strcmp(op, "signal") == 0) {
        char text[2048];
        char from[SNES_LOBBY_ID_LEN];
        int type = json_get_int(json, "type", 0);
        int flag = json_get_int(json, "flag", 0);
        text[0] = '\0';
        from[0] = '\0';
        json_get_str(json, "text", text, sizeof(text));
        json_get_str(json, "from_player_id", from, sizeof(from));
        if (type == SNES_LOBBY_SIG_RTT_PING) {
            /* Only the host answers latency probes. */
            if (g_lc.is_host)
                (void)snes_lobby_send_signal(SNES_LOBBY_SIG_RTT_PONG, 0, text);
            return;
        }
        if (type == SNES_LOBBY_SIG_RTT_PONG) {
            unsigned long long sent = 0;
            uint64_t now = lobby_mono_ms();
            int slot;
            if (sscanf(text, "%llu", &sent) == 1 && (uint64_t)sent <= now) {
                int ms = (int)(now - (uint64_t)sent);
                if (ms < 0) ms = 0;
                if (ms > 60000) ms = 60000;
                slot = local_member_slot();
                if (slot >= 0 && slot < SNES_LOBBY_MAX_MEMBERS)
                    g_lc.member_rtt_ms[slot] = ms;
                /* Tell the host (and peers) our measured RTT to host. */
                {
                    char report[32];
                    snprintf(report, sizeof(report), "%d", ms);
                    (void)snes_lobby_send_signal(SNES_LOBBY_SIG_RTT_REPORT, 0,
                                                 report);
                }
            }
            return;
        }
        if (type == SNES_LOBBY_SIG_RTT_REPORT) {
            int slot = member_slot_for_player(from);
            int ms = (int)strtol(text, NULL, 10);
            if (slot >= 0 && slot < SNES_LOBBY_MAX_MEMBERS && ms >= 0 &&
                ms <= 60000)
                g_lc.member_rtt_ms[slot] = ms;
            return;
        }
        if (type == SNES_LOBBY_SIG_MOD_REQ) {
            mod_xfer_on_request(from, text);
            return;
        }
        if (type == SNES_LOBBY_SIG_MOD_NAK) {
            mod_xfer_fail(text[0] ? text : "the host refused");
            return;
        }
        if (type > SNES_LOBBY_SIG_MOD_ICE_BASE &&
            type <= SNES_LOBBY_SIG_MOD_ICE_BASE + 6) {
            /* Only from the peer we are actually transferring with. An SDP
             * from anyone else is either a stale exchange or someone else's,
             * and feeding it to the agent breaks the live negotiation. */
            RNetSignal sig;
            const int t = mod_ice_type_for_push(type -
                                                SNES_LOBBY_SIG_MOD_ICE_BASE);
            memset(&sig, 0, sizeof(sig));
            sig.type = (RNetSignalType)t;
            sig.flag = (rnet_u8)flag;
            snprintf(sig.text, sizeof(sig.text), "%s", text);
            if (g_lc.xfer && from[0] && !strcmp(from, g_lc.xfer_peer)) {
                rnet_ice_xfer_push_signal(g_lc.xfer, &sig);
            } else if (from[0]) {
                /* No agent yet: the sender is still packing. Hold it. */
                if (g_lc.sig_hold_n == 0 ||
                    strcmp(g_lc.sig_hold_from, from) != 0) {
                    g_lc.sig_hold_n = 0;
                    snprintf(g_lc.sig_hold_from, sizeof(g_lc.sig_hold_from),
                             "%s", from);
                }
                if (g_lc.sig_hold_n <
                    (int)(sizeof(g_lc.sig_hold) / sizeof(g_lc.sig_hold[0]))) {
                    g_lc.sig_hold[g_lc.sig_hold_n++] = sig;
                } else {
                    /* Overflowing means the peer gathered far more candidates
                     * than a handshake needs while we did nothing with them --
                     * dropping the newest keeps the offer, which is the one
                     * that matters. */
                    fprintf(stderr, "snes_lobby: ICE hold buffer full; "
                                    "dropping a candidate\n");
                }
            }
            return;
        }
        enqueue_signal(type, flag, text);
        (void)flag;
        return;
    }
    if (strcmp(op, "need_mods") == 0) {
        /* Not op:"error": the server answers a refused join with its own op,
         * carrying the list. Falling through to the generic handler used to
         * drop it entirely -- no error code, no list, nothing logged -- so a
         * player saw the join simply do nothing and the log showed a clean
         * session. A refusal has to explain itself where it happens. */
        int i;
        g_lc.need_mods_count = parse_mod_pkg_array(json, "mods", g_lc.need_mods,
                                                   SNES_LOBBY_MAX_MODS);
        g_lc.need_mods_can_transfer = json_get_bool(json, "can_transfer", 0);
        json_get_str(json, "lobby_id", g_lc.need_mods_lobby_id,
                     sizeof(g_lc.need_mods_lobby_id));
        json_get_str(json, "host_player_id", g_lc.need_mods_host_player_id,
                     sizeof(g_lc.need_mods_host_player_id));
        snprintf(g_lc.join.last_error, sizeof(g_lc.join.last_error),
                 "need_mods");
        g_lc.join.ok = 0;
        g_lc.in_lobby = 0;
        fprintf(stderr,
                "snes_lobby: refused - this lobby needs %d mod(s) this build "
                "does not have (server can_transfer=%d)\n",
                g_lc.need_mods_count, g_lc.need_mods_can_transfer);
        for (i = 0; i < g_lc.need_mods_count; ++i)
            fprintf(stderr, "snes_lobby:   missing %s@%s%s%s\n",
                    g_lc.need_mods[i].id, g_lc.need_mods[i].ver,
                    g_lc.need_mods[i].name[0] ? " - " : "",
                    g_lc.need_mods[i].name);
        if (g_lc.need_mods_count == 0)
            fprintf(stderr,
                    "snes_lobby:   the server named none, which means it read "
                    "a plan it could not parse -- check the host's build\n");
        return;
    }
    if (strcmp(op, "error") == 0) {
        json_get_str(json, "code", g_lc.join.last_error, sizeof(g_lc.join.last_error));
        /* Keep seating valid: start/need_players/etc. must not block a later
         * successful op:launch from filling netplay_launch. */
        if (!g_lc.in_lobby)
            g_lc.join.ok = 0;
        return;
    }
    if (strcmp(op, "lobby_closed") == 0 || strcmp(op, "left") == 0 ||
        strcmp(op, "kicked") == 0) {
        g_lc.in_lobby = 0;
        g_lc.is_host = 0;
        g_lc.host_player_id[0] = '\0';
        g_lc.member_count = 0;
        g_lc.local_ready = 0;
        g_lc.all_ready = 0;
        g_lc.launch_pending = 0;
        memset(&g_lc.join, 0, sizeof(g_lc.join));
        match_caps_clear(&g_lc.match_caps);
        member_rtt_clear();
        return;
    }
}

static int set_nonblock(int fd)
{
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

int snes_lobby_connect(const char *ws_url)
{
    struct addrinfo hints, *res = NULL, *rp;
    char portstr[16];
    int fd = -1;
    char key_raw[16];
    char key_b64[32];
    char req[512];
    int i;

    /* Name the pin we will present. A join refused with version_mismatch tells
     * a player only that the two builds differ; without both values in the log
     * there is nothing to compare, and "dev" against "dev" used to look like
     * agreement even when the builds shared no code. */
    fprintf(stderr, "snes_lobby: this build presents game_version=\"%s\"\n",
            effective_game_version(NULL));

    snes_lobby_disconnect();
#if defined(_WIN32)
    {
        static int wsa;
        if (!wsa) {
            WSADATA d;
            WSAStartup(MAKEWORD(2, 2), &d);
            wsa = 1;
        }
    }
#endif
    {
        const char *use = ws_url && ws_url[0] ? ws_url : snes_lobby_default_url();
        if (parse_ws_url(use, g_lc.host, sizeof(g_lc.host), &g_lc.port, g_lc.path,
                         sizeof(g_lc.path)) != 0) {
            return -1;
        }
        snprintf(g_lc.url, sizeof(g_lc.url), "%s", use);
    }
    snprintf(portstr, sizeof(portstr), "%d", g_lc.port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g_lc.host, portstr, &hints, &res) != 0) {
        return -2;
    }
    for (rp = res; rp; rp = rp->ai_next) {
        fd = (int)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, (int)rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        return -3;
    }
    g_lc.fd = fd;
    for (i = 0; i < 16; ++i) {
        key_raw[i] = (char)(rand() & 0xff);
    }
    /* base64 16 bytes -> 24 chars; reuse server-side style via sha1 helper file's b64? */
    {
        static const char *B64 =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        int o = 0;
        for (i = 0; i < 16; i += 3) {
            unsigned v = ((unsigned char)key_raw[i] << 16);
            if (i + 1 < 16) {
                v |= ((unsigned char)key_raw[i + 1] << 8);
            }
            if (i + 2 < 16) {
                v |= (unsigned char)key_raw[i + 2];
            }
            key_b64[o++] = B64[(v >> 18) & 63];
            key_b64[o++] = B64[(v >> 12) & 63];
            key_b64[o++] = (i + 1 < 16) ? B64[(v >> 6) & 63] : '=';
            key_b64[o++] = (i + 2 < 16) ? B64[v & 63] : '=';
        }
        key_b64[o] = '\0';
    }
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n",
             g_lc.path, g_lc.host, g_lc.port, key_b64);
    if (send(fd, req, (int)strlen(req), 0) < 0) {
        close(fd);
        g_lc.fd = -1;
        return -4;
    }
    set_nonblock(fd);
    g_lc.connected = 1;
    g_lc.handshake_done = 0;
    g_lc.rx_http_len = 0;
    return 0;
}

void snes_lobby_disconnect(void)
{
    if (g_lc.fd >= 0) {
        close(g_lc.fd);
    }
    {
        char dname[SNES_LOBBY_NAME_LEN];
        /* The memset below would drop the pointer, not the agent: closing it
         * first is the difference between ending a transfer and leaking a
         * live UDP socket every time the lobby reconnects. */
        if (g_lc.xfer) rnet_ice_xfer_close(&g_lc.xfer);
        strncpy(dname, g_lc.display_name, sizeof(dname) - 1);
        memset(&g_lc, 0, sizeof(g_lc));
        g_lc.fd = -1;
        strncpy(g_lc.display_name, dname, sizeof(g_lc.display_name) - 1);
        member_rtt_clear();
    }
}

int snes_lobby_connected(void)
{
    return g_lc.connected && g_lc.fd >= 0;
}

const char *snes_lobby_url(void)
{
    if (!snes_lobby_connected() || !g_lc.url[0])
        return "";
    return g_lc.url;
}

void snes_lobby_set_display_name(const char *name)
{
    if (!name) {
        return;
    }
    strncpy(g_lc.display_name, name, sizeof(g_lc.display_name) - 1);
    g_lc.display_name[sizeof(g_lc.display_name) - 1] = '\0';
}

const char *snes_lobby_display_name(void)
{
    return g_lc.display_name;
}

const char *snes_lobby_player_id(void)
{
    return g_lc.player_id;
}

void snes_lobby_pump(void)
{
    char buf[4096];
#if defined(_WIN32)
    int n;
#else
    ssize_t n;
#endif
    /* Driven from the lobby pump so a transfer runs while the player sits in
     * the waiting room -- which is the only time one happens. Deliberately
     * before the connected() check: an agent mid-handshake still has to be
     * pumped so it can fail cleanly rather than hang if the WS drops. */
    snes_lobby_mod_xfer_pump();
    if (!snes_lobby_connected()) {
        return;
    }
    if (!g_lc.handshake_done) {
        n = recv(g_lc.fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (socket_would_block()) {
                return;
            }
            snes_lobby_disconnect();
            return;
        }
        if (n == 0) {
            snes_lobby_disconnect();
            return;
        }
        if (g_lc.rx_http_len + (size_t)n >= sizeof(g_lc.rx_http)) {
            snes_lobby_disconnect();
            return;
        }
        memcpy(g_lc.rx_http + g_lc.rx_http_len, buf, (size_t)n);
        g_lc.rx_http_len += (size_t)n;
        g_lc.rx_http[g_lc.rx_http_len] = '\0';
        {
            char *hdr_end = strstr(g_lc.rx_http, "\r\n\r\n");
            if (hdr_end) {
                size_t hdr_len;
                size_t leftover;
                if (!strstr(g_lc.rx_http, "101")) {
                    snes_lobby_disconnect();
                    return;
                }
                hdr_len = (size_t)(hdr_end - g_lc.rx_http) + 4;
                leftover = g_lc.rx_http_len > hdr_len ? g_lc.rx_http_len - hdr_len : 0;
                g_lc.handshake_done = 1;
                g_lc.ws_pending_len = 0;
                if (leftover > 0 && leftover <= sizeof(g_lc.ws_pending)) {
                    memcpy(g_lc.ws_pending, g_lc.rx_http + hdr_len, leftover);
                    g_lc.ws_pending_len = leftover;
                }
                g_lc.rx_http_len = 0;
                flush_pending();
                drain_ws_pending();
            }
        }
        return;
    }
    flush_pending();
    drain_ws_pending();
    for (;;) {
        size_t available = sizeof(g_lc.ws_pending) - g_lc.ws_pending_len;
        if (available == 0) {
            snes_lobby_disconnect();
            return;
        }
        n = recv(g_lc.fd,
                 (char *)g_lc.ws_pending + g_lc.ws_pending_len,
                 (int)available, 0);
        if (n < 0) {
            if (socket_would_block()) break;
            snes_lobby_disconnect();
            return;
        }
        if (n == 0) {
            snes_lobby_disconnect();
            return;
        }
        g_lc.ws_pending_len += (size_t)n;
        drain_ws_pending();
        if (!snes_lobby_connected()) {
            break;
        }
    }
    /* Guests: probe host RTT about once per second while seated. */
    if (g_lc.in_lobby && !g_lc.is_host && !g_lc.launch_pending) {
        uint64_t now = lobby_mono_ms();
        if (now >= g_lc.rtt_next_ping_ms) {
            char ts[32];
            snprintf(ts, sizeof(ts), "%llu", (unsigned long long)now);
            (void)snes_lobby_send_signal(SNES_LOBBY_SIG_RTT_PING, 0, ts);
            g_lc.rtt_next_ping_ms = now + 1000ull;
        }
    }
}

void snes_lobby_set_game_identity(const char *game_name,
                                  const char *game_version)
{
    if (game_name) {
        strncpy(g_lc.filter_game_name, game_name,
                sizeof(g_lc.filter_game_name) - 1);
        g_lc.filter_game_name[sizeof(g_lc.filter_game_name) - 1] = '\0';
    } else {
        g_lc.filter_game_name[0] = '\0';
    }
    if (game_version && game_version[0]) {
        strncpy(g_lc.filter_game_version, game_version,
                sizeof(g_lc.filter_game_version) - 1);
        g_lc.filter_game_version[sizeof(g_lc.filter_game_version) - 1] = '\0';
    } else {
        strncpy(g_lc.filter_game_version, SNES_GAME_VERSION,
                sizeof(g_lc.filter_game_version) - 1);
        g_lc.filter_game_version[sizeof(g_lc.filter_game_version) - 1] = '\0';
    }
}

const char *snes_lobby_game_version(void)
{
    return effective_game_version(NULL);
}

void snes_lobby_request_list(void)
{
    queue_list_request();
    flush_pending();
}

int snes_lobby_list_count(void)
{
    return g_lc.list_count;
}

int snes_lobby_list_get(int index, SnesLobbyRow *out)
{
    if (!out || index < 0 || index >= g_lc.list_count) {
        return 0;
    }
    *out = g_lc.list[index];
    return 1;
}

int snes_lobby_create(const char *name, const char *game_name,
                     const char *game_version, const char *password,
                     const char *host_bind, const SnesLobbyMatchCaps *match_caps,
                     int max_slots)
{
    char msg[1536];
    char caps_json[512];
    const char *gn;
    const char *gv;
    int n;
    int slots;
    if (!snes_lobby_connected()) {
        return -1;
    }
    slots = max_slots;
    if (slots < 2) slots = 2;
    if (slots > SNES_LOBBY_MAX_MEMBERS) slots = SNES_LOBBY_MAX_MEMBERS;
    gn = game_name && game_name[0] ? game_name
         : (g_lc.filter_game_name[0] ? g_lc.filter_game_name : "Game");
    gv = effective_game_version(game_version);
    if (game_name && game_name[0])
        snes_lobby_set_game_identity(game_name, gv);
    strncpy(g_lc.my_bind, host_bind && host_bind[0] ? host_bind : "0.0.0.0:7777",
            sizeof(g_lc.my_bind) - 1);
    g_lc.join.last_error[0] = '\0';
    caps_json[0] = '\0';
    if (match_caps && match_caps->valid) {
        g_lc.match_caps = *match_caps;
        append_match_caps_json(caps_json, sizeof(caps_json), match_caps);
    }
    n = snprintf(msg, sizeof(msg),
                 "{\"op\":\"create\",\"name\":\"%s\",\"game_name\":\"%s\",\"game_version\":\"%s\",\"password\":\"%s\","
                 "\"max_slots\":%d,\"host_bind\":\"%s\",\"display_name\":\"%s\"%s}",
                 name && name[0] ? name : "Lobby", gn, gv,
                 password ? password : "", slots, g_lc.my_bind,
                 g_lc.display_name[0] ? g_lc.display_name : "Host", caps_json);
    if (n < 0 || (size_t)n >= sizeof(msg)) return -1;
    queue_send(msg);
    flush_pending();
    return 0;
}

/* Prefer caller bind; never advertise :0 — lobby rewrites that to peer_ip:0
 * and rnet_session_start_lan rejects port 0 (host falls offline, guest alone). */
static void snes_lobby_normalize_guest_bind(const char *guest_bind, char *out,
                                            size_t out_cap)
{
    int port;
    if (!out || out_cap < 8)
        return;
    out[0] = '\0';
    if (guest_bind && guest_bind[0] && endpoint_has_usable_port(guest_bind)) {
        strncpy(out, guest_bind, out_cap - 1);
        out[out_cap - 1] = '\0';
        return;
    }
    port = rnet_udp_find_free_port(/*preferred=*/7778, 32);
    if (port <= 0)
        port = 7778;
    snprintf(out, out_cap, "0.0.0.0:%d", port);
}

int snes_lobby_join(const char *lobby_id, const char *password, const char *guest_bind)
{
    char msg[SNES_LOBBY_MAX_MODS * 256 + 1024];
    char offer[SNES_LOBBY_MAX_MODS * 256 + 64];
    const char *gn;
    const char *gv;
    int n;
    if (!snes_lobby_connected() || !lobby_id) {
        return -1;
    }
    gn = g_lc.filter_game_name;
    gv = effective_game_version(NULL);
    snes_lobby_normalize_guest_bind(guest_bind, g_lc.my_bind, sizeof(g_lc.my_bind));
    g_lc.join.last_error[0] = '\0';
    offer[0] = '\0';
    if (g_mod_offer_fn && !append_mod_offer(offer, sizeof(offer))) {
        /* Refuse the join rather than send a short offer. A truncated offer
         * would have the server turn this peer away over mods it is holding,
         * and the player would be told to install something already present --
         * an error nobody can act on. */
        snprintf(g_lc.join.last_error, sizeof(g_lc.join.last_error),
                 "mod_offer_too_large");
        return -1;
    }
    n = snprintf(msg, sizeof(msg),
                 "{\"op\":\"join\",\"lobby_id\":\"%s\",\"password\":\"%s\",\"guest_bind\":\"%s\","
                 "\"display_name\":\"%s\",\"game_name\":\"%s\",\"game_version\":\"%s\"%s}",
                 lobby_id, password ? password : "", g_lc.my_bind,
                 g_lc.display_name[0] ? g_lc.display_name : "Guest", gn, gv,
                 offer);
    if (n < 0 || (size_t)n >= sizeof(msg)) {
        snprintf(g_lc.join.last_error, sizeof(g_lc.join.last_error),
                 "join_too_large");
        return -1;
    }
    queue_send(msg);
    flush_pending();
    return 0;
}

int snes_lobby_leave(void)
{
    queue_send("{\"op\":\"leave\"}");
    flush_pending();
    g_lc.in_lobby = 0;
    g_lc.is_host = 0;
    g_lc.host_player_id[0] = '\0';
    g_lc.member_count = 0;
    g_lc.local_ready = 0;
    g_lc.all_ready = 0;
    g_lc.launch_pending = 0;
    match_caps_clear(&g_lc.match_caps);
    return 0;
}

int snes_lobby_kick(int slot)
{
    char msg[64];
    if (!snes_lobby_connected() || !g_lc.in_lobby || !g_lc.is_host)
        return -1;
    if (slot < 0 || slot >= SNES_LOBBY_MAX_MEMBERS)
        return -1;
    snprintf(msg, sizeof(msg), "{\"op\":\"kick\",\"slot\":%d}", slot);
    queue_send(msg);
    flush_pending();
    return 0;
}

int snes_lobby_move(int from_slot, int to_slot)
{
    char msg[96];
    if (!snes_lobby_connected() || !g_lc.in_lobby || !g_lc.is_host)
        return -1;
    if (from_slot < 0 || from_slot >= SNES_LOBBY_MAX_MEMBERS ||
        to_slot < 0 || to_slot >= SNES_LOBBY_MAX_MEMBERS ||
        from_slot == to_slot)
        return -1;
    snprintf(msg, sizeof(msg),
             "{\"op\":\"move\",\"from_slot\":%d,\"to_slot\":%d}",
             from_slot, to_slot);
    queue_send(msg);
    flush_pending();
    return 0;
}

int snes_lobby_in_lobby(void)
{
    return g_lc.in_lobby;
}

int snes_lobby_is_host(void)
{
    return g_lc.is_host;
}

const char *snes_lobby_host_player_id(void)
{
    return g_lc.host_player_id;
}

const SnesLobbyJoinInfo *snes_lobby_join_info(void)
{
    return &g_lc.join;
}

int snes_lobby_need_mods_count(void)
{
    return g_lc.need_mods_count;
}

const SnesLobbyModPkg *snes_lobby_need_mods_get(int index)
{
    if (index < 0 || index >= g_lc.need_mods_count)
        return NULL;
    return &g_lc.need_mods[index];
}

int snes_lobby_need_mods_can_transfer(void)
{
    return g_lc.need_mods_can_transfer;
}

const SnesLobbyMatchCaps *snes_lobby_match_caps(void)
{
    return &g_lc.match_caps;
}

int snes_lobby_set_match_caps(const SnesLobbyMatchCaps *caps)
{
    char msg[768];
    char caps_json[512];
    int n;
    if (!snes_lobby_connected() || !g_lc.in_lobby || !g_lc.is_host || !caps || !caps->valid)
        return -1;
    g_lc.match_caps = *caps;
    caps_json[0] = '\0';
    append_match_caps_json(caps_json, sizeof(caps_json), caps);
    n = snprintf(msg, sizeof(msg), "{\"op\":\"set_match_caps\"%s}", caps_json);
    if (n < 0 || (size_t)n >= sizeof(msg)) return -1;
    queue_send(msg);
    flush_pending();
    return 0;
}

int snes_lobby_member_count(void)
{
    return g_lc.member_count;
}

int snes_lobby_member_get(int index, SnesLobbyMember *out)
{
    if (!out || index < 0 || index >= g_lc.member_count) {
        return 0;
    }
    *out = g_lc.members[index];
    return 1;
}

int snes_lobby_member_latency_ms(int slot)
{
    if (slot < 0 || slot >= SNES_LOBBY_MAX_MEMBERS)
        return -1;
    if (g_lc.host_player_id[0]) {
        int i;
        for (i = 0; i < g_lc.member_count; ++i) {
            if (g_lc.members[i].slot == slot &&
                strcmp(g_lc.members[i].player_id, g_lc.host_player_id) == 0)
                return -1; /* host row */
        }
    }
    return g_lc.member_rtt_ms[slot];
}

int snes_lobby_member_is_host(const SnesLobbyMember *member)
{
    const char *host_id;
    if (!member || !member->player_id[0])
        return 0;
    host_id = snes_lobby_host_player_id();
    return host_id && host_id[0] && strcmp(member->player_id, host_id) == 0;
}

int snes_lobby_local_ready(void)
{
    return g_lc.local_ready;
}

int snes_lobby_all_ready(void)
{
    return g_lc.all_ready != 0 && g_lc.in_lobby && g_lc.join.player_count >= 2;
}

/* Every set_ready carries this peer's installed set.
 *
 * The server stores it against our seat and echoes it to the whole room, which
 * is how the HOST learns what each peer actually has -- the input to the
 * launch gate. It rides on set_ready rather than only on join because the set
 * can change while sitting in the lobby (a player installs the missing mod, or
 * a transfer completes), and a stale offer would keep the match locked after
 * the reason to lock it is gone. */
static void send_set_ready(int ready)
{
    char msg[SNES_LOBBY_MAX_MODS * 256 + 256];
    char offer[SNES_LOBBY_MAX_MODS * 256 + 64];
    int n;

    offer[0] = '\0';
    if (g_mod_offer_fn && !append_mod_offer(offer, sizeof(offer))) {
        /* Ready without the offer rather than not ready at all: the host's
         * gate then sees "this peer claims nothing" and holds the match, which
         * is the safe direction, and the log says why. */
        offer[0] = '\0';
        fprintf(stderr, "snes_lobby: could not announce the installed mod set "
                        "(too large); the host will see this peer as having "
                        "none\n");
    }
    n = snprintf(msg, sizeof(msg), "{\"op\":\"set_ready\",\"ready\":%s%s}",
                 ready ? "true" : "false", offer);
    if (n < 0 || (size_t)n >= sizeof(msg)) {
        queue_send(ready ? "{\"op\":\"set_ready\",\"ready\":true}"
                         : "{\"op\":\"set_ready\",\"ready\":false}");
        return;
    }
    queue_send(msg);
}

int snes_lobby_set_ready(int ready)
{
    if (!snes_lobby_connected() || !g_lc.in_lobby) {
        return -1;
    }
    send_set_ready(ready);
    flush_pending();
    return 0;
}

/* Does the peer in `slot` hold every package the host's plan names?
 *
 * A peer that has announced nothing counts as missing everything. That is the
 * safe reading and NOT a guess: a peer running this build always announces,
 * so silence means either an older build or a set too large to state, and in
 * both cases we do not know that it can play. */
static int member_missing_count(int slot)
{
    const SnesLobbyMatchCaps *caps = &g_lc.match_caps;
    int missing = 0;
    int i;
    int j;

    if (slot < 0 || slot >= SNES_LOBBY_MAX_MEMBERS)
        return 0;
    for (i = 0; i < caps->mod_count; ++i) {
        int have = 0;
        for (j = 0; j < g_lc.member_offer_count[slot]; ++j) {
            /* Matched on id ALONE, deliberately.
             *
             * The lobby's job is "does this player have the package at all",
             * because that is the question a download answers. Whether the
             * two builds resolve to the SAME simulation is settled later and
             * far more precisely: snes_netplay_rb exchanges the whole
             * effective set at session start -- per feature, with versions and
             * resolved option values -- and refuses on any difference, with
             * SNES_MODSET_VERSION as its own verdict for "right mod, wrong
             * version". Repeating a coarser version check here only adds a
             * second, earlier, less informative way to say no. */
            if (!strcmp(g_lc.member_offer[slot][j].id, caps->mods[i].id)) {
                have = 1;
                break;
            }
        }
        if (!have)
            missing++;
    }
    return missing;
}

int snes_lobby_match_blocked_by_mods(char *who, size_t who_cap,
                                     char *what, size_t what_cap)
{
    int n;
    int blocked = 0;

    if (who && who_cap) who[0] = '\0';
    if (what && what_cap) what[0] = '\0';
    if (!g_lc.in_lobby || g_lc.match_caps.mod_count <= 0)
        return 0;
    for (n = 0; n < g_lc.member_count; ++n) {
        int missing;
        if (!strcmp(g_lc.members[n].player_id, g_lc.player_id))
            continue;              /* the host runs the plan by definition */
        missing = member_missing_count(n);
        if (missing <= 0)
            continue;
        blocked += missing;
        if (who && who_cap && !who[0])
            snprintf(who, who_cap, "%s", g_lc.members[n].display_name);
        if (what && what_cap && !what[0]) {
            const SnesLobbyMatchCaps *caps = &g_lc.match_caps;
            int i;
            int j;
            for (i = 0; i < caps->mod_count; ++i) {
                int have = 0;
                for (j = 0; j < g_lc.member_offer_count[n]; ++j)
                    if (!strcmp(g_lc.member_offer[n][j].id, caps->mods[i].id)) {
                        have = 1;
                        break;
                    }
                if (!have) {
                    snprintf(what, what_cap, "%s@%s", caps->mods[i].id,
                             caps->mods[i].ver);
                    break;
                }
            }
        }
    }
    return blocked;
}

int snes_lobby_local_missing_mods(void)
{
    int n;
    for (n = 0; n < g_lc.member_count; ++n)
        if (!strcmp(g_lc.members[n].player_id, g_lc.player_id))
            return member_missing_count(n);
    return 0;
}

int snes_lobby_request_start(const SnesLobbyMatchCaps *match_caps)
{
    char msg[SNES_LOBBY_MAX_MODS * 256 + 1024];
    char caps_json[SNES_LOBBY_MAX_MODS * 256 + 512];
    char who[SNES_LOBBY_NAME_LEN];
    char what[SNES_LOBBY_MOD_ID_LEN + SNES_LOBBY_MOD_VER_LEN + 2];
    int n;
    if (!snes_lobby_connected() || !g_lc.in_lobby || !g_lc.is_host) {
        return -1;
    }
    /* THE gate. Not the door: a peer without the mods is welcome in the room,
     * and is expected to be here -- this is where they see what is missing and
     * pull it from the host. What cannot happen is the match starting while
     * two peers would patch guest memory differently, because that is a desync
     * dressed up as a match. Checked against every seated peer at the moment
     * of starting, which is the only moment the answer is current. */
    if (snes_lobby_match_blocked_by_mods(who, sizeof(who), what,
                                         sizeof(what)) > 0) {
        snprintf(g_lc.join.last_error, sizeof(g_lc.join.last_error),
                 "peer_needs_mods");
        fprintf(stderr,
                "snes_lobby: not starting -- %s does not have %s (and possibly "
                "more). They can download it from you in the lobby.\n",
                who[0] ? who : "a player", what[0] ? what : "a required mod");
        return -1;
    }
    caps_json[0] = '\0';
    if (match_caps && match_caps->valid) {
        g_lc.match_caps = *match_caps;
        append_match_caps_json(caps_json, sizeof(caps_json), match_caps);
    }
    n = snprintf(msg, sizeof(msg), "{\"op\":\"start\"%s}", caps_json);
    if (n < 0 || (size_t)n >= sizeof(msg)) return -1;
    queue_send(msg);
    flush_pending();
    return 0;
}

int snes_lobby_launch_pending(void)
{
    return g_lc.launch_pending;
}

void snes_lobby_clear_launch_pending(void)
{
    g_lc.launch_pending = 0;
}

void snes_lobby_clear_last_error(void)
{
    g_lc.join.last_error[0] = '\0';
}

int snes_lobby_try_fill_launch(SnesLobbyJoinInfo *out)
{
    const SnesLobbyJoinInfo *join;
    if (!out || !g_lc.launch_pending)
        return 0;
    join = &g_lc.join;
    if (!join->bind_hostport[0])
        return 0;
    /* Guests need a concrete host peer. Host may leave peer empty so transport
     * learns the guest from the first UDP packet. */
    if (join->local_slot != 0 && !join->peer_hostport[0])
        return 0;
    *out = *join;
    return 1;
}

/* ---- peer-to-peer mod transfer -----------------------------------------
 *
 * Bytes go over a dedicated ICE agent straight between the two players. The
 * lobby server carries only the SDP and candidate lines needed to build that
 * connection, on the same relay the seats already use.
 */

/* What an agent EMITS is not what its peer must be PUSHED.
 *
 * An agent describes itself with LOCAL_SDP / LOCAL_CANDIDATE; the far side has
 * to receive those as REMOTE_*, because to it they are the remote description.
 * Forwarded unchanged, both agents gather candidates, neither ever learns the
 * other's description, and both sit in "connecting" until they time out --
 * with nothing in either log to show they were talking past each other.
 *
 * snes_netplay.c does the same translation for the game's own session and says
 * so in a one-line comment. Kept as a named function here so the rule is a
 * thing that can be asserted, not a pair of ifs buried in a message handler.
 *
 * GATHERING_DONE and SET_CONTROLLING mean the same on both sides and pass
 * through. */
static int mod_ice_type_for_push(int emitted_type)
{
    if (emitted_type == (int)RNET_SIGNAL_LOCAL_SDP)
        return (int)RNET_SIGNAL_REMOTE_SDP;
    if (emitted_type == (int)RNET_SIGNAL_LOCAL_CANDIDATE)
        return (int)RNET_SIGNAL_REMOTE_CANDIDATE;
    return emitted_type;
}

static void mod_xfer_emit(const RNetSignal *msg, void *user)
{
    (void)user;
    if (!msg) return;
    (void)snes_lobby_send_signal_to(g_lc.xfer_peer,
                                    SNES_LOBBY_SIG_MOD_ICE_BASE + (int)msg->type,
                                    (int)msg->flag, msg->text);
}

static void mod_xfer_reset(void)
{
    if (g_lc.xfer) rnet_ice_xfer_close(&g_lc.xfer);
    g_lc.xfer = NULL;
    g_lc.xfer_busy = 0;
    g_lc.xfer_sending = 0;
    g_lc.xfer_peer[0] = '\0';
    g_lc.xfer_id[0] = '\0';
    g_lc.xfer_ver[0] = '\0';
    g_lc.xfer_sha[0] = '\0';
    g_lc.xfer_expect = 0;
    g_lc.sig_hold_n = 0;
    g_lc.sig_hold_from[0] = '\0';
}

static void mod_xfer_fail(const char *why)
{
    snprintf(g_lc.xfer_err, sizeof(g_lc.xfer_err), "%s",
             why && why[0] ? why : "transfer failed");
    fprintf(stderr, "snes_lobby: mod transfer failed - %s\n", g_lc.xfer_err);
    g_lc.xfer_progress = -2;
    mod_xfer_reset();
}

/* Build the ICE config from the lobby's Coturn mint. Copied into g_lc because
 * RNetIceConfig holds borrowed pointers that must outlive the agent. */
static int mod_xfer_ice_config(RNetIceConfig *ice, int controlling)
{
    const SnesLobbyTurnCredentials *tc = snes_lobby_turn_credentials();
    rnet_ice_config_init_defaults(ice);
    /* The role is decided HERE, before the agent exists.
     *
     * rnet_ice_config_init_defaults leaves controlling = 1, and
     * rnet_ice_xfer_open starts gathering immediately -- so leaving it alone
     * made both peers offerers, which is what libjuice was reporting as
     * "ICE role conflict (both controlling)". It still connected, because ICE
     * resolves a conflict by comparing tiebreakers, but the two then raced to
     * offer instead of one offering and one answering.
     *
     * The answerer defers gathering until the offer arrives (see
     * rnet_ice_agent_start_gathering), which is also why the requester used to
     * emit candidates before the sender had an agent at all. */
    ice->controlling = (rnet_u8)(controlling ? 1 : 0);
    if (!tc || !tc->valid)
        return 0;              /* host-candidate only; fine on a LAN */
    if (tc->stun_host[0]) {
        snprintf(g_lc.ice_stun, sizeof(g_lc.ice_stun), "%s", tc->stun_host);
        ice->stun_host = g_lc.ice_stun;
        ice->stun_port = (rnet_u16)(tc->stun_port > 0 ? tc->stun_port : 3478);
    }
    if (tc->turn_host[0]) {
        snprintf(g_lc.ice_turn, sizeof(g_lc.ice_turn), "%s", tc->turn_host);
        snprintf(g_lc.ice_user, sizeof(g_lc.ice_user), "%s", tc->username);
        snprintf(g_lc.ice_pass, sizeof(g_lc.ice_pass), "%s", tc->password);
        ice->turn_host = g_lc.ice_turn;
        ice->turn_port = (rnet_u16)(tc->turn_port > 0 ? tc->turn_port : 3478);
        ice->turn_user = g_lc.ice_user;
        ice->turn_pass = g_lc.ice_pass;
    }
    return 1;
}

static int mod_xfer_open(const char *peer, int controlling)
{
    RNetIceConfig ice;

    if (g_lc.xfer) rnet_ice_xfer_close(&g_lc.xfer);
    (void)mod_xfer_ice_config(&ice, controlling);
    if (rnet_ice_xfer_open(&g_lc.xfer, &ice, mod_xfer_emit, NULL) != 0 ||
        !g_lc.xfer) {
        mod_xfer_fail("could not open a direct connection");
        return -1;
    }
    snprintf(g_lc.xfer_peer, sizeof(g_lc.xfer_peer), "%s", peer ? peer : "");
    /* No SET_CONTROLLING signal is pushed here. It would arrive after
     * rnet_ice_xfer_open has already started gathering, so it could only
     * change a flag after the decision it governs had been made -- the role
     * belongs in the config above, where the agent reads it. */
    g_lc.xfer_busy = 1;
    g_lc.xfer_progress = 0;
    g_lc.xfer_err[0] = '\0';
    g_lc.xfer_last_state = -1;
    g_lc.xfer_started_ms = lobby_mono_ms();
    g_lc.xfer_connected_ms = 0;

    /* Replay anything that arrived while we were still getting ready, but
     * only from the peer this agent is for -- a hold from an abandoned
     * exchange would poison the new one. */
    if (g_lc.sig_hold_n > 0) {
        if (g_lc.xfer_peer[0] && !strcmp(g_lc.sig_hold_from, g_lc.xfer_peer)) {
            int i;
            fprintf(stderr, "snes_lobby: replaying %d held ICE signal(s)\n",
                    g_lc.sig_hold_n);
            for (i = 0; i < g_lc.sig_hold_n; ++i)
                rnet_ice_xfer_push_signal(g_lc.xfer, &g_lc.sig_hold[i]);
        }
        g_lc.sig_hold_n = 0;
        g_lc.sig_hold_from[0] = '\0';
    }
    return 0;
}

/* HOST side: a seated peer asked for a package. */
static void mod_xfer_on_request(const char *from, const char *text)
{
    char id[SNES_LOBBY_MOD_ID_LEN];
    char ver[SNES_LOBBY_MOD_VER_LEN];
    const char *at;
    size_t idlen;
    uint8_t *blob = NULL;
    uint32_t len = 0;
    char sha[65];
    char err[192];
    char header[512];
    uint8_t *hdr_copy;

    /* Every refusal below is reported to the asking peer AND logged here.
     * The first version only sent the reason down the wire, so a host whose
     * export failed showed a clean log while the guest was told "could not
     * pack the mod" -- the one machine that knew why said nothing. */
    err[0] = '\0';
    sha[0] = '\0';

    if (!from || !from[0] || !text) return;
    fprintf(stderr, "snes_lobby: %s asked for \"%s\"\n", from, text);
    if (g_lc.xfer_busy) {
        fprintf(stderr, "snes_lobby: refusing - already transferring\n");
        (void)snes_lobby_send_signal_to(from, SNES_LOBBY_SIG_MOD_NAK, 0,
                                        "the host is already sending a mod; "
                                        "try again in a moment");
        return;
    }
    at = strchr(text, '@');
    if (!at) return;
    idlen = (size_t)(at - text);
    if (idlen == 0 || idlen >= sizeof(id)) return;
    memcpy(id, text, idlen);
    id[idlen] = '\0';
    snprintf(ver, sizeof(ver), "%s", at + 1);

    /* Only ever send something this host actually runs. The plan is the list
     * the guest was shown; anything else is a request we have no reason to
     * honour, and honouring it would let a peer pull arbitrary packages off
     * this machine by name. */
    {
        int i;
        int in_plan = 0;
        for (i = 0; i < g_lc.match_caps.mod_count; ++i)
            if (!strcmp(g_lc.match_caps.mods[i].id, id)) {
                in_plan = 1;
                snprintf(ver, sizeof(ver), "%s", g_lc.match_caps.mods[i].ver);
                break;
            }
        if (!in_plan) {
            fprintf(stderr,
                    "snes_lobby: refusing - \"%s\" is not in this host's "
                    "published plan (%d package(s))\n",
                    id, g_lc.match_caps.mod_count);
            (void)snes_lobby_send_signal_to(from, SNES_LOBBY_SIG_MOD_NAK, 0,
                                            "that mod is not part of this "
                                            "lobby's plan");
            return;
        }
    }

    if (!g_mod_export_fn) {
        /* Distinct from an export that ran and failed: this build never
         * installed the hook, which is a wiring fault on this side, not
         * anything about the package. Previously both said "could not pack
         * the mod" -- and read err[] uninitialized to decide which. */
        fprintf(stderr, "snes_lobby: refusing - no mod export hook is "
                        "installed in this build\n");
        (void)snes_lobby_send_signal_to(from, SNES_LOBBY_SIG_MOD_NAK, 0,
                                        "the host's build cannot send mods");
        return;
    }
    if (g_mod_export_fn(id, ver, &blob, &len, sha, sizeof(sha), err,
                           sizeof(err), g_mod_hook_ctx) != 1) {
        fprintf(stderr, "snes_lobby: refusing - packing %s@%s failed: %s\n",
                id, ver, err[0] ? err : "(the exporter gave no reason)");
        (void)snes_lobby_send_signal_to(from, SNES_LOBBY_SIG_MOD_NAK, 0,
                                        err[0] ? err : "could not pack the mod");
        return;
    }

    if (mod_xfer_open(from, /*controlling=*/1) != 0) {
        if (g_mod_free_fn) g_mod_free_fn(blob);
        return;
    }
    g_lc.xfer_sending = 1;
    snprintf(g_lc.xfer_id, sizeof(g_lc.xfer_id), "%s", id);
    snprintf(g_lc.xfer_ver, sizeof(g_lc.xfer_ver), "%s", ver);

    /* Two blobs: what is coming, then the thing itself. The digest travels in
     * the header so the receiver can check the payload against a value that
     * did not come from the payload. */
    snprintf(header, sizeof(header),
             "{\"id\":\"%s\",\"ver\":\"%s\",\"len\":%u,\"sha256\":\"%s\"}",
             id, ver, (unsigned)len, sha);
    hdr_copy = (uint8_t *)malloc(strlen(header) + 1);
    if (!hdr_copy) {
        if (g_mod_free_fn) g_mod_free_fn(blob);
        mod_xfer_fail("out of memory");
        return;
    }
    memcpy(hdr_copy, header, strlen(header) + 1);
    if (rnet_ice_xfer_queue_blob(g_lc.xfer, hdr_copy, strlen(header)) != 0 ||
        rnet_ice_xfer_queue_blob(g_lc.xfer, blob, (size_t)len) != 0) {
        mod_xfer_fail("could not queue the mod for sending");
        return;
    }
    fprintf(stderr, "snes_lobby: sending %s@%s (%u bytes) to %s\n", id, ver,
            (unsigned)len, from);
}

/* GUEST side: a completed blob arrived. */
static void mod_xfer_on_blob(uint8_t *data, size_t len)
{
    char err[192];
    char id[96];
    char ver[32];

    fprintf(stderr, "snes_lobby: received %u byte(s) over the direct link\n",
            (unsigned)len);
    if (!g_lc.xfer_sha[0]) {
        /* First blob is the header. */
        char text[600];
        size_t n = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
        memcpy(text, data, n);
        text[n] = '\0';
        char got_id[SNES_LOBBY_MOD_ID_LEN];
        got_id[0] = '\0';
        json_get_str(text, "sha256", g_lc.xfer_sha, sizeof(g_lc.xfer_sha));
        json_get_str(text, "id", got_id, sizeof(got_id));
        /* The host is answering a specific request. If it describes a
         * different package, something is confused on one side or the other
         * and installing it anyway would put a mod on this machine that
         * nobody asked for. */
        if (g_lc.xfer_id[0] && got_id[0] && strcmp(got_id, g_lc.xfer_id) != 0) {
            free(data);
            mod_xfer_fail("the host offered a different mod than the one "
                          "requested");
            return;
        }
        if (got_id[0])
            snprintf(g_lc.xfer_id, sizeof(g_lc.xfer_id), "%s", got_id);
        json_get_str(text, "ver", g_lc.xfer_ver, sizeof(g_lc.xfer_ver));
        g_lc.xfer_expect = (uint32_t)json_get_int(text, "len", 0);
        free(data);
        if (!g_lc.xfer_sha[0] || !g_lc.xfer_expect)
            mod_xfer_fail("the host described the mod in a way we cannot read");
        return;
    }

    if ((uint32_t)len != g_lc.xfer_expect) {
        free(data);
        mod_xfer_fail("the mod arrived a different size than the host said");
        return;
    }
    if (!g_mod_install_fn) {
        free(data);
        mod_xfer_fail("this build cannot install mods");
        return;
    }
    /* The digest is checked inside the install callback, before anything is
     * unpacked -- see snes_mod_runtime_install_blob_c. */
    if (g_mod_install_fn(data, (uint32_t)len, g_lc.xfer_sha, id, sizeof(id),
                            ver, sizeof(ver), err, sizeof(err),
                            g_mod_hook_ctx) != 1) {
        free(data);
        mod_xfer_fail(err[0] ? err : "the mod could not be installed");
        return;
    }
    free(data);
    fprintf(stderr, "snes_lobby: installed %s@%s from the host\n", id, ver);
    g_lc.xfer_progress = 100;
    mod_xfer_reset();
    /* Re-announce: we now hold something we did not a moment ago, and the
     * host's launch gate is reading that announcement. */
    send_set_ready(g_lc.local_ready ? 1 : 0);
    flush_pending();
}

void snes_lobby_mod_xfer_pump(void)
{
    uint8_t *data = NULL;
    size_t len = 0;
    char err[160];

    if (!g_lc.xfer) return;
    rnet_ice_xfer_pump(g_lc.xfer);

    /* Narrate the handshake. Without this a transfer that never connects and
     * one that connects and stalls look identical from the log: both are just
     * silence after "asked the host". */
    {
        const RNetIceState st = rnet_ice_xfer_state(g_lc.xfer);
        if ((int)st != g_lc.xfer_last_state) {
            g_lc.xfer_last_state = (int)st;
            fprintf(stderr, "snes_lobby: mod transfer link is %s\n",
                    rnet_ice_state_name(st));
            if ((st == RNET_ICE_STATE_CONNECTED ||
                 st == RNET_ICE_STATE_COMPLETED) && !g_lc.xfer_connected_ms)
                g_lc.xfer_connected_ms = lobby_mono_ms();
        }
    }

    if (rnet_ice_xfer_failed(g_lc.xfer, err, sizeof(err))) {
        mod_xfer_fail(err);
        return;
    }

    /* Stall watchdog.
     *
     * A transfer that never connects used to sit at "busy" forever, and
     * because only one runs at a time every later attempt was refused with
     * "could not start the download" -- a message about the second click that
     * was really about the first one never ending. ICE either finds a path in
     * well under this or it is not going to. */
    {
        const uint64_t now = lobby_mono_ms();
        const uint64_t age = now - g_lc.xfer_started_ms;
        if (!g_lc.xfer_connected_ms && age > 45000u) {
            mod_xfer_fail("could not open a direct connection to the other "
                          "player (no route found)");
            return;
        }
        if (g_lc.xfer_connected_ms &&
            now - g_lc.xfer_connected_ms > 180000u) {
            mod_xfer_fail("the transfer stopped making progress");
            return;
        }
    }

    {
        const int p = rnet_ice_xfer_progress(g_lc.xfer);
        if (p >= 0) g_lc.xfer_progress = p;
    }
    while (rnet_ice_xfer_take_blob(g_lc.xfer, &data, &len)) {
        mod_xfer_on_blob(data, len);
        if (!g_lc.xfer) return;      /* finished or failed inside */
        data = NULL;
        len = 0;
    }
    /* The sender is done when everything queued has left. */
    if (g_lc.xfer_sending && rnet_ice_xfer_send_idle(g_lc.xfer)) {
        fprintf(stderr, "snes_lobby: %s@%s sent\n", g_lc.xfer_id, g_lc.xfer_ver);
        g_lc.xfer_progress = -1;
        mod_xfer_reset();
    }
}

int snes_lobby_mod_request(const char *package_id, const char *version)
{
    char text[192];
    const char *host = g_lc.host_player_id;

    if (!snes_lobby_connected() || !g_lc.in_lobby) return -1;
    if (g_lc.is_host) return -1;            /* the host IS the source */
    if (!host || !host[0]) return -1;
    if (g_lc.xfer_busy) {
        fprintf(stderr, "snes_lobby: not asking for %s - already transferring "
                        "%s\n", package_id ? package_id : "?", g_lc.xfer_id);
        return -2;              /* distinct: busy, not broken */
    }
    if (!package_id || !package_id[0]) return -1;

    if (mod_xfer_open(host, /*controlling=*/0) != 0) return -1;
    g_lc.xfer_sending = 0;
    g_lc.xfer_sha[0] = '\0';
    g_lc.xfer_expect = 0;
    /* Named NOW, not when the header arrives.
     *
     * snes_lobby_mod_in_flight() is what attributes progress to a row, and
     * what the "already transferring" message quotes. Left empty until the
     * first blob, the requesting side spent the whole connect phase unable to
     * say which package it was fetching: no row showed a bar, and the busy
     * refusal read "already transferring " with a blank where the name goes. */
    snprintf(g_lc.xfer_id, sizeof(g_lc.xfer_id), "%s", package_id);
    snprintf(g_lc.xfer_ver, sizeof(g_lc.xfer_ver), "%s", version ? version : "");
    snprintf(text, sizeof(text), "%s@%s", package_id, version ? version : "");
    if (snes_lobby_send_signal_to(host, SNES_LOBBY_SIG_MOD_REQ, 0, text) != 0) {
        mod_xfer_fail("could not reach the host");
        return -1;
    }
    fprintf(stderr, "snes_lobby: asked the host for %s\n", text);
    return 0;
}

void snes_lobby_mod_cancel(void)
{
    if (!g_lc.xfer_busy) return;
    fprintf(stderr, "snes_lobby: mod transfer cancelled\n");
    g_lc.xfer_progress = -1;
    mod_xfer_reset();
}

int snes_lobby_mod_progress(void)
{
    return g_lc.xfer_busy ? g_lc.xfer_progress : (g_lc.xfer_progress == -2 ? -2 : -1);
}

int snes_lobby_mod_failed(char *err, size_t err_cap)
{
    if (!g_lc.xfer_err[0]) return 0;
    if (err && err_cap) snprintf(err, err_cap, "%s", g_lc.xfer_err);
    return 1;
}

const char *snes_lobby_mod_in_flight(void)
{
    return g_lc.xfer_busy ? g_lc.xfer_id : "";
}

void snes_lobby_set_mod_transfer_hooks(SnesLobbyModExportFn export_fn,
                                       SnesLobbyModFreeFn free_fn,
                                       SnesLobbyModInstallFn install_fn,
                                       void *ctx)
{
    g_mod_export_fn = export_fn;
    g_mod_free_fn = free_fn;
    g_mod_install_fn = install_fn;
    g_mod_hook_ctx = ctx;
}

int snes_lobby_send_signal_to(const char *to_player_id, int type, int flag,
                              const char *text)
{
    char esc[4096];
    char msg[4608];
    const char *lid;
    if (!snes_lobby_connected() || !g_lc.in_lobby) {
        return -1;
    }
    lid = g_lc.join.lobby_id[0] ? g_lc.join.lobby_id : "";
    json_escape(text ? text : "", esc, sizeof(esc));
    /* An empty to_player_id broadcasts to the other seated members, which is
     * right for the game's own ICE but wrong for a transfer: a third player
     * would push a stranger's SDP into their agent and corrupt a negotiation
     * they are not part of. */
    snprintf(msg, sizeof(msg),
             "{\"op\":\"signal\",\"lobby_id\":\"%s\",\"to_player_id\":\"%s\","
             "\"type\":%d,\"flag\":%d,\"text\":\"%s\"}",
             lid, to_player_id ? to_player_id : "", type, flag, esc);
    /* Write immediately — ICE candidates arrive in bursts larger than pending_tx. */
    if (g_lc.handshake_done && g_lc.fd >= 0) {
        if (rnet_ws_write_text(g_lc.fd, msg, 1) < 0)
            return -1;
        return 0;
    }
    queue_send(msg);
    return 0;
}

int snes_lobby_send_signal(int type, int flag, const char *text)
{
    return snes_lobby_send_signal_to("", type, flag, text);
}

int snes_lobby_poll_signal(int *type, int *flag, char *text, size_t text_cap)
{
    int i;
    if (g_lc.sig_count <= 0) {
        return 0;
    }
    i = g_lc.sig_head;
    if (type) *type = g_lc.sig_q[i].type;
    if (flag) *flag = g_lc.sig_q[i].flag;
    if (text && text_cap) {
        strncpy(text, g_lc.sig_q[i].text, text_cap - 1);
        text[text_cap - 1] = '\0';
    }
    g_lc.sig_head = (g_lc.sig_head + 1) % (int)(sizeof(g_lc.sig_q) / sizeof(g_lc.sig_q[0]));
    g_lc.sig_count--;
    return 1;
}

int snes_lobby_request_turn_credentials(void)
{
    if (!snes_lobby_connected())
        return -1;
    /* Refresh if missing, expired, or never requested. */
    if (g_lc.turn.valid && g_lc.turn_received_at > 0 && g_lc.turn.ttl_secs > 0) {
        time_t now = time(NULL);
        if (now >= g_lc.turn_received_at &&
            (uint32_t)(now - g_lc.turn_received_at) + 60u < g_lc.turn.ttl_secs) {
            return 0; /* still fresh (60s skew margin) */
        }
    }
    return queue_turn_credentials_request();
}

const SnesLobbyTurnCredentials *snes_lobby_turn_credentials(void)
{
    if (g_lc.turn.valid && g_lc.turn_received_at > 0 && g_lc.turn.ttl_secs > 0) {
        time_t now = time(NULL);
        if (now < g_lc.turn_received_at ||
            (uint32_t)(now - g_lc.turn_received_at) >= g_lc.turn.ttl_secs) {
            clear_turn_credentials();
        }
    }
    return &g_lc.turn;
}

#endif /* SNES_HAS_LOBBY_CLIENT */

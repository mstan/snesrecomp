#include "snes_netplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#if defined(SNESRECOMP_NET)
#include "recomp_net/recomp_net.h"
/*
 * Rollback is an optional build (snesrecomp_enable_rollback). A game that
 * links netplay without it must still compile, so everything the rollback
 * host provides is reached through the shims below rather than by including
 * its header unconditionally — that header pulls in retcomm-rbengine, which
 * is not on the include path unless rollback was enabled.
 */
#if defined(SNESRECOMP_NET_ROLLBACK)
#include "snes_netplay_rb.h"
#else
#include <stdint.h>
struct SnesNetplayRbBindings;
static inline int  snes_netplay_rb_enabled(void) { return 0; }
static inline void snes_netplay_rb_set_default(int on) { (void)on; }
static inline void snes_netplay_rb_bind(const struct SnesNetplayRbBindings *b) { (void)b; }
static inline int  snes_netplay_rb_start(void) { return 0; }
static inline void snes_netplay_rb_shutdown(void) {}
static inline int  snes_netplay_rb_poll_admit(void) { return 0; }
static inline void snes_netplay_rb_finish_frame(void) {}
static inline void snes_netplay_rb_stage_local(uint16_t buttons) { (void)buttons; }
static inline uint32_t snes_netplay_rb_sim_tick(void) { return 0; }
#endif
#include "common_rtl.h"
#include "common_cpu_infra.h"
#if defined(SNES_HAS_LOBBY_CLIENT)
#include "snes_lobby_client.h"
#endif
#include "desktop/sdl_compat.h"
#endif

void snes_netplay_config_defaults(SnesNetplayConfig *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->local_slot = 0;
    cfg->slot_count = 2;
    cfg->input_player = -1; /* auto → resolve at start */
    cfg->input_delay = 2;
    /* Framework default: ROLLBACK. recomp-ui's lobby settles the mode
     * room-wide and also defaults it on; builds without the rollback host
     * ignore the field. Rolled out at scale 2026-08-29 (Alex) after LAN
     * delay-7 sessions validated the transport. */
    cfg->rollback = 1;
    cfg->session_id = 1;
    cfg->transport = 0;
    strncpy(cfg->bind_hostport, "0.0.0.0:7777", sizeof(cfg->bind_hostport) - 1);
    strncpy(cfg->peer_hostport, "127.0.0.1:7778", sizeof(cfg->peer_hostport) - 1);
}

static unsigned env_u(const char *name, unsigned def)
{
    const char *v = getenv(name);
    if (!v || !v[0]) return def;
    return (unsigned)strtoul(v, NULL, 10);
}

void snes_netplay_apply_env(SnesNetplayConfig *cfg)
{
    const char *v;
    if (!cfg) return;
    v = getenv("SNES_NETPLAY");
    if (v && v[0] && v[0] != '0') cfg->enabled = 1;
    v = getenv("SNES_NET_SLOT");
    if (v && v[0]) cfg->local_slot = (int)strtol(v, NULL, 10);
    v = getenv("SNES_NET_SLOTS");
    if (v && v[0]) cfg->slot_count = (int)strtol(v, NULL, 10);
    v = getenv("SNES_NET_INPUT_PLAYER");
    if (v && v[0]) cfg->input_player = (int)strtol(v, NULL, 10);
    v = getenv("SNES_NET_DELAY");
    if (v && v[0]) cfg->input_delay = (int)strtol(v, NULL, 10);
    cfg->session_id = env_u("SNES_NET_SESSION_ID", cfg->session_id);
    v = getenv("SNES_NET_BIND");
    if (v && v[0]) {
        strncpy(cfg->bind_hostport, v, sizeof(cfg->bind_hostport) - 1);
        cfg->bind_hostport[sizeof(cfg->bind_hostport) - 1] = '\0';
    }
    v = getenv("SNES_NET_PEER");
    if (v && v[0]) {
        strncpy(cfg->peer_hostport, v, sizeof(cfg->peer_hostport) - 1);
        cfg->peer_hostport[sizeof(cfg->peer_hostport) - 1] = '\0';
    }
    v = getenv("SNES_RB_PREDICTION");
    if (v && v[0]) cfg->input_prediction = (int)strtol(v, NULL, 10);
    v = getenv("SNES_NET_MODE");
    if (v && v[0]) {
        /* Operator override, both directions: "rollback"/"rb" force
         * rollback, anything else ("delay", ...) forces delay-sync. */
        cfg->rollback =
            (strcmp(v, "rollback") == 0 || strcmp(v, "rb") == 0) ? 1 : 0;
    }
    v = getenv("SNES_NET_TRANSPORT");
    if (v && v[0]) {
        if (strcmp(v, "ice") == 0 || strcmp(v, "ICE") == 0)
            cfg->transport = 1;
        else if (strcmp(v, "lan") == 0 || strcmp(v, "LAN") == 0)
            cfg->transport = 2;
    }
}

static SnesNetplayCaptureSyncBytes g_capture_sync_bytes;
static SnesNetplayApplySyncBytes g_apply_sync_bytes;

void snes_netplay_set_sync_byte_hooks(SnesNetplayCaptureSyncBytes capture,
                                      SnesNetplayApplySyncBytes apply)
{
    g_capture_sync_bytes = capture;
    g_apply_sync_bytes = apply;
}

#if !defined(SNESRECOMP_NET)

int  snes_netplay_rollback_active(void) { return 0; }

int  snes_netplay_active(void) { return 0; }
int  snes_netplay_is_running(void) { return 0; }
const char *snes_netplay_transport_name(void) { return "none"; }
int  snes_netplay_ice_failed(void) { return 0; }
int  snes_netplay_local_slot(void) { return -1; }
int  snes_netplay_slot_count(void) { return 2; }
int  snes_netplay_input_player(void) { return 0; }
uint32_t snes_netplay_sim_tick(void) { return 0; }
uint32_t snes_netplay_frames_finished(void) { return 0; }
int  snes_netplay_start(const SnesNetplayConfig *cfg)
{
    (void)cfg;
    return -1;
}
void snes_netplay_shutdown(void) {}
void snes_netplay_connect_wait_reset(void) {}
int  snes_netplay_connect_timed_out(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return 0;
}
void snes_netplay_stage_local(uint16_t buttons) { (void)buttons; }
int  snes_netplay_needs_local_sample(void) { return 0; }
int  snes_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash)
{
    (void)tick;
    (void)local_hash;
    (void)remote_hash;
    return 0;
}
int  snes_netplay_peer_disconnected(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return 0;
}
int  snes_netplay_poll_admit(void) { return 1; }
void snes_netplay_pump(void) {}
void snes_netplay_apply_host_sync(void) {}
void snes_netplay_finish_frame(void) {}
int  snes_netplay_remote_lead(void) { return 0; }
int  snes_netplay_input_delay(void) { return 2; }
uint32_t snes_netplay_published_inputs(void) { return 0; }
uint32_t snes_netplay_active_mask(void) { return 0; }

static int g_return_to_lobby_stub;
void snes_netplay_request_return_to_lobby(void) { g_return_to_lobby_stub = 1; }
int  snes_netplay_return_to_lobby_requested(void) { return g_return_to_lobby_stub; }
void snes_netplay_clear_return_to_lobby(void) { g_return_to_lobby_stub = 0; }

int  snes_netplay_is_host(void) { return 0; }
int  snes_netplay_request_save(int slot)
{
    (void)slot;
    return 0;
}
int  snes_netplay_request_load(int slot)
{
    (void)slot;
    return 0;
}
int  snes_netplay_state_barrier(void) { return 0; }
void snes_netplay_diag_tick(void) {}

#else /* SNESRECOMP_NET */

/* App-layer LOAD phases. Library busy covers probe/xfer; xfer!=NONE also
 * covers the post-apply ready rendezvous (size==0 probe does not stall
 * try_admit by itself). */
enum {
    NP_XFER_NONE = 0,
    NP_XFER_LOAD_PROBE,
    NP_XFER_LOAD_SEND,
    NP_XFER_LOAD_READY
};
#define NP_LOAD_READY_CRC 0x4C4F4144u /* 'LOAD' */
#define NP_LOAD_COOLDOWN_MS 1500u

typedef struct {
    RNetSession *session;
    uint16_t     staged_buttons;
    int          staged_valid;
    int          active;
    int          slot_count;
    int          local_slot;
    int          input_player; /* resolved 0/1 */
    int          needs_advance;
    int          latched_for_tick;
    uint32_t     latched_sim_tick;
    uint16_t     published[SNES_NETPLAY_MAX_SLOTS];
    uint8_t      host_sync[2];       /* game-defined slot-0 sync bytes */
    int          host_sync_valid;
    int          use_ice;
    int          guest_sandbox;      /* save root redirected to saves/netplay */
    int          sram_sync_sent;     /* host: SRAM blob transfer started */
    int          sram_sync_done;     /* both: initial SRAM sync finished */
    int          host_sram_applied;  /* host already has live SRAM */
    /* LOAD sync FSM (MotK-style probe → optional xfer → ready → hard_resync). */
    int          xfer;               /* NP_XFER_* */
    int          xfer_slot;
    int          load_applied_local; /* snapshot applied; waiting ready/resync */
    uint32_t     load_cooldown_until_ms; /* debounce after completed load */
    /* Owned buffers for RNetIceConfig pointers (juice may retain them). */
    char         ice_stun_host[128];
    char         ice_turn_host[128];
    char         ice_turn_user[192];
    char         ice_turn_pass[128];
    char         ice_bind_addr[RNET_IPV4_ADDRESS_TEXT_MAX];
    int          ice_has_turn;
    /* Match metadata for net_diag.jsonl summary line. */
    char         match_mode[24];   /* hosted_lobby | direct_ip */
    char         lobby_server[256];
    char         lobby_id[64];
    char         bind_hostport[64];
    char         peer_hostport[64];
    int          input_delay;
    int          input_prediction;   /* P; 0 = engine default */
    int          force_input_relay;
    uint32_t     session_id;
    int          is_host;
    unsigned     ice_stun_port;
    unsigned     ice_turn_port;
    int          diag_session; /* bumped each start; resets diag file */
    uint32_t     frames_finished; /* RtlRunFrame + finish_frame count */
} NetplayState;

static NetplayState g_np;

/*
 * Apply one published row set to the runtime seats.
 *
 * Seats 0 and 1 stay in g_np.published for snes_netplay_published_inputs(),
 * which every existing game reads and packs into RtlRunFrame. Seats 2..7
 * cannot fit that word, so they go straight in through RtlSetPadState — the
 * same entry point a local multitap game uses. A game therefore needs no
 * netplay-specific code to gain seats beyond the second.
 */
static void np_apply_published(const uint16_t *buttons, int slots)
{
    int i;
    for (i = 0; i < SNES_NETPLAY_MAX_SLOTS; ++i)
        g_np.published[i] = 0;
    if (!buttons || slots <= 0)
        return;
    if (slots > SNES_NETPLAY_MAX_SLOTS)
        slots = SNES_NETPLAY_MAX_SLOTS;
    for (i = 0; i < slots; ++i) {
        g_np.published[i] = buttons[i] & 0x0FFFu;
        if (i >= 2)
            RtlSetPadState(i, g_np.published[i]);
    }
}
/* 1 once snes_netplay_rb has taken over admit for this session. Set only
 * after the rollback host starts cleanly, so a failed start falls back to
 * delay-sync rather than leaving the session with no admit path at all. */
static int g_np_rollback;

/* Rollback resolves each seat's row itself (wire row or hold-last invent) and
 * hands the result back here, so snes_netplay_published_inputs() reads the
 * same way in both modes and games need no change. */
static void np_rb_publish(uint32_t tick, const uint16_t *buttons, int slots)
{
    (void)tick;
    np_apply_published(buttons, slots);
}

static void np_rb_apply_sync(const uint8_t in[2])
{
    if (g_apply_sync_bytes)
        g_apply_sync_bytes(in);
}

static void np_rollback_try_start(void)
{
#if defined(SNESRECOMP_NET_ROLLBACK)
    SnesNetplayRbBindings b;

    g_np_rollback = 0;
    if (!snes_netplay_rb_enabled())
        return;
    memset(&b, 0, sizeof(b));
    b.session = &g_np.session;
    b.local_slot = &g_np.local_slot;
    b.slot_count = &g_np.slot_count;
    b.input_delay = &g_np.input_delay;
    b.input_prediction = &g_np.input_prediction;
    b.force_turn = 0;
    b.publish = &np_rb_publish;
    b.apply_sync_bytes = &np_rb_apply_sync;
    snes_netplay_rb_bind(&b);
    if (snes_netplay_rb_start()) {
        g_np_rollback = 1;
    } else {
        fprintf(stderr,
                "snes_netplay: rollback host failed to start — "
                "falling back to delay-sync for this session\n");
        snes_netplay_rb_bind(NULL);
    }
#else
    /* Not built with rollback: delay-sync is the only admit path. */
    g_np_rollback = 0;
#endif
}
static int g_return_to_lobby;
static uint32_t g_connect_wait_started_ms;
static FILE *g_diag_file;
static int g_diag_file_session;
static uint32_t g_diag_last_write_ms;
static int g_diag_mkdir_done;
static int g_diag_summary_written;

void snes_netplay_request_return_to_lobby(void) { g_return_to_lobby = 1; }
int  snes_netplay_return_to_lobby_requested(void) { return g_return_to_lobby; }
void snes_netplay_clear_return_to_lobby(void) { g_return_to_lobby = 0; }

void snes_netplay_connect_wait_reset(void)
{
    g_connect_wait_started_ms = 0;
}

int snes_netplay_connect_timed_out(uint32_t timeout_ms)
{
    uint32_t now;
    if (!timeout_ms || !snes_netplay_active())
        return 0;
    if (snes_netplay_is_running()) {
        g_connect_wait_started_ms = 0;
        return 0;
    }
    now = SDL_GetTicks();
    if (!g_connect_wait_started_ms)
        g_connect_wait_started_ms = now ? now : 1u;
    return (uint32_t)(now - g_connect_wait_started_ms) >= timeout_ms;
}

static void encode_pad(uint16_t buttons, RNetInputSample *out, rnet_u32 tick)
{
    memset(out, 0, sizeof(*out));
    out->tick = tick;
    out->size = SNES_NETPLAY_PAD_BYTES;
    out->bytes[0] = (rnet_u8)(buttons & 0xFFu);
    out->bytes[1] = (rnet_u8)((buttons >> 8) & 0xFFu);
    if (g_capture_sync_bytes)
        g_capture_sync_bytes(&out->bytes[2]);
    out->valid = 1;
}

static void np_prime_after_hard_resync(void)
{
    RNetInputSample neutral;
    encode_pad(0, &neutral, 0);
    rnet_session_prime_delay_inputs(g_np.session, neutral.bytes, neutral.size);
}

static uint16_t decode_pad(const RNetInputSample *in)
{
    if (!in || !in->valid || in->size < 2)
        return 0;
    return (uint16_t)in->bytes[0] | ((uint16_t)in->bytes[1] << 8);
}

static void host_sample_local(rnet_u32 tick, RNetInputSample *out, void *ctx)
{
    NetplayState *st = (NetplayState *)ctx;
    uint16_t buttons = st->staged_valid ? st->staged_buttons : 0;
    encode_pad(buttons, out, tick);
}

static void host_publish(rnet_u32 tick, const RNetInputSample *by_slot, int slots, void *ctx)
{
    NetplayState *st = (NetplayState *)ctx;
    uint16_t buttons[SNES_NETPLAY_MAX_SLOTS];
    int i;
    int n = slots;
    (void)tick;
    (void)st;
    st->host_sync_valid = 0;
    if (!by_slot || slots <= 0) {
        np_apply_published(NULL, 0);
        return;
    }
    if (n > SNES_NETPLAY_MAX_SLOTS) n = SNES_NETPLAY_MAX_SLOTS;
    for (i = 0; i < n; ++i)
        buttons[i] = decode_pad(&by_slot[i]) & 0x0FFFu;
    np_apply_published(buttons, n);
    /* Slot 0 carries the authoritative game-defined sync bytes. */
    if (by_slot[0].valid && by_slot[0].size >= 4) {
        st->host_sync[0] = by_slot[0].bytes[2];
        st->host_sync[1] = by_slot[0].bytes[3];
        st->host_sync_valid = 1;
    }
}

void snes_netplay_apply_host_sync(void)
{
    if (!snes_netplay_active() || !g_np.host_sync_valid || !g_apply_sync_bytes)
        return;
    g_apply_sync_bytes(g_np.host_sync);
}

#if defined(SNES_HAS_LOBBY_CLIENT) && defined(RNET_ENABLE_ICE)
static void host_on_signal(const RNetSignal *msg, void *ctx)
{
    (void)ctx;
    if (!msg) return;
    (void)snes_lobby_send_signal((int)msg->type, (int)msg->flag, msg->text);
}

static void drain_lobby_signals(void)
{
    int type = 0, flag = 0;
    char text[2048];
    if (!g_np.session) return;
    while (snes_lobby_poll_signal(&type, &flag, text, sizeof(text))) {
        RNetSignal sig;
        memset(&sig, 0, sizeof(sig));
        /* Peers emit LOCAL_*; push_signal expects REMOTE_* for SDP/candidates. */
        if (type == (int)RNET_SIGNAL_LOCAL_SDP)
            type = (int)RNET_SIGNAL_REMOTE_SDP;
        else if (type == (int)RNET_SIGNAL_LOCAL_CANDIDATE)
            type = (int)RNET_SIGNAL_REMOTE_CANDIDATE;
        sig.type = (RNetSignalType)type;
        sig.flag = (rnet_u8)(flag & 0xFF);
        strncpy(sig.text, text, sizeof(sig.text) - 1);
        rnet_session_push_signal(g_np.session, &sig);
    }
}
#else
static void drain_lobby_signals(void) {}
#endif

static int resolve_use_ice(const SnesNetplayConfig *cfg)
{
    int in_motk_room = 0;

    if (cfg->transport == 2) return 0; /* force LAN */
#if defined(SNES_HAS_LOBBY_CLIENT)
    in_motk_room = snes_lobby_connected() && snes_lobby_in_lobby();
#endif

#if defined(RNET_ENABLE_ICE) && defined(SNES_HAS_LOBBY_CLIENT)
    if (cfg->transport == 1) {
        if (!in_motk_room) {
            fprintf(stderr,
                    "snes_netplay: ICE requested but MotK lobby not connected\n");
            return -1;
        }
        return 1;
    }
    /* Auto: hosted MotK room always uses ICE. Do not demote to LAN when the
     * lobby rewrites 0.0.0.0 binds to a private TCP peer IP (often wrong —
     * e.g. router .1). LAN file-registry (no MotK seat) stays on LAN UDP. */
    if (in_motk_room)
        return 1;
    return 0;
#else
    {
        int online_requested = cfg->transport == 1 ||
                               (cfg->transport == 0 && in_motk_room);
        if (online_requested) {
            fprintf(stderr,
                    "snes_netplay: hosted lobby requires ICE, but ICE is not "
                    "available in this build (configure with "
                    "SNESRECOMP_NET_ICE=ON)\n");
            return -1;
        }
    }
    return 0;
#endif
}

int snes_netplay_rollback_active(void)
{
    return g_np_rollback && g_np.active;
}

int snes_netplay_active(void)
{
    return g_np.active && g_np.session != NULL;
}

int snes_netplay_is_running(void)
{
    return snes_netplay_active() && rnet_session_is_running(g_np.session);
}

const char *snes_netplay_transport_name(void)
{
    if (!snes_netplay_active()) return "none";
    return g_np.use_ice ? "ice" : "lan";
}

int snes_netplay_ice_failed(void)
{
#if defined(RNET_ENABLE_ICE)
    if (!snes_netplay_active() || !g_np.use_ice)
        return 0;
    return rnet_session_ice_state(g_np.session) == RNET_ICE_STATE_FAILED;
#else
    return 0;
#endif
}

int snes_netplay_local_slot(void)
{
    return snes_netplay_active() ? g_np.local_slot : -1;
}

int snes_netplay_input_player(void)
{
    return snes_netplay_active() ? g_np.input_player : 0;
}

uint32_t snes_netplay_sim_tick(void)
{
    if (!snes_netplay_active()) return 0;
    if (g_np_rollback) return snes_netplay_rb_sim_tick();
    return rnet_session_sim_tick(g_np.session);
}

uint32_t snes_netplay_frames_finished(void)
{
    return snes_netplay_active() ? g_np.frames_finished : 0;
}

void snes_netplay_stage_local(uint16_t buttons)
{
    buttons &= 0x0FFFu;
    if (g_np_rollback) {
        snes_netplay_rb_stage_local(buttons);
        g_np.staged_buttons = buttons;
        g_np.staged_valid = 1;
        return;
    }
    if (snes_netplay_active() && rnet_session_is_running(g_np.session)) {
        uint32_t t = rnet_session_sim_tick(g_np.session);
        if (g_np.latched_for_tick && g_np.latched_sim_tick == t)
            return;
        g_np.staged_buttons = buttons;
        g_np.staged_valid = 1;
        g_np.latched_for_tick = 1;
        g_np.latched_sim_tick = t;
        return;
    }
    g_np.staged_buttons = buttons;
    g_np.staged_valid = 1;
}

int snes_netplay_needs_local_sample(void)
{
    if (!snes_netplay_active()) return 0;
    /* Rollback re-reads the live pad on every admit attempt: a stalled tick
     * that later admits must carry the player's current input, not the pad
     * they were holding when the stall began. */
    if (g_np_rollback) return 1;
    if (!rnet_session_is_running(g_np.session)) return 1;
    {
        uint32_t t = rnet_session_sim_tick(g_np.session);
        return !(g_np.latched_for_tick && g_np.latched_sim_tick == t);
    }
}

int snes_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash)
{
    if (!snes_netplay_active()) return 0;
    return rnet_session_input_desync(g_np.session, tick, local_hash, remote_hash);
}

int snes_netplay_peer_disconnected(uint32_t timeout_ms)
{
    if (!snes_netplay_active()) return 0;
    if (timeout_ms == 0) timeout_ms = 1500u;
    return rnet_session_peer_disconnected(g_np.session, (rnet_u64)timeout_ms);
}

uint32_t snes_netplay_published_inputs(void)
{
    return (uint32_t)g_np.published[0] | ((uint32_t)g_np.published[1] << 12);
}

int snes_netplay_slot_count(void)
{
    return snes_netplay_active() && g_np.slot_count > 0 ? g_np.slot_count : 2;
}

uint32_t snes_netplay_active_mask(void)
{
    return 3u << 30;
}

int snes_netplay_start(const SnesNetplayConfig *cfg)
{
    RNetConfig rcfg;
    RNetHostVTable host;
    int use_ice;
    int in_player;

    if (!cfg || !cfg->enabled) return -1;
    if (g_np.session) snes_netplay_shutdown();
    snes_netplay_connect_wait_reset();
    /* Settled session mode (config default rollback; lobby launch and
     * SNES_NET_MODE already folded in by the callers/apply_env). */
    snes_netplay_rb_set_default(cfg->rollback);

    rnet_config_init_defaults(&rcfg);
    {
        int seats = cfg->slot_count > 0 ? cfg->slot_count : 2;
        int slot;
        if (seats < 2) seats = 2;
        if (seats > SNES_NETPLAY_MAX_SLOTS) seats = SNES_NETPLAY_MAX_SLOTS;
        /* Seats past the second only exist behind a multitap. Refusing to
         * open a wider session than the machine can route is the "abort
         * rather than silently degrade" rule: a peer that quietly ran two
         * seats while the other ran five would desync on input, not on
         * anything that names itself. */
        if (seats > 2 && RtlPlayerCount() < seats) {
            fprintf(stderr,
                    "snes_netplay: %d seats requested but the port "
                    "configuration reaches only %d — enable a multitap "
                    "(SNES_MULTITAP / RtlSetMultitap) on every peer\n",
                    seats, RtlPlayerCount());
            return -1;
        }
        rcfg.slot_count = (rnet_u8)seats;
        slot = cfg->local_slot < 0 ? 0 : cfg->local_slot;
        if (slot >= seats) slot = seats - 1;
        rcfg.local_slot = (rnet_u8)slot;
    }
    rcfg.input_delay = (rnet_u8)(cfg->input_delay < 0 ? 0
                                : (cfg->input_delay > 20 ? 20 : cfg->input_delay));
    rcfg.session_id = cfg->session_id ? cfg->session_id : 1u;

    /* Host resolves auto (-1) before start; accept only 0/1 here. */
    in_player = (cfg->input_player == 1) ? 1 : 0;

    use_ice = resolve_use_ice(cfg);
    if (use_ice < 0)
        return -4;

    memset(&host, 0, sizeof(host));
    host.sample_local = host_sample_local;
    host.publish = host_publish;
    host.ctx = &g_np;
#if defined(SNES_HAS_LOBBY_CLIENT) && defined(RNET_ENABLE_ICE)
    if (use_ice)
        host.on_signal = host_on_signal;
#endif

    g_np.session = rnet_session_create(&rcfg, &host);
    if (!g_np.session) return -2;

    if (use_ice) {
#if defined(RNET_ENABLE_ICE)
        RNetIceConfig ice;
        RNetIpv4Address addrs[8];
        int naddr;
        const char *env_turn_host = getenv("SNES_NET_TURN_HOST");
        const char *env_turn_user = getenv("SNES_NET_TURN_USER");
        const char *env_turn_pass = getenv("SNES_NET_TURN_PASS");
        const char *env_stun = getenv("SNES_NET_STUN_HOST");

        g_np.ice_has_turn = 0;
        g_np.ice_stun_host[0] = '\0';
        g_np.ice_turn_host[0] = '\0';
        g_np.ice_turn_user[0] = '\0';
        g_np.ice_turn_pass[0] = '\0';
        g_np.ice_bind_addr[0] = '\0';

        rnet_ice_config_init_defaults(&ice);
        ice.controlling = (rcfg.local_slot == 0) ? 1u : 0u;

        /* Prefer a concrete LAN IPv4 for host candidates (not 0.0.0.0). */
        naddr = rnet_ipv4_enumerate(addrs, sizeof(addrs) / sizeof(addrs[0]));
        if (naddr > 0 && addrs[0].address[0]) {
            snprintf(g_np.ice_bind_addr, sizeof(g_np.ice_bind_addr), "%s",
                     addrs[0].address);
            ice.bind_address = g_np.ice_bind_addr;
        }

#if defined(SNES_HAS_LOBBY_CLIENT)
        /* Refresh lobby Coturn mint; pump briefly so welcome prefetch can land. */
        if (snes_lobby_connected()) {
            int i;
            (void)snes_lobby_request_turn_credentials();
            for (i = 0; i < 50; ++i) {
                const SnesLobbyTurnCredentials *tc = snes_lobby_turn_credentials();
                if (tc && tc->valid)
                    break;
                snes_lobby_pump();
                SDL_Delay(10);
            }
        }
        {
            const SnesLobbyTurnCredentials *tc = snes_lobby_turn_credentials();
            if (tc && tc->valid) {
                if (tc->stun_host[0]) {
                    snprintf(g_np.ice_stun_host, sizeof(g_np.ice_stun_host),
                             "%s", tc->stun_host);
                    ice.stun_host = g_np.ice_stun_host;
                    ice.stun_port = (rnet_u16)(tc->stun_port > 0 ? tc->stun_port
                                                                  : 3478);
                }
                snprintf(g_np.ice_turn_host, sizeof(g_np.ice_turn_host), "%s",
                         tc->turn_host);
                snprintf(g_np.ice_turn_user, sizeof(g_np.ice_turn_user), "%s",
                         tc->username);
                snprintf(g_np.ice_turn_pass, sizeof(g_np.ice_turn_pass), "%s",
                         tc->password);
                ice.turn_host = g_np.ice_turn_host;
                ice.turn_user = g_np.ice_turn_user;
                ice.turn_pass = g_np.ice_turn_pass;
                ice.turn_port = (rnet_u16)(tc->turn_port > 0 ? tc->turn_port
                                                              : 3478);
                g_np.ice_has_turn = 1;
            }
        }
#endif
        /* Env overrides win (dev / private Coturn without lobby mint). */
        if (env_stun && env_stun[0]) {
            snprintf(g_np.ice_stun_host, sizeof(g_np.ice_stun_host), "%s",
                     env_stun);
            ice.stun_host = g_np.ice_stun_host;
            ice.stun_port = (rnet_u16)env_u("SNES_NET_STUN_PORT", ice.stun_port
                                                                     ? ice.stun_port
                                                                     : 3478);
        }
        if (env_turn_host && env_turn_host[0] && env_turn_user &&
            env_turn_user[0] && env_turn_pass && env_turn_pass[0]) {
            snprintf(g_np.ice_turn_host, sizeof(g_np.ice_turn_host), "%s",
                     env_turn_host);
            snprintf(g_np.ice_turn_user, sizeof(g_np.ice_turn_user), "%s",
                     env_turn_user);
            snprintf(g_np.ice_turn_pass, sizeof(g_np.ice_turn_pass), "%s",
                     env_turn_pass);
            ice.turn_host = g_np.ice_turn_host;
            ice.turn_user = g_np.ice_turn_user;
            ice.turn_pass = g_np.ice_turn_pass;
            ice.turn_port = (rnet_u16)env_u("SNES_NET_TURN_PORT", 3478);
            g_np.ice_has_turn = 1;
        }

        if (!g_np.ice_stun_host[0] && ice.stun_host && ice.stun_host[0]) {
            snprintf(g_np.ice_stun_host, sizeof(g_np.ice_stun_host), "%s",
                     ice.stun_host);
        }
        g_np.ice_stun_port = ice.stun_port ? (unsigned)ice.stun_port : 19302u;
        g_np.ice_turn_port = ice.turn_port ? (unsigned)ice.turn_port : 0u;

        if (g_np.ice_has_turn) {
            fprintf(stderr,
                    "snes_netplay: ICE stun=%s:%u turn=%s:%u user=%s bind=%s\n",
                    ice.stun_host ? ice.stun_host : "(default)",
                    (unsigned)ice.stun_port,
                    ice.turn_host, (unsigned)ice.turn_port, ice.turn_user,
                    ice.bind_address ? ice.bind_address : "(any)");
        } else {
            fprintf(stderr,
                    "snes_netplay: ICE STUN-only (no TURN) stun=%s:%u "
                    "bind=%s — remote NAT may hang after a few frames; "
                    "configure Coturn on the lobby or SNES_NET_TURN_*\n",
                    ice.stun_host ? ice.stun_host : "(default)",
                    (unsigned)ice.stun_port,
                    ice.bind_address ? ice.bind_address : "(any)");
        }

        {
            int force_turn = cfg->force_turn ? 1 : 0;
#if defined(SNESRECOMP_NET_FORCE_TURN)
            force_turn = 1;
#endif
            {
                const char *ft = getenv("SNES_NET_FORCE_TURN");
                if (ft && ft[0] && ft[0] != '0')
                    force_turn = 1;
            }
            if (force_turn && !g_np.ice_has_turn) {
                fprintf(stderr,
                        "snes_netplay: FORCE_TURN requires Coturn credentials "
                        "(lobby get_turn_credentials or SNES_NET_TURN_*)\n");
                rnet_session_destroy(g_np.session);
                g_np.session = NULL;
                return -4;
            }
            if (force_turn) {
                ice.force_relay = 1;
                fprintf(stderr,
                        "snes_netplay: FORCE_TURN — ICE will use relay-only "
                        "candidates (host match_caps / all peers)\n");
            }
        }

        if (rnet_session_start_ice(g_np.session, &ice) != 0) {
            fprintf(stderr,
                    "snes_netplay: start_ice failed; refusing unsafe LAN "
                    "fallback for an online lobby\n");
            rnet_session_destroy(g_np.session);
            g_np.session = NULL;
            return -4;
        }
#else
        fprintf(stderr, "snes_netplay: ICE requested but not built\n");
        rnet_session_destroy(g_np.session);
        g_np.session = NULL;
        return -4;
#endif
    }

    if (!use_ice) {
        if (rnet_session_start_lan(g_np.session, cfg->bind_hostport, cfg->peer_hostport) != 0) {
            rnet_session_destroy(g_np.session);
            g_np.session = NULL;
            return -3;
        }
    }

    g_np.active = 1;
    g_np.slot_count = (int)rcfg.slot_count;
    g_np.local_slot = (int)rcfg.local_slot;
    g_np.input_player = in_player;
    g_np.staged_valid = 0;
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.latched_sim_tick = 0;
    g_np.host_sync_valid = 0;
    g_np.host_sync[0] = g_np.host_sync[1] = 0;
    memset(g_np.published, 0, sizeof(g_np.published));
    g_np.use_ice = use_ice;
    g_np.sram_sync_sent = 0;
    g_np.sram_sync_done = 0;
    g_np.host_sram_applied = 0;
    g_np.xfer = NP_XFER_NONE;
    g_np.xfer_slot = 0;
    g_np.load_applied_local = 0;
    g_np.load_cooldown_until_ms = 0;
    g_np.input_delay = (int)rcfg.input_delay;
    /* Session-settled invent runway. Clamp to the engine's accepted band so
     * a malformed lobby value cannot disable prediction outright. */
    g_np.input_prediction = cfg->input_prediction;
    if (g_np.input_prediction && g_np.input_prediction < 2)
        g_np.input_prediction = 2;
    if (g_np.input_prediction > 32)
        g_np.input_prediction = 32;
    g_np.force_input_relay = cfg->force_input_relay ? 1 : 0;
    g_np.session_id = rcfg.session_id;
    g_np.is_host = (g_np.local_slot == 0) ? 1 : 0;
    snprintf(g_np.bind_hostport, sizeof(g_np.bind_hostport), "%s",
             cfg->bind_hostport);
    snprintf(g_np.peer_hostport, sizeof(g_np.peer_hostport), "%s",
             cfg->peer_hostport);
    g_np.lobby_server[0] = '\0';
    g_np.lobby_id[0] = '\0';
#if defined(SNES_HAS_LOBBY_CLIENT)
    if (use_ice && snes_lobby_connected() && snes_lobby_in_lobby()) {
        const char *url = snes_lobby_url();
        const SnesLobbyJoinInfo *ji = snes_lobby_join_info();
        snprintf(g_np.match_mode, sizeof(g_np.match_mode), "hosted_lobby");
        snprintf(g_np.lobby_server, sizeof(g_np.lobby_server), "%s",
                 (url && url[0]) ? url : snes_lobby_default_url());
        if (ji && ji->lobby_id[0])
            snprintf(g_np.lobby_id, sizeof(g_np.lobby_id), "%s", ji->lobby_id);
        g_np.is_host = snes_lobby_is_host() ? 1 : 0;
    } else
#endif
    {
        snprintf(g_np.match_mode, sizeof(g_np.match_mode), "direct_ip");
    }
    g_np.frames_finished = 0;
    g_np.diag_session++;
    g_diag_summary_written = 0;
    if (g_diag_file) {
        fclose(g_diag_file);
        g_diag_file = NULL;
    }
    g_diag_file_session = 0;
    g_diag_last_write_ms = 0;

    /* Guest: sandbox SRAM/savestate paths so host sync never touches personal saves. */
    if (g_np.local_slot != 0) {
        RtlSetSaveRoot("saves/netplay");
        RtlEnsureSaveDir();
        g_np.guest_sandbox = 1;
        fprintf(stderr, "snes_netplay: guest save root -> %s\n", RtlSaveRoot());
    } else {
        RtlSetSaveRoot(NULL);
        g_np.guest_sandbox = 0;
    }

    /* Frame-locked SPC drain starts from a clean accumulator on both peers. */
    RtlNetplayAudioReset();

    np_rollback_try_start();

    fprintf(stderr,
            "snes_netplay: started transport=%s slot=%d input_player=%d session=%u "
            "delay=%u force_input_relay=%d bind=%s peer=%s\n",
            use_ice ? "ice" : "lan", g_np.local_slot, g_np.input_player,
            (unsigned)rcfg.session_id, (unsigned)rcfg.input_delay,
            g_np.force_input_relay, cfg->bind_hostport,
            /* Lobby peer rewrite is unused for ICE (candidates via WS). */
            use_ice ? "(ice)" : cfg->peer_hostport);
    return 0;
}

void snes_netplay_shutdown(void)
{
    snes_netplay_rb_shutdown();
#if defined(SNESRECOMP_NET_ROLLBACK)
    snes_netplay_rb_bind(NULL);
#endif
    g_np_rollback = 0;
    if (g_diag_file) {
        fclose(g_diag_file);
        g_diag_file = NULL;
    }
    g_diag_file_session = 0;
    g_diag_summary_written = 0;
    g_diag_last_write_ms = 0;
    if (g_np.session) {
        (void)rnet_session_send_bye(g_np.session);
        rnet_session_destroy(g_np.session);
        g_np.session = NULL;
    }
    if (g_np.guest_sandbox) {
        /* Flush host-synced mirror into the sandbox only. Then switch back to
         * personal saves/ and restore offline SRAM into RAM. RtlReadSram is a
         * no-op when saves/save.srm is missing — without clearing first, host
         * bytes would remain in g_sram and the game's post-shutdown
         * RtlWriteSram() would leak them into personal storage. */
        RtlWriteSram();
        RtlSetSaveRoot(NULL);
        if (g_sram && g_sram_size > 0)
            memset(g_sram, 0, (size_t)g_sram_size);
        RtlReadSram();
        fprintf(stderr, "snes_netplay: guest restored personal save root -> %s\n",
                RtlSaveRoot());
    }
    memset(&g_np, 0, sizeof(g_np));
    snes_netplay_connect_wait_reset();
}

static int np_read_slot_file(int slot, uint8_t **out, size_t *out_size);

static int np_xfer_busy(void)
{
    if (g_np.xfer != NP_XFER_NONE)
        return 1;
    return rnet_session_state_busy(g_np.session) ||
           rnet_session_state_take_ready(g_np.session, NULL, NULL, NULL, NULL);
}

static int np_load_cooldown_active(void)
{
    uint32_t now;
    if (!g_np.load_cooldown_until_ms)
        return 0;
    now = SDL_GetTicks();
    if ((int32_t)(now - g_np.load_cooldown_until_ms) >= 0) {
        g_np.load_cooldown_until_ms = 0;
        return 0;
    }
    return 1;
}

static int np_slot_crc(int slot, rnet_u32 *size_out, rnet_u32 *crc_out)
{
    uint8_t *buf = NULL;
    size_t n = 0;
    if (np_read_slot_file(slot, &buf, &n) != 0 || !buf || n == 0)
        return 0;
    if (size_out)
        *size_out = (rnet_u32)n;
    if (crc_out)
        *crc_out = rnet_checksum(buf, n);
    free(buf);
    return 1;
}

static int np_apply_slot_file(int slot)
{
    uint8_t *buf = NULL;
    size_t n = 0;
    int ok;
    if (np_read_slot_file(slot, &buf, &n) != 0 || !buf) {
        fprintf(stderr, "snes_netplay: load slot=%d — local file missing\n", slot);
        return 0;
    }
    ok = RtlLoadSnapshotFromMemory(buf, n) ? 1 : 0;
    if (!ok)
        fprintf(stderr, "snes_netplay: load slot=%d — apply failed (%zu bytes)\n",
                slot, n);
    free(buf);
    return ok;
}

static void np_commit_load_sync(void)
{
    rnet_session_hard_resync(g_np.session);
    np_prime_after_hard_resync();
    RtlNetplayAudioReset();
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.staged_valid = 0;
    g_np.load_applied_local = 0;
    g_np.xfer = NP_XFER_NONE;
    g_np.load_cooldown_until_ms = SDL_GetTicks() + NP_LOAD_COOLDOWN_MS;
    if (!g_np.load_cooldown_until_ms)
        g_np.load_cooldown_until_ms = 1;
    fprintf(stderr, "snes_netplay: load sync committed (hard_resync)\n");
}

static void np_enter_load_ready(int slot)
{
    g_np.xfer = NP_XFER_LOAD_READY;
    g_np.xfer_slot = slot;
    g_np.load_applied_local = 1;
    if (g_np.local_slot != 0)
        return;
    if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_LOAD, (rnet_u8)slot, 0,
                                 NP_LOAD_READY_CRC) != 0) {
        fprintf(stderr, "snes_netplay: load ready probe failed — forcing resync\n");
        np_commit_load_sync();
        return;
    }
    fprintf(stderr, "snes_netplay: load slot=%d — waiting mutual ready\n", slot);
}

static int np_write_slot_file(int slot, const void *data, size_t size)
{
    char name[128];
    FILE *f;
    RtlEnsureSaveDir();
    RtlSaveSlotPath(slot, name, sizeof(name));
    f = fopen(name, "wb");
    if (!f) {
        fprintf(stderr, "snes_netplay: failed to write %s\n", name);
        return -1;
    }
    if (fwrite(data, 1, size, f) != size) {
        fclose(f);
        fprintf(stderr, "snes_netplay: short write %s\n", name);
        return -1;
    }
    fclose(f);
    fprintf(stderr, "snes_netplay: wrote synced save %s (%zu bytes)\n", name, size);
    return 0;
}

static int np_read_slot_file(int slot, uint8_t **out, size_t *out_size)
{
    char name[128];
    FILE *f;
    long sz;
    uint8_t *buf;
    RtlSaveSlotPath(slot, name, sizeof(name));
    f = fopen(name, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    if (sz <= 0 || (size_t)sz > 512u * 1024u) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = buf;
    *out_size = (size_t)sz;
    return 0;
}

static void np_apply_sram_blob(const void *data, size_t size)
{
    if (!g_sram || g_sram_size <= 0 || !data || size == 0)
        return;
    size_t n = size < (size_t)g_sram_size ? size : (size_t)g_sram_size;
    memcpy(g_sram, data, n);
    if (n < (size_t)g_sram_size)
        memset(g_sram + n, 0, (size_t)g_sram_size - n);
    RtlWriteSram(); /* host → main; guest → sandbox */
}

static void np_apply_ready_state(void)
{
    rnet_u8 op = 0, slot = 0;
    const void *data = NULL;
    size_t size = 0;
    if (!rnet_session_state_take_ready(g_np.session, &op, &slot, &data, &size))
        return;
    if (!data || size == 0) {
        rnet_session_state_finish(g_np.session, 0);
        return;
    }

    if (op == RNET_STATE_OP_SAVE) {
        /* Host already wrote immediately; guest stores into sandbox root. */
        if (g_np.local_slot != 0)
            np_write_slot_file((int)slot, data, size);
        rnet_session_state_finish(g_np.session, 0);
        return;
    }

    if (op == RNET_STATE_OP_SRAM) {
        if (g_np.local_slot != 0 || !g_np.host_sram_applied) {
            np_apply_sram_blob(data, size);
            fprintf(stderr, "snes_netplay: applied synced SRAM (%zu bytes)\n", size);
        }
        g_np.sram_sync_done = 1;
        g_np.host_sram_applied = 0;
        rnet_session_state_finish(g_np.session, 0);
        g_np.needs_advance = 0;
        g_np.latched_for_tick = 0;
        return;
    }

    /* LOAD: both peers apply from the transferred blob, then ready-rendezvous
     * before hard_resync (keeps input epochs aligned). */
    if (!g_np.load_applied_local) {
        if (!RtlLoadSnapshotFromMemory(data, size)) {
            fprintf(stderr, "snes_netplay: load snapshot failed (%zu bytes)\n", size);
            rnet_session_state_finish(g_np.session, 0);
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        fprintf(stderr, "snes_netplay: applied synced load slot=%u (%zu bytes)\n",
                (unsigned)slot, size);
        if (g_np.local_slot != 0)
            np_write_slot_file((int)slot, data, size);
    } else {
        fprintf(stderr, "snes_netplay: load slot=%u transfer done (already applied)\n",
                (unsigned)slot);
    }
    rnet_session_state_finish(g_np.session, 0);
    np_enter_load_ready((int)slot);
}

static void np_guest_handle_probe(void)
{
    rnet_u8 op = 0, slot = 0;
    rnet_u32 size = 0, crc = 0;
    int match = 0;

    if (g_np.local_slot == 0)
        return;
    if (!rnet_session_state_probe_pending(g_np.session, &op, &slot, &size, &crc))
        return;

    /* Post-load ready rendezvous. */
    if (op == RNET_STATE_OP_LOAD && size == 0 && crc == NP_LOAD_READY_CRC) {
        if (!g_np.load_applied_local)
            return;
        if (rnet_session_state_probe_reply(g_np.session, 1) != 0)
            return;
        np_commit_load_sync();
        return;
    }

    if (size == 0) {
        /* SAVE size==0 coord unused on SNES — ACK so host is not stuck. */
        (void)rnet_session_state_probe_reply(g_np.session, 1);
        return;
    }

    if (op == RNET_STATE_OP_SRAM) {
        /* Host drives SRAM via state_begin today; answer hash if probed. */
        match = 0;
        if (g_sram && g_sram_size > 0) {
            rnet_u32 local_crc = rnet_checksum(g_sram, (size_t)g_sram_size);
            match = ((rnet_u32)g_sram_size == size && local_crc == crc);
        }
        (void)rnet_session_state_probe_reply(g_np.session, match);
        if (match)
            g_np.sram_sync_done = 1;
        return;
    }

    {
        rnet_u32 local_sz = 0, local_crc = 0;
        match = np_slot_crc((int)slot, &local_sz, &local_crc) && local_sz == size &&
                local_crc == crc;
        if (op == RNET_STATE_OP_LOAD && match && !g_np.load_applied_local) {
            if (!np_apply_slot_file((int)slot)) {
                fprintf(stderr,
                        "snes_netplay: guest load slot=%u — hash matched but "
                        "apply failed; requesting transfer\n",
                        (unsigned)slot);
                match = 0;
            }
        }
        (void)rnet_session_state_probe_reply(g_np.session, match);
        if (op == RNET_STATE_OP_LOAD) {
            g_np.xfer_slot = (int)slot;
            if (match) {
                g_np.xfer = NP_XFER_LOAD_READY;
                g_np.load_applied_local = 1;
                fprintf(stderr,
                        "snes_netplay: guest load slot=%u — hashes match, applied "
                        "(skip transfer)\n",
                        (unsigned)slot);
            } else {
                g_np.xfer = NP_XFER_LOAD_SEND;
                fprintf(stderr,
                        "snes_netplay: guest load slot=%u — hash miss, waiting "
                        "transfer\n",
                        (unsigned)slot);
            }
        }
    }
}

static void np_host_drive_load_xfer(void)
{
    int match = 0;
    uint8_t *buf = NULL;
    size_t n = 0;

    if (g_np.local_slot != 0 || !g_np.session)
        return;

    switch (g_np.xfer) {
    case NP_XFER_LOAD_PROBE:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        if (match) {
            if (!np_apply_slot_file(g_np.xfer_slot)) {
                g_np.xfer = NP_XFER_NONE;
                return;
            }
            fprintf(stderr,
                    "snes_netplay: host load slot=%d — hashes match, skip transfer\n",
                    g_np.xfer_slot);
            np_enter_load_ready(g_np.xfer_slot);
            return;
        }
        if (np_read_slot_file(g_np.xfer_slot, &buf, &n) != 0 || !buf) {
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        if (rnet_session_state_begin(g_np.session, RNET_STATE_OP_LOAD,
                                     (rnet_u8)g_np.xfer_slot, buf, n) != 0) {
            free(buf);
            fprintf(stderr, "snes_netplay: state_begin(load) failed\n");
            g_np.xfer = NP_XFER_NONE;
            return;
        }
        free(buf);
        g_np.xfer = NP_XFER_LOAD_SEND;
        fprintf(stderr, "snes_netplay: host load slot=%d — transferring %zu bytes\n",
                g_np.xfer_slot, n);
        return;

    case NP_XFER_LOAD_READY:
        if (!rnet_session_state_probe_take_reply(g_np.session, &match))
            return;
        rnet_session_state_probe_finish(g_np.session);
        np_commit_load_sync();
        return;

    case NP_XFER_LOAD_SEND:
        /* Completion handled in np_apply_ready_state → np_enter_load_ready. */
        return;

    default:
        return;
    }
}

static void np_maybe_start_sram_sync(void)
{
    if (g_np.local_slot != 0 || g_np.sram_sync_sent || g_np.sram_sync_done)
        return;
    if (!rnet_session_is_running(g_np.session))
        return;
    if (np_xfer_busy())
        return;
    if (!g_sram || g_sram_size <= 0) {
        g_np.sram_sync_done = 1;
        return;
    }
    g_np.host_sram_applied = 1; /* host already has live SRAM */
    if (rnet_session_state_begin(g_np.session, RNET_STATE_OP_SRAM, 0, g_sram,
                                 (size_t)g_sram_size) != 0) {
        fprintf(stderr, "snes_netplay: state_begin(SRAM) failed\n");
        g_np.sram_sync_done = 1;
        g_np.host_sram_applied = 0;
        return;
    }
    g_np.sram_sync_sent = 1;
    fprintf(stderr, "snes_netplay: syncing host SRAM (%d bytes)\n", g_sram_size);
}

int snes_netplay_is_host(void)
{
    return snes_netplay_active() && g_np.local_slot == 0;
}

int snes_netplay_request_save(int slot)
{
    uint8_t *buf;
    size_t cap = 512u * 1024u;
    size_t n;
    if (!snes_netplay_active() || !rnet_session_is_running(g_np.session))
        return 0;
    if (g_np.local_slot != 0) {
        fprintf(stderr, "snes_netplay: ignore save (guest; host-only)\n");
        return 1;
    }
    if (np_xfer_busy()) {
        fprintf(stderr, "snes_netplay: save busy\n");
        return 1;
    }
    if (slot < 0) slot = 0;
    if (slot > 19) slot = 19;

    /* Host-immediate: persist locally now, then async-ship to guest (no sim stall). */
    RtlSaveLoad(kSaveLoad_Save, slot);
    RtlWriteSram(); /* keep battery file continuous with host progress */

    buf = (uint8_t *)malloc(cap);
    if (!buf) return 1;
    n = RtlSaveSnapshotToMemory(buf, cap);
    if (n == 0) {
        free(buf);
        fprintf(stderr, "snes_netplay: snapshot serialize failed\n");
        return 1;
    }
    if (rnet_session_state_begin(g_np.session, RNET_STATE_OP_SAVE, (rnet_u8)slot, buf, n) != 0) {
        fprintf(stderr, "snes_netplay: state_begin(save) failed\n");
    } else {
        fprintf(stderr, "snes_netplay: host saved; async sync slot=%d (%zu bytes)\n", slot, n);
    }
    free(buf);
    return 1;
}

int snes_netplay_request_load(int slot)
{
    rnet_u32 size = 0, crc = 0;
    if (!snes_netplay_active() || !rnet_session_is_running(g_np.session))
        return 0;
    if (g_np.local_slot != 0) {
        fprintf(stderr, "snes_netplay: ignore load (guest; host-only)\n");
        return 1;
    }
    if (np_xfer_busy()) {
        fprintf(stderr, "snes_netplay: load busy\n");
        return 1;
    }
    if (np_load_cooldown_active()) {
        fprintf(stderr, "snes_netplay: load cooldown\n");
        return 1;
    }
    if (slot < 0) slot = 0;
    if (slot > 19) slot = 19;

    if (!np_slot_crc(slot, &size, &crc)) {
        fprintf(stderr, "snes_netplay: no save in slot %d\n", slot);
        return 1;
    }

    /* Hash probe first — skip the ~300KB ICE transfer when guest already has
     * the blob. Apply + hard_resync happen only after probe/xfer + ready. */
    if (rnet_session_state_probe(g_np.session, RNET_STATE_OP_LOAD, (rnet_u8)slot, size,
                                 crc) != 0) {
        fprintf(stderr, "snes_netplay: load probe failed\n");
        return 1;
    }
    g_np.xfer = NP_XFER_LOAD_PROBE;
    g_np.xfer_slot = slot;
    g_np.load_applied_local = 0;
    fprintf(stderr, "snes_netplay: host load slot=%d — hash probe (%u bytes)\n", slot,
            (unsigned)size);
    return 1;
}

int snes_netplay_state_barrier(void)
{
    RNetSessionStats st;
    if (!snes_netplay_active() || !g_np.session)
        return 0;
    if (g_np.xfer != NP_XFER_NONE)
        return 1;
    if (rnet_session_state_busy(g_np.session))
        return 1;
    memset(&st, 0, sizeof(st));
    rnet_session_get_stats(g_np.session, &st);
    if (st.state_busy || st.last_stall == RNET_ADMIT_STATE_XFER)
        return 1;
    return 0;
}

static int np_diag_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("SNES_NET_DIAG");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached;
}

static unsigned np_diag_interval_ms(void)
{
    static unsigned cached = 0;
    unsigned hz;
    if (cached)
        return cached;
    hz = env_u("SNES_NET_DIAG_HZ", 2);
    if (hz < 1) hz = 1;
    if (hz > 30) hz = 30;
    cached = 1000u / hz;
    if (cached < 1) cached = 1;
    return cached;
}

static void np_diag_escape(char *out, size_t out_len, const char *in)
{
    size_t oi = 0;
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!in)
        return;
    for (; *in && oi + 2 < out_len; ++in) {
        char c = *in;
        if (c == '"' || c == '\\') {
            if (oi + 3 >= out_len)
                break;
            out[oi++] = '\\';
            out[oi++] = c;
        } else if ((unsigned char)c < 0x20) {
            /* skip control chars */
        } else {
            out[oi++] = c;
        }
    }
    out[oi] = '\0';
}

static const char *np_diag_ice_path(const RNetSessionStats *st)
{
    if (!g_np.use_ice)
        return "lan";
    if (!st)
        return "pending";
    if (st->ice_state == RNET_ICE_STATE_FAILED)
        return "failed";
    if (st->ice_path[0])
        return st->ice_path;
    if (st->ice_state == RNET_ICE_STATE_COMPLETED ||
        st->ice_state == RNET_ICE_STATE_CONNECTED)
        return "unknown";
    return "pending";
}

/* Map ICE candidate type → NAT family for soak triage. */
static const char *np_diag_ice_nat(const char *path)
{
    if (!g_np.use_ice)
        return "lan";
    if (!path || !path[0] || strcmp(path, "pending") == 0)
        return "pending";
    if (strcmp(path, "failed") == 0)
        return "failed";
    if (strcmp(path, "relay") == 0)
        return "turn";
    if (strcmp(path, "srflx") == 0 || strcmp(path, "prflx") == 0)
        return "stun";
    if (strcmp(path, "host") == 0)
        return "host";
    return "unknown";
}

static int np_diag_path_ready(const RNetSessionStats *st)
{
    if (!g_np.use_ice)
        return 1;
    if (!st)
        return 0;
    if (st->ice_state == RNET_ICE_STATE_FAILED)
        return 1;
    /* Prefer a nominated path; fall back to connected/completed. */
    if (st->ice_path[0] && strcmp(st->ice_path, "pending") != 0 &&
        strcmp(st->ice_path, "unknown") != 0)
        return 1;
    if (st->ice_state == RNET_ICE_STATE_COMPLETED ||
        st->ice_state == RNET_ICE_STATE_CONNECTED)
        return 1;
    return 0;
}

static void np_diag_write_summary(FILE *f, const RNetSessionStats *st, uint32_t now)
{
    char server_esc[280];
    char lobby_esc[80];
    char bind_esc[80];
    char peer_esc[80];
    char stun_esc[140];
    char turn_esc[140];
    char ice_local_esc[120];
    char ice_remote_esc[120];
    const char *path = np_diag_ice_path(st);
    const char *nat = np_diag_ice_nat(path);
    const char *ice_state =
        st ? rnet_ice_state_name(st->ice_state) : "idle";

    np_diag_escape(server_esc, sizeof(server_esc), g_np.lobby_server);
    np_diag_escape(lobby_esc, sizeof(lobby_esc), g_np.lobby_id);
    np_diag_escape(bind_esc, sizeof(bind_esc), g_np.bind_hostport);
    np_diag_escape(peer_esc, sizeof(peer_esc), g_np.peer_hostport);
    np_diag_escape(stun_esc, sizeof(stun_esc), g_np.ice_stun_host);
    np_diag_escape(turn_esc, sizeof(turn_esc), g_np.ice_turn_host);
    np_diag_escape(ice_local_esc, sizeof(ice_local_esc),
                   st ? st->ice_local : "");
    np_diag_escape(ice_remote_esc, sizeof(ice_remote_esc),
                   st ? st->ice_remote : "");

    fprintf(f,
            "{\"type\":\"summary\",\"t_ms\":%u,\"match\":\"%s\","
            "\"lobby_server\":\"%s\",\"lobby_id\":\"%s\",\"is_host\":%d,"
            "\"slot\":%d,\"session_id\":%u,\"input_delay\":%d,"
            "\"force_input_relay\":%d,"
            "\"transport\":\"%s\",\"bind\":\"%s\",\"peer\":\"%s\","
            "\"turn_configured\":%d,\"stun_host\":\"%s\",\"stun_port\":%u,"
            "\"turn_host\":\"%s\",\"turn_port\":%u,\"ice_state\":\"%s\","
            "\"ice_path\":\"%s\",\"ice_nat\":\"%s\","
            "\"ice_local\":\"%s\",\"ice_remote\":\"%s\"}\n",
            (unsigned)now, g_np.match_mode[0] ? g_np.match_mode : "unknown",
            server_esc, lobby_esc, g_np.is_host, g_np.local_slot,
            (unsigned)g_np.session_id, g_np.input_delay, g_np.force_input_relay,
            g_np.use_ice ? "ice" : "lan", bind_esc, peer_esc,
            g_np.ice_has_turn ? 1 : 0, stun_esc, g_np.ice_stun_port, turn_esc,
            g_np.ice_turn_port, ice_state ? ice_state : "idle", path, nat,
            ice_local_esc, ice_remote_esc);
}

void snes_netplay_diag_tick(void)
{
    RNetSessionStats st;
    uint32_t now;
    const char *transport;
    const char *ice_state;
    const char *path;

    if (!np_diag_enabled() || !snes_netplay_active() || !g_np.session)
        return;

    rnet_session_get_stats(g_np.session, &st);

    /* Hold samples until ICE nominates a path so the summary can lead the file. */
    if (!g_diag_summary_written && !np_diag_path_ready(&st))
        return;

    now = SDL_GetTicks();
    if (g_diag_last_write_ms &&
        (uint32_t)(now - g_diag_last_write_ms) < np_diag_interval_ms() &&
        g_diag_summary_written)
        return;
    g_diag_last_write_ms = now ? now : 1u;

    if (!g_diag_mkdir_done) {
        g_diag_mkdir_done = 1;
#ifdef _WIN32
        _mkdir("saves");
        _mkdir("saves\\netplay");
#else
        mkdir("saves", 0755);
        mkdir("saves/netplay", 0755);
#endif
    }

    if (!g_diag_file || g_diag_file_session != g_np.diag_session) {
        char pathbuf[64];
        if (g_diag_file) {
            fclose(g_diag_file);
            g_diag_file = NULL;
        }
        /* Single shared path; truncate on each new match so the summary
         * stays at the top (same-machine dual clients will overwrite). */
        snprintf(pathbuf, sizeof(pathbuf), "saves/netplay/net_diag.jsonl");
        g_diag_file = fopen(pathbuf, "wb");
        if (!g_diag_file)
            return;
        setvbuf(g_diag_file, NULL, _IOLBF, 0);
        g_diag_file_session = g_np.diag_session;
        g_diag_summary_written = 0;
        fprintf(stderr, "snes_netplay: diag writing %s "
                        "(SNES_NET_DIAG_HZ interval %ums)\n",
                pathbuf, np_diag_interval_ms());
    }

    if (!g_diag_summary_written) {
        np_diag_write_summary(g_diag_file, &st, now);
        g_diag_summary_written = 1;
    }

    {
        char ice_local_esc[120];
        char ice_remote_esc[120];
        const char *stall = rnet_admit_stall_name(st.last_stall);
        int using_turn_path = (strcmp(np_diag_ice_path(&st), "relay") == 0) ? 1 : 0;

        transport = snes_netplay_transport_name();
        ice_state = rnet_ice_state_name(st.ice_state);
        path = np_diag_ice_path(&st);
        np_diag_escape(ice_local_esc, sizeof(ice_local_esc), st.ice_local);
        np_diag_escape(ice_remote_esc, sizeof(ice_remote_esc), st.ice_remote);

        fprintf(g_diag_file,
                "{\"t_ms\":%u,\"slot\":%d,\"transport\":\"%s\",\"ice_state\":\"%s\","
                "\"ice_path\":\"%s\",\"ice_nat\":\"%s\",\"turn\":%d,"
                "\"ice_local\":\"%s\",\"ice_remote\":\"%s\","
                "\"running\":%d,\"sim_tick\":%u,\"frames_finished\":%u,"
                "\"delay\":%u,\"stall\":\"%s\","
                "\"stall_ms\":%u,\"stall_max_ms\":%u,\"stall_streaks\":%u,"
                "\"consec_stalls\":%u,\"admit_ok\":%u,\"remote_lead\":%d,"
                "\"remote_wire\":%u,\"peer_rx_age_ms\":%llu,\"peer_gone\":%d,"
                "\"desync\":%d,\"desync_tick\":%u,\"state_busy\":%d,\"state_op\":%u,"
                "\"pkts_rx\":%u,\"input_sends\":%u}\n",
                (unsigned)now, g_np.local_slot, transport ? transport : "none",
                ice_state ? ice_state : "idle", path, np_diag_ice_nat(path),
                using_turn_path, ice_local_esc, ice_remote_esc, st.is_running,
                (unsigned)st.sim_tick, (unsigned)g_np.frames_finished,
                (unsigned)st.delay, stall ? stall : "unknown",
                (unsigned)st.last_admit_wait_ms, (unsigned)st.max_admit_wait_ms,
                (unsigned)st.stall_streaks, (unsigned)st.consecutive_stalls,
                (unsigned)st.admit_ok_count, st.remote_lead,
                (unsigned)st.highest_remote_wire,
                (unsigned long long)st.last_peer_rx_age_ms, st.peer_gone,
                st.input_desync, (unsigned)st.desync_tick, st.state_busy,
                (unsigned)st.state_op, (unsigned)st.packets_rx,
                (unsigned)st.input_bundle_sends);
    }
}

static void np_pump_session(void)
{
#if defined(SNES_HAS_LOBBY_CLIENT)
    if (g_np.use_ice || snes_lobby_connected())
        snes_lobby_pump();
#endif
    drain_lobby_signals();
    rnet_session_pump(g_np.session);
    np_guest_handle_probe();
    np_apply_ready_state();
    np_host_drive_load_xfer();
    if (rnet_session_is_running(g_np.session))
        np_maybe_start_sram_sync();
}

void snes_netplay_pump(void)
{
    if (!snes_netplay_active())
        return;
    np_pump_session();
    snes_netplay_diag_tick();
}

int snes_netplay_poll_admit(void)
{
    rnet_u32 sim;
    int admitted = 0;
    if (!snes_netplay_active()) return 1;

    np_pump_session();
    if (!rnet_session_is_running(g_np.session)) {
        snes_netplay_diag_tick();
        return 0;
    }

    /* Titles with no battery RAM skip the initial sync barrier. */
    if (!g_np.sram_sync_done && (!g_sram || g_sram_size <= 0))
        g_np.sram_sync_done = 1;
    /* Guest stalls until initial SRAM arrives; host stalls via state_stall_sim. */
    if (!g_np.sram_sync_done && g_np.local_slot != 0) {
        snes_netplay_diag_tick();
        return 0;
    }
    /* App-layer load barrier (probe / xfer / ready rendezvous). */
    if (g_np.xfer != NP_XFER_NONE) {
        snes_netplay_diag_tick();
        return 0;
    }

    if (g_np_rollback) {
        int ok = snes_netplay_rb_poll_admit();
        snes_netplay_diag_tick();
        return ok;
    }

    if (g_np.needs_advance) return 1;

    sim = rnet_session_sim_tick(g_np.session);
    if (rnet_session_try_admit(g_np.session, sim)) {
        g_np.needs_advance = 1;
        snes_netplay_apply_host_sync();
        admitted = 1;
    }
    snes_netplay_diag_tick();
    return admitted;
}

void snes_netplay_finish_frame(void)
{
    if (!snes_netplay_active()) return;
    if (g_np_rollback) {
        snes_netplay_rb_finish_frame();
        g_np.frames_finished++;
        return;
    }
    if (!g_np.needs_advance) return;
    rnet_session_advance(g_np.session);
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.frames_finished++;
}

int snes_netplay_remote_lead(void)
{
    RNetSessionStats st;
    if (!snes_netplay_active())
        return 0;
    memset(&st, 0, sizeof(st));
    rnet_session_get_stats(g_np.session, &st);
    return st.remote_lead;
}

int snes_netplay_input_delay(void)
{
    RNetSessionStats st;
    if (!snes_netplay_active())
        return 2;
    memset(&st, 0, sizeof(st));
    rnet_session_get_stats(g_np.session, &st);
    return st.delay > 0 ? (int)st.delay : 2;
}

#endif /* SNESRECOMP_NET */

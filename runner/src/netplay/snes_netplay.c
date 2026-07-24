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
#include "common_rtl.h"
#include "common_cpu_infra.h"
#if defined(SNES_HAS_LOBBY_CLIENT)
#include "snes_lobby_client.h"
#endif
#include <SDL.h>
#endif

void snes_netplay_config_defaults(SnesNetplayConfig *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->local_slot = 0;
    cfg->input_player = -1; /* auto → resolve at start */
    cfg->input_delay = 2;
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

int  snes_netplay_active(void) { return 0; }
int  snes_netplay_is_running(void) { return 0; }
const char *snes_netplay_transport_name(void) { return "none"; }
int  snes_netplay_ice_failed(void) { return 0; }
int  snes_netplay_local_slot(void) { return -1; }
int  snes_netplay_input_player(void) { return 0; }
uint32_t snes_netplay_sim_tick(void) { return 0; }
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
void snes_netplay_apply_host_sync(void) {}
void snes_netplay_finish_frame(void) {}
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

#else /* SNESRECOMP_NET */

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
    uint16_t     published[2];
    uint8_t      host_sync[2];       /* game-defined slot-0 sync bytes */
    int          host_sync_valid;
    int          use_ice;
    int          guest_sandbox;      /* save root redirected to saves/netplay */
    int          sram_sync_sent;     /* host: SRAM blob transfer started */
    int          sram_sync_done;     /* both: initial SRAM sync finished */
    int          host_load_applied;  /* host already applied LOAD locally */
    int          host_sram_applied;  /* host already has live SRAM */
    /* Owned buffers for RNetIceConfig pointers (juice may retain them). */
    char         ice_stun_host[128];
    char         ice_turn_host[128];
    char         ice_turn_user[192];
    char         ice_turn_pass[128];
    char         ice_bind_addr[RNET_IPV4_ADDRESS_TEXT_MAX];
    int          ice_has_turn;
} NetplayState;

static NetplayState g_np;
static int g_return_to_lobby;
static uint32_t g_connect_wait_started_ms;

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
    int i;
    (void)tick;
    st->published[0] = 0;
    st->published[1] = 0;
    st->host_sync_valid = 0;
    if (!by_slot || slots <= 0) return;
    for (i = 0; i < slots && i < 2; ++i)
        st->published[i] = decode_pad(&by_slot[i]) & 0x0FFFu;
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
    return rnet_session_sim_tick(g_np.session);
}

void snes_netplay_stage_local(uint16_t buttons)
{
    buttons &= 0x0FFFu;
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

    rnet_config_init_defaults(&rcfg);
    rcfg.slot_count = 2;
    rcfg.local_slot = (rnet_u8)(cfg->local_slot < 0 ? 0 : (cfg->local_slot > 1 ? 1 : cfg->local_slot));
    rcfg.input_delay = (rnet_u8)(cfg->input_delay < 0 ? 0 : (cfg->input_delay > 16 ? 16 : cfg->input_delay));
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
            int force_turn = 0;
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
                fprintf(stderr,
                        "snes_netplay: FORCE_TURN — ICE will use relay-only "
                        "candidates (both peers must match)\n");
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
    g_np.published[0] = g_np.published[1] = 0;
    g_np.use_ice = use_ice;
    g_np.sram_sync_sent = 0;
    g_np.sram_sync_done = 0;
    g_np.host_load_applied = 0;
    g_np.host_sram_applied = 0;

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

    fprintf(stderr,
            "snes_netplay: started transport=%s slot=%d input_player=%d session=%u "
            "delay=%u bind=%s peer=%s\n",
            use_ice ? "ice" : "lan", g_np.local_slot, g_np.input_player,
            (unsigned)rcfg.session_id, (unsigned)rcfg.input_delay,
            cfg->bind_hostport,
            /* Lobby peer rewrite is unused for ICE (candidates via WS). */
            use_ice ? "(ice)" : cfg->peer_hostport);
    return 0;
}

void snes_netplay_shutdown(void)
{
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

static int np_xfer_busy(void)
{
    return rnet_session_state_busy(g_np.session) ||
           rnet_session_state_take_ready(g_np.session, NULL, NULL, NULL, NULL);
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

    /* LOAD: guest applies; host already applied immediately at request time. */
    if (g_np.local_slot != 0 || !g_np.host_load_applied) {
        if (!RtlLoadSnapshotFromMemory(data, size)) {
            fprintf(stderr, "snes_netplay: load snapshot failed (%zu bytes)\n", size);
        } else {
            fprintf(stderr, "snes_netplay: applied synced load slot=%u (%zu bytes)\n",
                    (unsigned)slot, size);
            if (g_np.local_slot != 0)
                np_write_slot_file((int)slot, data, size);
        }
    } else {
        fprintf(stderr, "snes_netplay: guest caught up; host load already applied\n");
        g_np.host_load_applied = 0;
    }
    rnet_session_state_finish(g_np.session, 1);
    np_prime_after_hard_resync();
    RtlNetplayAudioReset();
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.staged_valid = 0;
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
    uint8_t *buf = NULL;
    size_t n = 0;
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
    if (slot < 0) slot = 0;
    if (slot > 19) slot = 19;

    if (np_read_slot_file(slot, &buf, &n) != 0) {
        fprintf(stderr, "snes_netplay: no save in slot %d\n", slot);
        return 1;
    }

    /* Host-immediate apply from local file; stall admit until guest catches up. */
    if (!RtlLoadSnapshotFromMemory(buf, n)) {
        fprintf(stderr, "snes_netplay: host local load failed\n");
        free(buf);
        return 1;
    }
    g_np.host_load_applied = 1;
    RtlNetplayAudioReset();
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
    g_np.staged_valid = 0;
    fprintf(stderr, "snes_netplay: host applied load slot=%d; syncing guest (%zu bytes)\n",
            slot, n);

    if (rnet_session_state_begin(g_np.session, RNET_STATE_OP_LOAD, (rnet_u8)slot, buf, n) != 0) {
        fprintf(stderr, "snes_netplay: state_begin(load) failed\n");
        g_np.host_load_applied = 0;
    }
    free(buf);
    return 1;
}

int snes_netplay_poll_admit(void)
{
    rnet_u32 sim;
    if (!snes_netplay_active()) return 1;

#if defined(SNES_HAS_LOBBY_CLIENT)
    if (g_np.use_ice || snes_lobby_connected())
        snes_lobby_pump();
#endif
    drain_lobby_signals();

    rnet_session_pump(g_np.session);
    np_apply_ready_state();
    if (!rnet_session_is_running(g_np.session))
        return 0;

    np_maybe_start_sram_sync();
    /* Titles with no battery RAM skip the initial sync barrier. */
    if (!g_np.sram_sync_done && (!g_sram || g_sram_size <= 0))
        g_np.sram_sync_done = 1;
    /* Guest stalls until initial SRAM arrives; host stalls via state_stall_sim. */
    if (!g_np.sram_sync_done && g_np.local_slot != 0)
        return 0;

    if (g_np.needs_advance) return 1;

    sim = rnet_session_sim_tick(g_np.session);
    if (rnet_session_try_admit(g_np.session, sim)) {
        g_np.needs_advance = 1;
        /* Apply game-defined slot-0 state before RtlRunFrame. */
        snes_netplay_apply_host_sync();
        return 1;
    }
    return 0;
}

void snes_netplay_finish_frame(void)
{
    if (!snes_netplay_active()) return;
    if (!g_np.needs_advance) return;
    rnet_session_advance(g_np.session);
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
}

#endif /* SNESRECOMP_NET */

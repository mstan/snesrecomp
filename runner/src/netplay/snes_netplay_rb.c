#include "snes_netplay_rb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes_state_digest.h"
#include "common_rtl.h"
#include "snes/snes.h"

extern Snes *g_snes;

/* recomp-net and retcomm-rbengine both carry 8 seats; the SNES side reaches
 * that many with a Super Multitap in each port (docs/MULTITAP.md). Seats past
 * the second are published through RtlSetPadState by the facade, so nothing
 * in the resim path needs to know how many there are. */
#define RB_MAX_SLOTS 8
/* SNES pads are 12 bits, active high. rbengine's invent helpers fill an
 * unknown row with 0xFFFF because PSX pads are active low — on SNES that
 * would read as every button held. 12-bit rows can never legitimately be
 * 0xFFFF, so the sentinel is unambiguous and rb_row_sanitize maps it to
 * neutral. Seeding a neutral row per seat at start means hold-last almost
 * never reaches the fallback in the first place. */
#define RB_PSX_NEUTRAL 0xFFFFu
#define RB_BUTTON_MASK 0x0FFFu

/* Matches RNET_RB_SEAL_ROWS_CHUNK_MAX in recomp-net's private
 * src/protocol/rnet_protocol.h (24). The public headers do not export it, and
 * the send path silently truncates anything larger — a chunk of 32 would post
 * 24 rows, skip 8, and leave peer_seal_rows_complete permanently false. */
#define RB_SEAL_CHUNK 24u

typedef enum RbEpisodeStage {
    kRbIdle = 0,
    kRbSealing,      /* rows sealed locally; waiting on peer seal rows */
    kRbReplaying,    /* baseline loaded; resim in progress (runs inline) */
    kRbVerifying,    /* POST sent; waiting for peer POST */
    kRbTipHold       /* POST matched; Live runs while seals stay open */
} RbEpisodeStage;

static struct {
    SnesNetplayRbBindings b;
    RNetRbSession  *rb;
    RbeSnapRing    *snaps;
    RbeInputHist    ih;
    RbeHashConfirm  hc;

    int      started;
    uint32_t sim;            /* authoritative local sim tick */
    uint16_t staged;
    int      staged_valid;
    uint16_t resolved[RB_MAX_SLOTS];
    uint8_t  sync_bytes[2];
    int      sync_valid;

    /* Episode */
    RbEpisodeStage   stage;
    RNetRbCorrection corr;
    int              initiator;
    uint32_t         epoch_seq;
    uint32_t         peer_post_digest;
    uint32_t         local_post_digest;
    int              peer_post_seen;
    /* Baseline digest exchange. Both sides must digest the SAME tick, and a
     * peer's digest routinely arrives before we have loaded our own baseline
     * snapshot, so it is buffered until ours exists. */
    SnesStateDigestParts local_base;
    int              local_base_valid;
    uint32_t         peer_base_epoch;
    uint32_t         peer_base_tick;
    uint32_t         peer_base_master, peer_base_wram;
    uint32_t         peer_base_apu, peer_base_ppu;
    int              peer_base_valid;
    uint32_t         cooldown_until_tick;
    uint32_t         stage_entered_ms;
    uint32_t         seal_timeout_ms;

    uint32_t tip_prepared_for;
    int      tip_prepared_valid;

    /* Resim */
    int      in_resim;
    uint32_t resim_audio_cursor;
    int      saved_disable_render;

    /* Snapshot staging (one reusable buffer; the ring owns its own copies). */
    uint8_t *snap_scratch;
    size_t   snap_scratch_cap;

    /* Counters / diagnostics */
    uint32_t episode_count;
    uint32_t desync_count;
    uint64_t resim_ticks;
    uint32_t fork_tick;
    int      fork_seen;
    const char *fork_partition;
    const char *stall_tag;
    uint32_t snap_interval;
    int      prediction_cap;
    int      force_invent_slot;  /* validation knob one-shot; -1 = idle */
} g_rb;

/* ── env ─────────────────────────────────────────────────────────────── */

static int rb_env_int(const char *name, int def, int lo, int hi)
{
    const char *v = getenv(name);
    long n;
    char *end;
    if (!v || !v[0])
        return def;
    n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < lo || n > hi)
        return def;
    return (int)n;
}

/* The settled session mode, handed in by snes_netplay_start() from the
 * config. ROLLBACK is the framework default (rolled out at scale
 * 2026-08-29): snes_netplay_config_defaults() seeds rollback=1, recomp-ui's
 * lobby settles the mode room-wide (its model also defaults on), and
 * SNES_NET_MODE is the operator override in BOTH directions — all of which
 * is folded into cfg->rollback before start() calls here. NETPLAY.md §4 is
 * satisfied by the settlement being room-wide, not per-process. */
static int s_rb_default;

void snes_netplay_rb_set_default(int on)
{
    s_rb_default = on ? 1 : 0;
}

int snes_netplay_rb_enabled(void)
{
    return s_rb_default;
}

/* ── row helpers ─────────────────────────────────────────────────────── */

static void rb_row_sanitize(RNetRbFrame *f)
{
    if (!f)
        return;
    if (f->buttons == RB_PSX_NEUTRAL)
        f->buttons = 0u; /* PSX-shaped neutral → SNES neutral */
    f->buttons &= RB_BUTTON_MASK;
    f->stick_x = 0;
    f->stick_y = 0;
    f->analog = 0;
}

static void rb_row_make(RNetRbFrame *f, uint32_t tick, uint16_t buttons,
                        int predicted)
{
    memset(f, 0, sizeof(*f));
    f->tick = tick;
    f->buttons = buttons & RB_BUTTON_MASK;
    f->is_predicted = predicted ? 1u : 0u;
    f->is_valid = 1u;
}

static int rb_slot_count(void)
{
    int n = g_rb.b.slot_count ? *g_rb.b.slot_count : 2;
    if (n < 1) n = 1;
    if (n > RB_MAX_SLOTS) n = RB_MAX_SLOTS;
    return n;
}

static int rb_local_slot(void)
{
    int s = g_rb.b.local_slot ? *g_rb.b.local_slot : 0;
    return (s >= 0 && s < RB_MAX_SLOTS) ? s : 0;
}

static RNetSession *rb_session(void)
{
    return g_rb.b.session ? *g_rb.b.session : NULL;
}

static int rb_input_delay(void)
{
    int d = g_rb.b.input_delay ? *g_rb.b.input_delay : 2;
    return d < 0 ? 0 : d;
}

/* ── snapshots ───────────────────────────────────────────────────────── */

static int rb_snap_serialize(void *ctx, uint32_t tick, uint8_t **out_data,
                             size_t *out_len)
{
    size_t bound, n;
    uint8_t *copy;
    (void)ctx;
    (void)tick;

    bound = RtlRollbackSnapshotBound();
    if (g_rb.snap_scratch_cap < bound) {
        uint8_t *nb = (uint8_t *)realloc(g_rb.snap_scratch, bound);
        if (!nb)
            return 0;
        g_rb.snap_scratch = nb;
        g_rb.snap_scratch_cap = bound;
    }
    n = RtlRollbackSaveToMemory(g_rb.snap_scratch, g_rb.snap_scratch_cap);
    if (n == 0)
        return 0;
    copy = (uint8_t *)malloc(n);
    if (!copy)
        return 0;
    memcpy(copy, g_rb.snap_scratch, n);
    *out_data = copy;
    *out_len = n;
    return 1;
}

static int rb_snap_deserialize(void *ctx, uint32_t tick, const uint8_t *data,
                               size_t len)
{
    (void)ctx;
    (void)tick;
    return RtlRollbackLoadFromMemory(data, len) ? 1 : 0;
}

static const RbeSnapVTable g_snap_vt = {
    NULL, &rb_snap_serialize, &rb_snap_deserialize
};

/*
 * A snapshot keyed T is the state BEFORE tick T runs, which is what
 * recomp-net's replay contract wants: load_state(load) then advance_sim(t)
 * for t = load..target re-runs the load tick itself. Keying it "after T"
 * instead would replay every episode one tick short of its own mismatch.
 */
static void rb_snap_take(uint32_t tick)
{
    if (!g_rb.snaps)
        return;
    if (g_rb.snap_interval > 1u && (tick % g_rb.snap_interval) != 0u)
        return;
    (void)rbe_snap_ring_save(g_rb.snaps, tick, &g_snap_vt);
}

/* Deepest tick <= want that we still hold a snapshot for. Returns 0 and
 * leaves *out untouched when the ring cannot reach back that far — the
 * caller must then refuse the episode rather than load the wrong tick. */
static int rb_snap_floor(uint32_t want, uint32_t *out)
{
    uint32_t oldest;
    uint32_t t;

    if (!g_rb.snaps || rbe_snap_ring_count(g_rb.snaps) == 0)
        return 0;
    oldest = rbe_snap_ring_oldest_tick(g_rb.snaps);
    if (want < oldest)
        return 0;
    for (t = want; ; --t) {
        if (rbe_snap_ring_has(g_rb.snaps, t)) {
            *out = t;
            return 1;
        }
        if (t == oldest)
            break;
    }
    return 0;
}

/* ── digests ─────────────────────────────────────────────────────────── */

static uint32_t rb_digest(uint32_t partition)
{
    return snes_state_digest(partition);
}

/* ── sim advance ─────────────────────────────────────────────────────── */

static void rb_publish_resolved(uint32_t tick)
{
    if (g_rb.b.publish)
        g_rb.b.publish(tick, g_rb.resolved, rb_slot_count());
}

static void rb_apply_sync_bytes(void)
{
    if (g_rb.sync_valid && g_rb.b.apply_sync_bytes)
        g_rb.b.apply_sync_bytes(g_rb.sync_bytes);
}

static uint32_t rb_run_frame_inputs(void)
{
    uint32_t p1 = g_rb.resolved[0] & RB_BUTTON_MASK;
    uint32_t p2 = (rb_slot_count() > 1 ? g_rb.resolved[1] : 0u) & RB_BUTTON_MASK;
    /* Seats 0 and 1 ride the packed word; 2..7 were already applied by
     * rb_publish_resolved, which routes them through RtlSetPadState.
     * Both seats plugged, matching snes_netplay_active_mask(). */
    return p1 | (p2 << 12) | (3u << 30);
}

/* Pull the row every seat should simulate at `tick` out of the sealed table.
 * Only used during resim: Live resolves from history/wire instead. */
/* Set by rb_load_sealed_rows when a row is missing, so the abort can name the
 * seat and tick instead of saying only that "a" row was absent. */
static int      g_rb_missing_slot = -1;
static uint32_t g_rb_missing_tick;

static int rb_load_sealed_rows(uint32_t tick)
{
    int slots = rb_slot_count();
    int i;

    for (i = 0; i < slots; ++i) {
        RNetRbFrame f;
        if (!rnet_rb_get_sealed_frame(g_rb.rb, i, tick, &f)) {
            g_rb_missing_slot = i;
            g_rb_missing_tick = tick;
            return 0;
        }
        rb_row_sanitize(&f);
        g_rb.resolved[i] = f.buttons;
    }
    return 1;
}

static int rb_advance_sim(void *ctx, uint32_t tick)
{
    (void)ctx;
    if (!rb_load_sealed_rows(tick))
        return 0;
    rb_publish_resolved(tick);
    rb_apply_sync_bytes();
    RtlRunFrame(rb_run_frame_inputs());
    g_rb.resim_ticks++;
    return 1;
}

static int rb_vt_save_state(void *ctx, uint32_t tick)
{
    (void)ctx;
    if (!g_rb.snaps)
        return 0;
    return rbe_snap_ring_save(g_rb.snaps, tick, &g_snap_vt);
}

static int rb_vt_load_state(void *ctx, uint32_t tick)
{
    (void)ctx;
    if (!g_rb.snaps)
        return 0;
    return rbe_snap_ring_load(g_rb.snaps, tick, &g_snap_vt);
}

static uint32_t rb_vt_state_digest(void *ctx, uint32_t tick, uint32_t partition)
{
    (void)ctx;
    (void)tick;
    return rb_digest(partition);
}

static uint8_t rb_vt_hash_confirm_through(void *ctx, uint32_t tick)
{
    (void)ctx;
    return rbe_hc_confirm_through(&g_rb.hc, tick);
}

static uint8_t rb_vt_get_input_row(void *ctx, int32_t slot, uint32_t tick,
                                   RNetRbFrame *out)
{
    (void)ctx;
    if (!out || slot < 0 || slot >= rb_slot_count())
        return 0;
    if (!rbe_ih_get(&g_rb.ih, (int)slot, tick, out))
        return 0;
    rb_row_sanitize(out);
    return 1;
}

/* ── resim window ────────────────────────────────────────────────────── */

/*
 * Resim replays ticks the player has already seen and heard. Presentation
 * must not repeat with it (recomp-ai-rules/NETPLAY.md §1: the presented image
 * is never simulation), so the renderer is off for the span and the audio
 * the resim re-produces is dropped by rewinding the DSP producer cursor back
 * to where it stood before the rewind. The consumer cursor is never touched —
 * it belongs to the audio thread.
 */
static void rb_resim_begin(void)
{
    g_rb.in_resim = 1;
    g_rb.resim_audio_cursor = RtlAudioProducerCursor();
    if (g_snes) {
        g_rb.saved_disable_render = g_snes->disableRender ? 1 : 0;
        g_snes->disableRender = true;
    }
}

static void rb_resim_end(void)
{
    RtlAudioRewindProducer(g_rb.resim_audio_cursor);
    if (g_snes)
        g_snes->disableRender = g_rb.saved_disable_render ? true : false;
    g_rb.in_resim = 0;
}

/* ── scheduler gates ─────────────────────────────────────────────────── */

static uint32_t rb_gate_now_ms(void *ctx) { (void)ctx; return rbe_mono_ms(); }

static uint8_t rb_gate_episode_active(void *ctx)
{
    (void)ctx;
    return (g_rb.stage == kRbSealing || g_rb.stage == kRbReplaying ||
            g_rb.stage == kRbVerifying) ? 1u : 0u;
}

static uint8_t rb_gate_tip_holding(void *ctx)
{
    (void)ctx;
    return g_rb.stage == kRbTipHold ? 1u : 0u;
}

static uint32_t rb_gate_episode_count(void *ctx)
{
    (void)ctx;
    return g_rb.episode_count;
}

static uint64_t rb_gate_replay_ticks(void *ctx)
{
    (void)ctx;
    return g_rb.resim_ticks;
}

static void rb_bind_sched(void)
{
    RbeSchedBridge br;
    static int s_rollback = 1;

    memset(&br, 0, sizeof(br));
    br.session = g_rb.b.session;
    br.input_delay = g_rb.b.input_delay;
    br.input_prediction = &g_rb.prediction_cap;
    br.local_slot = g_rb.b.local_slot;
    br.force_turn = g_rb.b.force_turn;
    br.rollback = &s_rollback;
    br.gates.now_ms = &rb_gate_now_ms;
    br.gates.episode_active = &rb_gate_episode_active;
    br.gates.tip_holding = &rb_gate_tip_holding;
    br.gates.episode_count = &rb_gate_episode_count;
    br.gates.replay_ticks_total = &rb_gate_replay_ticks;
    /* Media / lockstep gates stay NULL: the SNES host has no FMV path, so
     * invent is never held for media and auto-D always samples. */
    rbe_sched_bind(&br);
}

/* ── lifecycle ───────────────────────────────────────────────────────── */

void snes_netplay_rb_bind(const SnesNetplayRbBindings *b)
{
    if (!b) {
        memset(&g_rb.b, 0, sizeof(g_rb.b));
        return;
    }
    g_rb.b = *b;
}

int snes_netplay_rb_start(void)
{
    RNetRbConfig cfg;
    RNetRollbackVTable vt;
    int slots = rb_slot_count();
    int i;

    snes_netplay_rb_shutdown();
    g_rb.force_invent_slot = -1;

    /* Session-settled P (recomp-ui: P = 4 + D) when the host bound one;
     * SNES_RB_PREDICTION remains the operator override. A P below the delay
     * it must cover is what produced the observed freezes: pred_depth walks
     * to the cap during a relay stall, pcap FREEZE stops the sim, and the
     * hitch shows as debt. */
    {
        /* Order: explicit session value (lobby publishes P = 4 + D), else the
         * same 4 + D rule applied locally, clamped 6..16 exactly as
         * recomp-ui's np_rb_prediction_frames_from_rtt_ms does. A fixed
         * default of 8 was SHORTER than D on any relayed session (measured
         * at D=9), so pred_depth walked to the cap during a stall and
         * tripped pcap FREEZE — the hitch, with resims themselves healthy
         * (mispredict_age_max=1). SNES_RB_PREDICTION still overrides. */
        int bound = g_rb.b.input_prediction ? *g_rb.b.input_prediction : 0;
        int dflt;
        if (bound >= 2) {
            dflt = bound;
        } else {
            dflt = 4 + rb_input_delay();
            if (dflt < 6) dflt = 6;
            if (dflt > 16) dflt = 16;
        }
        g_rb.prediction_cap = rb_env_int("SNES_RB_PREDICTION", dflt, 1, 32);
    }
    g_rb.snap_interval = (uint32_t)rb_env_int("SNES_RB_SNAP_INTERVAL", 1, 1, 16);
    g_rb.seal_timeout_ms =
        (uint32_t)rb_env_int("SNES_RB_EPISODE_TIMEOUT_MS", 2000, 100, 30000);

    memset(&cfg, 0, sizeof(cfg));
    cfg.local_slot = (uint32_t)rb_local_slot();
    cfg.delay = (uint32_t)rb_input_delay();
    cfg.slot_count = (uint32_t)slots;
    cfg.tip_runway = (uint32_t)rb_env_int("SNES_RB_TIP_RUNWAY",
                                          RNET_RB_TIP_RUNWAY_DEFAULT, 0, 32);
    /* Keep the light-tip ceiling at the runway: a coalesced episode's depth
     * grows toward tip_runway, and a ceiling below it silently costs the
     * fast path a second round trip (recomp-net docs/rollback.md, "Light
     * tip"). */
    cfg.light_tip_max_depth = cfg.tip_runway > RNET_RB_LIGHT_TIP_MAX_DEPTH
                                  ? cfg.tip_runway
                                  : RNET_RB_LIGHT_TIP_MAX_DEPTH;

    memset(&vt, 0, sizeof(vt));
    vt.ctx = NULL;
    vt.save_state = &rb_vt_save_state;
    vt.load_state = &rb_vt_load_state;
    vt.advance_sim = &rb_advance_sim;
    vt.state_digest = &rb_vt_state_digest;
    vt.hash_confirm_through = &rb_vt_hash_confirm_through;
    vt.get_input_row = &rb_vt_get_input_row;

    g_rb.rb = rnet_rb_create(&cfg, &vt);
    if (!g_rb.rb)
        return 0;

    g_rb.snaps = rbe_snap_ring_create(
        (uint32_t)rb_env_int("SNES_RB_SNAP_DEPTH",
                             (int)RBE_SNAP_RING_DEFAULT_DEPTH, 8, 240));
    if (!g_rb.snaps) {
        rnet_rb_destroy(g_rb.rb);
        g_rb.rb = NULL;
        return 0;
    }

    rbe_ih_reset(&g_rb.ih, slots);
    rbe_hc_reset(&g_rb.hc);

    /* Seed a neutral row per seat so hold-last never has to fall back to the
     * PSX-shaped 0xFFFF sentinel on the first invent. */
    for (i = 0; i < slots; ++i) {
        RNetRbFrame f;
        rb_row_make(&f, 0u, 0u, 0);
        rbe_ih_put(&g_rb.ih, i, &f);
    }

    rb_bind_sched();
    if (rb_session())
        rnet_session_set_rb_peer_slot(rb_session(),
                                      rb_local_slot() == 0 ? 1 : 0);

    g_rb.sim = 0;
    g_rb.stage = kRbIdle;
    g_rb.started = 1;
    g_rb.stall_tag = NULL;
    fprintf(stderr,
            "snes_netplay: ROLLBACK start slot=%d slots=%d D=%d P=%d "
            "snap_interval=%u tip_runway=%u\n",
            rb_local_slot(), slots, rb_input_delay(), g_rb.prediction_cap,
            (unsigned)g_rb.snap_interval, (unsigned)cfg.tip_runway);
    return 1;
}

void snes_netplay_rb_shutdown(void)
{
    if (g_rb.rb) {
        rnet_rb_destroy(g_rb.rb);
        g_rb.rb = NULL;
    }
    if (g_rb.snaps) {
        rbe_snap_ring_destroy(g_rb.snaps);
        g_rb.snaps = NULL;
    }
    free(g_rb.snap_scratch);
    g_rb.snap_scratch = NULL;
    g_rb.snap_scratch_cap = 0;
    rbe_sched_bind(NULL);
    g_rb.started = 0;
    g_rb.stage = kRbIdle;
    g_rb.sim = 0;
    g_rb.in_resim = 0;
    g_rb.staged_valid = 0;
    g_rb.sync_valid = 0;
    g_rb.peer_post_seen = 0;
    g_rb.episode_count = 0;
    g_rb.desync_count = 0;
    g_rb.resim_ticks = 0;
    g_rb.fork_seen = 0;
    g_rb.stall_tag = NULL;
}

void snes_netplay_rb_stage_local(uint16_t buttons)
{
    g_rb.staged = buttons & RB_BUTTON_MASK;
    g_rb.staged_valid = 1;
}

/* ── episode teardown ────────────────────────────────────────────────── */

#define RB_COOLDOWN_TICKS 30u

static void rb_stage_set(RbEpisodeStage stage)
{
    g_rb.stage = stage;
    g_rb.stage_entered_ms = rbe_mono_ms();
}

static void rb_episode_clear(void)
{
    rb_stage_set(kRbIdle);
    g_rb.initiator = 0;
    g_rb.peer_post_seen = 0;
    g_rb.local_base_valid = 0;
    g_rb.peer_base_valid = 0;
    memset(&g_rb.corr, 0, sizeof(g_rb.corr));
    if (g_rb.rb)
        rnet_rb_session_reset(g_rb.rb);
}

static void rb_episode_abort(uint8_t abort_class, const char *why)
{
    RNetSession *s = rb_session();
    fprintf(stderr,
            "snes_netplay: RB abort epoch=%u load=%u target=%u class=%u — %s\n",
            (unsigned)g_rb.corr.epoch_id, (unsigned)g_rb.corr.load_tick,
            (unsigned)g_rb.corr.target_tick, (unsigned)abort_class,
            why ? why : "?");
    if (s)
        rnet_session_send_rb_sync(s, g_rb.corr.epoch_id, abort_class,
                                  g_rb.sim, 0u,
                                  (rnet_u8)(g_rb.corr.slot < 0 ? 0 : g_rb.corr.slot),
                                  RNET_RB_SYNC_OP_ABORT, 0u);
    rb_episode_clear();
    g_rb.cooldown_until_tick = g_rb.sim + RB_COOLDOWN_TICKS;
}

/* ── seal-row exchange ───────────────────────────────────────────────── */

/*
 * Publish this peer's authoritative rows for the sealed span.
 *
 * Two wire fields here are NOT ticks, and sending ticks in them silently
 * posted nothing at all:
 *
 *   row_begin  is an OFFSET into the sealed span, zero-based
 *              (rnet_rb_export_seal_rows_chunk / rnet_rb_apply_peer_seal_rows
 *              both index `sealed[offset]`; recomp-net's own episode test
 *              passes 0, 3, 4). Passing load_tick made the very first export
 *              take `row_begin >= sealed_span`, return count 0, and break the
 *              loop before a single packet went out.
 *
 *   mismatch   carries seal_base_tick, which is the LOAD tick, not
 *              corr.mismatch_tick. The receiver rejects the chunk outright
 *              when it disagrees. The two are equal whenever the snapshot
 *              floor lands on the mismatch itself, which is why this hid.
 *
 * The cost was invisible in a healthy match and total in an unhealthy one.
 * A FOLLOWER never notices: rnet_rb_fill_local_row pre-seals a remote seat
 * straight from wire-confirmed history, so its mask completes with no peer
 * message. An INITIATOR cannot — the row that opened the episode is by
 * definition predicted, so it is the one row that must come from the peer.
 * Measured over a real LAN match: follower 12/12 episodes replayed, initiator
 * 12/12 "timed out waiting for peer seal rows" after the full 2 s budget, one
 * stall every 45 frames. No rollback correction had ever completed.
 */
static void rb_send_local_seal_rows(void)
{
    RNetSession *s = rb_session();
    int slot = rb_local_slot();
    uint32_t span = g_rb.corr.target_tick >= g_rb.corr.load_tick
                        ? g_rb.corr.target_tick - g_rb.corr.load_tick + 1u
                        : 0u;
    uint32_t off = 0;

    if (!s || !g_rb.rb)
        return;
    while (off < span) {
        RNetRbFrame rows[RB_SEAL_CHUNK];
        uint32_t count = 0;
        uint32_t want = span - off;
        if (want > RB_SEAL_CHUNK)
            want = RB_SEAL_CHUNK;
        if (!rnet_rb_export_seal_rows_chunk(g_rb.rb, slot, off, want, rows,
                                            &count) || count == 0)
            break;
        rnet_session_send_rb_seal_rows(s, g_rb.corr.epoch_id,
                                       g_rb.corr.load_tick,
                                       g_rb.corr.target_tick, (rnet_u8)slot,
                                       off, rows, (rnet_u16)count);
        off += count;
    }
}

/* ── replay ──────────────────────────────────────────────────────────── */

static void rb_baseline_try_compare(void);

static void rb_send_baseline(void)
{
    RNetSession *s = rb_session();
    SnesStateDigestParts p;
    if (!s)
        return;
    snes_state_digest_parts(&p);
    /* Ours is the digest of the tick we just loaded. Keep it: the peer's copy
     * is compared against THIS, never against whatever the live machine
     * happens to hold when the message lands. */
    g_rb.local_base = p;
    g_rb.local_base_valid = 1;
    rnet_session_send_rb_baseline(s, g_rb.corr.epoch_id, g_rb.corr.load_tick,
                                  p.master, p.wram, p.apu, p.ppu);
    rb_baseline_try_compare();
}

/*
 * Load the baseline snapshot and resim the sealed span. Runs inline: on SNES
 * a resim tick is a plain RtlRunFrame, and the deepest span the library will
 * open is bounded by the tip runway, so the whole replay is tens of frames of
 * emulation with no host stack to unwind.
 */
static int rb_run_replay(void)
{
    uint32_t t;

    if (!rb_vt_load_state(NULL, g_rb.corr.load_tick)) {
        rb_episode_abort(RNET_RB_ABORT_CLASS_NO_SNAP, "no snapshot at load tick");
        return 0;
    }
    rb_send_baseline();
    /* rb_send_baseline compares digests and may abort the episode outright on
     * a fork, which clears corr and resets the session. Carrying on into the
     * loop below then re-aborted with "sealed row missing mid-replay" — one
     * cause, two log lines, and a half-replayed timeline in between. */
    if (g_rb.stage == kRbIdle)
        return 0;
    rnet_rb_set_phase(g_rb.rb, nRNetRbPhaseReplay);

    rb_resim_begin();
    for (t = g_rb.corr.load_tick; t <= g_rb.corr.target_tick; ++t) {
        /* Re-key the ring onto the replayed timeline as we go: every entry
         * from here on must describe the corrected run, not the dead one. */
        rb_snap_take(t);
        if (!rb_advance_sim(NULL, t)) {
            char why[128];
            rb_resim_end();
            snprintf(why, sizeof(why),
                     "sealed row missing mid-replay: slot=%d tick=%u "
                     "(span %u..%u, seal_base=%u span_len=%u)",
                     g_rb_missing_slot, (unsigned)g_rb_missing_tick,
                     (unsigned)g_rb.corr.load_tick,
                     (unsigned)g_rb.corr.target_tick,
                     (unsigned)rnet_rb_get_seal_base_tick(g_rb.rb),
                     (unsigned)rnet_rb_get_seal_span(g_rb.rb));
            rb_episode_abort(RNET_RB_ABORT_CLASS_ABORT, why);
            return 0;
        }
    }
    rb_resim_end();
    /* Anything keyed past the target belongs to the timeline we just
     * discarded; a later episode must never load one of those. */
    (void)rbe_snap_ring_drop_after(g_rb.snaps, g_rb.corr.target_tick);

    g_rb.sim = g_rb.corr.target_tick + 1u;
    if (rb_session())
        rnet_session_set_sim_tick(rb_session(), g_rb.sim);
    return 1;
}

static void rb_enter_verify(void)
{
    RNetSession *s = rb_session();
    uint32_t master = rb_digest(SNES_DIGEST_PART_MASTER);

    rnet_rb_set_phase(g_rb.rb, nRNetRbPhaseVerify);
    g_rb.local_post_digest = master;
    if (s)
        rnet_session_send_rb_post(s, g_rb.corr.epoch_id, g_rb.corr.target_tick,
                                  master, 0u, 1u);
    rb_stage_set(kRbVerifying);
    g_rb.peer_post_seen = 0;
}

static void rb_commit_episode(void)
{
    RNetSession *s = rb_session();

    rnet_rb_on_post_match(g_rb.rb);
    rnet_rb_commit_promote_sealed(g_rb.rb);
    /* Ticks through the target are now agreed; drop the live-invent
     * FRAME_COMMITs that preceded the correction so the watermark restarts
     * from a tick both peers actually ran. */
    rbe_hc_prime_after(&g_rb.hc, g_rb.corr.target_tick);
    rbe_sched_note_episode_boundary();
    if (rnet_rb_enter_tip_hold(g_rb.rb)) {
        rb_stage_set(kRbTipHold);
    } else {
        rb_episode_clear();
    }
    if (s) {
        rnet_session_send_rb_sync(s, g_rb.corr.epoch_id,
                                  g_rb.corr.mismatch_tick, g_rb.corr.load_tick,
                                  g_rb.corr.target_tick,
                                  (rnet_u8)(g_rb.corr.slot < 0 ? 0 : g_rb.corr.slot),
                                  RNET_RB_SYNC_OP_COMMIT, 0u);
        rnet_session_send_rb_resolved(s, rnet_rb_resolved_through(g_rb.rb));
    }
}

/* ── episode open ────────────────────────────────────────────────────── */

static int rb_cooldown_active(void)
{
    return g_rb.cooldown_until_tick != 0u && g_rb.sim < g_rb.cooldown_until_tick;
}

static int rb_begin_episode(uint32_t mismatch_tick, int slot, int as_initiator,
                            uint32_t peer_load, uint32_t peer_target,
                            uint32_t peer_epoch, uint8_t peer_flags)
{
    RNetSession *s = rb_session();
    uint32_t load;
    uint32_t target;
    uint32_t sim_tip = g_rb.sim ? g_rb.sim - 1u : 0u;

    if (!g_rb.rb || g_rb.stage != kRbIdle)
        return 0;
    if (as_initiator && rb_cooldown_active())
        return 0;
    if (mismatch_tick == 0u)
        return 0;

    if (as_initiator) {
        if (!rb_snap_floor(mismatch_tick, &load)) {
            /* No snapshot reaches back that far. Refusing is the honest
             * outcome — loading a different tick would resim a timeline
             * neither peer ran. */
            rbe_sched_note_mispredict(g_rb.sim - mismatch_tick);
            return 0;
        }
        target = rnet_rb_suggest_target(g_rb.rb, mismatch_tick, sim_tip);
        /* suggest_target adds tip_seal_slack (default 2) so the tick after the
         * replay is already sealed. That only works if this host can hand the
         * engine its own seat's rows for those ticks, and it cannot:
         * rb_vt_get_input_row reads the admitted input history, which by
         * definition stops at the tip. An initiator detects its mismatch on
         * the very next tick, so the slack landed two ticks in its own future
         * and the replay died on "sealed row missing mid-replay: slot=0
         * tick=<tip+1>" — measured on episode #1 of every session, after
         * which the peers were divergent and every later episode forked at
         * the baseline instead. Replay only ticks we hold authoritative input
         * for; the engine re-seals the tip normally, and rnet_rb_extend_target
         * still grows the span when the peer advertises more. */
        if (target > sim_tip)
            target = sim_tip;
    } else {
        load = peer_load;
        target = peer_target;
        if (!rbe_snap_ring_has(g_rb.snaps, load)) {
            if (s)
                rnet_session_send_rb_sync(s, peer_epoch, mismatch_tick, load,
                                          rnet_rb_resolved_through(g_rb.rb),
                                          (rnet_u8)(slot < 0 ? 0 : slot),
                                          RNET_RB_SYNC_OP_NACK, 0u);
            return 0;
        }
    }
    if (target < mismatch_tick)
        target = mismatch_tick;

    memset(&g_rb.corr, 0, sizeof(g_rb.corr));
    g_rb.corr.epoch_id = as_initiator
                             ? (((++g_rb.epoch_seq) << 1) |
                                (uint32_t)(rb_local_slot() & 1))
                             : peer_epoch;
    g_rb.corr.mismatch_tick = mismatch_tick;
    g_rb.corr.load_tick = load;
    g_rb.corr.target_tick = target;
    g_rb.corr.slot = slot;
    g_rb.corr.initiator = as_initiator ? 1u : 0u;
    g_rb.corr.from_peer_notify = as_initiator ? 0u : 1u;
    g_rb.corr.flags = as_initiator
                          ? (rnet_rb_is_light_tip_candidate_ex(
                                 load, target, rnet_rb_resolved_through(g_rb.rb),
                                 rnet_rb_get_light_tip_max_depth(g_rb.rb))
                                 ? RNET_RB_CORR_LIGHT_TIP : 0u)
                          : (peer_flags & RNET_RB_SYNC_FLAG_LIGHT_TIP
                                 ? RNET_RB_CORR_LIGHT_TIP : 0u);

    rnet_rb_begin_episode(g_rb.rb, &g_rb.corr);
    rnet_rb_seal_inputs(g_rb.rb, load, target, slot);
    if (!rnet_rb_inputs_sealed(g_rb.rb)) {
        rb_episode_clear();
        return 0;
    }

    if (as_initiator && s)
        rnet_session_send_rb_sync(s, g_rb.corr.epoch_id, mismatch_tick, load,
                                  target, (rnet_u8)(slot < 0 ? 0 : slot),
                                  RNET_RB_SYNC_OP_BEGIN,
                                  (rnet_u8)(g_rb.corr.flags & RNET_RB_CORR_LIGHT_TIP
                                                ? RNET_RB_SYNC_FLAG_LIGHT_TIP : 0u));

    rb_send_local_seal_rows();
    g_rb.initiator = as_initiator;
    rb_stage_set(kRbSealing);
    g_rb.episode_count++;
    /* A resim episode is the whole point of rollback and previously left no
     * trace in the log — "mispredict=N" counts DETECTIONS (and one of its two
     * call sites is the refusal path), so it could not answer "did we
     * actually rewind and replay?". load..target is the replayed span. */
    fprintf(stderr,
            "rbe: RESIM episode #%u %s slot=%d mismatch=%u load=%u target=%u "
            "(rewind %u frames, replay %u)\n",
            (unsigned)g_rb.episode_count,
            as_initiator ? "initiator" : "follower", slot,
            (unsigned)mismatch_tick, (unsigned)load, (unsigned)target,
            (unsigned)(g_rb.sim > load ? g_rb.sim - load : 0u),
            /* load..target INCLUSIVE — a one-tick span replays 1 frame, not
             * 0. The old "replay 0" read as "nothing was re-run". */
            (unsigned)(target >= load ? target - load + 1u : 0u));
    return 1;
}

/* ── wire ingress ────────────────────────────────────────────────────── */

/*
 * Compare the two baseline digests once BOTH exist. Ours only exists after
 * rb_run_replay() has loaded the snapshot for corr.load_tick — until then the
 * live machine is sitting on a later tick, and digesting it would compare two
 * different instants. Doing exactly that was the defect: on a loopback pair
 * the peer's BASELINE always beat our own load, so every single episode
 * reported a fork and aborted (measured 83/83), the correction never ran, and
 * the follower ended up blank on a genuinely divergent guest. Over a real
 * link the race is merely usually lost, which is why it read as an occasional
 * desync rather than a broken mechanism.
 */
static void rb_baseline_try_compare(void)
{
    const SnesStateDigestParts *mine = &g_rb.local_base;

    if (!g_rb.local_base_valid || !g_rb.peer_base_valid)
        return;
    if (g_rb.peer_base_epoch != g_rb.corr.epoch_id ||
        g_rb.peer_base_tick != g_rb.corr.load_tick)
        return;
    g_rb.peer_base_valid = 0;   /* one verdict per exchange */

    if (mine->master == g_rb.peer_base_master)
        return;

    /* The two peers do not agree on the state they are about to replay from,
     * so the replay is doomed before it starts. Name the subsystem that
     * moved — "the state differs" is not a diagnosis. */
    g_rb.fork_seen = 1;
    g_rb.fork_tick = g_rb.corr.load_tick;
    if (mine->wram != g_rb.peer_base_wram)
        g_rb.fork_partition = snes_state_digest_part_name(SNES_DIGEST_PART_WRAM);
    else if (mine->apu != g_rb.peer_base_apu)
        g_rb.fork_partition = snes_state_digest_part_name(SNES_DIGEST_PART_APU);
    else if (mine->ppu != g_rb.peer_base_ppu)
        g_rb.fork_partition = snes_state_digest_part_name(SNES_DIGEST_PART_PPU);
    else
        g_rb.fork_partition = "other";
    g_rb.desync_count++;
    fprintf(stderr,
            "snes_netplay: RB BASELINE FORK tick=%u partition=%s "
            "local=%08x peer=%08x\n",
            (unsigned)g_rb.corr.load_tick, g_rb.fork_partition,
            (unsigned)mine->master, (unsigned)g_rb.peer_base_master);
    rb_episode_abort(RNET_RB_ABORT_CLASS_ABORT, "baseline digest fork");
}

static void rb_on_peer_baseline(uint32_t epoch, uint32_t load_tick,
                                uint32_t master, uint32_t wram, uint32_t apu,
                                uint32_t ppu)
{
    if (g_rb.stage == kRbIdle || epoch != g_rb.corr.epoch_id ||
        load_tick != g_rb.corr.load_tick)
        return;
    g_rb.peer_base_epoch  = epoch;
    g_rb.peer_base_tick   = load_tick;
    g_rb.peer_base_master = master;
    g_rb.peer_base_wram   = wram;
    g_rb.peer_base_apu    = apu;
    g_rb.peer_base_ppu    = ppu;
    g_rb.peer_base_valid  = 1;
    rb_baseline_try_compare();
}

static void rb_on_peer_post(uint32_t epoch, uint32_t target, uint32_t master)
{
    if (g_rb.stage != kRbVerifying || epoch != g_rb.corr.epoch_id)
        return;
    /* After a tip-extend the peer may still deliver a POST for the prior tip
     * while we verify the new one; latching that digest is a false fork. */
    if (!rbe_rb_peer_post_tip_ok(target, g_rb.corr.target_tick))
        return;
    g_rb.peer_post_digest = master;
    g_rb.peer_post_seen = 1;
}

static void rb_drain_wire(void)
{
    RNetSession *s = rb_session();
    rnet_u32 epoch, a, b, c;
    rnet_u8 slot, op, flags;

    if (!s)
        return;

    while (rnet_session_take_rb_sync(s, &epoch, &a, &b, &c, &slot, &op, &flags)) {
        switch (op) {
        case RNET_RB_SYNC_OP_BEGIN:
            /* Concurrent dual initiation: lower initiator slot wins, the
             * loser yields and follows (recomp-net docs/rollback.md). */
            if (g_rb.stage != kRbIdle) {
                int peer_slot = rb_local_slot() == 0 ? 1 : 0;
                if (g_rb.initiator && peer_slot < rb_local_slot())
                    rb_episode_clear();
                else
                    break;
            }
            rb_begin_episode(a, (int)slot, 0, b, c, epoch, flags);
            break;
        case RNET_RB_SYNC_OP_NACK:
            if (g_rb.stage != kRbIdle && epoch == g_rb.corr.epoch_id) {
                /* c carries the follower's confirmed frontier: demote to a
                 * mutually provable tick instead of guessing load-1. */
                rnet_rb_demote_resolved_through(g_rb.rb, c);
                rb_episode_abort(RNET_RB_ABORT_CLASS_REALIGN, "peer NACK");
            }
            break;
        case RNET_RB_SYNC_OP_ABORT:
            if (g_rb.stage != kRbIdle && epoch == g_rb.corr.epoch_id) {
                rb_episode_clear();
                /* Mirror the sender's cooldown class so both peers re-arm on
                 * the same schedule. */
                g_rb.cooldown_until_tick =
                    g_rb.sim + (a == RNET_RB_ABORT_CLASS_REALIGN
                                    ? 0u : RB_COOLDOWN_TICKS);
            }
            break;
        case RNET_RB_SYNC_OP_COMMIT:
            if (g_rb.stage == kRbTipHold && epoch == g_rb.corr.epoch_id)
                rb_episode_clear();
            break;
        default:
            break;
        }
    }

    for (;;) {
        RNetRbFrame rows[RB_SEAL_CHUNK];
        rnet_u16 count = 0;
        rnet_u32 row_begin = 0;
        if (!rnet_session_take_rb_seal_rows(s, &epoch, &a, &b, &slot,
                                            &row_begin, rows, &count))
            break;
        if (g_rb.stage == kRbIdle || epoch != g_rb.corr.epoch_id || count == 0)
            continue;
        {
            rnet_u16 i;
            for (i = 0; i < count; ++i)
                rb_row_sanitize(&rows[i]);
        }
        rnet_rb_apply_peer_seal_rows(g_rb.rb, epoch, a, b, (int32_t)slot,
                                     row_begin, rows, count);
    }

    {
        rnet_u32 dm, dw, da, dp;
        while (rnet_session_take_rb_baseline(s, &epoch, &a, &dm, &dw, &da, &dp))
            rb_on_peer_baseline(epoch, a, dm, dw, da, dp);
    }

    while (rnet_session_take_rb_post(s, &epoch, &a, &b, &c, &op))
        rb_on_peer_post(epoch, a, b);

    while (rnet_session_take_rb_resolved(s, &a)) {
        if (g_rb.rb)
            rnet_rb_set_peer_convergence(g_rb.rb, a);
    }

    while (rnet_session_take_rb_frame_commit(s, &a, &b))
        rbe_hc_note_peer(&g_rb.hc, a, b);
}

/* ── episode pump ────────────────────────────────────────────────────── */

/*
 * An episode stalls the sim while it waits on the peer. A lost SEAL_ROWS or
 * POST datagram must therefore not be able to wait forever — without a
 * watchdog a single dropped packet freezes the match with no diagnosis. The
 * budget is generous relative to any plausible RTT: expiring is a real
 * failure, so it aborts loudly and takes the cooldown.
 */
static int rb_stage_expired(void)
{
    uint32_t now = rbe_mono_ms();
    return (uint32_t)(now - g_rb.stage_entered_ms) > g_rb.seal_timeout_ms;
}

static void rb_pump_episode(void)
{
    switch (g_rb.stage) {
    case kRbSealing:
        if (rnet_rb_all_peer_seal_rows_complete(g_rb.rb)) {
            rnet_rb_set_phase(g_rb.rb, nRNetRbPhaseAwaitingBaseline);
            if (rb_run_replay())
                rb_enter_verify();
        } else if (rb_stage_expired()) {
            rb_episode_abort(RNET_RB_ABORT_CLASS_ABORT,
                             "timed out waiting for peer seal rows");
        }
        break;
    case kRbVerifying:
        if (g_rb.peer_post_seen) {
            uint32_t local = g_rb.local_post_digest;
            if (local == g_rb.peer_post_digest) {
                rb_commit_episode();
            } else {
                g_rb.desync_count++;
                g_rb.fork_seen = 1;
                g_rb.fork_tick = g_rb.corr.target_tick;
                g_rb.fork_partition = "post";
                fprintf(stderr,
                        "snes_netplay: RB POST FORK tick=%u local=%08x "
                        "peer=%08x\n",
                        (unsigned)g_rb.corr.target_tick, (unsigned)local,
                        (unsigned)g_rb.peer_post_digest);
                rnet_rb_on_post_diverge(g_rb.rb);
                rb_episode_abort(RNET_RB_ABORT_CLASS_ABORT, "post digest fork");
            }
        } else if (rb_stage_expired()) {
            rb_episode_abort(RNET_RB_ABORT_CLASS_ABORT,
                             "timed out waiting for peer POST");
        }
        break;
    case kRbTipHold:
        if (g_rb.sim >
            g_rb.corr.target_tick + rnet_rb_get_tip_runway(g_rb.rb))
            rb_episode_clear();
        break;
    default:
        break;
    }
}

/* Validation knob (SNES_RB_FORCE_MISPREDICT=N): every Nth tick, IGNORE the
 * remote row that already arrived so the engine must invent one, and make
 * that invented row deliberately wrong. The true row is still in the session
 * and is consumed on a later tick, where reconcile compares it against the
 * invention, finds the mismatch, and must rewind and replay.
 *
 * This shape matters. An earlier version corrupted the ARRIVED row in place,
 * which made the local sim run a frame on input it had already received
 * correctly — an divergence the peer had no matching episode for, so a failed
 * correction (measured: "timed out waiting for peer seal rows" on the very
 * first episode) left the two sides permanently forked and the follower
 * black. Withholding-then-inventing reproduces a REAL late-arrival instead,
 * where the correction is the same one natural play would perform.
 *
 * Needed because a healthy session never exercises rollback at all: with D
 * covering the RTT the remote row is always already present (measured:
 * pred_depth=0 on ~85% of admits over a real match, zero invents on
 * loopback), so nothing is predicted and nothing is ever rewound. */
static int rb_force_mispredict_every(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("SNES_RB_FORCE_MISPREDICT");
        cached = (v && v[0]) ? atoi(v) : 0;
        if (cached < 0) cached = 0;
        if (cached)
            fprintf(stderr,
                    "rbe: FORCE MISPREDICT every %d remote row "
                    "(validation only)\n", cached);
    }
    return cached;
}

/* ── reconcile late wire against predicted history ───────────────────── */

static void rb_reconcile_wire(void)
{
    RNetSession *s = rb_session();
    int slots = rb_slot_count();
    int local = rb_local_slot();
    uint32_t from = rnet_rb_resolved_through(g_rb.rb) + 1u;
    int slot;

    /* Nothing older than the history window can be reconciled anyway, and an
     * un-advancing watermark would otherwise make this scan grow without
     * bound as the match runs. */
    if (g_rb.sim > RBE_INPUT_HIST_DEPTH &&
        from < g_rb.sim - RBE_INPUT_HIST_DEPTH)
        from = g_rb.sim - RBE_INPUT_HIST_DEPTH;

    if (!s)
        return;
    for (slot = 0; slot < slots; ++slot) {
        uint32_t t;
        if (slot == local)
            continue;
        for (t = from; t < g_rb.sim; ++t) {
            RNetInputSample sample;
            RNetRbFrame published;
            RNetRbFrame wire;

            if (!rbe_ih_get(&g_rb.ih, slot, t, &published))
                continue;
            if (!published.is_predicted)
                continue;
            if (!rnet_session_peek_remote_input(
                    s, slot, rbe_sched_wire_for_sim(t), &sample) ||
                !sample.valid)
                continue;

            rb_row_make(&wire, t,
                        (uint16_t)(sample.bytes[0] |
                                   ((uint16_t)sample.bytes[1] << 8)),
                        0);
            if (wire.buttons == published.buttons) {
                /* Prediction held: promote in place, no episode. */
                rbe_ih_promote(&g_rb.ih, slot, &wire);
                continue;
            }

            {
                RNetInputContractFrame pub_c, wire_c;
                RNetInputContractDecision d;
                rbe_ih_frame_to_contract(&published, &pub_c);
                rbe_ih_frame_to_contract(&wire, &wire_c);
                d = rnet_rb_decide_stick_replace(g_rb.rb, &pub_c, &wire_c,
                                                 1 /* completed sim */);
                rbe_ih_promote(&g_rb.ih, slot, &wire);
                if (!rnet_input_contract_decision_is_rewind(d))
                    continue;
            }

            rbe_sched_note_mispredict(g_rb.sim > t ? g_rb.sim - t : 0u);
            if (g_rb.stage == kRbIdle)
                rb_begin_episode(t, slot, 1, 0u, 0u, 0u, 0u);
            else if (g_rb.stage == kRbTipHold &&
                     rnet_rb_can_extend_target(g_rb.rb, g_rb.sim))
                rnet_rb_extend_target(g_rb.rb, g_rb.sim);
            return; /* one correction at a time; the rest follow next tick */
        }
    }
}

/* ── live admit ──────────────────────────────────────────────────────── */

int snes_netplay_rb_poll_admit(void)
{
    RNetSession *s = rb_session();
    RNetSessionStats st;
    uint32_t wire;
    int slots = rb_slot_count();
    int local = rb_local_slot();
    int any_invent = 0;
    int slot;

    if (!g_rb.started || !s)
        return 0;
    if (!rnet_session_is_running(s))
        return 0;

    rb_drain_wire();
    rb_reconcile_wire();
    rb_pump_episode();

    /* Seal / Replay / Verify own the sim; Live must not advance under them. */
    if (g_rb.stage == kRbSealing || g_rb.stage == kRbVerifying) {
        g_rb.stall_tag = "episode";
        return 0;
    }

    memset(&st, 0, sizeof(st));
    rnet_session_get_stats(s, &st);
    rbe_sched_sync_delay_from_session();

    /*
     * Seal the local tip for this sim tick exactly ONCE.
     *
     * prepare_local_tip samples the staged pad and stores it at sim+D, then
     * emits it. Calling it again on a later stalled attempt at the same sim
     * tick would overwrite a wire tick the peer may already have consumed —
     * the two peers would then simulate different local input for that tick,
     * with no mismatch anywhere to detect it. Delay-sync latches for the same
     * reason (NetplayState.latched_for_tick).
     */
    if (!g_rb.tip_prepared_valid || g_rb.tip_prepared_for != g_rb.sim) {
        rnet_session_prepare_local_tip(s, g_rb.sim);
        g_rb.tip_prepared_for = g_rb.sim;
        g_rb.tip_prepared_valid = 1;
    }

    wire = rbe_sched_wire_for_sim(g_rb.sim);
    if (rbe_sched_pre_admit(g_rb.sim, wire, &st)) {
        g_rb.stall_tag = rbe_sched_admit_stall_tag();
        return 0;
    }

    for (slot = 0; slot < slots; ++slot) {
        RNetInputSample sample;
        RNetRbFrame row;

        memset(&sample, 0, sizeof(sample));
        if (slot == local) {
            /*
             * Our own seat simulates the row we PUBLISHED for this wire tick
             * — sampled D ticks ago — not the pad currently being held. The
             * live pad belongs to tick sim+D and the peer will not see it
             * until then; simulating it here would fork the two peers on
             * local input, which is the one thing rollback cannot correct
             * because neither side ever reports a mismatch.
             *
             * Before the delay pipeline has filled (sim < D) there is no
             * published row yet and neutral is the agreed value, matching the
             * neutral priming delay-sync does at session start.
             */
            uint16_t published = 0u;
            if (rnet_session_peek_input(s, slot, wire, &sample) && sample.valid)
                published = (uint16_t)(sample.bytes[0] |
                                       ((uint16_t)sample.bytes[1] << 8));
            rb_row_make(&row, g_rb.sim, published, 0);
            rbe_ih_put(&g_rb.ih, slot, &row);
            g_rb.resolved[slot] = row.buttons;
            if (slot == 0 && sample.valid && sample.size >= 4) {
                g_rb.sync_bytes[0] = sample.bytes[2];
                g_rb.sync_bytes[1] = sample.bytes[3];
                g_rb.sync_valid = 1;
            }
            continue;
        }

        {
            /* Validation: withhold an arrived row so the engine must invent
             * (see rb_force_mispredict_every). peek does not consume, so the
             * true row is still there for the reconcile pass. */
            const int every = rb_force_mispredict_every();
            if (every > 0 && slot != rb_local_slot()) {
                static unsigned long n;
                if ((++n % (unsigned long)every) == 0ul) {
                    g_rb.force_invent_slot = slot;
                    fprintf(stderr,
                            "rbe: forced late row slot=%d sim=%u "
                            "(invent + corrupt)\n",
                            slot, (unsigned)g_rb.sim);
                }
            }
        }
        if (g_rb.force_invent_slot != slot &&
            rnet_session_peek_remote_input(s, slot, wire, &sample) &&
            sample.valid) {
            rb_row_make(&row, g_rb.sim,
                        (uint16_t)(sample.bytes[0] |
                                   ((uint16_t)sample.bytes[1] << 8)),
                        0);
            rbe_ih_put(&g_rb.ih, slot, &row);
            g_rb.resolved[slot] = row.buttons;
            rbe_sched_note_remote_hit();
            /* Slot 0 carries the authoritative game sync bytes. */
            if (slot == 0 && sample.size >= 4) {
                g_rb.sync_bytes[0] = sample.bytes[2];
                g_rb.sync_bytes[1] = sample.bytes[3];
                g_rb.sync_valid = 1;
            }
            continue;
        }

        {
            const char *why = NULL;
            if (rbe_sched_on_remote_miss(slot, g_rb.sim, wire, &st,
                                          g_rb.prediction_cap, &why)) {
                g_rb.stall_tag = why ? why : "remote_miss";
                return 0;
            }
            if (!rbe_ih_invent_hold_last(&g_rb.ih, slot, g_rb.sim, &row))
                return 0;
            if (g_rb.force_invent_slot == slot) {
                /* Guarantee the invention is WRONG: hold-last would otherwise
                 * match a peer sitting on the same buttons, and a correct
                 * prediction exercises nothing. */
                row.buttons ^= 0x0040u;   /* Left: never an idle bit */
                g_rb.force_invent_slot = -1;
            }
            rb_row_sanitize(&row);
            /* Sanitise wrote through a copy; store the corrected row so the
             * PSX-shaped neutral can never reach the sim or a seal. */
            rbe_ih_put(&g_rb.ih, slot, &row);
            g_rb.resolved[slot] = row.buttons;
            any_invent = 1;
        }
    }

    rbe_sched_post_admit(any_invent);
    rbe_sched_clear_admit_stall();
    g_rb.stall_tag = NULL;
    rb_snap_take(g_rb.sim); /* state before this tick — see rb_snap_take */
    rb_publish_resolved(g_rb.sim);
    rb_apply_sync_bytes();
    return 1;
}

void snes_netplay_rb_finish_frame(void)
{
    RNetSession *s = rb_session();
    uint32_t master;

    if (!g_rb.started || g_rb.in_resim)
        return;

    master = rb_digest(SNES_DIGEST_PART_MASTER);
    rbe_hc_note_local(&g_rb.hc, g_rb.sim, master);
    if (s)
        rnet_session_send_rb_frame_commit(s, g_rb.sim, master);

    g_rb.sim++;
    if (s)
        rnet_session_set_sim_tick(s, g_rb.sim);

    /* A stuck watermark whose next tick aged out of the ring is not a fork. */
    (void)rbe_hc_heal_stale_gap(&g_rb.hc);
    if (g_rb.rb)
        rnet_rb_set_peer_convergence(g_rb.rb, rbe_hc_resolved_through(&g_rb.hc));
}

/* ── diagnostics ─────────────────────────────────────────────────────── */

uint32_t snes_netplay_rb_sim_tick(void) { return g_rb.sim; }
uint32_t snes_netplay_rb_episode_count(void) { return g_rb.episode_count; }
uint32_t snes_netplay_rb_invent_count(void) { return g_rb.ih.invent_count; }
uint32_t snes_netplay_rb_promote_count(void) { return g_rb.ih.promote_count; }
uint64_t snes_netplay_rb_resim_ticks(void) { return g_rb.resim_ticks; }
uint32_t snes_netplay_rb_desync_count(void) { return g_rb.desync_count; }

int snes_netplay_rb_episode_active(void)
{
    return g_rb.stage != kRbIdle;
}

const char *snes_netplay_rb_stall_tag(void)
{
    return g_rb.stall_tag;
}

int snes_netplay_rb_last_fork(uint32_t *tick, const char **partition)
{
    if (!g_rb.fork_seen)
        return 0;
    if (tick)
        *tick = g_rb.fork_tick;
    if (partition)
        *partition = g_rb.fork_partition ? g_rb.fork_partition : "?";
    return 1;
}

#ifndef SNES_NETPLAY_H
#define SNES_NETPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Delay-sync netplay facade over recomp-net for SNES recomp hosts.
 *
 * Lockstep contract (matches recomp-net host_integration.md / MotK psx_netplay):
 *   wait_admit (publish pads for tick T) → RtlRunFrame →
 *   finish_frame (advance) → wait_admit for T+1 → …
 * Guest must NOT run while linking or while try_admit fails.
 *
 * Input ownership (MotK-style):
 *   - Each peer stages one local device sample; recomp-net maps it onto
 *     that peer's local_slot (host 0 = sim P1, guest 1 = sim P2).
 *   - input_player selects which host device (0/1) to sample; -1 = auto
 *     (host resolves before start: guest prefers device 0 so P1 keyboard
 *     wraps into sim P2; sole offline-P2 pad on host samples device 1;
 *     both pads live → same-PC dual seat uses local_slot).
 *   - While active, publish is the sole source of pads for RtlRunFrame.
 *
 * Pad blob (4 bytes per slot):
 *   [0..1] LE uint16 — 12 SNES controller bits (RtlRunFrame low 12 / 12..23)
 *   [2..3] optional game-defined deterministic sync bytes. Slot 0 is
 *          authoritative and its bytes are applied before each admitted frame.
 *
 * Transport:
 *   LAN  — rnet_session_start_lan(bind, peer)
 *   ICE  — rnet_session_start_ice + lobby WS signal relay (SNES_HAS_LOBBY_CLIENT)
 */

#define SNES_NETPLAY_PAD_BYTES 4
/* recomp-net carries up to RNET_MAX_SLOTS (8) seats; the SNES side reaches
 * that many only with a Super Multitap in each port. See docs/MULTITAP.md. */
#define SNES_NETPLAY_MAX_SLOTS 8

typedef struct SnesNetplayConfig {
    int         enabled;
    int         local_slot;    /* 0 .. slot_count-1 — lobby / wire slot */
    /* Seats in this match, 2..SNES_NETPLAY_MAX_SLOTS. Anything above 2 needs
     * a multitap configured on both peers, and every peer must agree: the
     * seat count is part of the settled session requirement
     * (recomp-ai-rules/NETPLAY.md §4). Env: SNES_NET_SLOTS. */
    int         slot_count;
    int         input_player;  /* 0/1 host device index; -1 = auto */
    int         input_delay;   /* frames; default 2 */
    /* Invent runway cap (P) for rollback; 0 = engine default. The lobby
     * publishes P = 4 + D (RecompLauncherCNetplayLaunch.input_prediction).
     * Env: SNES_RB_PREDICTION overrides. */
    int         input_prediction;
    /* Session mode: 1 = rollback (the framework default — recomp-ui's lobby
     * settles it room-wide and defaults ON), 0 = delay-sync. Builds without
     * the rollback host ignore it and run delay-sync. SNES_NET_MODE
     * overrides both ways ("rollback"/"rb" or "delay"). */
    int         rollback;
    uint32_t    session_id;
    char        bind_hostport[64];
    char        peer_hostport[64];
    /* 0 = auto (private/loopback peer → LAN, else ICE when lobby+ICE built),
     * 1 = force ICE, 2 = force LAN. Env SNES_NET_TRANSPORT=lan|ice overrides. */
    int         transport;
    /* Host match_caps / UI: force ICE typ relay (TURN). Env SNES_NET_FORCE_TURN
     * still overrides when set. */
    int         force_turn;
    /* 1 = lobby-server UDP input relay (match_caps.force_input_relay). */
    int         force_input_relay;
} SnesNetplayConfig;

void snes_netplay_config_defaults(SnesNetplayConfig *cfg);
void snes_netplay_apply_env(SnesNetplayConfig *cfg);

/* Optional game-specific deterministic state carried in pad bytes 2..3.
 * Games that need this (for example, for an RNG seed) register both callbacks
 * before starting netplay. With no callbacks, these bytes stay zero and the
 * generic runtime never touches game WRAM. */
typedef void (*SnesNetplayCaptureSyncBytes)(uint8_t out[2]);
typedef void (*SnesNetplayApplySyncBytes)(const uint8_t in[2]);
void snes_netplay_set_sync_byte_hooks(SnesNetplayCaptureSyncBytes capture,
                                      SnesNetplayApplySyncBytes apply);

int  snes_netplay_active(void);
/*
 * 1 while this session's admit path is ROLLBACK rather than delay-sync.
 *
 * Host scaffolding that reasons about input runway must branch on this: the
 * delay-sync starvation latch stalls when remote_lead falls below D, which is
 * the normal, intended condition under rollback (running ahead of confirmed
 * remote input is the whole mechanism). Leaving that latch armed throttles
 * rollback back into lockstep.
 */
int  snes_netplay_rollback_active(void);
int  snes_netplay_is_running(void);
/* "ice", "lan", or "none"; useful for user-facing connection diagnostics. */
const char *snes_netplay_transport_name(void);
/* 1 when ICE transport reached FAILED (STUN/TURN path dead). */
int  snes_netplay_ice_failed(void);
int  snes_netplay_local_slot(void);
/* Resolved host device index (0/1) used for local capture. */
int  snes_netplay_input_player(void);
uint32_t snes_netplay_sim_tick(void);
/* Admitted RtlRunFrame + finish_frame count for this session (0 if inactive). */
uint32_t snes_netplay_frames_finished(void);

int  snes_netplay_start(const SnesNetplayConfig *cfg);
void snes_netplay_shutdown(void);

/*
 * Connect-wait clock (session-scoped). Reset on start / shutdown so rematch
 * after Escape / soft-return cannot inherit a stale 30s timer from a prior
 * wait. Used by snes_host_barrier_admit; games should not keep their own.
 *
 * snes_netplay_connect_timed_out: while active and transport not running,
 * starts/continues the wait; returns 1 when timeout_ms elapsed (0 disables).
 * Clears automatically once snes_netplay_is_running() is true.
 */
void snes_netplay_connect_wait_reset(void);
int  snes_netplay_connect_timed_out(uint32_t timeout_ms);

/* Stage local pad bits (12 SNES buttons) for the current sim tick. */
void snes_netplay_stage_local(uint16_t buttons);

int  snes_netplay_needs_local_sample(void);
int  snes_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash);
int  snes_netplay_peer_disconnected(uint32_t timeout_ms);

/*
 * Ingress / lobby / INPUT retransmit without try_admit. Used by the host
 * starvation latch while waiting for remote runway to refill.
 */
void snes_netplay_pump(void);

/*
 * Pump + try_admit. On success, published pads are ready via
 * snes_netplay_published_inputs(). Returns 1 if admitted, 0 if stall.
 * Also applies the optional slot-0 game sync bytes before simulation.
 */
int  snes_netplay_poll_admit(void);

/* Call after RtlRunFrame for an admitted tick. */
void snes_netplay_finish_frame(void);

/* highest_remote_wire - sim_tick (0 if inactive; can be negative). */
int  snes_netplay_remote_lead(void);
/* Session input delay frames (default 2 when inactive). */
int  snes_netplay_input_delay(void);

/* Re-apply the last slot-0 game sync bytes (normally done inside poll_admit). */
void snes_netplay_apply_host_sync(void);

/* P1 | (P2<<12) button bits from the last successful publish (0 if none).
 * Seats 2..7 do not fit this word and are not carried by it: publish pushes
 * them straight into the runtime through RtlSetPadState, the same door a
 * local multitap game uses, so the game's frame loop is identical either
 * way. */
uint32_t snes_netplay_published_inputs(void);

/* Seat count in the running session (2 when inactive). */
int snes_netplay_slot_count(void);

/* Both slots plugged: (3u << 30) for RtlRunFrame active-controller bits. */
uint32_t snes_netplay_active_mask(void);

/*
 * Soft-return to the MotK lobby room after a match (peer BYE / ESC / window
 * close). Host sets the flag, tears down the game session, and re-opens the
 * launcher with resume_netplay_room.
 */
void snes_netplay_request_return_to_lobby(void);
int  snes_netplay_return_to_lobby_requested(void);
void snes_netplay_clear_return_to_lobby(void);

/* Host-only savestate sync (chunked over recomp-net). Load uses hash-probe
 * first (skip transfer when guest already has the blob), then a short ready
 * rendezvous before hard_resync. SAVE still ships async after host write.
 * Guests use saves/netplay/ so personal saves/ is never overwritten.
 * Returns 1 if netplay handled the request, 0 if offline — caller may RtlSaveLoad. */
int  snes_netplay_is_host(void);
int  snes_netplay_request_save(int slot);
int  snes_netplay_request_load(int slot);

/* 1 while a save/load/SRAM probe or transfer is in flight (app + library).
 * Host barrier must not count these stalls toward delay_sync_starvation. */
int  snes_netplay_state_barrier(void);

/*
 * Netplay diagnostics JSONL dump (saves/netplay/net_diag_slot{N}.jsonl).
 * Enabled when SNES_NET_DIAG=1 (or non-empty / non-zero). Optional
 * SNES_NET_DIAG_HZ (default 2, clamp 1..30) controls sample rate.
 * First line is a match summary; samples follow. Safe to call every
 * poll_admit / frame; rate-limited internally.
 */
void snes_netplay_diag_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* SNES_NETPLAY_H */

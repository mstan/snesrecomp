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
 *   [2..3] host RNG seed — DP $1A/$1B (Metal Warriors; copied from host slot 0
 *          into WRAM on every admitted tick so scramble / new-battlefield
 *          rolls match). Guest pad bytes are ignored for seed apply.
 *
 * Transport:
 *   LAN  — rnet_session_start_lan(bind, peer)
 *   ICE  — rnet_session_start_ice + lobby WS signal relay (SNES_HAS_LOBBY_CLIENT)
 */

#define SNES_NETPLAY_PAD_BYTES 4

typedef struct SnesNetplayConfig {
    int         enabled;
    int         local_slot;    /* 0 or 1 — lobby / wire slot */
    int         input_player;  /* 0/1 host device index; -1 = auto */
    int         input_delay;   /* frames; default 2 */
    uint32_t    session_id;
    char        bind_hostport[64];
    char        peer_hostport[64];
    /* 0 = auto (private/loopback peer → LAN, else ICE when lobby+ICE built),
     * 1 = force ICE, 2 = force LAN. Env SNES_NET_TRANSPORT=lan|ice overrides. */
    int         transport;
} SnesNetplayConfig;

void snes_netplay_config_defaults(SnesNetplayConfig *cfg);
void snes_netplay_apply_env(SnesNetplayConfig *cfg);

int  snes_netplay_active(void);
int  snes_netplay_is_running(void);
int  snes_netplay_local_slot(void);
/* Resolved host device index (0/1) used for local capture. */
int  snes_netplay_input_player(void);
uint32_t snes_netplay_sim_tick(void);

int  snes_netplay_start(const SnesNetplayConfig *cfg);
void snes_netplay_shutdown(void);

/* Stage local pad bits (12 SNES buttons) for the current sim tick. */
void snes_netplay_stage_local(uint16_t buttons);

int  snes_netplay_needs_local_sample(void);
int  snes_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash);
int  snes_netplay_peer_disconnected(uint32_t timeout_ms);

/*
 * Pump + try_admit. On success, published pads are ready via
 * snes_netplay_published_inputs(). Returns 1 if admitted, 0 if stall.
 * Also applies host RNG seed ($1A/$1B) from slot-0 pad bytes before sim.
 */
int  snes_netplay_poll_admit(void);

/* Call after RtlRunFrame for an admitted tick. */
void snes_netplay_finish_frame(void);

/* Re-apply host RNG seed from the last admit (normally done inside poll_admit). */
void snes_netplay_apply_host_rng(void);

/* P1 | (P2<<12) button bits from the last successful publish (0 if none). */
uint32_t snes_netplay_published_inputs(void);

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

/* Host-only savestate sync (chunked over recomp-net). Host applies/writes
 * immediately; guest catch-up is async (load/SRAM stall admit until applied).
 * Guests use saves/netplay/ so personal saves/ is never overwritten.
 * Returns 1 if netplay handled the request, 0 if offline — caller may RtlSaveLoad. */
int  snes_netplay_is_host(void);
int  snes_netplay_request_save(int slot);
int  snes_netplay_request_load(int slot);

#ifdef __cplusplus
}
#endif

#endif /* SNES_NETPLAY_H */

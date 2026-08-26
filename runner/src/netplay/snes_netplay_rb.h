#ifndef SNES_NETPLAY_RB_H
#define SNES_NETPLAY_RB_H

/*
 * SNES rollback host (SNES_NET_MODE=rollback).
 *
 * Layering, matching lib/retcomm-rbengine/docs/host_integration.md:
 *
 *   snes_netplay (facade, mode gate)
 *     ├── snes_netplay_rb        this file — snapshots, digests, resim, episode wire
 *     ├── retcomm-rbengine       invent policy, input history, hash_confirm, snap ring
 *     └── recomp-net             RNetSession tips + RNetRbSession episode FSM
 *
 * The delay-sync path never calls anything here, and rollback never changes
 * how delay-sync admits — `SNES_NET_MODE=delay` (or a lobby peer that does not
 * advertise rollback) keeps the shipped behaviour exactly.
 *
 * What SNES makes easy, relative to the PSX host this is modelled on: the
 * guest frame is a plain `RtlRunFrame` call that returns, so every snapshot
 * and every rewind happens at a frame boundary with no host call stack to
 * unwind. There is no basic-block-edge poll, no longjmp resume, and no media
 * lockstep — the entire MotK FMV apparatus is simply absent.
 *
 * What SNES makes hard is documented at the two subtractions in
 * common_rtl.h (rollback snapshots carry sim residue the guest blob omits)
 * and snes_state_digest.h (the audio output ring is host state and never
 * enters a digest).
 *
 * Single-threaded: the guest thread owns the session, the rings, and the
 * episode. The SDL audio thread touches only the DSP output ring, under
 * RtlApuLock.
 */

#include <stdint.h>

#include "recomp_net/recomp_net.h"
#include "retcomm_rbengine/retcomm_rbengine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnesNetplayRbBindings {
    RNetSession **session;   /* live pointer; facade may repoint on restart */
    int          *local_slot;
    int          *slot_count;
    int          *input_delay;
    int           force_turn;
    /* Publish resolved pads for a tick into the facade, so the game's
     * snes_netplay_published_inputs() sees them for the live frame. */
    void (*publish)(uint32_t tick, const uint16_t *buttons, int slots);
    /* Optional game sync bytes riding pad bytes 2..3. Capture is NOT a
     * binding: slot 0's bytes are captured by the facade's wire sample
     * callback and reach both peers over the wire, so both apply the same
     * ones. Capturing them again locally would apply bytes the peer never
     * saw. */
    void (*apply_sync_bytes)(const uint8_t in[2]);
} SnesNetplayRbBindings;

/* 1 when the build/env selected rollback for this session. */
int  snes_netplay_rb_enabled(void);

void snes_netplay_rb_bind(const SnesNetplayRbBindings *b);
int  snes_netplay_rb_start(void);
void snes_netplay_rb_shutdown(void);

/*
 * Live admit. Pumps the RB wire, prepares the local tip, resolves each seat's
 * row for the tick (wire row, or hold-last invent inside the prediction cap),
 * publishes, and returns 1 when the caller should run one guest frame.
 * 0 = stall; the host presents its held frame and retries.
 */
int  snes_netplay_rb_poll_admit(void);

/* Call after RtlRunFrame for an admitted tick: snapshot if due, digest,
 * emit FRAME_COMMIT, advance the session clock, and service any episode
 * that the newly arrived wire opened. */
void snes_netplay_rb_finish_frame(void);

/* Local pad for the tick being staged (12 SNES button bits, active high). */
void snes_netplay_rb_stage_local(uint16_t buttons);

/* Diagnostics for the facade's net_diag / OSD. */
uint32_t snes_netplay_rb_sim_tick(void);
uint32_t snes_netplay_rb_episode_count(void);
uint32_t snes_netplay_rb_invent_count(void);
uint32_t snes_netplay_rb_promote_count(void);
uint64_t snes_netplay_rb_resim_ticks(void);
uint32_t snes_netplay_rb_desync_count(void);
int      snes_netplay_rb_episode_active(void);
const char *snes_netplay_rb_stall_tag(void);
/* Last observed digest fork: 1 if one has happened, with the partition name
 * and tick. Partition naming comes from snes_state_digest_part_name. */
int  snes_netplay_rb_last_fork(uint32_t *tick, const char **partition);

#ifdef __cplusplus
}
#endif

#endif /* SNES_NETPLAY_RB_H */

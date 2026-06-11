#ifndef SNESRECOMP_PPU_DMA_TRACE_H
#define SNESRECOMP_PPU_DMA_TRACE_H

#include <stdint.h>
#include <stdio.h>

/* ── Always-on PPU + DMA observability ring ───────────────────────────────
 *
 * Compiled into EVERY build (Release included). Records, continuously:
 *   - every A->B (and B->A) DMA transfer triggered via $420B — the
 *     VRAM/CGRAM/OAM graphics uploads, captured with their source
 *     bank:addr, B-bus destination register, and size; and
 *   - a per-frame snapshot of the live PPU state (forced-blank/brightness,
 *     main/sub screen-enable, BG mode, non-zero CGRAM/VRAM counts).
 *
 * This exists so "is forced-blank stuck on / is the palette black / is VRAM
 * being populated / where is a malfunctioning DMA sourcing from" is answered
 * from RECORDED HISTORY, not by arming a trace at probe time. The rings
 * always record; env vars only control optional streaming verbosity, and a
 * targeted dump (post-mortem / on exit) pulls the slice of interest.
 *
 *   SNESRECOMP_PPU_HEARTBEAT=<N>  stream a per-frame PPU summary to stderr
 *                                 every N frames (0 = off, the default).
 *   SNESRECOMP_DMA_LOG=1          stream every recorded DMA to stderr.
 *   SNESRECOMP_HEARTBEAT_WRAM=<a1,a2,...>
 *                                 up to PPUDMA_WRAM_PROBE_MAX hex WRAM
 *                                 offsets; each is read as a 16-bit word
 *                                 into EVERY per-frame snapshot from frame
 *                                 0, appended to the heartbeat line as
 *                                 [addr]=value, and included in the ring
 *                                 dump. The probe set is configuration;
 *                                 the capture is always-on history.
 */

#define PPUDMA_WRAM_PROBE_MAX 8

/* Record one channel's config at $420B (MDMAEN) trigger time, captured
 * BEFORE the transfer consumes aAdr/size. fromB != 0 is a B->A transfer;
 * 0 is the common A->B (memory -> PPU register) case. */
void ppudma_record_dma(int channel, int fromB, uint8_t aBank, uint16_t aAdr,
                       uint8_t bAdr, uint16_t size);

/* Snapshot the live PPU (reads g_ppu) once per frame. `frame` is the host
 * frame counter. Resets the per-frame DMA tally. */
void ppudma_frame_snapshot(int frame);

/* Serialize both rings as JSON object members (trailing comma, no enclosing
 * braces) for the post-mortem report. */
void ppudma_dump_json(FILE *f);

#endif /* SNESRECOMP_PPU_DMA_TRACE_H */

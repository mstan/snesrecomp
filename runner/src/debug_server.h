#ifndef DEBUG_SERVER_H
#define DEBUG_SERVER_H

#include <stdint.h>

// Initialize the debug TCP server on the given port. Non-blocking.
// Returns 0 on success, -1 on failure.
int debug_server_init(int port);

// Poll for commands from a connected client. Non-blocking.
// Call this once per frame (or at any safe pause point).
// If a client sends "pause", this will block until "continue" is received.
void debug_server_poll(void);

// Shutdown the server.
void debug_server_shutdown(void);

// Start in paused state (game waits for 'step' or 'continue' command).
void debug_server_start_paused(void);

// Block until unpaused. Call this once per frame in the main game loop.
void debug_server_wait_if_paused(void);

// Returns slot number (0-9) if a loadstate was requested via TCP, or -1 if none.
// Consumes the request (only returns it once).
int debug_server_consume_loadstate(void);

// Snapshot the current frame's state (CPU/PPU/DMA/WRAM/VRAM/CGRAM/OAM)
// into the history ring buffer. Called once per frame from common_cpu_infra.
// Cross-runtime divergence comparison is done by an external tool that
// reads from both runtimes' TCP servers — not in here.
void debug_server_record_frame(int frame);

// Set pointers the server needs to inspect game state.
void debug_server_set_ram(uint8_t *ram, uint32_t ram_size);
void debug_server_set_frame_counter(int *counter);
void debug_server_set_snapshots(void *mine, void *theirs, void *before);

// Per-dispatch tracing: call from Process*Objects dispatch functions.
// trace_before saves g_ram key bytes; trace_after captures post-call state.
void debug_dispatch_trace_before(int obj_number);
void debug_dispatch_trace_after(void);

// Map16 write instrumentation — called from IndirWriteByte when writing to Map16 range.
void debug_server_log_map16_write(uint16_t ram_addr, uint8_t value,
                                   uint16_t ptr_lo, uint8_t ptr_bank,
                                   uint16_t offset);

// MMIO register-write trace. Call from snes_write paths after the write
// completes. Captures entries for addresses in [s_reg_trace_lo, s_reg_trace_hi).
// Disabled by default; enable via the "trace_reg <lo> <hi>" TCP command.
void debug_server_on_reg_write(uint16_t adr, uint8_t val);

// VRAM-word write trace. Call from every path that mutates ppu->vram —
// ppu_write $2118/$2119 cases, WriteVramWord, and any hand-written code
// that writes g_ppu->vram directly (e.g. LoadStripeImage_UploadToVRAM).
// Disabled by default; enable via "trace_vram <lo> <hi>" (word addresses).
void debug_server_on_vram_write(uint16_t adr_word, uint16_t value);

#endif

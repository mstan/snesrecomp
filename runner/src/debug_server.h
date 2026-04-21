#ifndef DEBUG_SERVER_H
#define DEBUG_SERVER_H

#include <stdint.h>

// Reverse debugger build flag. See snesrecomp/REVERSE_DEBUGGER.md.
// When 0: the generator emits raw `g_ram[x] = val` stores exactly as it
// always has; no hooks are compiled; zero runtime cost. When 1: the
// generator must be rerun with `recomp.py --reverse-debug` so every WRAM
// store becomes a call to rdb_store8 / rdb_store16, which records into
// an in-memory ring for TCP readout. Flip by defining to 1 on the
// compiler command line (or editing this file) AND regenerating all
// src/gen/smw_*_gen.c. Mixing a debug build with non-debug generated C
// is a no-op silently; mixing a non-debug build with debug generated C
// fails to link.
#ifndef SNESRECOMP_REVERSE_DEBUG
#define SNESRECOMP_REVERSE_DEBUG 1
#endif

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

// MMIO register-write trace. Call from snes_write paths after the write
// completes. Captures entries for addresses in [s_reg_trace_lo, s_reg_trace_hi).
// Disabled by default; enable via the "trace_reg <lo> <hi>" TCP command.
void debug_server_on_reg_write(uint16_t adr, uint8_t val);

// VRAM-word write trace. Call from every path that mutates ppu->vram —
// ppu_write $2118/$2119 cases, WriteVramWord, and any hand-written code
// that writes g_ppu->vram directly (e.g. LoadStripeImage_UploadToVRAM).
// Disabled by default; enable via "trace_vram <lo> <hi>" (word addresses).
void debug_server_on_vram_write(uint16_t adr_word, uint16_t value);

#if SNESRECOMP_REVERSE_DEBUG
// Tier-1 reverse-debugger WRAM write hooks. Called from every WRAM store
// in the recomp-generated C when the generator was invoked with
// --reverse-debug. Never called from a non-debug generation; these
// functions do not exist when SNESRECOMP_REVERSE_DEBUG == 0.
//
// Address is uint32_t (not uint16_t!) because the generated C writes to
// bank $7F via `g_ram[0x10000 + off]` and `*(uint16*)(g_ram + 0x18000)`,
// which exceed uint16_t range. A tighter cast here silently wraps
// bank-$7F writes into bank $7E and causes cross-bank state corruption
// — classic latent 128KB-WRAM-over-16-bit-SNES-semantics bug.
extern uint8_t g_ram[];
void debug_on_wram_write_byte(uint32_t addr, uint8_t val);
void debug_on_wram_write_word(uint32_t addr, uint16_t val);
static inline void rdb_store8(uint32_t addr, uint8_t val) {
    g_ram[addr] = val;
    debug_on_wram_write_byte(addr, val);
}
static inline void rdb_store16(uint32_t addr, uint16_t val) {
    *(uint16_t *)(g_ram + addr) = val;
    debug_on_wram_write_word(addr, val);
}
#define RDB_STORE8(addr, val)  rdb_store8((uint32_t)(addr), (uint8_t)(val))
#define RDB_STORE16(addr, val) rdb_store16((uint32_t)(addr), (uint16_t)(val))
#endif

#endif

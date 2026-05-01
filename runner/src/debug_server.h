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

// VRAM byte-write trace. Call from every path that mutates ppu->vram —
// ppu_write $2118/$2119 cases, WriteVramWord, and any hand-written code
// that writes g_ppu->vram directly (e.g. LoadStripeImage_UploadToVRAM).
// byte_addr is the byte address into the 64KB VRAM (0..$FFFF); $2118
// targets even bytes, $2119 odd. Default-armed for the full byte range
// in Oracle builds so probes can query the ring backward in history.
void debug_server_on_vram_write(uint32_t byte_addr, uint8_t value);

// Oracle-side VRAM byte-write trace. Same shape as the recomp ring
// above but written by the snes9x trampoline; no recomp-side
// attribution because snes9x is the reference. cmd_vram_write_diff
// walks the two rings forward in lockstep to identify the first
// divergent (byte_addr, value) pair across the streams.
void debug_server_on_oracle_vram_write(uint32_t byte_addr, uint8_t value);

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
// Tier-1 write hooks take both the OLD value (what was in WRAM before the
// store) and the NEW value. Capturing both lets `get_wram_trace` emit
// old/new for every recorded entry — previously only `new` was kept,
// which left the trace unable to answer "what was the value before this
// function wrote?" without a separate per-frame sampling workaround.
// The inline helpers read `g_ram[addr]` before the store to capture old,
// then store, then call the hook. Added 2026-04-23.
void debug_on_wram_write_byte(uint32_t addr, uint8_t old_val, uint8_t new_val);
void debug_on_wram_write_word(uint32_t addr, uint16_t old_val, uint16_t new_val);
static inline void rdb_store8(uint32_t addr, uint8_t val) {
    uint8_t old_val = g_ram[addr];
    g_ram[addr] = val;
    debug_on_wram_write_byte(addr, old_val, val);
}
static inline void rdb_store16(uint32_t addr, uint16_t val) {
    uint16_t old_val = *(uint16_t *)(g_ram + addr);
    *(uint16_t *)(g_ram + addr) = val;
    debug_on_wram_write_word(addr, old_val, val);
}
#define RDB_STORE8(addr, val)  rdb_store8((uint32_t)(addr), (uint8_t)(val))
#define RDB_STORE16(addr, val) rdb_store16((uint32_t)(addr), (uint16_t)(val))

// Tier-2 block-level execution hook. Emitted by --reverse-debug at every
// basic-block boundary (function entry + every label). When trace_blocks
// is active, records (frame, depth, pc, func, a, x, y) so a probe can
// replay the exact intra-function execution path AND see the recomp
// register tracker's value for A/X/Y at each block entry. The register
// values are passed in by the generator from its abstract-register
// state; if a register is unknown at the emission point, the generator
// passes RDB_REG_UNKNOWN (0xFFFFFFFF).
//
// When inactive, the call returns immediately — but the call itself is
// unconditional, so non-debug builds need recomp.py NOT to emit
// RDB_BLOCK_HOOK at all (gated by the --reverse-debug flag at gen
// time, same as RDB_STORE*).
#define RDB_REG_UNKNOWN 0xFFFFFFFFu
void debug_on_block_enter(uint32_t pc, uint32_t a, uint32_t x, uint32_t y);
#define RDB_BLOCK_HOOK(pc, a, x, y) \
    debug_on_block_enter((uint32_t)(pc), (uint32_t)(a), (uint32_t)(x), (uint32_t)(y))

// Tier-4 reads: WRAM read trace. Every g_ram[X] read in --reverse-debug
// generated code routes through RDB_LOAD8 / RDB_LOAD16, which in
// non-instrumented builds expand to plain memory accesses (zero
// runtime cost, byte-identical Release|x64 binary). In Oracle builds
// the macros call debug_wram_read_byte/word which records (frame,
// block_idx, addr, val, func, parent) into a ring filtered by armed
// trace ranges. Read-trace is the symmetric counterpart of Tier 1's
// write trace; needed to answer "what value was in $XX when this
// branch evaluated".
//
// Gated identically to RDB_INSN_HOOK: ENABLE_ORACLE_BACKEND auto-
// defines SNESRECOMP_TIER4 (+ implicitly Tier-4-reads).
#if defined(SNESRECOMP_TIER4) || defined(ENABLE_ORACLE_BACKEND)
uint8_t  debug_wram_read_byte(uint32_t addr);
uint16_t debug_wram_read_word(uint32_t addr);
#  define RDB_LOAD8(addr)  debug_wram_read_byte((uint32_t)(addr))
#  define RDB_LOAD16(addr) debug_wram_read_word((uint32_t)(addr))
#else
#  define RDB_LOAD8(addr)  (g_ram[(addr)])
#  define RDB_LOAD16(addr) (*(uint16_t *)(g_ram + (addr)))
#endif

// Tier-4 per-instruction hook. Emitted by recomp.py --reverse-debug
// at the top of every C-equivalent of a 65816 instruction. The runtime
// ring captures (frame, block_idx, pc, mnemonic_id), giving full
// instruction-level execution trace for divergence analysis at the
// finest possible granularity short of CPU-cycle accounting. Mnemonic
// is passed as a small int (mnemonic table index) to keep entries
// compact; the table is published over TCP via get_insn_mnemonics.
//
// Gated on SNESRECOMP_TIER4 (separate from SNESRECOMP_REVERSE_DEBUG)
// so that Oracle builds can opt in without bloating production
// Release|x64 binaries. The Oracle MSBuild config defines
// ENABLE_ORACLE_BACKEND which auto-defines SNESRECOMP_TIER4 below;
// Release|x64 leaves both undefined and the macro expands to a void
// no-op, costing zero bytes in the binary regardless of how many
// times the generator emitted it.
#if defined(SNESRECOMP_TIER4) || defined(ENABLE_ORACLE_BACKEND)
#  ifndef SNESRECOMP_TIER4
#    define SNESRECOMP_TIER4 1
#  endif
// Tier-4 extended insn-hook: carries the full recomp symbolic tracker
// state. a/x/y/b are RDB_REG_UNKNOWN when recomp couldn't statically
// pin them. mx_flags packs m (bit 0) and x (bit 1) bits per ROM
// insn (e_flag is always 0 — recomp doesn't model emulation mode).
void debug_on_insn_enter(uint32_t pc, uint32_t mnem_id,
                         uint32_t a, uint32_t x, uint32_t y, uint32_t b,
                         uint32_t mx_flags);
#  define RDB_INSN_HOOK(pc, mnem_id, a, x, y, b, mx) \
      debug_on_insn_enter((uint32_t)(pc), (uint32_t)(mnem_id), \
                          (uint32_t)(a), (uint32_t)(x), (uint32_t)(y), \
                          (uint32_t)(b), (uint32_t)(mx))
#else
#  define RDB_INSN_HOOK(pc, mnem_id, a, x, y, b, mx) ((void)0)
#endif
#endif

#endif

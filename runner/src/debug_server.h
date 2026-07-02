#ifndef DEBUG_SERVER_H
#define DEBUG_SERVER_H

#include <stdint.h>

// SNESRECOMP_TRACE gates the debug-server runtime. When 0 (Production
// builds), every public debug_server_* function declaration below is
// replaced with a `static inline` no-op stub so call sites in main.c
// and common_rtl.c compile to nothing. debug_server.c can then be
// excluded from the build, dropping ~140 MB of static buffers + heavy
// snprintf code from the link.
#ifndef SNESRECOMP_TRACE
#define SNESRECOMP_TRACE 0
#endif

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

#if SNESRECOMP_TRACE

typedef struct CpuState CpuState;

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

// Called from cpu_trace_block() in normal trace builds. Lets the TCP
// breakpoint commands park on generated basic-block PCs without requiring
// reverse-debug WRAM-store instrumentation.
void debug_server_on_trace_block(CpuState *cpu, uint32_t pc24);

// Returns slot number (0-9) if a loadstate was requested via TCP, or -1 if none.
// Consumes the request (only returns it once).
int debug_server_consume_loadstate(void);

// Same handoff for a TCP-requested savestate (savestate <slot> command).
int debug_server_consume_savestate(void);

// TCP controller override. The returned input mask uses the runner's
// controller layout: player 1 in bits 0..11, player 2 in bits 12..23.
// The active mask is already shifted into the high controller-present bits.
uint32_t debug_server_get_controller_inputs(void);
uint32_t debug_server_get_controller_active_mask(void);

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

// Always-on OAM observability. on_oam_write records every word landed in
// ppu->oam (is_high=0; index = word index 0..255) or byte landed in
// ppu->highOam (is_high=1; index = byte index 0..31) — call from ppu_write's
// OAMDATA ($2104) case, which also carries OAM DMA. on_oam_render snapshots
// the 128-slot OAM digest the scanline renderer is about to consume — call at
// the top of ppu_runLine(line 0). Both stamp a shared monotonic seq so writes
// and render-reads form one total order. Queried via oam_write_get /
// oam_render_get / oam_state.
void debug_server_on_oam_write(int is_high, uint16_t index, uint16_t value);
void debug_server_on_oam_render(void);

// Per-function profiling — called from RecompStackPush and at the
// watchdog trip point in common_cpu_infra.c. Records a histogram of
// function names and the frame they were latched at.
void debug_server_profile_push(const char *name);
void debug_server_profile_latch(int frame_num);

#else  /* SNESRECOMP_TRACE = 0 — Production: no-op every call. */

static inline int debug_server_init(int port) { (void)port; return 0; }
static inline void debug_server_poll(void) { }
static inline void debug_server_shutdown(void) { }
static inline void debug_server_start_paused(void) { }
static inline void debug_server_wait_if_paused(void) { }
static inline void debug_server_on_trace_block(void *cpu, uint32_t pc24) { (void)cpu; (void)pc24; }
static inline int debug_server_consume_loadstate(void) { return -1; }
static inline int debug_server_consume_savestate(void) { return -1; }
static inline uint32_t debug_server_get_controller_inputs(void) { return 0; }
static inline uint32_t debug_server_get_controller_active_mask(void) { return 0; }
static inline void debug_server_record_frame(int frame) { (void)frame; }
static inline void debug_server_set_ram(uint8_t *ram, uint32_t ram_size) { (void)ram; (void)ram_size; }
static inline void debug_server_on_reg_write(uint16_t adr, uint8_t val) { (void)adr; (void)val; }
static inline void debug_server_on_vram_write(uint32_t byte_addr, uint8_t value) { (void)byte_addr; (void)value; }
static inline void debug_server_on_oracle_vram_write(uint32_t byte_addr, uint8_t value) { (void)byte_addr; (void)value; }
static inline void debug_server_on_oam_write(int is_high, uint16_t index, uint16_t value) { (void)is_high; (void)index; (void)value; }
static inline void debug_server_on_oam_render(void) { }
static inline void debug_server_profile_push(const char *name) { (void)name; }
static inline void debug_server_profile_latch(int frame_num) { (void)frame_num; }

#endif  /* SNESRECOMP_TRACE */

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

// WRAM read trace macros. Every g_ram[X] read in --reverse-debug
// generated code routes through RDB_LOAD8 / RDB_LOAD16, which expand to
// plain memory accesses (zero runtime cost, byte-identical Release|x64
// binary).
#define RDB_LOAD8(addr)  (g_ram[(addr)])
#define RDB_LOAD16(addr) (*(uint16_t *)(g_ram + (addr)))

// Per-instruction hook. Emitted by recomp.py --reverse-debug at the top
// of every C-equivalent of a 65816 instruction. Expands to a void no-op,
// costing zero bytes in the binary regardless of how many times the
// generator emitted it.
#define RDB_INSN_HOOK(pc, mnem_id, a, x, y, b, mx) ((void)0)
#endif

#endif

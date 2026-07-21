/*
 * cpu_state.c — implementations for the v2 runtime CpuState.
 *
 * Address routing for the byte/word memory helpers:
 *   $00-$3F:0000-$1FFF / $7E:0000-$1FFF       -> g_ram (low WRAM mirror)
 *   $7E:0000-$FFFF                            -> g_ram[0x00000-0x0FFFF]
 *   $7F:0000-$FFFF                            -> g_ram[0x10000-0x1FFFF]
 *   $00-$3F:2000-$5FFF / $80-$BF:2000-$5FFF   -> SNES hardware regs
 *                                                (PPU, APU, joypad, DMA)
 *                                                routed via WriteReg/ReadReg
 *   $70-$7D:0000-$7FFF / $F0-$FD:0000-$7FFF   -> LoROM battery SRAM
 *                                                (cart->ram via g_sram)
 *   $00-$3F:6000-$7FFF / $80-$BF:6000-$7FFF   -> HiROM battery SRAM
 *                                                (cart->ram via g_sram)
 *   $00-$7D:8000-$FFFF / $80-$FF:8000-$FFFF   -> ROM (reads via RomPtr;
 *                                                writes are NOPs)
 *
 * The hardware-register routing is what unblocks boot: every PPU/APU/DMA
 * register write the recompiled code emits goes through WriteReg, so
 * INIDISP / NMITIMEN / OBSEL / DMA setup actually take effect. Without
 * it, $2100 stays at the snes9x default (forced-blank ON) and the
 * screen never lights up.
 *
 * The SRAM routing is what unblocks save/menu: every read against the
 * cart's battery RAM (SMW's VerifySaveFile, save data writes, password
 * tables, etc.) goes through g_sram so save data lives in cart->ram
 * instead of tripping RomPtr-invalid.
 */

#include "cpu_state.h"
#include "common_rtl.h"
#include "cpu_trace.h"
#include "snes/snes.h"
#include "snes/cart.h"

extern Snes *g_snes;

CpuState g_cpu;

/* ── Scoped write-log ring (dev, env-gated) ────────────────────────────────
 * Captures the exact guest write sequence (WRAM + hardware regs) performed
 * while a scope is armed. BOTH engines funnel here: the AOT body calls
 * cpu_write8/16 directly, and the interpreter's bridge_bus_write ultimately
 * calls cpu_write8/16 too — so a single ring captures either engine with no
 * per-engine instrumentation. Used to first-divergence-diff one function's
 * writes between AOT and interp (the interp is the oracle).
 *
 * Arming: RecompStackPush(name)/Pop wrap the AOT body (name-matched), and
 * interp_tier_dispatch_balanced wraps the interp run. A scope that produces
 * zero writes is NOT counted, so the empty AOT push-then-bounce that precedes
 * a denied interp run does not consume a call slot — call indices stay aligned
 * between an all-AOT run and a denied(interp) run.
 *
 * Env: SNESRECOMP_WLOG=<path> enables; SNESRECOMP_WLOG_MAXCALLS caps captured
 * calls (default 3). Zero cost when SNESRECOMP_WLOG is unset. */
int  g_wlog_active = 0;              /* fast-path gate read in cpu_write8/16 */
static FILE *g_wlog_fp = NULL;
static int   g_wlog_inited = 0;
static int   g_wlog_calls = 0;
static int   g_wlog_max_calls = 3;
static int   g_wlog_scope_open = 0;
static int   g_wlog_scope_wrote = 0;
static char  g_wlog_tag[80];
static uint16 g_wlog_eA, g_wlog_eX, g_wlog_eY;
static uint8  g_wlog_eM, g_wlog_eXf, g_wlog_eDB, g_wlog_ePB;

static void wlog_lazy_init(void) {
    if (g_wlog_inited) return;
    g_wlog_inited = 1;
    const char *p = getenv("SNESRECOMP_WLOG");
    if (p && p[0]) g_wlog_fp = fopen(p, "w");
    const char *m = getenv("SNESRECOMP_WLOG_MAXCALLS");
    if (m && m[0]) g_wlog_max_calls = atoi(m);
}

void wlog_scope_enter(const char *tag) {
    wlog_lazy_init();
    if (!g_wlog_fp) return;
    if (g_wlog_calls >= g_wlog_max_calls) { g_wlog_active = 0; return; }
    /* snapshot entry register state from the single global CpuState */
    g_wlog_eA = g_cpu.A; g_wlog_eX = g_cpu.X; g_wlog_eY = g_cpu.Y;
    g_wlog_eM = (uint8)(g_cpu.m_flag & 1); g_wlog_eXf = (uint8)(g_cpu.x_flag & 1);
    g_wlog_eDB = g_cpu.DB; g_wlog_ePB = g_cpu.PB;
    size_t n = 0; if (tag) { while (tag[n] && n < sizeof(g_wlog_tag) - 1) { g_wlog_tag[n] = tag[n]; n++; } }
    g_wlog_tag[n] = 0;
    g_wlog_scope_open = 1; g_wlog_scope_wrote = 0; g_wlog_active = 1;
}

void wlog_scope_exit(void) {
    if (!g_wlog_scope_open) return;
    g_wlog_scope_open = 0; g_wlog_active = 0;
    if (g_wlog_scope_wrote) {
        if (g_wlog_fp)
            fprintf(g_wlog_fp, "# EXIT %d A=%04X X=%04X Y=%04X M=%d Xf=%d DB=%02X\n",
                    g_wlog_calls, g_cpu.A, g_cpu.X, g_cpu.Y,
                    (int)(g_cpu.m_flag & 1), (int)(g_cpu.x_flag & 1), g_cpu.DB);
        g_wlog_calls++;
        if (g_wlog_fp) fflush(g_wlog_fp);
    }
}

/* ── Always-on address-range write logger ─────────────────────────────────
 * Independent of the scope ring above: logs EVERY write whose 16-bit address
 * falls in [lo,hi], tagged with frame + the current AOT function name. Both
 * engines funnel through cpu_write8/16, so an all-AOT run and an all-interp
 * (deny-all) run produce comparable streams; diffing them finds the first
 * divergent write (e.g. an APU command $2140-$2143) and, from the AOT stream's
 * function tag at that point, the culprit function. Env:
 * SNESRECOMP_WLOG_ADDR="LO:HI:PATH" (hex LO/HI). Zero cost when unset. */
static int    g_wlog_addr_inited = 0;
static FILE  *g_wlog_addr_fp = NULL;
static uint16 g_wlog_addr_lo = 0xFFFF, g_wlog_addr_hi = 0x0000;
static long   g_wlog_addr_n = 0, g_wlog_addr_cap = 2000000;
static int    g_wlog_addr_state = 0;

static void wlog_addr_lazy(void) {
    g_wlog_addr_inited = 1;
    const char *p = getenv("SNESRECOMP_WLOG_ADDR");
    if (!p || !p[0]) return;
    unsigned lo = 0, hi = 0; char path[512] = {0};
    /* "LO:HI:PATH" */
    if (sscanf(p, "%x:%x:%511[^\n]", &lo, &hi, path) >= 3 && path[0]) {
        g_wlog_addr_lo = (uint16)lo; g_wlog_addr_hi = (uint16)hi;
        g_wlog_addr_fp = fopen(path, "w");
        /* Unbuffered: writes to a narrow addr range are rare, and probes
         * commonly force-kill the process (no atexit flush) after a free-run
         * capture. On Windows/MSVCRT _IOLBF degrades to full buffering, so use
         * _IONBF to make the log durable across force-kill and readable while
         * the game is still running. */
        if (g_wlog_addr_fp) setvbuf(g_wlog_addr_fp, NULL, _IONBF, 0);
    }
    const char *c = getenv("SNESRECOMP_WLOG_ADDR_CAP");
    if (c && c[0]) g_wlog_addr_cap = strtol(c, NULL, 0);
    g_wlog_addr_state = getenv("SNESRECOMP_WLOG_STATE") != NULL;
}

static inline void wlog_addr_note(uint8 bank, uint16 addr, uint16 v, int width) {
    if (!g_wlog_addr_inited) wlog_addr_lazy();
    if (!g_wlog_addr_fp) return;
    if (addr < g_wlog_addr_lo || addr > g_wlog_addr_hi) return;
    if (g_wlog_addr_n++ >= g_wlog_addr_cap) return;
    extern int snes_frame_counter;
    extern const char *g_last_recomp_func;
    fprintf(g_wlog_addr_fp, "f%-6d %02X:%04X=%0*X w%d %s",
            snes_frame_counter, bank, addr, width * 2,
            (unsigned)(v & (width == 1 ? 0xFF : 0xFFFF)), width,
            g_last_recomp_func ? g_last_recomp_func : "?");
    if (g_wlog_addr_state)
        {
        extern uint32_t g_interp_wlog_pc24;
        fprintf(g_wlog_addr_fp,
                " A=%04X X=%04X Y=%04X S=%04X D=%04X DB=%02X M=%u Xf=%u"
                " IPC=%06X"
                " p34=%02X%04X p38=%04X p3C=%04X p3E=%04X"
                " p42=%02X p46=%02X p4A=%02X p4E=%02X"
                " p52=%02X p53=%02X p54=%04X p56=%02X p57=%02X",
                g_cpu.A, g_cpu.X, g_cpu.Y, g_cpu.S, g_cpu.D, g_cpu.DB,
                (unsigned)(g_cpu.m_flag & 1), (unsigned)(g_cpu.x_flag & 1),
                (unsigned)(g_interp_wlog_pc24 & 0xFFFFFFu),
                cpu_read8(&g_cpu, 0x00, (uint16)(g_cpu.D + 0x36)),
                cpu_read16(&g_cpu, 0x00, (uint16)(g_cpu.D + 0x34)),
                cpu_read16(&g_cpu, 0x00, (uint16)(g_cpu.D + 0x38)),
                cpu_read16(&g_cpu, 0x00, (uint16)(g_cpu.D + 0x3C)),
                cpu_read16(&g_cpu, 0x00, (uint16)(g_cpu.D + 0x3E)),
                cpu_read8(&g_cpu, 0x00, (uint16)(g_cpu.D + 0x42)),
                cpu_read8(&g_cpu, 0x00, (uint16)(g_cpu.D + 0x46)),
                cpu_read8(&g_cpu, 0x00, (uint16)(g_cpu.D + 0x4A)),
                cpu_read8(&g_cpu, 0x00, (uint16)(g_cpu.D + 0x4E)),
                cpu_read8(&g_cpu, 0x00, (uint16)(g_cpu.D + 0x52)),
                cpu_read8(&g_cpu, 0x00, (uint16)(g_cpu.D + 0x53)),
                cpu_read16(&g_cpu, 0x00, (uint16)(g_cpu.D + 0x54)),
                cpu_read8(&g_cpu, 0x00, (uint16)(g_cpu.D + 0x56)),
                cpu_read8(&g_cpu, 0x00, (uint16)(g_cpu.D + 0x57)));
        }
    fputc('\n', g_wlog_addr_fp);
}

static inline void wlog_note(uint8 bank, uint16 addr, uint16 v, int width) {
    if (!g_wlog_active || !g_wlog_fp) return;
    if (!g_wlog_scope_wrote) {
        g_wlog_scope_wrote = 1;
        fprintf(g_wlog_fp,
                "# CALL %d %s A=%04X X=%04X Y=%04X M=%d Xf=%d DB=%02X PB=%02X\n",
                g_wlog_calls, g_wlog_tag, g_wlog_eA, g_wlog_eX, g_wlog_eY,
                (int)g_wlog_eM, (int)g_wlog_eXf, g_wlog_eDB, g_wlog_ePB);
    }
    fprintf(g_wlog_fp, "%02X:%04X=%0*X w%d\n",
            bank, addr, width * 2, (unsigned)(v & (width == 1 ? 0xFF : 0xFFFF)), width);
}

/* Map a 24-bit logical address onto a g_ram offset. Returns -1 for
 * addresses that are NOT WRAM — the caller routes those to the HW-reg
 * helpers (WriteReg/ReadReg) or to ROM. */
static int cpu_ram_offset(uint8 bank, uint16 addr) {
    if (bank == 0x7E) return (int)addr;
    if (bank == 0x7F) return 0x10000 + (int)addr;
    if (addr < 0x2000 && (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        return (int)addr;
    }
    return -1;
}

/* True when (bank, addr) addresses an SNES hardware register that should
 * be routed through the framework's WriteReg/ReadReg dispatch. The HW
 * register window is $2000-$5FFF in low banks ($00-$3F, $80-$BF). */
static int is_hw_reg(uint8 bank, uint16 addr) {
    if (addr < 0x2000 || addr >= 0x6000) return 0;
    if (bank <= 0x3F) return 1;
    if (bank >= 0x80 && bank <= 0xBF) return 1;
    return 0;
}

/* Map a 24-bit logical address onto a g_sram offset for cart battery
 * RAM. Returns -1 if (bank, addr) is NOT SRAM. Mirrors snes9x's
 * cart_readLorom and cart_readHirom SRAM mappings so save-data
 * accesses route to cart->ram instead of falling through to RomPtr
 * (which would trip the RomPtr-invalid off-rails detector). */
static int cpu_sram_offset(uint8 bank, uint16 addr) {
    if (g_sram_size == 0 || g_sram == NULL) return -1;
    int cart_type = g_snes && g_snes->cart ? g_snes->cart->type : 0;
    /* LoROM SRAM: banks $70-$7D + $F0-$FD, addr $0000-$7FFF. */
    if (cart_type == CART_LOROM &&
        ((bank >= 0x70 && bank < 0x7E) || (bank >= 0xF0 && bank < 0xFE))
        && addr < 0x8000) {
        return (int)((((bank & 0xF) << 15) | addr) & (g_sram_size - 1));
    }
    /* HiROM SRAM: banks $00-$3F + $80-$BF, addr $6000-$7FFF. */
    if (cart_type == CART_HIROM &&
        (bank < 0x40 || (bank >= 0x80 && bank < 0xC0))
        && addr >= 0x6000 && addr < 0x8000) {
        return (int)((((bank & 0x3F) << 13) | (addr & 0x1FFF))
                     & (g_sram_size - 1));
    }
    return -1;
}

/* APU pacing: every HW-register touch advances the main-CPU cycle
 * estimate. v1 did this in `debug_on_block_enter` (RDB_BLOCK_HOOK); v2
 * doesn't emit those, so without this bump g_main_cpu_cycles_estimate
 * stays at 0, snes_catchupApu never advances the SPC, and SMW's
 * "wait for $2140 == $BBAA" poll loop spins forever waiting for a
 * response that the APU can't produce.
 *
 * Per-touch granularity is overshooting reality (real CPU does ~6
 * cycles per insn, far less than 24 per touch) but the SPC handshake
 * doesn't care about precise timing — it just needs *some* cycles to
 * elapse so the IPL ROM runs to the point of writing $BBAA. */
#include <stdio.h>
#include <stdlib.h>   /* getenv/strtol for the SNES_COSIM write watchpoint */
/* APU pacing: every HW-register touch advances the main-CPU cycle
 * estimate. v1 did this in `debug_on_block_enter`; v2 doesn't emit
 * those, so without this bump the SPC never advances and SMW's
 * "wait for $2140 == $BBAA" handshake spins forever.
 *
 * The 256-cycle increment is tuned to roughly match v1's per-block
 * pacing amortised over the recomp's tight CPU read loops. The
 * minimum-cycle floor in snes_catchupApu (snes.c) ensures the SPC
 * actually progresses on each call.
 *
 * Only APU-port touches ($2140-$217F) feed the APU catch-up counter
 * (issue #4): general HW touches massively over-count during load-heavy
 * phases ($2118 decompression spam) and used to convert into SPC-cycle
 * bursts that overflowed the DSP output ring at scene transitions,
 * audibly skipping the music. Handshake loops poll the ports themselves,
 * so APU touches alone keep every upload/ack protocol self-pacing. The
 * all-touch estimate stays for trace timestamps and diagnostics. */
static inline void cpu_pace_cycles(uint16 addr) {
    g_main_cpu_cycles_estimate += 256;
    if (addr >= 0x2140 && addr <= 0x217F)
        g_apu_pace_cycles_estimate += 256;
}

/* Word-access HW touch: ONE credit per 16-bit access, same as a byte access.
 * Touch parity across execution tiers holds because the interp tier routes
 * HW word accesses through cpu_read16/cpu_write16 too (interp_bridge word-bus
 * hooks), so both models see the identical credit sequence — the property the
 * co-sim shared APU clock (SNES_COSIM_APU_SHARED) relies on. */
static inline void cpu_pace_cycles_word(uint16 addr) {
    cpu_pace_cycles(addr);
}

/* Optional debug — disabled in release. Set BUILD_CPU_HW_LOG=1 in the
 * build to enable verbose per-touch logging. */
#define BUILD_CPU_HW_LOG 0
static uint64_t s_hw_touch_count = 0;
static uint16 s_last_hw_addr = 0;
static int s_last_hw_was_read = 0;
static int s_apu_writes_logged = 0;

/* Logger reachable from generated code. Disabled at release. */
void cpu_dbg_funcname(const char *name) {
    (void)name;
#if BUILD_CPU_HW_LOG
    static int n = 0;
    if (n++ < 50) {
        fprintf(stderr, "[func#%d] %s (touch=%llu)\n",
                n, name, (unsigned long long)s_hw_touch_count);
        fflush(stderr);
    }
#endif
}
static void cpu_hw_log(uint16 addr, int is_read, uint16 val) {
    s_last_hw_addr = addr;
    s_last_hw_was_read = is_read;
    if (!is_read && addr >= 0x2140 && addr <= 0x2143) {
        s_apu_writes_logged++;
    }
    s_hw_touch_count++;
#if BUILD_CPU_HW_LOG
    (void)val;
    if (s_hw_touch_count % 1000000 == 0) {
        fprintf(stderr, "[hw-pace] touches=%llu\n", (unsigned long long)s_hw_touch_count);
        fflush(stderr);
    }
#else
    (void)val;
#endif
}

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0) return cpu->ram[off];
    if (is_hw_reg(bank, addr)) { cpu_pace_cycles(addr); cpu_hw_log(addr, 1, 0); return ReadReg(addr); }
    if (g_snes && g_snes->cart && g_snes->cart->type == CART_SUPERFX)
        return cart_read(g_snes->cart, bank, addr);
    int sram = cpu_sram_offset(bank, addr);
    if (sram >= 0) return g_sram[sram];
    /* ROM read. RomPtr requires the global g_rom pointer to be live. */
    return *RomPtr(((uint32)bank << 16) | addr);
}

uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0) {
        /* A 16-bit guest access wraps its high-byte address within the same
         * bank. The corresponding host WRAM offsets are not necessarily
         * contiguous: $7F:FFFF -> $7F:0000 maps $1FFFF -> $10000, while
         * $00:1FFF -> $00:2000 crosses from WRAM into an I/O register. */
        uint16 hi_addr = (uint16)(addr + 1);
        int hi_off = cpu_ram_offset(bank, hi_addr);
        uint8 hi = (hi_off >= 0)
            ? cpu->ram[hi_off]
            : cpu_read8(cpu, bank, hi_addr);
        return (uint16)cpu->ram[off] | ((uint16)hi << 8);
    }
    if (is_hw_reg(bank, addr)) { cpu_pace_cycles_word(addr); cpu_hw_log(addr, 1, 0); return ReadRegWord(addr); }
    if (g_snes && g_snes->cart && g_snes->cart->type == CART_SUPERFX)
        return (uint16)cart_read(g_snes->cart, bank, addr) |
               ((uint16)cart_read(g_snes->cart, bank, (uint16)(addr + 1)) << 8);
    int sram_lo = cpu_sram_offset(bank, addr);
    if (sram_lo >= 0) {
        /* Compose word from two byte fetches. If the high byte crosses
         * out of SRAM (e.g. word read at $70:$7FFF), fall through to
         * cpu_read8 for that byte so the boundary is handled by the
         * same routing logic. */
        int sram_hi = cpu_sram_offset(bank, (uint16)(addr + 1));
        uint8 hi = (sram_hi >= 0)
            ? g_sram[sram_hi]
            : cpu_read8(cpu, bank, (uint16)(addr + 1));
        return (uint16)g_sram[sram_lo] | ((uint16)hi << 8);
    }
    /* ROM word read. */
    const uint8 *p = RomPtr(((uint32)bank << 16) | addr);
    return (uint16)p[0] | ((uint16)p[1] << 8);
}

void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
    if (g_wlog_active) wlog_note(bank, addr, v, 1);
    wlog_addr_note(bank, addr, v, 1);
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0) {
        uint8 old = cpu->ram[off];
        cpu->ram[off] = v;
#ifdef SNES_COSIM
        /* Exact per-write WRAM watchpoint (dev, env-gated): names the recompiled
         * function performing the store (not block-poll approximate). Set
         * SNESRECOMP_WRITE_WATCH=0xADDR. Zero cost when unset. */
        {
            extern const char *g_last_recomp_func;
            static long wa = -2; static FILE *wf = NULL; static int hits = 0;
            static long wmax = 400;
            if (wa == -2) {
                const char *e = getenv("SNESRECOMP_WRITE_WATCH");
                wa = (e && e[0]) ? strtol(e, NULL, 0) : -1;
                if (wa >= 0) { const char *lp = getenv("SNESRECOMP_WRITE_WATCH_LOG");
                               wf = fopen(lp && lp[0] ? lp : "writewatch.log", "w");
                               const char *mx = getenv("SNESRECOMP_WRITE_WATCH_MAX");
                               if (mx && mx[0]) wmax = strtol(mx, NULL, 0); }
            }
            if (wa >= 0 && wf && off == (int)wa && hits < wmax) {
                extern int snes_frame_counter;
                fprintf(wf, "[writewatch] f=%d $%04x = %02x (was %02x) bank=%02x addr=%04x "
                            "by %s (m=%d x=%d) S=%04x\n",
                        snes_frame_counter, off, v, old, bank, addr, g_last_recomp_func,
                        cpu->m_flag & 1, cpu->x_flag & 1, cpu->S);
                fflush(wf); hits++;
            }
        }
#endif
        cpu_trace_wram_write_check(cpu, bank, addr, off,
                                   (uint16)old, (uint16)v, 1);
        /* Also route through the dedicated 1M-entry WRAM-only ring so
         * writes survive when the main cpu_trace ring gets buried by
         * unrelated events (e.g. a BCS-self-loop block firing millions
         * of BLOCK events). IndirWriteByte/Word (common_rtl.h) already
         * does this for indirect stores; mirror it here for the
         * cpu_write8/16 path. */
#if SNESRECOMP_REVERSE_DEBUG
        extern void debug_on_wram_write_byte(uint32_t, uint8_t, uint8_t);
        debug_on_wram_write_byte((uint32_t)off, old, v);
#endif
        return;
    }
    if (is_hw_reg(bank, addr)) { cpu_pace_cycles(addr); cpu_hw_log(addr, 0, v); WriteReg(addr, v); return; }
    if (g_snes && g_snes->cart && g_snes->cart->type == CART_SUPERFX) {
        cart_write(g_snes->cart, bank, addr, v); return;
    }
    int sram = cpu_sram_offset(bank, addr);
    if (sram >= 0) { g_sram[sram] = v; return; }
    /* ROM / unmapped write: drop. */
}

void cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v) {
    if (g_wlog_active) wlog_note(bank, addr, v, 2);
    wlog_addr_note(bank, addr, v, 2);
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0) {
        uint16 hi_addr = (uint16)(addr + 1);
        int hi_off = cpu_ram_offset(bank, hi_addr);
        if (hi_off != off + 1) {
            /* Same boundary class as cpu_read16 above. Route each byte
             * through the authoritative byte bus so WRAM wrap and WRAM->I/O
             * crossings preserve their distinct mappings and side effects. */
            cpu_write8(cpu, bank, addr, (uint8)(v & 0xFF));
            cpu_write8(cpu, bank, hi_addr, (uint8)(v >> 8));
            return;
        }
    }
    if (off >= 0 && off + 1 < 0x20000) {
        uint16 old = (uint16)cpu->ram[off]
                   | ((uint16)cpu->ram[off + 1] << 8);
        cpu->ram[off]     = (uint8)(v & 0xFF);
        cpu->ram[off + 1] = (uint8)(v >> 8);
#ifdef SNES_COSIM
        {
            extern const char *g_last_recomp_func;
            static long wa = -2; static FILE *wf = NULL; static int hits = 0;
            if (wa == -2) {
                const char *e = getenv("SNESRECOMP_WRITE_WATCH");
                wa = (e && e[0]) ? strtol(e, NULL, 0) : -1;
                if (wa >= 0) { const char *lp = getenv("SNESRECOMP_WRITE_WATCH_LOG16");
                               wf = fopen(lp && lp[0] ? lp : "writewatch16.log", "w"); }
            }
            if (wa >= 0 && wf && (off == (int)wa || off + 1 == (int)wa) && hits < 400) {
                fprintf(wf, "[writewatch16] $%04x=%04x @off%04x (byte $%04lx now %02x, was %02x) "
                            "bank=%02x addr=%04x by %s (m=%d x=%d)\n",
                        off, v, off, wa, cpu->ram[wa], (uint8)(old >> ((wa-off)*8)),
                        bank, addr, g_last_recomp_func, cpu->m_flag & 1, cpu->x_flag & 1);
                fflush(wf); hits++;
            }
        }
#endif
        cpu_trace_wram_write_check(cpu, bank, addr, off, old, v, 2);
#if SNESRECOMP_REVERSE_DEBUG
        extern void debug_on_wram_write_word(uint32_t, uint16_t, uint16_t);
        debug_on_wram_write_word((uint32_t)off, old, v);
#endif
        return;
    }
    if (is_hw_reg(bank, addr)) { cpu_pace_cycles_word(addr); cpu_hw_log(addr, 0, v); WriteRegWord(addr, v); return; }
    if (g_snes && g_snes->cart && g_snes->cart->type == CART_SUPERFX) {
        cart_write(g_snes->cart, bank, addr, (uint8)v);
        cart_write(g_snes->cart, bank, (uint16)(addr + 1), (uint8)(v >> 8));
        return;
    }
    int sram_lo = cpu_sram_offset(bank, addr);
    if (sram_lo >= 0) {
        g_sram[sram_lo] = (uint8)(v & 0xFF);
        int sram_hi = cpu_sram_offset(bank, (uint16)(addr + 1));
        if (sram_hi >= 0) g_sram[sram_hi] = (uint8)(v >> 8);
        else cpu_write8(cpu, bank, (uint16)(addr + 1), (uint8)(v >> 8));
        return;
    }
    /* ROM / unmapped write: drop. */
}

/* ── PEI-trampoline dispatch helper (2026-05-24, narrow detector) ──────
 *
 * Called from _emit_return on trampoline-flagged Returns when the
 * runtime balance check (cpu->S != _entry_s) fires. The caller has
 * already popped the topmost frame from cpu->S and computed
 * (PB, PC+1) as `pc24`. We binary-search g_dispatch_table for an
 * entry matching `pc24` and, if found, call the variant fnptr for
 * the runtime (m, x) flags.
 *
 * Not-found case: pc24 doesn't correspond to a known function entry.
 * Returning NORMAL lets the host C call stack unwind back through the
 * chain of `return cpu_dispatch_pc(...)` tail calls to the original
 * site, which then resumes naturally.
 */

/* Diagnostic ring for dispatch events — instrumentation added during
 * MMX Dr Light "sprite vanish" diagnosis (2026-05-24). Each entry
 * records (pc24, mx_idx, found, frame) for one cpu_dispatch_pc call.
 * Always-on (small fixed allocation, no perf concern). TCP cmd
 * `dispatch_log_get` dumps the ring. */
typedef struct DispatchLogEntry {
    uint32_t pc24;
    uint32_t source_pc24;
    const char *func_name;
    uint8_t  mx_idx;     /* (m<<1)|x */
    uint8_t  found;      /* 1 if entry found in table, 0 if miss */
    uint8_t  mirror;     /* 1 if found only via LoROM bank-mirror lookup */
    uint8_t  pad;
    uint32_t frame;
} DispatchLogEntry;

#define DISPATCH_LOG_CAP 1024
static DispatchLogEntry g_dispatch_log[DISPATCH_LOG_CAP];
static unsigned g_dispatch_log_idx;  /* monotonic; modulo via CAP for storage */

/* Always-on aggregate found tallies (the ring only keeps the last CAP
 * events, so a whole-run found:0 rate is not recoverable from it). Never
 * evicted; read via cpu_dispatch_found_totals for the interp_stats command. */
static uint64_t g_dispatch_found1;   /* dispatches that hit an exact AOT body */
static uint64_t g_dispatch_found0;   /* dispatches with no AOT body (interp)  */

extern int snes_frame_counter;  /* common_rtl.c — game frame number */
extern const char *g_last_recomp_func;

static void _dispatch_log_record(uint32 pc24, uint32 source_pc24,
                                 unsigned mx_idx,
                                 int found, int via_mirror) {
    unsigned slot = g_dispatch_log_idx % DISPATCH_LOG_CAP;
    g_dispatch_log[slot].pc24 = pc24;
    g_dispatch_log[slot].source_pc24 = source_pc24;
    g_dispatch_log[slot].func_name = g_last_recomp_func;
    g_dispatch_log[slot].mx_idx = (uint8_t)mx_idx;
    g_dispatch_log[slot].found = (uint8_t)(found ? 1 : 0);
    g_dispatch_log[slot].mirror = (uint8_t)(via_mirror ? 1 : 0);
    g_dispatch_log[slot].pad = 0;
    g_dispatch_log[slot].frame = (uint32_t)snes_frame_counter;
    g_dispatch_log_idx++;
    if (found) g_dispatch_found1++; else g_dispatch_found0++;
}

unsigned cpu_dispatch_log_count(void) {
    return g_dispatch_log_idx;
}

/* Whole-run aggregate: exact AOT-hit vs interp-miss dispatch tallies. */
void cpu_dispatch_found_totals(uint64_t *found1, uint64_t *found0) {
    if (found1) *found1 = g_dispatch_found1;
    if (found0) *found0 = g_dispatch_found0;
}

const DispatchLogEntry *cpu_dispatch_log_at(unsigned i) {
    if (i >= g_dispatch_log_idx) return NULL;
    if (g_dispatch_log_idx > DISPATCH_LOG_CAP &&
        i < g_dispatch_log_idx - DISPATCH_LOG_CAP) return NULL;
    return &g_dispatch_log[i % DISPATCH_LOG_CAP];
}

/* Post-mortem JSON: serialize the always-on dispatch ring (last
 * DISPATCH_LOG_CAP runtime indirect dispatches) into the unified report,
 * with a trailing comma like the other dump_*_json sections. ALWAYS-ON
 * (Production too) — it reads the ring, never arms it. Engine-shared so
 * every game's post_mortem.c gets the section by calling this once.
 *
 * The ring records every cpu_dispatch_pc_from (RTS/RTL trampoline) AND
 * cpu_dispatch_call_pc (runtime-pointer JSR (abs,X) — SM enemy/PLM/eproj
 * AI). `found:0` entries name targets with no exact AOT body
 * (known rows run on LLE; unknown continuations unwind) — the promotion
 * worklist. The TCP
 * `dispatch_log_get` command dumps the same ring live; this is the only
 * readable copy when the TCP server is unavailable (SM). */
void CpuDispatchLogDumpJson(FILE *f) {
    unsigned total = g_dispatch_log_idx;
    unsigned n = total < DISPATCH_LOG_CAP ? total : DISPATCH_LOG_CAP;
    unsigned start = total - n;
    fprintf(f,
        "  \"dispatch_log\": {\"total\": %u, \"shown\": %u, \"events\": [",
        total, n);
    for (unsigned i = 0; i < n; i++) {
        const DispatchLogEntry *e = &g_dispatch_log[(start + i) % DISPATCH_LOG_CAP];
        const char *nm = e->func_name ? e->func_name : "(none)";
        fprintf(f,
            "%s{\"i\":%u,\"pc24\":%u,\"source_pc24\":%u,\"func\":\"%s\","
            "\"mx\":%u,\"found\":%u,\"mirror\":%u,\"frame\":%u}",
            (i ? "," : ""), start + i,
            (unsigned)e->pc24, (unsigned)e->source_pc24, nm,
            (unsigned)e->mx_idx, (unsigned)e->found,
            (unsigned)e->mirror, (unsigned)e->frame);
    }
    fprintf(f, "]},\n");
}

/* ── RAM-routine dispatch guard ────────────────────────────────────────────
 * A WRAM-resident ($7E/$7F) AOT body is a literal recompilation of a snapshot
 * of runtime-generated code. WRAM is mutable, so bouncing to that body is only
 * faithful while the live bytes still equal the recompiled snapshot. Every
 * dispatch-resolution path consults _ram_guard_blocks() for RAM targets; a
 * mismatch (or a RAM body with no guard record — fail safe) suppresses the AOT
 * bounce so the interpreter floor runs the real bytes, and logs loudly. */
static const RamRoutineGuard *_ram_guard_find(uint32 pc24) {
    unsigned lo = 0, hi = g_ram_routine_guard_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        uint32 m = g_ram_routine_guards[mid].pc24;
        if (m == pc24) return &g_ram_routine_guards[mid];
        if (m < pc24) lo = mid + 1;
        else          hi = mid;
    }
    return NULL;
}

/* Rate-limited: log the first sight of each distinct pc24 and then on
 * power-of-two counts, so a persistent mismatch stays visible without flooding
 * the log. Returns the (post-increment) hit count for message context. */
static uint64 _ram_guard_note(uint32 pc24) {
    static struct { uint32 pc; uint64 n; } seen[32];
    static int seen_n;
    int i;
    for (i = 0; i < seen_n; i++)
        if (seen[i].pc == pc24) break;
    if (i == seen_n) {
        if (seen_n < 32) { seen[seen_n].pc = pc24; seen[seen_n].n = 0; i = seen_n++; }
        else i = 0; /* table full: fold into slot 0 (loudness over precision) */
    }
    return ++seen[i].n;
}

static int _ram_guard_blocks(CpuState *cpu, uint32 pc24) {
    pc24 &= 0xFFFFFFu;
    uint8 bank = (uint8)((pc24 >> 16) & 0xFF);
    if (bank != 0x7E && bank != 0x7F) return 0;   /* ROM target: never guarded */
    const RamRoutineGuard *g = _ram_guard_find(pc24);
    if (g == NULL) {
        uint64 c = _ram_guard_note(pc24);
        if ((c & (c - 1)) == 0) {
            fprintf(stderr,
                "[ram-guard] $%06X has an AOT body but no guard record; "
                "refusing to bounce, running interpreter floor [x%llu]\n",
                pc24, (unsigned long long)c);
            fflush(stderr);
        }
        return 1;
    }
    uint32 h = 2166136261u;
    uint16 addr = (uint16)(pc24 & 0xFFFF);
    for (uint32 i = 0; i < g->len; i++) {
        h ^= cpu_read8(cpu, bank, (uint16)(addr + i));
        h *= 16777619u;
    }
    if (h != g->hash) {
        uint64 c = _ram_guard_note(pc24);
        if ((c & (c - 1)) == 0) {
            fprintf(stderr,
                "[ram-guard] $%06X live WRAM bytes no longer match the "
                "recompiled snapshot (expected FNV %08X, got %08X); running "
                "interpreter floor [x%llu]\n",
                pc24, g->hash, h, (unsigned long long)c);
            fflush(stderr);
        }
        return 1;
    }
    return 0;
}

static const DispatchEntry *_cpu_dispatch_find(uint32 pc24) {
    unsigned lo = 0;
    unsigned hi = g_dispatch_table_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        uint32 mid_pc = g_dispatch_table[mid].pc24;
        if (mid_pc == pc24) return &g_dispatch_table[mid];
        if (mid_pc < pc24) lo = mid + 1;
        else               hi = mid;
    }
    return NULL;
}

static RecompReturn (*_cpu_dispatch_lookup(CpuState *cpu, uint32 pc24))(CpuState *) {
    const DispatchEntry *row = _cpu_dispatch_find(pc24);
    if (row != NULL) {
        unsigned idx = (unsigned)(((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1));
        RecompReturn (*fp)(CpuState *) = row->variant[idx];
        /* A WRAM body may only run while its live bytes match the snapshot. */
        if (fp != NULL && _ram_guard_blocks(cpu, pc24)) return NULL;
        return fp;
    }
    return NULL;
}

RecompReturn cpu_dispatch_pc_from(CpuState *cpu, uint32 pc24,
                                  uint16 entry_s_for_miss_restore,
                                  uint32 source_pc24) {
    pc24 &= 0xFFFFFFu;
    source_pc24 &= 0xFFFFFFu;
    unsigned mx_idx = (unsigned)(((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1));
    int via_mirror = 0;
    int known_entry = 0;
    const DispatchEntry *row = _cpu_dispatch_find(pc24);
    RecompReturn (*fp)(CpuState *) = NULL;
    if (row != NULL) {
        known_entry = 1;
        fp = row->variant[mx_idx];
        /* WRAM body: only if the live bytes still match the snapshot. On a
         * mismatch fp drops to NULL but known_entry stays 1, so the popped-
         * return interpreter floor below runs the real (current) bytes. */
        if (fp != NULL && _ram_guard_blocks(cpu, pc24)) fp = NULL;
    }
    if (fp == NULL) {
        /* LoROM bank-mirror fallback: $00-$3F and $80-$BF share bytes.
         * Cfg may declare a function in one bank while the trampoline
         * popped (PB:PC) lands on the mirror. Try the other bank
         * before giving up — matches set_name_resolver's alias. */
        uint8 bank = (uint8)((pc24 >> 16) & 0xFF);
        if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) {
            const DispatchEntry *mirror_row = _cpu_dispatch_find(pc24 ^ 0x800000u);
            if (mirror_row != NULL) {
                known_entry = 1;
                fp = mirror_row->variant[mx_idx];
                if (fp != NULL) via_mirror = 1;
            }
        }
    }
    _dispatch_log_record(pc24, source_pc24, mx_idx, fp != NULL, via_mirror);
    if (fp == NULL) {
        if (known_entry) {
            /* The manifest knows this is a function boundary, but deliberately
             * emitted no body for the live M/X state.  That is not a return
             * continuation miss: execute the exact ROM bytes.  cpu->S already
             * reflects the RTS/RTL pop that reached this tail target. */
            cpu->host_return_valid = 0;
            cpu->PB = (uint8)(pc24 >> 16);
            return interp_tier_dispatch_popped_return(
                cpu, pc24, source_pc24, entry_s_for_miss_restore);
        }
        /* Not found: the popped (PB:PC) is a normal mid-caller return addr,
         * not a known function entry. Unwind by restoring cpu->S to the value
         * the caller expects after THIS function returns and returning NORMAL.
         * The caller passes entry_s_for_miss_restore = entry_s + frame_size
         * (the S after this function pops its own return frame) — so a
         * balanced hrv=0 callee returns with its frame correctly popped, and a
         * PEI trampoline discards its residual params up to that point. Passing
         * bare entry_s here would under-pop by frame_size and leak the caller's
         * frame on every miss (the heavy-load DMA-queue-corruption softlock;
         * cf. MMX Dr Light "sprite vanish" 2026-05-24). */
        cpu->S = entry_s_for_miss_restore;
        cpu->PB = (uint8)(pc24 >> 16);
        return RECOMP_RETURN_NORMAL;
    }
    /* Option-1: a dispatched entry has no paired host-C caller. The target
     * runs with host_return_valid=0 so its RTS/RTL re-dispatches on the
     * popped PC rather than host-returning into this dispatch frame. The
     * chain unwinds when a dispatch misses (S restored above) -> NORMAL. */
    cpu->host_return_valid = 0;
    cpu->PB = (uint8)(pc24 >> 16);
    return fp(cpu);
}

RecompReturn cpu_dispatch_pc(CpuState *cpu, uint32 pc24,
                              uint16 entry_s_for_miss_restore) {
    return cpu_dispatch_pc_from(cpu, pc24, entry_s_for_miss_restore, 0xFFFFFFu);
}

/* Runtime-pointer JSR (abs,X) call (cfg-free; emitted by codegen
 * _emit_runtime_dispatch for a reachable WRAM-pointer dispatch — SM
 * enemy/PLM/eproj AI interpreters). The 16-bit pointer was already read
 * from WRAM by the caller; pc24 = PB:pointer is the live target.
 *
 * Unlike cpu_dispatch_pc_from (a trampoline/RTS dispatch — dispatch ABI,
 * hrv=0, target re-dispatches its own RTS), this is a paired host CALL: we
 * push the 2-byte JSR return frame here and enter the AOT body with hrv=2 so
 * its RTS host-returns through that frame (popping it, S restored). The
 * handler's return value (NORMAL, or an NLR SKIP_N) propagates to the
 * caller, which falls through to the post-JSR block.
 *
 * On a lookup miss — the pointer names a target with no AOT body for the
 * live (m,x) — fall to the interpreter tier (interp_tier_run_call) which
 * runs the real ROM bytes and unwinds to the post-call S. The pushed frame
 * is the target's return frame either way, so the stack stays balanced. The
 * dispatch is recorded in the always-on g_dispatch_log ring (and the tier-2
 * gap manifest when it falls to the interpreter). */
RecompReturn cpu_dispatch_call_pc(CpuState *cpu, uint32 pc24,
                                  uint32 source_pc24) {
    pc24 &= 0xFFFFFFu;
    source_pc24 &= 0xFFFFFFu;
    /* Push the 2-byte JSR return frame (Option-1 cpu->S ABI): pushed value
     * = return_addr - 1 = source_pc24 + 2 (JSR (abs,X) is 3 bytes; RTS adds
     * 1 on pop -> source_pc24 + 3, the post-JSR instruction). */
    uint16 iret16 = (uint16)((source_pc24 + 2) & 0xFFFFu);
    cpu_write8(cpu, 0x00, cpu->S, (uint8)((iret16 >> 8) & 0xFF));
    cpu->S = (uint16)(cpu->S - 1);
    cpu_write8(cpu, 0x00, cpu->S, (uint8)(iret16 & 0xFF));
    cpu->S = (uint16)(cpu->S - 1);

    unsigned mx_idx = (unsigned)(((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1));
    int via_mirror = 0;
    RecompReturn (*fp)(CpuState *) = _cpu_dispatch_lookup(cpu, pc24);
    if (fp == NULL) {
        uint8 bank = (uint8)((pc24 >> 16) & 0xFF);
        if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) {
            fp = _cpu_dispatch_lookup(cpu, pc24 ^ 0x800000u);
            if (fp != NULL) via_mirror = 1;
        }
    }
    _dispatch_log_record(pc24, source_pc24, mx_idx, fp != NULL, via_mirror);
    if (fp != NULL) {
        /* Paired host-call: handler host-returns through the pushed frame. */
        cpu->host_return_valid = 2;
        return fp(cpu);
    }
    /* No AOT body for the live pointer -> interpreter tier. */
    return interp_tier_run_call(cpu, pc24, source_pc24);
}

RecompReturn cpu_dispatch_call_pc_pushed(CpuState *cpu, uint32 pc24,
                                         uint32 source_pc24,
                                         uint8 frame_size,
                                         uint32 *return_pc24) {
    pc24 &= 0xFFFFFFu;
    source_pc24 &= 0xFFFFFFu;
    unsigned mx_idx = (unsigned)(((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1));
    int via_mirror = 0;
    RecompReturn (*fp)(CpuState *) = _cpu_dispatch_lookup(cpu, pc24);
    if (fp == NULL) {
        uint8 bank = (uint8)((pc24 >> 16) & 0xFF);
        if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) {
            fp = _cpu_dispatch_lookup(cpu, pc24 ^ 0x800000u);
            if (fp != NULL) via_mirror = 1;
        }
    }
    _dispatch_log_record(pc24, source_pc24, mx_idx, fp != NULL, via_mirror);
    if (fp != NULL) {
        cpu->host_return_valid = frame_size;
        RecompReturn r = fp(cpu);
        return r;
    }
    return interp_tier_run_call_frame(cpu, pc24, source_pc24, frame_size,
                                      return_pc24);
}

/* Paired-call dispatch for the interpreter bridge's AOT-bounce (interp_bridge.c).
 * The interpreter has ALREADY pushed the call's return frame (frame_size bytes:
 * JSR/JSR(abs,X)=2, JSL=3) onto cpu->S and set in.pc to the target. Run the
 * target's live (m,x) variant with host_return_valid=frame_size so its RTS/RTL
 * HOST-RETURNS to the bridge (popping that frame, S restored to pre-call) and
 * the bridge resumes interpreting at the return address.
 *
 * This replaces the old bounce via cpu_dispatch_pc (dispatch ABI, hrv=0), whose
 * callee RTS RE-DISPATCHES on the popped return address. That is wrong in the
 * bridge context whenever the return address coincides with a registered
 * function entry: e.g. SamusDrawHandler_Default's "JSR HandleChargingBeamGfxAudio"
 * returns to $90:EB55, which is also the entry of sub_90EB55 — the dispatch HITS
 * it and runs the next routine as part of the callee's return, over-popping
 * cpu->S by the frame size and double-executing. Measured: the Samus-draw tail
 * dispatch left S +2 -> DB=$74 -> WriteEnemyOams freeze. Host-returning instead
 * makes the bounce stack-exact regardless of what sits at the return address. */
RecompReturn cpu_dispatch_pc_paired(CpuState *cpu, uint32 pc24, uint8 frame_size) {
    pc24 &= 0xFFFFFFu;
    unsigned mx_idx = (unsigned)(((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1));
    int via_mirror = 0;
    RecompReturn (*fp)(CpuState *) = _cpu_dispatch_lookup(cpu, pc24);
    if (fp == NULL) {
        uint8 bank = (uint8)((pc24 >> 16) & 0xFF);
        if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) {
            fp = _cpu_dispatch_lookup(cpu, pc24 ^ 0x800000u);
            if (fp != NULL) via_mirror = 1;
        }
    }
    _dispatch_log_record(pc24, 0xFFFFFFu, mx_idx, fp != NULL, via_mirror);
    if (fp == NULL) return RECOMP_RETURN_NORMAL;  /* caller already checked has_entry */
    cpu->host_return_valid = frame_size;
    return fp(cpu);
}

/* Read-only probe: would a dispatch to pc24 find an exact AOT body?
 * Mirrors cpu_dispatch_pc_from's lookup + LoROM bank-mirror fallback so
 * the interpreter bounce never leaves LLE for a NULL manifest variant. */
int cpu_dispatch_has_entry(CpuState *cpu, uint32 pc24) {
    pc24 &= 0xFFFFFFu;
    if (_cpu_dispatch_lookup(cpu, pc24) != NULL) return 1;
    uint8 bank = (uint8)((pc24 >> 16) & 0xFF);
    if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0))
        if (_cpu_dispatch_lookup(cpu, pc24 ^ 0x800000u) != NULL) return 1;
    return 0;
}

uint8 cpu_dispatch_inline_arg_bytes(uint32 pc24) {
    pc24 &= 0xFFFFFFu;
    for (int pass = 0; pass < 2; pass++) {
        unsigned lo = 0, hi = g_dispatch_table_count;
        while (lo < hi) {
            unsigned mid = lo + (hi - lo) / 2;
            uint32 mid_pc = g_dispatch_table[mid].pc24;
            if (mid_pc < pc24) lo = mid + 1;
            else if (mid_pc > pc24) hi = mid;
            else return g_dispatch_table[mid].inline_arg_bytes;
        }
        uint8 bank = (uint8)(pc24 >> 16);
        if (pass || !((bank < 0x40) || (bank >= 0x80 && bank < 0xC0)))
            break;
        pc24 ^= 0x800000u;
    }
    return 0;
}

void cpu_state_init(CpuState *cpu, uint8 *ram) {
    cpu->A = 0;
    /* No cpu->B init — B is derived from (A >> 8) and has no separate state. */
    cpu->X = 0;
    cpu->Y = 0;
    cpu->S = 0x01FF;
    cpu->D = 0;
    cpu->DB = 0;
    cpu->PB = 0;
    /* Reset state per 65816 spec: emulation=1, M=X=I=1 (P=0x34). */
    cpu->P = CPU_P_M | CPU_P_X | CPU_P_I;
    cpu->m_flag = 1;
    cpu->x_flag = 1;
    cpu->emulation = 1;
    cpu->host_return_valid = 0;
    cpu->_flag_N = 0;
    cpu->_flag_V = 0;
    cpu->_flag_Z = 0;
    cpu->_flag_C = 0;
    cpu->_flag_I = 1;
    cpu->_flag_D = 0;
    cpu->cycles = 0;
    cpu->master_cycles = 0;
    cpu->ram = ram;
    /* NLR pending-skip is NOT on CpuState — it's a function-local in
     * each emitted v2 function. See cpu_state.h for design rationale. */
}

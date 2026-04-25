/*
 * emu_oracle_cmds.c — backend-agnostic TCP commands for emulator-oracle.
 *
 * Routes every emu_* command through g_active_backend. New backends
 * (bsnes, mesen-s) light up automatically as long as they export a
 * snes_oracle_backend_t instance and the registry in this file picks
 * them up via ENABLE_*_ORACLE defines.
 *
 * Gated entirely on ENABLE_ORACLE_BACKEND; absent from non-Oracle
 * builds — the compilation unit is excluded from Release|x64.
 */
#ifdef ENABLE_ORACLE_BACKEND

#include "snes_oracle_backend.h"
#include "debug_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Non-static wrappers around debug_server's internal send helpers. See
 * debug_server.c bottom. Signature matches send_fmt exactly. */
extern void debug_server_send_line(const char *line);
extern void debug_server_send_fmt(const char *fmt, ...);

/* ---- Backend registry ---- */
#ifdef ENABLE_SNES9X_ORACLE
extern const snes_oracle_backend_t g_snes9x_backend;
#endif
#ifdef ENABLE_BSNES_ORACLE
extern const snes_oracle_backend_t g_bsnes_backend;
#endif

static const snes_oracle_backend_t *const s_backends[] = {
#ifdef ENABLE_SNES9X_ORACLE
    &g_snes9x_backend,
#endif
#ifdef ENABLE_BSNES_ORACLE
    &g_bsnes_backend,
#endif
    (const snes_oracle_backend_t *)0
};

const snes_oracle_backend_t *g_active_backend = (const snes_oracle_backend_t *)0;
static char s_cached_rom_path[512] = {0};

const char *snes_oracle_rom_path(void) {
    return s_cached_rom_path[0] ? s_cached_rom_path : (const char *)0;
}

int snes_oracle_init_default(const char *rom_path) {
    if (!rom_path || !*rom_path) return -1;
    strncpy(s_cached_rom_path, rom_path, sizeof(s_cached_rom_path) - 1);
    s_cached_rom_path[sizeof(s_cached_rom_path) - 1] = 0;

    const snes_oracle_backend_t *def = s_backends[0];
    if (!def) return -2;  /* no backends compiled in */
    int rc = def->init(rom_path);
    if (rc != 0) return rc;
    g_active_backend = def;
    return 0;
}

int snes_oracle_select(const char *name) {
    if (!name) return -1;
    const snes_oracle_backend_t *target = (const snes_oracle_backend_t *)0;
    for (int i = 0; s_backends[i]; i++) {
        if (strcmp(s_backends[i]->name, name) == 0) { target = s_backends[i]; break; }
    }
    if (!target) return -2;
    if (target == g_active_backend) return 0;

    if (g_active_backend && g_active_backend->shutdown)
        g_active_backend->shutdown();
    g_active_backend = (const snes_oracle_backend_t *)0;

    if (!s_cached_rom_path[0]) return -3;
    int rc = target->init(s_cached_rom_path);
    if (rc != 0) return rc;
    g_active_backend = target;
    return 0;
}

/* Called from main.c after RtlRunFrame each frame. No-op when no
 * backend is active. */
void emu_oracle_run_frame(uint16_t joypad1, uint16_t joypad2) {
    if (!g_active_backend) return;
    g_active_backend->run_frame(joypad1, joypad2);
}

/* ---- TCP command handlers ----
 * Match the debug_server convention: void h(const char *args), where
 * `args` is the trailing portion after the command name (possibly
 * empty, never NULL). */

static void h_emu_list(const char *args) {
    (void)args;
    char buf[512];
    int pos = snprintf(buf, sizeof(buf),
                       "{\"ok\":true,\"active\":\"%s\",\"backends\":[",
                       g_active_backend ? g_active_backend->name : "");
    for (int i = 0; s_backends[i]; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"%s\"",
                        i ? "," : "", s_backends[i]->name);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    debug_server_send_line(buf);
}

static void h_emu_select(const char *args) {
    char name[32] = {0};
    if (args) {
        int i = 0;
        while (*args == ' ') args++;
        while (*args && *args != ' ' && *args != '\n' && i < 31) name[i++] = *args++;
    }
    if (!name[0]) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"usage: emu_select <name>\"}");
        return;
    }
    int rc = snes_oracle_select(name);
    if (rc == 0)
        debug_server_send_fmt("{\"ok\":true,\"active\":\"%s\"}",
                              g_active_backend ? g_active_backend->name : "");
    else
        debug_server_send_fmt("{\"ok\":false,\"error\":\"select failed\",\"rc\":%d}", rc);
}

static void h_emu_is_loaded(const char *args) {
    (void)args;
    int loaded = (g_active_backend && g_active_backend->is_loaded && g_active_backend->is_loaded());
    debug_server_send_fmt("{\"ok\":true,\"loaded\":%s,\"active\":\"%s\"}",
                          loaded ? "true" : "false",
                          g_active_backend ? g_active_backend->name : "");
}

/* Streams hex to handle up to full WRAM (128 KB). The prior 1024-byte clamp
 * against a 4 KB stack hex buffer silently truncated larger requests — the
 * same archetype as cmd_read_ram's clamp, found during the 2026-04-23
 * tooling-audit. */
extern void debug_server_send_raw(const void *data, int len);
static void h_emu_read_wram(const char *args) {
    if (!g_active_backend) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"no active backend\"}");
        return;
    }
    unsigned int addr = 0, len = 1;
    if (!args || sscanf(args, "%x %u", &addr, &len) < 1) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"usage: emu_read_wram <hex_addr> [len]\"}");
        return;
    }
    if (len < 1) len = 1;
    if (len > 0x20000) len = 0x20000;  /* full WRAM */
    if (addr >= 0x20000u || addr + len > 0x20000u) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"wram range out of bounds\",\"addr\":\"0x%x\",\"len\":%u}", addr, len);
        return;
    }
    static uint8_t buf[0x20000];
    g_active_backend->get_wram(buf);
    char hdr[128];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "{\"ok\":true,\"addr\":\"0x%05x\",\"len\":%u,\"hex\":\"",
                        addr, len);
    debug_server_send_raw(hdr, hlen);
    char chunk[4096];
    for (unsigned int i = 0; i < len; ) {
        int pos = 0;
        for (; i < len && pos < 4000; i++)
            pos += snprintf(chunk + pos, sizeof(chunk) - pos, "%02x", buf[addr + i]);
        debug_server_send_raw(chunk, pos);
    }
    debug_server_send_raw("\"}\n", 3);
}

/* emu_read_vram <hex_addr> [len_decimal]
 * Snapshot a byte range from the oracle backend's PPU VRAM.
 * VRAM is 64 KB (byte-indexed; SNES hardware is word-indexed,
 * so adr_word = adr_byte / 2). */
static void h_emu_read_vram(const char *args) {
    if (!g_active_backend) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"no active backend\"}");
        return;
    }
    if (!g_active_backend->get_vram) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"get_vram not implemented by backend\","
                              "\"backend\":\"%s\"}", g_active_backend->name);
        return;
    }
    unsigned int addr = 0, len = 1;
    if (!args || sscanf(args, "%x %u", &addr, &len) < 1) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"usage: emu_read_vram <hex_addr> [len]\"}");
        return;
    }
    if (len < 1) len = 1;
    if (len > 0x10000) len = 0x10000;
    if (addr >= 0x10000u || addr + len > 0x10000u) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"vram range out of bounds\",\"addr\":\"0x%x\",\"len\":%u}", addr, len);
        return;
    }
    static uint8_t buf[0x10000];
    g_active_backend->get_vram(buf);
    char hdr[128];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "{\"ok\":true,\"addr\":\"0x%04x\",\"len\":%u,\"hex\":\"",
                        addr, len);
    debug_server_send_raw(hdr, hlen);
    char chunk[4096];
    for (unsigned int i = 0; i < len; ) {
        int pos = 0;
        for (; i < len && pos < 4000; i++)
            pos += snprintf(chunk + pos, sizeof(chunk) - pos, "%02x", buf[addr + i]);
        debug_server_send_raw(chunk, pos);
    }
    debug_server_send_raw("\"}\n", 3);
}

/* fuzz_run_snippet <rom_hex> <a> <x> <y> <s> <d> <db> <p>
 *
 * Phase B differential fuzz entry point. All integer args are decimal
 * except rom_hex. Snippet bytes live in rom_hex (arbitrary length up
 * to 256). Runs the snippet via snes9x_bridge_fuzz_run_snippet; on
 * success, emits the WRAM $0000-$1FFF snapshot as a hex blob — the
 * Python orchestrator computes the delta against the baseline.
 *
 * Only snes9x backend implements this today; others return error.
 */
extern int snes9x_bridge_fuzz_run_snippet(
    const uint8_t *rom_bytes, int rom_len,
    uint16_t seed_a, uint16_t seed_x, uint16_t seed_y,
    uint16_t seed_s, uint16_t seed_d,
    uint8_t seed_db, uint8_t seed_p,
    uint8_t *out_wram);

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static void h_fuzz_run_snippet(const char *args) {
    if (!g_active_backend || strcmp(g_active_backend->name, "snes9x") != 0) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"fuzz requires snes9x backend\"}");
        return;
    }
    if (!args) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"usage: fuzz_run_snippet <hex> a x y s d db p\"}");
        return;
    }
    /* Parse: first token is hex string; rest are decimal ints. */
    char hex[512];
    unsigned int a = 0, x = 0, y = 0, s = 0, d = 0, db = 0, p = 0;
    int n = sscanf(args, "%511s %u %u %u %u %u %u %u",
                   hex, &a, &x, &y, &s, &d, &db, &p);
    if (n < 8) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"expected 8 args (hex a x y s d db p), got %d\"}", n);
        return;
    }
    /* Decode hex into bytes. */
    int hex_len = (int)strlen(hex);
    if (hex_len % 2 != 0 || hex_len > 512) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"bad hex length %d\"}", hex_len);
        return;
    }
    uint8_t rom[256];
    int rom_len = hex_len / 2;
    for (int i = 0; i < rom_len; i++) {
        int hi = hex_nibble(hex[i*2]);
        int lo = hex_nibble(hex[i*2 + 1]);
        if (hi < 0 || lo < 0) {
            debug_server_send_fmt("{\"ok\":false,\"error\":\"bad hex char at %d\"}", i*2);
            return;
        }
        rom[i] = (uint8_t)((hi << 4) | lo);
    }

    static uint8_t wram_after[0x2000];
    int rc = snes9x_bridge_fuzz_run_snippet(
        rom, rom_len,
        (uint16_t)a, (uint16_t)x, (uint16_t)y,
        (uint16_t)s, (uint16_t)d,
        (uint8_t)db, (uint8_t)p,
        wram_after);
    if (rc != 0) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"fuzz_run returned %d\"}", rc);
        return;
    }
    /* Emit WRAM as hex blob. Format mirrors emu_read_wram. */
    char hdr[128];
    int hlen = snprintf(hdr, sizeof(hdr), "{\"ok\":true,\"wram_hex\":\"");
    debug_server_send_raw(hdr, hlen);
    char chunk[4096];
    for (int i = 0; i < 0x2000; ) {
        int pos = 0;
        for (; i < 0x2000 && pos < 4000; i++)
            pos += snprintf(chunk + pos, sizeof(chunk) - pos, "%02x", wram_after[i]);
        debug_server_send_raw(chunk, pos);
    }
    debug_server_send_raw("\"}\n", 3);
}

/* emu_func_snap_set <hex_pc24> — register a 24-bit PC; subsequent
 * frames will capture WRAM into a buffer at every dispatch whose
 * PB:PC matches. 0 disables. snes9x backend only.
 * emu_func_snap_get <hex_addr> [len] — read from the captured buffer.
 */
extern void snes9x_bridge_func_snap_set(uint32_t pc24);
extern int  snes9x_bridge_func_snap_count(void);
extern int  snes9x_bridge_func_snap_frame(void);
extern const uint8_t* snes9x_bridge_func_snap_buf(void);

static void h_emu_func_snap_set(const char *args) {
    if (!g_active_backend || strcmp(g_active_backend->name, "snes9x") != 0) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"snap requires snes9x backend\"}");
        return;
    }
    unsigned int pc24 = 0;
    sscanf(args ? args : "", "%x", &pc24);
    snes9x_bridge_func_snap_set(pc24);
    debug_server_send_fmt("{\"ok\":true,\"pc24\":\"0x%06x\"}", pc24);
}

static void h_emu_func_snap_get(const char *args) {
    if (!g_active_backend || strcmp(g_active_backend->name, "snes9x") != 0) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"snap requires snes9x backend\"}");
        return;
    }
    int frame = snes9x_bridge_func_snap_frame();
    int count = snes9x_bridge_func_snap_count();
    if (frame < 0) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"no snapshot yet\","
                              "\"count\":%d}", count);
        return;
    }
    unsigned int addr = 0, len = 1;
    if (sscanf(args ? args : "", "%x %u", &addr, &len) < 1) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"usage: emu_func_snap_get <hex_addr> [len]\"}");
        return;
    }
    if (len < 1) len = 1;
    if (len > 0x20000) len = 0x20000;
    if (addr >= 0x20000u || addr + len > 0x20000u) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"addr range out of bounds\"}");
        return;
    }
    const uint8_t *buf = snes9x_bridge_func_snap_buf();
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "{\"ok\":true,\"frame\":%d,\"count\":%d,\"addr\":\"0x%05x\",\"len\":%u,\"hex\":\"",
        frame, count, addr, len);
    debug_server_send_raw(hdr, hlen);
    char chunk[4096];
    for (unsigned int i = 0; i < len; ) {
        int pos = 0;
        for (; i < len && pos < 4000; i++)
            pos += snprintf(chunk + pos, sizeof(chunk) - pos, "%02x", buf[addr + i]);
        debug_server_send_raw(chunk, pos);
    }
    debug_server_send_raw("\"}\n", 3);
}

/* Drive the active backend forward N frames without advancing the
 * recomp side. Used to re-sync the two runtimes when their boot
 * sequences progress at different rates. Capped to avoid runaway.
 * Max N is 100000 (~28 minutes at 60 Hz). */
static void h_emu_step(const char *args) {
    if (!g_active_backend) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"no active backend\"}");
        return;
    }
    int n = 1;
    if (args && sscanf(args, "%d", &n) < 1) n = 1;
    if (n < 1) n = 1;
    if (n > 100000) n = 100000;
    for (int i = 0; i < n; i++)
        g_active_backend->run_frame(0, 0);
    debug_server_send_fmt("{\"ok\":true,\"advanced\":%d}", n);
}

/* emu_wram_delta [hex_lo] [hex_hi]
 *
 * Returns the set of WRAM bytes that changed during the MOST RECENT
 * emu frame (run_frame or emu_step 1). This is the snes9x analog of
 * Tier 1's WRAM write trace, but at per-frame granularity rather than
 * per-instruction. Pairs with recomp's `get_wram_trace` for side-by-
 * side "what got written this frame" comparison.
 *
 * Defaults to the low 8 KB of bank 7E ($0000-$1FFF), where SMW's
 * gameplay state lives. Cap response at 512 diffs to keep JSON sane.
 */
static void h_emu_wram_delta(const char *args) {
    if (!g_active_backend) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"no active backend\"}");
        return;
    }
    if (strcmp(g_active_backend->name, "snes9x") != 0) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"wram_delta not implemented for %s\"}",
                              g_active_backend->name);
        return;
    }
    unsigned int lo = 0x0000, hi = 0x1FFF;
    if (args) sscanf(args, "%x %x", &lo, &hi);
    if (hi >= 0x20000) hi = 0x1FFFF;
    if (lo > hi) lo = hi;

    extern int snes9x_bridge_get_wram_delta(uint32_t, uint32_t,
                                            uint32_t *, uint8_t *, uint8_t *, int);
    static uint32_t addrs[512];
    static uint8_t  before[512];
    static uint8_t  after[512];
    int n = snes9x_bridge_get_wram_delta(lo, hi, addrs, before, after, 512);

    char buf[32768];
    int pos = snprintf(buf, sizeof(buf),
                       "{\"ok\":true,\"lo\":\"0x%05x\",\"hi\":\"0x%05x\",\"count\":%d,\"log\":[",
                       lo, hi, n);
    int budget = (int)sizeof(buf) - 64;
    for (int i = 0; i < n && pos < budget; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"adr\":\"0x%05x\",\"before\":\"0x%02x\",\"after\":\"0x%02x\"}",
                        i ? "," : "", addrs[i], before[i], after[i]);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    debug_server_send_line(buf);
}

/* emu_insn_trace_on / off / reset
 * emu_insn_trace_count
 * emu_nmi_count
 * emu_get_insn_trace [from=N] [to=N] [pc_lo=H] [pc_hi=H] [limit=N]
 *
 * Per-instruction execution trace on the snes9x backend. Captures
 * full hardware register state (A, X, Y, S, D, DB, P_W, cycles) at
 * every opcode dispatch, plus a separate NMI counter. Closes the
 * gap that recomp's symbolic tracker can only provide A/X/Y/B —
 * hardware always knows the truth.
 */
static void h_emu_insn_trace_on(const char *args) {
    (void)args;
    if (!g_active_backend || strcmp(g_active_backend->name, "snes9x") != 0) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"insn_trace requires snes9x backend\"}");
        return;
    }
    extern void snes9x_bridge_insn_trace_on(void);
    snes9x_bridge_insn_trace_on();
    debug_server_send_fmt("{\"ok\":true,\"max_entries\":1048576}");
}

static void h_emu_insn_trace_off(const char *args) {
    (void)args;
    extern void snes9x_bridge_insn_trace_off(void);
    snes9x_bridge_insn_trace_off();
    debug_server_send_fmt("{\"ok\":true}");
}

static void h_emu_insn_trace_reset(const char *args) {
    (void)args;
    extern void snes9x_bridge_insn_trace_reset(void);
    snes9x_bridge_insn_trace_reset();
    debug_server_send_fmt("{\"ok\":true}");
}

static void h_emu_insn_trace_count(const char *args) {
    (void)args;
    extern uint64_t snes9x_bridge_insn_trace_count(void);
    debug_server_send_fmt("{\"ok\":true,\"count\":%llu}",
                          (unsigned long long)snes9x_bridge_insn_trace_count());
}

static void h_emu_nmi_count(const char *args) {
    (void)args;
    extern uint64_t snes9x_bridge_nmi_count(void);
    debug_server_send_fmt("{\"ok\":true,\"count\":%llu}",
                          (unsigned long long)snes9x_bridge_nmi_count());
}

static void h_emu_get_insn_trace(const char *args) {
    extern uint64_t snes9x_bridge_insn_trace_count(void);
    extern int snes9x_bridge_insn_trace_get(uint64_t i, int32_t *frame,
                                            uint32_t *pc24, uint8_t *op,
                                            uint8_t *db, uint16_t *a, uint16_t *x,
                                            uint16_t *y, uint16_t *s, uint16_t *d,
                                            uint16_t *p_w, int32_t *cycles);
    int32_t from_idx = 0;
    int32_t limit = 256;
    unsigned int pc_lo = 0, pc_hi = 0xFFFFFFu;
    if (args) {
        const char *p;
        if ((p = strstr(args, "from="))) sscanf(p + 5, "%d", &from_idx);
        if ((p = strstr(args, "limit="))) sscanf(p + 6, "%d", &limit);
        if ((p = strstr(args, "pc_lo="))) sscanf(p + 6, "%x", &pc_lo);
        if ((p = strstr(args, "pc_hi="))) sscanf(p + 6, "%x", &pc_hi);
    }
    if (limit < 1) limit = 1;
    if (limit > 4096) limit = 4096;
    uint64_t total = snes9x_bridge_insn_trace_count();

    static char buf[262144];
    int pos = snprintf(buf, sizeof(buf),
                       "{\"ok\":true,\"total\":%llu,\"log\":[",
                       (unsigned long long)total);
    int budget = (int)sizeof(buf) - 256;
    int emitted = 0;
    int first = 1;
    for (uint64_t i = (uint64_t)from_idx; i < total && pos < budget && emitted < limit; i++) {
        int32_t frame; uint32_t pc24; uint8_t op, db;
        uint16_t a, x, y, s, d, p_w; int32_t cycles;
        if (!snes9x_bridge_insn_trace_get(i, &frame, &pc24, &op, &db,
                                          &a, &x, &y, &s, &d, &p_w, &cycles)) break;
        if (pc24 < pc_lo || pc24 > pc_hi) continue;
        // P_W bit 8 = emulation; bit 5 = m_flag (memory width); bit 4 = x_flag (index width).
        // We surface them as separate booleans for probe convenience.
        int e_flag = (p_w & 0x100) ? 1 : 0;
        int m_flag = (p_w & 0x20)  ? 1 : 0;
        int x_flag = (p_w & 0x10)  ? 1 : 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"i\":%llu,\"f\":%d,\"pc\":\"0x%06x\",\"op\":\"0x%02x\","
            "\"a\":\"0x%04x\",\"x\":\"0x%04x\",\"y\":\"0x%04x\","
            "\"s\":\"0x%04x\",\"d\":\"0x%04x\",\"db\":\"0x%02x\","
            "\"p\":\"0x%04x\",\"m\":%d,\"x_flag\":%d,\"e\":%d,\"cyc\":%d}",
            first ? "" : ",",
            (unsigned long long)i, frame, pc24, op,
            a, x, y, s, d, db, p_w, m_flag, x_flag, e_flag, cycles);
        first = 0;
        emitted++;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "],\"emitted\":%d}", emitted);
    debug_server_send_line(buf);
}

/* emu_wram_trace_add <hex_lo> [hex_hi]
 * emu_wram_trace_reset
 * emu_get_wram_trace
 *
 * Installs a write-hook inside snes9x's memory bus (via getset.h's
 * s9x_write_hook). The hook records every write that hits a watched
 * WRAM range, capturing (frame, addr, pc24, before, after, bank_source).
 * This is the snes9x analog of recomp's Tier 1 trace_wram / get_wram_trace,
 * and it answers "which PC in the ROM wrote this byte" — exactly what
 * we need to close bug #8.
 *
 * Only snes9x implements this. bsnes will grow equivalent commands
 * when that backend is added. */
static void h_emu_wram_trace_add(const char *args) {
    if (!g_active_backend || strcmp(g_active_backend->name, "snes9x") != 0) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"wram_trace requires snes9x backend\"}");
        return;
    }
    unsigned int lo = 0, hi = 0;
    int n = args ? sscanf(args, "%x %x", &lo, &hi) : 0;
    if (n < 1) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"usage: emu_wram_trace_add <hex_lo> [hex_hi]\"}");
        return;
    }
    if (n < 2) hi = lo;
    extern int snes9x_bridge_watch_add(uint32_t lo, uint32_t hi);
    int rc = snes9x_bridge_watch_add(lo, hi);
    if (rc < 0)
        debug_server_send_fmt("{\"ok\":false,\"error\":\"watch_add failed\",\"rc\":%d}", rc);
    else
        debug_server_send_fmt("{\"ok\":true,\"lo\":\"0x%05x\",\"hi\":\"0x%05x\",\"nranges\":%d}", lo, hi, rc);
}

static void h_emu_wram_trace_reset(const char *args) {
    (void)args;
    if (!g_active_backend || strcmp(g_active_backend->name, "snes9x") != 0) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"wram_trace requires snes9x backend\"}");
        return;
    }
    extern void snes9x_bridge_watch_clear(void);
    snes9x_bridge_watch_clear();
    debug_server_send_fmt("{\"ok\":true}");
}

static void h_emu_get_wram_trace(const char *args) {
    (void)args;
    if (!g_active_backend || strcmp(g_active_backend->name, "snes9x") != 0) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"wram_trace requires snes9x backend\"}");
        return;
    }
    extern int snes9x_bridge_watch_count(void);
    extern int snes9x_bridge_watch_get(int, uint32_t *, uint32_t *, uint32_t *,
                                       uint8_t *, uint8_t *, uint8_t *);
    int n = snes9x_bridge_watch_count();

    static char buf[524288];   /* same size as recomp get_wram_trace */
    int pos = snprintf(buf, sizeof(buf), "{\"ok\":true,\"count\":%d,\"log\":[", n);
    int budget = (int)sizeof(buf) - 128;
    for (int i = 0; i < n && pos < budget; i++) {
        uint32_t f = 0, addr = 0, pc = 0;
        uint8_t before = 0, after = 0, bank = 0;
        if (!snes9x_bridge_watch_get(i, &f, &addr, &pc, &before, &after, &bank)) break;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"f\":%u,\"adr\":\"0x%05x\",\"pc\":\"0x%06x\","
                        "\"before\":\"0x%02x\",\"after\":\"0x%02x\",\"bank_src\":\"0x%02x\"}",
                        i ? "," : "", (unsigned)f, addr, pc, before, after, bank);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    debug_server_send_line(buf);
}

/* find_first_divergence [subsystem=wram] [lo=0] [hi=0x1FFFF] [context=16]
 *
 * Compares the recompiled runtime's live state against the oracle backend's
 * live state AT THE CURRENT FRAME. Returns the first byte offset where they
 * differ, plus a window of surrounding bytes for context. Caller invokes
 * this at whatever point they suspect divergence; the runtime and backend
 * run in lock-step per frame, so "current frame" on both sides is the same.
 *
 * Subsystem: only `wram` is supported today. The backend interface also
 * declares get_vram / get_cgram / get_oam slots, but no backend has shipped
 * them yet; those return "not implemented" until the snes9x bridge (or
 * another backend) fills them in.
 *
 * Scope note: this is the single-instant comparator. A frame-range
 * stepping variant (walk forward N frames, stop at first divergence) is a
 * natural follow-up once the backend exposes a blocking "run to frame N"
 * entry point — today's TCP-handler-synchronous model can't drive recomp's
 * main-loop frame advance from inside a command.
 */
extern uint8_t g_ram[0x20000];  /* recomp's 128 KB WRAM, see common_rtl.h */

static void h_find_first_divergence(const char *args) {
    if (!g_active_backend) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"no active backend\"}");
        return;
    }

    char subsystem[16] = "wram";
    unsigned int lo = 0x00000, hi = 0x1FFFF;
    int context = 16;
    if (args && *args) {
        /* Best-effort parse: <word> [hex_lo] [hex_hi] [dec_context]. All optional. */
        const char *p = args;
        while (*p == ' ') p++;
        if (*p && *p != ' ') {
            int i = 0;
            while (*p && *p != ' ' && i < 15) subsystem[i++] = *p++;
            subsystem[i] = 0;
        }
        sscanf(p, "%x %x %d", &lo, &hi, &context);
    }
    if (strcmp(subsystem, "wram") != 0) {
        debug_server_send_fmt(
            "{\"ok\":false,\"error\":\"subsystem not implemented\","
            "\"subsystem\":\"%s\",\"supported\":[\"wram\"]}",
            subsystem);
        return;
    }
    if (hi >= 0x20000) hi = 0x1FFFF;
    if (lo > hi) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"lo > hi\"}");
        return;
    }
    if (context < 0) context = 0;
    if (context > 256) context = 256;

    /* Copy both sides into stable buffers, then scan. get_wram may run
     * non-trivial work on the backend side; do it once. */
    static uint8_t oracle_wram[0x20000];
    g_active_backend->get_wram(oracle_wram);
    const uint8_t *recomp_wram = g_ram;  /* live recomp WRAM */

    int first_diff = -1;
    int diff_count = 0;
    for (unsigned int i = lo; i <= hi; i++) {
        if (recomp_wram[i] != oracle_wram[i]) {
            if (first_diff < 0) first_diff = (int)i;
            diff_count++;
        }
    }

    char buf[16384];
    int pos;
    if (first_diff < 0) {
        pos = snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"match\":true,\"subsystem\":\"%s\","
            "\"lo\":\"0x%05x\",\"hi\":\"0x%05x\",\"bytes_scanned\":%u}",
            subsystem, lo, hi, (unsigned)(hi - lo + 1));
    } else {
        /* Context window: `context` bytes on each side of first_diff,
         * clipped to [lo, hi]. */
        int ctx_lo = first_diff - context;
        int ctx_hi = first_diff + context;
        if (ctx_lo < (int)lo) ctx_lo = (int)lo;
        if (ctx_hi > (int)hi) ctx_hi = (int)hi;

        pos = snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"match\":false,\"subsystem\":\"%s\","
            "\"first_diff\":\"0x%05x\",\"recomp\":\"0x%02x\",\"oracle\":\"0x%02x\","
            "\"diff_count\":%d,\"lo\":\"0x%05x\",\"hi\":\"0x%05x\","
            "\"context\":[",
            subsystem, (unsigned)first_diff,
            recomp_wram[first_diff], oracle_wram[first_diff],
            diff_count, lo, hi);
        int first_emit = 1;
        for (int i = ctx_lo; i <= ctx_hi && pos + 128 < (int)sizeof(buf); i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "%s{\"adr\":\"0x%05x\",\"r\":\"0x%02x\",\"o\":\"0x%02x\",\"diff\":%s}",
                first_emit ? "" : ",",
                (unsigned)i, recomp_wram[i], oracle_wram[i],
                (recomp_wram[i] != oracle_wram[i]) ? "true" : "false");
            first_emit = 0;
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    }
    debug_server_send_line(buf);
}

static void h_emu_cpu_regs(const char *args) {
    (void)args;
    if (!g_active_backend) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"no active backend\"}");
        return;
    }
    SnesCpuRegs r = {0};
    g_active_backend->get_cpu_regs(&r);
    debug_server_send_fmt(
        "{\"ok\":true,"
        "\"a\":\"0x%04x\",\"x\":\"0x%04x\",\"y\":\"0x%04x\","
        "\"s\":\"0x%04x\",\"d\":\"0x%04x\",\"pc\":\"0x%04x\","
        "\"db\":\"0x%02x\",\"pb\":\"0x%02x\",\"p\":\"0x%02x\",\"e\":%d}",
        r.a, r.x, r.y, r.s, r.d, r.pc, r.db, r.pb, r.p, r.emulation_mode);
}

/* Dispatcher. Returns 1 if the command was one of ours and was
 * handled, 0 to let the standard s_commands[] scan continue. */
int emu_oracle_handle_cmd(const char *cmd, const char *args) {
    if (!cmd) return 0;
    if (strcmp(cmd, "emu_list") == 0)      { h_emu_list(args);      return 1; }
    if (strcmp(cmd, "emu_select") == 0)    { h_emu_select(args);    return 1; }
    if (strcmp(cmd, "emu_is_loaded") == 0) { h_emu_is_loaded(args); return 1; }
    if (strcmp(cmd, "emu_read_wram") == 0) { h_emu_read_wram(args); return 1; }
    if (strcmp(cmd, "emu_read_vram") == 0) { h_emu_read_vram(args); return 1; }
    if (strcmp(cmd, "fuzz_run_snippet") == 0) { h_fuzz_run_snippet(args); return 1; }
    if (strcmp(cmd, "emu_func_snap_set") == 0) { h_emu_func_snap_set(args); return 1; }
    if (strcmp(cmd, "emu_func_snap_get") == 0) { h_emu_func_snap_get(args); return 1; }
    if (strcmp(cmd, "emu_cpu_regs") == 0)  { h_emu_cpu_regs(args);  return 1; }
    if (strcmp(cmd, "emu_step") == 0)      { h_emu_step(args);      return 1; }
    if (strcmp(cmd, "emu_wram_delta") == 0){ h_emu_wram_delta(args); return 1; }
    if (strcmp(cmd, "emu_wram_trace_add") == 0)   { h_emu_wram_trace_add(args);   return 1; }
    if (strcmp(cmd, "emu_wram_trace_reset") == 0) { h_emu_wram_trace_reset(args); return 1; }
    if (strcmp(cmd, "emu_get_wram_trace") == 0)   { h_emu_get_wram_trace(args);   return 1; }
    if (strcmp(cmd, "emu_insn_trace_on") == 0)    { h_emu_insn_trace_on(args);    return 1; }
    if (strcmp(cmd, "emu_insn_trace_off") == 0)   { h_emu_insn_trace_off(args);   return 1; }
    if (strcmp(cmd, "emu_insn_trace_reset") == 0) { h_emu_insn_trace_reset(args); return 1; }
    if (strcmp(cmd, "emu_insn_trace_count") == 0) { h_emu_insn_trace_count(args); return 1; }
    if (strcmp(cmd, "emu_nmi_count") == 0)        { h_emu_nmi_count(args);        return 1; }
    if (strcmp(cmd, "emu_get_insn_trace") == 0)   { h_emu_get_insn_trace(args);   return 1; }
    if (strcmp(cmd, "find_first_divergence") == 0){ h_find_first_divergence(args); return 1; }
    return 0;
}

#endif /* ENABLE_ORACLE_BACKEND */

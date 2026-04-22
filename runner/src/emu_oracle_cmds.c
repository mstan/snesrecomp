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
    if (len > 1024) len = 1024;
    if (addr >= 0x20000u || addr + len > 0x20000u) {
        debug_server_send_fmt("{\"ok\":false,\"error\":\"wram range out of bounds\",\"addr\":\"0x%x\",\"len\":%u}", addr, len);
        return;
    }
    static uint8_t buf[0x20000];
    g_active_backend->get_wram(buf);
    char hex[4096];
    int pos = 0;
    for (unsigned int i = 0; i < len && pos + 3 < (int)sizeof(hex); i++)
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x", buf[addr + i]);
    hex[pos] = 0;
    debug_server_send_fmt("{\"ok\":true,\"addr\":\"0x%05x\",\"len\":%u,\"hex\":\"%s\"}",
                          addr, len, hex);
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
    return 0;
}

#endif /* ENABLE_ORACLE_BACKEND */

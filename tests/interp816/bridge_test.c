/*
 * interp_bridge Phase-1 contract harness (no game / no ROM).
 *
 * Proves the interp<->AOT bridge mechanics deterministically with fakes:
 *   - cpu_read8/cpu_write8  -> a flat RAM (the bus the bridge routes through);
 *   - cpu_dispatch_pc / cpu_dispatch_has_entry -> ONE known "compiled" entry
 *     whose fake body mutates A and pops its return frame (modelling a real
 *     AOT function's RTS: pop frame, dispatch-miss on return addr, S restored).
 *
 * Scenarios:
 *   S1: interp routine that JSRs into the compiled entry -> the bounce runs the
 *       compiled body, state syncs, stack stays balanced, resume at return addr.
 *   S2: pure interp routine (no call) -> exits balanced, no bounce.
 *   S3: interp routine that JSRs a NON-compiled target -> interpreted through,
 *       its RTS returns to caller level (no premature exit), final RTS exits.
 *   S13: runtime-call fallback executing M=0 PLA; RTL consumes its inner JSR
 *        and an outer JSL, propagating SKIP_1 to the compiled caller.
 *
 * Build/run: tests/interp816/run.sh (WSL gcc). Validation only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interp_bridge.h"   /* -> cpu_state.h (types, inline frame helpers) */
#include "tier2_capture.h"
#include "snes.h"            /* Snes storage for the bridge's APU clock hook */
#include "sa1.h"

CpuState g_cpu;

#define MEMSZ 0x1000000u
static uint8_t *RAM;
static int      g_aot_called;
static int      g_aot_rewrites_return;
static int      g_aot_nested_rewrite;
static int      g_aot_interp_nlr;
static int      g_aot_double_rewrite;
static int      g_aot_crosses_interp_owner;
static int      g_aot_skips_interp_owner;
static int      g_aot_deadline_unwind;
static int      g_owner_target_result;
static int      g_aot_tail_chain_probe;
static int      g_aot_skips_root;
static int      g_tail_chain_direct;
static int      g_nested_chain_direct;
#define FAKE_AOT 0x008100u
#define FAKE_AOT_2 0x008200u

/* ── fakes the bridge links against (cpu_state.c provides these in prod) ── */
/* The Phase-2 manifest recorder stamps the live frame counter on each
 * discovery; the bridge references it as extern. */
int snes_frame_counter = 0;
const char *rtl_game_title(void) { return "bridge_test"; }
static Snes g_test_snes;
Snes *g_snes = &g_test_snes;
uint64_t g_apu_last_sync_master;
int g_interp_apu_driving;
bool rtl_apu_frame_timeline_active(void) { return false; }
bool sa1_cpu_irq_pending(const Sa1 *sa1) { (void)sa1; return false; }
int g_recomp_stack_top;
uint16_t g_cpu_entry_s[64];
uint8 g_memsel;
void debug_on_block_enter(uint32_t pc, uint32_t a, uint32_t x, uint32_t y) {
    (void)pc; (void)a; (void)x; (void)y;
}
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
void snes_refresh_charge(void) {}

/* Attribution-scope stubs (common_cpu_infra.c in the real runner). The test
 * doubles record what the bridge pushed so the interp scope is testable: the
 * write rings copy g_last_recomp_func / the stack at write time, and if the
 * bridge stops installing its interp@$ name, interpreted writes silently
 * re-attribute to the stale enclosing AOT frame. */
const char *g_last_recomp_func = "(none)";
static const char *g_push_log[16];
static int g_push_count = 0;
static int g_push_depth = 0;
static int g_pop_underflow = 0;
void RecompStackPush(const char *name) {
    if (g_push_count < 16) g_push_log[g_push_count] = name;
    g_push_count++;
    g_push_depth++;
}
void RecompStackPop(void) {
    if (g_push_depth <= 0) g_pop_underflow = 1;
    g_push_depth--;
}
void snes_catchupApu(Snes *snes) { (void)snes; }
void snes_sync_master_clock(Snes *snes, uint64_t master_clock) {
    (void)snes; (void)master_clock;
}
void cart_sync_coprocessors(Cart *cart, uint64_t master_clock) {
    (void)cart; (void)master_clock;
}
/* cpu_state.c isn't linked here; the bridge's constructor installs its
 * step-ring dump into this hook, so provide the slot. */
void (*g_interp_recent_dump_hook)(int n, FILE *out) = 0;
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
    (void)cpu; return RAM[(((uint32)bank << 16) | addr) & 0xFFFFFF];
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
    (void)cpu; RAM[(((uint32)bank << 16) | addr) & 0xFFFFFF] = v;
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr) {
    uint8 lo = cpu_read8(cpu, bank, addr);
    uint8 hi = cpu_read8(cpu, bank, (uint16)(addr + 1));
    return (uint16)(lo | ((uint16)hi << 8));
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v) {
    cpu_write8(cpu, bank, addr, (uint8)v);
    cpu_write8(cpu, bank, (uint16)(addr + 1), (uint8)(v >> 8));
}
void wlog_scope_enter(const char *tag) { (void)tag; }
void wlog_scope_exit(void) {}
int cpu_take_tailcall_return_context(uint16_t *entry_s, uint8_t *hrv) {
    (void)entry_s; (void)hrv; return 0;
}
void cpu_interrupt_context_enter(void) {}
void cpu_interrupt_context_leave(void) {}
int cpu_interrupt_context_active(void) { return 0; }
uint8 cpu_dispatch_inline_arg_bytes(uint32 pc24) {
    (void)pc24; return 0;
}
int cpu_dispatch_has_entry(CpuState *cpu, uint32 pc24) {
    (void)cpu;
    pc24 &= 0xFFFFFF;
    return pc24 == FAKE_AOT ||
           ((g_aot_double_rewrite || g_aot_crosses_interp_owner) &&
            pc24 == FAKE_AOT_2);
}
static int g_abandon_called;
static int g_post_return_skip;
int cpu_resolve_post_return_skip(uint16_t post_s) {
    (void)post_s; return g_post_return_skip;
}
RecompReturn cpu_unresolved_abandon_balanced(CpuState *cpu, uint32 site_pc24,
                                             uint16 entry_s, uint8 hrv) {
    (void)site_pc24; g_abandon_called++;
    cpu->S = (uint16)(entry_s + hrv);
    return RECOMP_RETURN_NORMAL;
}
RecompReturn cpu_dispatch_pc(CpuState *cpu, uint32 pc24, uint16 miss_restore) {
    if ((pc24 & 0xFFFFFF) == FAKE_AOT) {
        g_aot_called++;
        cpu->A = (uint16)(cpu->A + 0x0100);     /* observable "compiled" work */
        cpu->S = (uint16)(cpu->S + 2);          /* models RTS popping its frame */
        return RECOMP_RETURN_NORMAL;
    }
    cpu->S = miss_restore;
    return RECOMP_RETURN_NORMAL;
}
RecompReturn cpu_dispatch_pc_paired(CpuState *cpu, uint32 pc24,
                                    uint8 frame_size) {
    cpu->host_return_valid = frame_size;
    if (g_aot_skips_interp_owner && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        /* Model interpreter outer -> interpreter inner -> compiled root ->
         * compiled child. The child manually discards the generated child and
         * root frames, then its RTS consumes the interpreted inner frame and
         * lands at the outer continuation. No generated ancestor owns that
         * PC, so the active interpreter must resume it directly. */
        g_aot_called++;
        const int base = g_recomp_stack_top;
        g_recomp_stack_top += 2;
        g_cpu_entry_s[base] = cpu->S;
        cpu->S = (uint16)(cpu->S - 2);       /* root JSRs compiled child */
        g_cpu_entry_s[base + 1] = cpu->S;
        const uint16 ret_s = (uint16)(cpu->S + 4); /* expose inner's frame */
        cpu->S = (uint16)(ret_s + 2);        /* final RTS pops inner frame */
        g_owner_target_result =
            interp_bridge_return_targets_owner(ret_s, cpu->S);
        g_recomp_stack_top = base;
        if (g_owner_target_result)
            return interp_bridge_lle_yield_unwind(cpu, 0x008003);
        return RECOMP_RETURN_NORMAL;
    }
    if (g_aot_deadline_unwind && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        g_aot_called++;
        cpu->master_cycles = 200;
        if (interp_bridge_lle_master_deadline_reached(cpu))
            return interp_bridge_lle_yield_unwind(cpu, 0x008100);
        cpu->A = 0x0100;
        cpu->S = (uint16)(cpu->S + frame_size);
        return RECOMP_RETURN_NORMAL;
    }
    if (g_aot_skips_root && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        /* Model a compiled root whose nested helper consumes the root's guest
         * JSL frame and returns SKIP_1 through the root host frame. The owning
         * interpreter must consume that one skip and continue after its JSL. */
        g_aot_called++;
        cpu->S = (uint16)(cpu->S + frame_size);
        return RECOMP_RETURN_SKIP_1;
    }
    if (g_aot_double_rewrite && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        /* Computed-RTS dispatcher shape: a synthetic handler address is
         * pushed and immediately popped, so the interpreted caller's real
         * JSR frame remains on the guest stack.  Continue at the selected
         * handler through the rewritten-return bridge. */
        g_aot_called++;
        return interp_tier_dispatch_rewritten_return(cpu, 0x008300, 0x0081FE);
    }
    if (g_aot_double_rewrite && (pc24 & 0xFFFFFF) == FAKE_AOT_2) {
        /* Later in that interpreted handler, a second AOT helper performs
         * PLA; PLA; RTS: consume its own JSR frame plus the dispatcher's
         * preserved caller frame, then resume the interpreted grandparent. */
        g_aot_called++;
        cpu->S = (uint16)(cpu->S + 4);
        return interp_tier_dispatch_rewritten_return(cpu, 0x008003, 0x0082FE);
    }
    if (g_aot_tail_chain_probe && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        const int base = g_recomp_stack_top;
        g_recomp_stack_top += 3;
        g_cpu_entry_s[base] = 0x01FA;
        g_cpu_entry_s[base + 1] = 0x01FA;
        g_cpu_entry_s[base + 2] = 0x01FA;
        g_tail_chain_direct = interp_bridge_has_direct_paired_bounce();
        g_cpu_entry_s[base + 1] = 0x01F8; /* materialized nested JSR */
        g_nested_chain_direct = interp_bridge_has_direct_paired_bounce();
        g_recomp_stack_top = base;
        cpu->S = (uint16)(cpu->S + frame_size);
        return RECOMP_RETURN_NORMAL;
    }
    if (g_aot_interp_nlr && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        /* Model PLA; PLA; RTS in a direct AOT bounce: consume the AOT JSR
         * frame plus its interpreted caller's JSR frame, then continue in
         * the interpreted grandparent at $8003. */
        g_aot_called++;
        cpu->S = (uint16)(cpu->S + 4);
        return interp_tier_dispatch_rewritten_return(cpu, 0x008003, 0x0081FE);
    }
    if (g_aot_nested_rewrite && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        /* Model bridge -> compiled root -> compiled parent -> rewritten-return
         * callee.  The rewritten landing is the parent's PLB/PLP/RTL epilogue;
         * interpreting it consumes only that parent's guest frame.  SKIP_1
         * then removes the matching host parent and the compiled root resumes,
         * eventually returning normally to the bridge. */
        g_aot_called++;
        g_recomp_stack_top++;                 /* paired AOT root */
        g_cpu_entry_s[g_recomp_stack_top - 1] = cpu->S;

        cpu_write8(cpu, 0, cpu->S, 0x00); cpu->S--; /* parent JSL bank */
        cpu_write8(cpu, 0, cpu->S, 0x81); cpu->S--; /* return high */
        cpu_write8(cpu, 0, cpu->S, 0x7F); cpu->S--; /* return low ($8180) */
        cpu_write8(cpu, 0, cpu->S, cpu->P); cpu->S--;  /* parent PHP */
        cpu_write8(cpu, 0, cpu->S, cpu->DB); cpu->S--; /* parent PHB */
        g_recomp_stack_top += 2;              /* parent + rewrite callee */
        g_cpu_entry_s[g_recomp_stack_top - 2] = (uint16)(cpu->S + 2);
        g_cpu_entry_s[g_recomp_stack_top - 1] = cpu->S;

        RecompReturn r = interp_tier_dispatch_rewritten_return(
            cpu, 0x008200, 0x0081FE);
        g_recomp_stack_top -= 2;
        if (r != RECOMP_RETURN_SKIP_1) {
            g_recomp_stack_top--;
            return r;
        }

        cpu->A = (uint16)(cpu->A + 0x0100);  /* root continued after parent */
        cpu->S = (uint16)(cpu->S + frame_size); /* root returns to bridge */
        g_recomp_stack_top--;
        return RECOMP_RETURN_NORMAL;
    }
    if (g_aot_crosses_interp_owner &&
        (pc24 & 0xFFFFFF) == FAKE_AOT) {
        /* Compiled root calls an untranslated routine, creating a nested
         * interpreter below the root's live host frame. */
        g_aot_called++;
        g_recomp_stack_top++;
        g_cpu_entry_s[g_recomp_stack_top - 1] = cpu->S;
        cpu_write8(cpu, 0, cpu->S, 0x81); cpu->S--;
        cpu_write8(cpu, 0, cpu->S, 0x7F); cpu->S--;
        RecompReturn r = interp_tier_run_call_frame(
            cpu, 0x008400, 0x0081F0, 2, NULL);
        g_recomp_stack_top--;
        if (r == RECOMP_RETURN_SKIP_1)
            return RECOMP_RETURN_NORMAL;
        return r;
    }
    if (g_aot_crosses_interp_owner &&
        (pc24 & 0xFFFFFF) == FAKE_AOT_2) {
        /* Consume this call frame and the nested interpreter owner's frame,
         * leaving the rewritten continuation in the compiled root. */
        g_aot_called++;
        g_recomp_stack_top++;
        g_cpu_entry_s[g_recomp_stack_top - 1] = cpu->S;
        cpu->S = (uint16)(cpu->S + frame_size + 2);
        RecompReturn r = interp_tier_dispatch_rewritten_return(
            cpu, 0x008200, 0x0082FE);
        g_recomp_stack_top--;
        return r;
    }
    if (g_aot_rewrites_return && (pc24 & 0xFFFFFF) == FAKE_AOT) {
        g_aot_called++;
        g_recomp_stack_top++;
        cpu->S = (uint16)(cpu->S + frame_size); /* rewritten RTS/RTL popped it */
        RecompReturn r = interp_tier_dispatch_rewritten_return(
            cpu, 0x008200, 0x0081FE);
        g_recomp_stack_top--;
        return r;
    }
    return cpu_dispatch_pc(cpu, pc24, cpu->S);
}

static int g_fail = 0, g_check = 0;
#define CHECK(cond, ...) do { g_check++; if (!(cond)) { \
    g_fail++; printf("    FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static CpuState g_c;
static void init_cpu(void) {
    memset(&g_c, 0, sizeof g_c);
    g_c.S = 0x01FF; g_c.emulation = 1; g_c.m_flag = 1; g_c.x_flag = 1;
    g_c._flag_I = 1; g_c.ram = RAM; cpu_mirrors_to_p(&g_c);
}
static void load(uint32 pc24, const uint8_t *code, int len) {
    memcpy(&RAM[pc24 & 0xFFFFFF], code, (size_t)len);
}

int main(void) {
    const char *journal = "tier2_bridge_test.jsonl";
    remove(journal);
#ifdef _WIN32
    _putenv_s("SNESRECOMP_TIER2_JOURNAL", journal);
#else
    setenv("SNESRECOMP_TIER2_JOURNAL", journal, 1);
#endif
    tier2_capture_set_default_enabled(1);
    RAM = malloc(MEMSZ);

    printf("S0 APU timeline policy remains cartridge-scoped\n");
    CHECK(!interp_bridge_use_absolute_apu_timeline(false, false),
          "inactive non-SA1 timeline must use legacy catch-up");
    CHECK(!interp_bridge_use_absolute_apu_timeline(true, false),
          "active non-SA1 timeline must use legacy catch-up");
    CHECK(!interp_bridge_use_absolute_apu_timeline(false, true),
          "inactive SA1 timeline must use legacy catch-up");
    CHECK(interp_bridge_use_absolute_apu_timeline(true, true),
          "active SA1 timeline must suppress duplicate catch-up");

    /* S1: LDA #$01 ; JSR $8100 (compiled) ; RTS */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t c[] = {0xA9,0x01, 0x20,0x00,0x81, 0x60};
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);          /* sentinel return frame */
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S1 JSR-into-compiled bounce\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK(g_c.A == 0x0101, "A=%04X exp 0101 (01 from LDA + 0100 from AOT)", g_c.A);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (balanced)", g_c.S); }

    /* S2: LDA #$09 ; RTS  (no call) */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t c[] = {0xA9,0x09, 0x60};
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);
      g_push_count = 0; g_push_depth = 0; g_pop_underflow = 0;
      const char *func_before = g_last_recomp_func;
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S2 pure interp routine\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 0, "aot_called=%d exp 0", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x09, "A.lo=%02X exp 09", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF", g_c.S);
      /* Attribution scope: the run pushed its interp@$ entry name, popped it
       * on exit, and restored g_last_recomp_func. */
      CHECK(g_push_count >= 1, "push_count=%d exp >=1 (interp scope pushed)",
            g_push_count);
      CHECK(g_push_count >= 1 && g_push_log[0] &&
            strcmp(g_push_log[0], "interp@$008000") == 0,
            "pushed name '%s' exp 'interp@$008000'",
            g_push_count >= 1 && g_push_log[0] ? g_push_log[0] : "(null)");
      CHECK(g_push_depth == 0, "push_depth=%d exp 0 (balanced)", g_push_depth);
      CHECK(!g_pop_underflow, "pop underflow");
      CHECK(g_last_recomp_func == func_before,
            "g_last_recomp_func not restored after run"); }

    /* S3: JSR $8200 (NOT compiled) ; RTS  /  $8200: LDA #$33 ; RTS */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t caller[] = {0x20,0x00,0x82, 0x60};
      uint8_t callee[] = {0xA9,0x33, 0x60};
      load(0x8000, caller, sizeof caller);
      load(0x8200, callee, sizeof callee);
      cpu_push_jsr_return_frame(&g_c);
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S3 interpret-through non-compiled call\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 0, "aot_called=%d exp 0 (no compiled body)", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x33, "A.lo=%02X exp 33", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (balanced through nested RTS)", g_c.S); }

    /* S4: interp_tier_dispatch (the production tier-down entry, tail-dispatch
     * shape): a caller frame is on the stack (as after a JSR into the
     * dispatcher); the dispatched target runs and RTSes past entry. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t c[] = {0xA9,0x07, 0x60};      /* $8000: LDA #$07 ; RTS */
      load(0x8000, c, sizeof c);
      long hits0 = interp_tier_hit_count();
      cpu_push_jsr_return_frame(&g_c);       /* the (inherited) caller frame */
      RecompReturn r = interp_tier_dispatch(&g_c, 0x008000);
      printf("S4 interp_tier_dispatch (tail-dispatch entry)\n");
      CHECK(r == RECOMP_RETURN_NORMAL, "r=%d exp NORMAL(0)", (int)r);
      CHECK((g_c.A & 0xFF) == 0x07, "A.lo=%02X exp 07", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (caller frame consumed)", g_c.S);
      CHECK(interp_tier_hit_count() == hits0 + 1, "hit_count delta exp 1"); }

    /* S5: interp_tier_dispatch_balanced (SM abandon-site upgrade). A clean
     * routine interprets to completion -> NORMAL, balanced, abandon NOT used. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0; g_abandon_called = 0;
      /* Even if an unrelated ancestor would match this post-S, the bridge
       * must not consult it when the tail consumed exactly its own frame. */
      g_post_return_skip = 1;
      uint8_t c[] = {0xA9,0x0C, 0x60};      /* $8000: LDA #$0C ; RTS */
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);       /* inherited caller frame (hrv=2) */
      uint16 entry_s = g_c.S;                /* function entry S = after caller's push */
      RecompReturn r = interp_tier_dispatch_balanced(&g_c, 0x008000, 0x00C0DE,
                                                     entry_s, 2);
      printf("S5 interp_tier_dispatch_balanced (clean -> interpret, no abandon)\n");
      CHECK(r == RECOMP_RETURN_NORMAL, "r=%d exp NORMAL", (int)r);
      CHECK((g_c.A & 0xFF) == 0x0C, "A.lo=%02X exp 0C (interpreted)", g_c.A & 0xFF);
      CHECK(g_abandon_called == 0, "abandon_called=%d exp 0 (clean interp)", g_abandon_called);
      CHECK(g_c.S == (uint16)(entry_s + 2), "S=%04X exp %04X (frame popped)", g_c.S, (uint16)(entry_s + 2)); }

    /* S5b: a shared suffix interpreted by the balanced tail tier performs a
     * guest non-local return (PLA; PLA; RTS).  It consumes the current return
     * frame and then returns through a compiled ancestor, so the bridge must
     * propagate the resolver's SKIP_N instead of resuming that ancestor. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_abandon_called = 0;
      g_post_return_skip = 1;
      uint8_t c[] = {0x68,0x68,0x60};        /* PLA ; PLA ; RTS */
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);       /* ancestor's return frame */
      cpu_push_jsr_return_frame(&g_c);       /* current function's frame */
      uint16 entry_s = g_c.S;
      RecompReturn r = interp_tier_dispatch_balanced(&g_c, 0x008000, 0x00C0DE,
                                                     entry_s, 2);
      printf("S5b balanced tail propagates interpreted non-local return\n");
      CHECK(r == RECOMP_RETURN_SKIP_1, "r=%d exp SKIP_1", (int)r);
      CHECK(g_abandon_called == 0, "abandon_called=%d exp 0 (clean interp)", g_abandon_called);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (both frames consumed)", g_c.S); }

    /* S6: rewritten return enters the caller internally. The interpreter
     * consumes that caller's frame, so the bridge must propagate SKIP_1
     * instead of letting the host resume and execute its epilogue twice. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_post_return_skip = 1;
      uint8_t c[] = {0x60};                    /* internal caller PC: RTS */
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);
      RecompReturn r = interp_tier_dispatch_rewritten_return(
          &g_c, 0x008000, 0x00C0DF);
      printf("S6 rewritten return skips consumed host caller\n");
      CHECK(r == RECOMP_RETURN_SKIP_1, "r=%d exp SKIP_1", (int)r);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (caller frame consumed once)", g_c.S); }

    /* S6b: a direct AOT bounce non-locally returns through an interpreted
     * caller. The owning ordinary (non-scheduler) interpreter must resume at
     * the grandparent's real continuation and never execute the skipped
     * inner continuation. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_interp_nlr = 1;
      uint8_t outer[] = {0x20,0x00,0x82, 0xA9,0x5A, 0x60};
      uint8_t inner[] = {0x20,0x00,0x81, 0xA9,0xEE, 0x60};
      load(0x8000, outer, sizeof outer);
      load(0x8200, inner, sizeof inner);
      cpu_push_jsr_return_frame(&g_c);       /* outer host sentinel */
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S6b AOT NLR resumes owning ordinary interpreter\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x5A,
            "A.lo=%02X exp 5A (inner continuation skipped)", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (all frames balanced)", g_c.S);
      g_aot_interp_nlr = 0; }

    /* S6bb: an AOT child non-locally returns through its compiled parent and
     * an interpreted inner caller, landing in the interpreted outer caller.
     * The generated-only ancestor table cannot see either interpreted frame;
     * the mixed-tier resolver must unwind the generated bounce and resume the
     * real popped continuation in the existing interpreter. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_skips_interp_owner = 1; g_owner_target_result = 0;
      uint8_t outer[] = {0x20,0x00,0x82, 0xA9,0x5A, 0x60};
      uint8_t inner[] = {0x20,0x00,0x81, 0xA9,0xEE, 0x60};
      load(0x8000, outer, sizeof outer);
      load(0x8200, inner, sizeof inner);
      cpu_push_jsr_return_frame(&g_c);
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S6bb nested AOT NLR resumes interpreted grandparent\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK(g_owner_target_result == 1,
            "owner_target=%d exp 1", g_owner_target_result);
      CHECK((g_c.A & 0xFF) == 0x5A,
            "A.lo=%02X exp 5A (inner continuation skipped)",
            g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (all frames balanced)", g_c.S);
      g_aot_skips_interp_owner = 0; }

    /* S6c: LttP's sprite dispatch shape performs two rewritten AOT returns in
     * one interpreted call chain.  The first computed RTS preserves the
     * caller frame and selects an interpreted handler; a later AOT helper
     * consumes its own frame plus that preserved frame.  Both continuations
     * belong to the same owning interpreter.  The wrapper epilogue must run
     * once, never once in a nested bridge and again in its owner. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_double_rewrite = 1;
      uint8_t outer[] = {0x20,0x00,0x81, 0xA9,0x5A, 0x60};
      uint8_t handler[] = {0x20,0x00,0x82, 0xA9,0xEE, 0x60};
      load(0x8000, outer, sizeof outer);
      load(0x8300, handler, sizeof handler);
      cpu_push_jsr_return_frame(&g_c);
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S6c consecutive rewritten AOT returns stay in one interpreter\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 2, "aot_called=%d exp 2", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x5A,
            "A.lo=%02X exp 5A (handler continuation skipped)", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (all frames balanced)", g_c.S);
      g_aot_double_rewrite = 0; }

    /* S7: an interrupt-owned stable-value poll must cooperatively yield at
     * CMP while the sampled WRAM byte is unchanged, then resume and return
     * normally after the next frame changes it. This is the canonical shape
     * used by Super Metroid's message-box setup during ship entry. */
    { memset(RAM, 0, MEMSZ); init_cpu();
      uint8_t c[] = {
          0xAD,0x10,0x00,                    /* LDA $0010 */
          0xCD,0x10,0x00,                    /* CMP $0010 */
          0xF0,0xFB,                         /* BEQ CMP */
          0x60                               /* RTS */
      };
      load(0x8000, c, sizeof c); RAM[0x10] = 0x34;
      cpu_push_jsr_return_frame(&g_c);
      int rc1 = interp_bridge_run_loop(&g_c, 0x008000, 0x008003, 0x0020, 0xFF);
      printf("S7 interrupt-owned stable-value poll yields and resumes\n");
      CHECK(rc1 == 1, "first rc=%d exp 1 (clean cooperative yield)", rc1);
      CHECK((g_c.A & 0xFF) == 0x34, "A.lo=%02X exp 34 (sample retained)", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FD, "yield S=%04X exp 01FD (caller frame retained)", g_c.S);
      CHECK(interp_bridge_lle_resume_pc() == 0x008003,
            "resume=$%06X exp $008003 (CMP)",
            (unsigned)interp_bridge_lle_resume_pc());
      RAM[0x10] = 0x35;                      /* models the next frame's NMI */
      int rc2 = interp_bridge_run_loop(&g_c, interp_bridge_lle_resume_pc(),
                                       0x008003, 0x0020, 0xFF);
      CHECK(rc2 == 1, "second rc=%d exp 1 (poll exits)", rc2);
      CHECK(g_c.S == 0x01FF, "return S=%04X exp 01FF (balanced)", g_c.S); }

    /* S7b: host depth alone must not hide interpreter ownership across a
     * pure architectural tail chain. Every tail callee inherits the paired
     * root's entry-S watermark; a real nested JSR introduces a lower one. */
    { memset(RAM, 0, MEMSZ); init_cpu();
      g_aot_tail_chain_probe = 1;
      g_tail_chain_direct = g_nested_chain_direct = -1;
      uint8_t caller[] = {0x20,0x00,0x81, 0x60}; /* JSR fake AOT; RTS */
      load(0x8000, caller, sizeof caller);
      cpu_push_jsr_return_frame(&g_c);
      int rc = interp_bridge_run(&g_c, 0x008000);
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_tail_chain_direct == 1,
            "tail-chain direct=%d exp 1", g_tail_chain_direct);
      CHECK(g_nested_chain_direct == 0,
            "nested-chain direct=%d exp 0", g_nested_chain_direct);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF", g_c.S);
      g_aot_tail_chain_probe = 0; }

    /* S8: when an AOT callee bounced from an LLE interpreter frame rewrites
     * its return address, the rewritten continuation belongs to that active
     * interpreter. It must not be run in a nested tier frame and converted to
     * SKIP_1 (there is no compiled guest parent to skip). */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_rewrites_return = 1;
      uint8_t caller[] = {0x20,0x00,0x81};       /* JSR fake AOT */
      uint8_t continuation[] = {
          0xA9,0x5A,                             /* rewritten landing: LDA #$5A */
          0xAD,0x20,0x00, 0xD0,0xFB             /* scheduler yield loop */
      };
      load(0x8000, caller, sizeof caller);
      load(0x8200, continuation, sizeof continuation);
      RAM[0x20] = 0;
      int rc = interp_bridge_run_loop(&g_c, 0x008000, 0x008202, 0x0020, 0);
      printf("S8 LLE bounce resumes a rewritten return in its interpreter\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x5A,
            "A.lo=%02X exp 5A (rewritten continuation executed)", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (bounce frame consumed once)", g_c.S);
      g_aot_rewrites_return = 0; }

    /* S8b: an AOT root reached from the LLE scheduler can non-locally return
     * through its own compiled host frame while still landing normally in the
     * interpreted scheduler. SKIP_1 is consumed at that mixed-tier boundary;
     * abandoning the scheduler here leaves its current task permanently
     * marked running. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_skips_root = 1;
      uint8_t scheduler[] = {
          0x22,0x00,0x81,0x00,                 /* JSL fake compiled root */
          0xA9,0x5A,                           /* scheduler continuation */
          0xAD,0x20,0x00, 0xD0,0xFB            /* cooperative wait loop */
      };
      load(0x8000, scheduler, sizeof scheduler);
      RAM[0x20] = 0;
      int rc = interp_bridge_run_loop(&g_c, 0x008000, 0x008006, 0x0020, 0);
      printf("S8b scheduler consumes AOT-root SKIP_1\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x5A,
            "A.lo=%02X exp 5A (scheduler continuation executed)", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (JSL frame consumed once)", g_c.S);
      g_aot_skips_root = 0; }

    /* S8c: a compiled callee that reaches the host's master deadline while
     * bounced from scheduler mode must return to the host. It must not be
     * treated like a cooperative yield primitive, because that would continue
     * interpreting past the host's expired bound. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_deadline_unwind = 1;
      interp_bridge_set_master_deadline(100);
      uint8_t scheduler[] = {
          0x22,0x00,0x81,0x00,                 /* JSL fake compiled root */
          0xA9,0x5A,                           /* must not execute */
          0xAD,0x20,0x00, 0xD0,0xFB            /* cooperative wait loop */
      };
      load(0x8000, scheduler, sizeof scheduler);
      RAM[0x20] = 0;
      int rc = interp_bridge_run_loop(&g_c, 0x008000, 0x008006, 0x0020, 0);
      printf("S8c scheduler deadline unwind returns to host\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x00,
            "A.lo=%02X exp 00 (scheduler continuation not executed)",
            g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FC,
            "S=%04X exp 01FC (compiled JSL frame retained)", g_c.S);
      CHECK(interp_bridge_lle_resume_pc() == 0x008100,
            "resume=$%06X exp $008100 (compiled deadline resume)",
            (unsigned)interp_bridge_lle_resume_pc());
      interp_bridge_set_master_deadline(0);
      g_aot_deadline_unwind = 0; }

    /* S9: the same rewrite below the paired AOT root belongs to a compiled
     * ancestor, not directly to the scheduler interpreter.  Finish that
     * ancestor's epilogue in the nested tier, propagate SKIP_1 through its
     * host frame, then let the root return normally to the bridge. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_nested_rewrite = 1;
      uint8_t caller[] = {
          0x22,0x00,0x81,0x00,                 /* JSL fake compiled root */
          0xAD,0x20,0x00, 0xD0,0xFB            /* scheduler yield loop */
      };
      uint8_t parent_epilogue[] = {0xAB,0x28,0x6B}; /* PLB; PLP; RTL */
      load(0x8000, caller, sizeof caller);
      load(0x8200, parent_epilogue, sizeof parent_epilogue);
      RAM[0x20] = 0;
      int rc = interp_bridge_run_loop(&g_c, 0x008000, 0x008004, 0x0020, 0);
      printf("S9 nested AOT rewritten return resumes compiled ancestor\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK(g_c.A == 0x0100,
            "A=%04X exp 0100 (compiled root resumed after parent)", g_c.A);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (all guest frames balanced)", g_c.S);
      CHECK(g_recomp_stack_top == 0, "recomp depth=%d exp 0", g_recomp_stack_top);
      g_aot_nested_rewrite = 0; }

    /* S9b: a direct nested bounce can rewrite past the interpreter owner's
     * stack boundary into a compiled ancestor. That ancestor epilogue must
     * execute in a nested tier and propagate SKIP_1, rather than being resumed
     * by both the interpreter and its compiled host frame. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      g_aot_crosses_interp_owner = 1;
      g_post_return_skip = 1;
      uint8_t caller[] = {0x20,0x00,0x81, 0xA9,0x5A, 0x60};
      uint8_t ancestor_epilogue[] = {0x60};
      uint8_t nested[] = {0x20,0x00,0x82, 0xA9,0xEE, 0x60};
      load(0x8000, caller, sizeof caller);
      load(0x8200, ancestor_epilogue, sizeof ancestor_epilogue);
      load(0x8400, nested, sizeof nested);
      cpu_push_jsr_return_frame(&g_c);
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S9b rewritten return crossing interpreter owner skips ancestor\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 2, "aot_called=%d exp 2", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x5A,
            "A.lo=%02X exp 5A (compiled root consumed once)", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (ancestor consumed once)", g_c.S);
      CHECK(g_recomp_stack_top == 0, "recomp depth=%d exp 0", g_recomp_stack_top);
      g_aot_crosses_interp_owner = 0; }

    /* S10: a synchronous message box waits for fresh automatic-joypad data
     * by polling $4218/$4219. With no input it must yield to the host and
     * resume at the start of the hardware poll; once a button appears, it
     * exits through the original RTS with the caller frame balanced. */
    { memset(RAM, 0, MEMSZ); init_cpu();
      uint8_t c[] = {
          0xAD,0x12,0x42,                    /* LDA $4212 */
          0x89,0x01,                         /* BIT #$01 */
          0xD0,0xF9,                         /* BNE start */
          0xAD,0x18,0x42,                    /* LDA $4218 */
          0xD0,0x05,                         /* BNE done */
          0xAD,0x19,0x42,                    /* LDA $4219 */
          0xF0,0xEF,                         /* BEQ start */
          0x60                               /* done: RTS */
      };
      uint8_t scheduler_wait[] = {
          0xAD,0x20,0x00, 0xD0,0xFB          /* LDA $20; BNE self */
      };
      load(0x8000, c, sizeof c);
      load(0x8100, scheduler_wait, sizeof scheduler_wait);
      RAM[0x4212] = 0; RAM[0x4218] = 0; RAM[0x4219] = 0;
      /* Real JSR frame returning to the scheduler wait at $8100. */
      cpu_write8(&g_c, 0, g_c.S, 0x80); g_c.S--;
      cpu_write8(&g_c, 0, g_c.S, 0xFF); g_c.S--;
      int rc1 = interp_bridge_run_loop(&g_c, 0x008000,
                                       0x008100, 0x0020, 0);
      printf("S10 automatic-joypad wait yields and resumes\n");
      CHECK(rc1 == 1, "first rc=%d exp 1 (input wait yielded)", rc1);
      CHECK(g_c.S == 0x01FD, "yield S=%04X exp 01FD (caller retained)", g_c.S);
      CHECK(interp_bridge_lle_resume_pc() == 0x008000,
            "resume=$%06X exp $008000 (re-read joypad)",
            (unsigned)interp_bridge_lle_resume_pc());
      RAM[0x4218] = 0x80;                    /* host supplies a button */
      int rc2 = interp_bridge_run_loop(&g_c, interp_bridge_lle_resume_pc(),
                                       0x008100, 0x0020, 0);
      CHECK(rc2 == 1, "second rc=%d exp 1 (input observed)", rc2);
      CHECK(g_c.S == 0x01FF, "return S=%04X exp 01FF (balanced)", g_c.S); }

    /* S11: a dispatch-table row with no exact AOT M/X body is a known LLE
     * entry, not a mid-caller continuation.  cpu_dispatch_pc_from invokes
     * this bridge after the prior RTS already popped, so the current S is the
     * target's unwind watermark and the inherited return frame is consumed. */
    { memset(RAM, 0, MEMSZ); init_cpu();
      uint8_t c[] = {0xA9,0x44, 0x60};       /* LDA #$44 ; RTS */
      load(0x8300, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);       /* inherited target return frame */
      RecompReturn r = interp_tier_dispatch_popped_return(
          &g_c, 0x008300, 0x0082FE, 0x01FF);
      printf("S11 known non-AOT dispatch row executes exact LLE\n");
      CHECK(r == RECOMP_RETURN_NORMAL, "r=%d exp NORMAL", (int)r);
      CHECK((g_c.A & 0xFF) == 0x44, "A.lo=%02X exp 44", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (inherited frame consumed)", g_c.S); }

    /* S12: the production feedback set must grow beyond the historical 4096
     * ceiling without dropping tuples. Kind is part of the tuple key, too. */
    { int before = 0, after = 0;
      interp_tier2_stats(&before, NULL, NULL);
      for (unsigned i = 0; i < 4352; ++i)
          Tier2CoverageTestRecord(0xC00000u + i, 0xC10000u + i,
                                  (uint8_t)(i & 3), 3, 1);
      Tier2CoverageTestRecord(0xD00000u, 0xD10000u, 3, 3, 1);
      Tier2CoverageTestRecord(0xD00000u, 0xD10000u, 3, 4, 1);
      interp_tier2_stats(&after, NULL, NULL);
      printf("S12 growable, kind-exact coverage set\n");
      CHECK(after == before + 4354, "sites=%d exp %d", after, before + 4354); }

    /* S13: a runtime-pointer JSR falls back to the interpreter at a state
     * handler whose 16-bit PLA consumes that inner JSR frame, then RTL
     * consumes the compiled caller's outer JSL frame. This is a clean guest
     * non-local return, not a balanced call return, so propagate at least
     * SKIP_1 even when the synthetic resolver has no matching ancestor. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_post_return_skip = 0;
      g_c.emulation = 0; g_c.m_flag = 0; g_c.x_flag = 0;
      cpu_mirrors_to_p(&g_c);
      uint8_t c[] = {0x68,0x6B};              /* PLA (16-bit) ; RTL */
      load(0x8400, c, sizeof c);
      cpu_push_jsl_return_frame(&g_c);        /* compiled caller's outer frame */
      cpu_push_jsr_return_frame(&g_c);        /* runtime dispatch's call frame */
      RecompReturn r = interp_tier_run_call_frame(
          &g_c, 0x008400, 0x0083FC, 2, NULL);
      printf("S13 runtime call propagates interpreted PLA; RTL NLR\n");
      CHECK(r == RECOMP_RETURN_SKIP_1, "r=%d exp SKIP_1", (int)r);
      CHECK(g_c.S == 0x01FF,
            "S=%04X exp 01FF (inner JSR and outer JSL consumed)", g_c.S); }

    printf("\n==== interp_bridge Phase-1: %d/%d checks passed ====\n", g_check - g_fail, g_check);
    if (g_fail) { printf("RESULT: FAIL (%d)\n", g_fail); return 1; }
    tier2_capture_close();
    remove(journal);
    printf("RESULT: PASS\n");
    return 0;
}

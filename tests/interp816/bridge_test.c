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
 *
 * Build/run: tests/interp816/run.sh (WSL gcc). Validation only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interp_bridge.h"   /* -> cpu_state.h (types, inline frame helpers) */
#include "snes.h"            /* Snes storage for the bridge's APU clock hook */

#define MEMSZ 0x1000000u
static uint8_t *RAM;
static int      g_aot_called;
static int      g_aot_rewrites_return;
static int      g_aot_nested_rewrite;
static int      g_aot_interp_nlr;
#define FAKE_AOT 0x008100u

/* ── fakes the bridge links against (cpu_state.c provides these in prod) ── */
/* The Phase-2 manifest recorder stamps the live frame counter on each
 * discovery; the bridge references it as extern. */
int snes_frame_counter = 0;
static Snes g_test_snes;
Snes *g_snes = &g_test_snes;
uint64_t g_apu_last_sync_master;
int g_interp_apu_driving;
int g_recomp_stack_top;
uint8 g_memsel;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
void snes_catchupApu(Snes *snes) { (void)snes; }
void snes_sync_master_clock(Snes *snes, uint64_t master_clock) {
    (void)snes; (void)master_clock;
}
void cart_sync_coprocessors(Cart *cart, uint64_t master_clock) {
    (void)cart; (void)master_clock;
}
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
    (void)cpu; return (pc24 & 0xFFFFFF) == FAKE_AOT;
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

        cpu_write8(cpu, 0, cpu->S, 0x00); cpu->S--; /* parent JSL bank */
        cpu_write8(cpu, 0, cpu->S, 0x81); cpu->S--; /* return high */
        cpu_write8(cpu, 0, cpu->S, 0x7F); cpu->S--; /* return low ($8180) */
        cpu_write8(cpu, 0, cpu->S, cpu->P); cpu->S--;  /* parent PHP */
        cpu_write8(cpu, 0, cpu->S, cpu->DB); cpu->S--; /* parent PHB */
        g_recomp_stack_top += 2;              /* parent + rewrite callee */

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
    RAM = malloc(MEMSZ);

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
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S2 pure interp routine\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 0, "aot_called=%d exp 0", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x09, "A.lo=%02X exp 09", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF", g_c.S); }

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

    printf("\n==== interp_bridge Phase-1: %d/%d checks passed ====\n", g_check - g_fail, g_check);
    if (g_fail) { printf("RESULT: FAIL (%d)\n", g_fail); return 1; }
    printf("RESULT: PASS\n");
    return 0;
}

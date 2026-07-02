/*
 * cosim.c -- differential co-simulation park/step engine + TCP server
 * (SNES_COSIM.md). Frame-keyed checkpoint lockstep: the guest PARKS at every
 * checkpoint boundary (every SNES_COSIM_STRIDE frames) and advances only when
 * the coordinator grants budget via `step N`. Compiled into BOTH the recomp
 * `snes-cosim` build and the `snes-cosim-ref` build (shared with cosim_state.c).
 *
 * DEV/DIAGNOSTICS ONLY (`#ifdef SNES_COSIM`); never in a shipping config.
 *
 * v1 checkpoints on FRAME boundaries (guest-aligned, no regen): both sides stop
 * after completing the same guest frame. Finer intra-frame drilling (block-
 * leader checkpoints, needs regen of a cosim_block(pc) hook) is a documented v2.
 */
#ifdef SNES_COSIM
#include "cosim_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET sock_t;
#  define BADSOCK   INVALID_SOCKET
#  define CLOSESOCK closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   typedef int sock_t;
#  define BADSOCK   (-1)
#  define CLOSESOCK close
#endif

#define FNV_OFF 1469598103934665603ULL
#define FNV_PRM 1099511628211ULL
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= FNV_PRM; }
    return h;
}

#define RING 512
typedef struct {
    uint64_t cp, ruler, combined, chain;
    uint32_t leader;
} CpRow;

static CpRow    s_ring[RING];
static uint64_t s_cp      = 0;             /* checkpoints completed */
static uint64_t s_chain   = FNV_OFF;       /* cumulative chain hash */
static uint64_t s_stride  = 1;             /* checkpoint every N frames */
static uint64_t s_frame   = 0;             /* frames seen */
static long     s_budget  = 0;             /* checkpoints left to run before park */
static int      s_pending_reply = 0;       /* a `step` is awaiting its parked reply */
static int      s_audit_period  = 0;       /* hash-vs-byte audit cadence (gate 4) */

/* Instruction-granular lockstep (SNES_COSIM_SYNC_PC): frame-model + cycle-model
 * differences between the recomp A-side (compiled prefix + interp) and the
 * pure-interp B-side make frame/master-cycle strides misalign. When a sync PC
 * (low-16) is set, checkpointing switches to per-interpreted-opcode: both sides
 * reach the sync PC in identical guest state, then interpret the SAME opcodes,
 * so a per-opcode cpu/ram compare pinpoints the exact divergence. */
static uint32_t s_sync_pc16   = 0xFFFFFFFFu;  /* disabled sentinel */
static uint64_t s_istride     = 1;            /* checkpoint every N interpreted opcodes */
static int      s_insn_armed  = 0;            /* sync PC reached */
static uint64_t s_insn        = 0;            /* interpreted opcodes since arm */

static sock_t s_listen = BADSOCK, s_client = BADSOCK;
static int    s_inited = 0;
static CosimSnapshot s_last;               /* snapshot at the current park point */

/* ── socket line I/O ────────────────────────────────────────────────────── */
static void sock_send(const char *s) {
    if (s_client == BADSOCK) return;
    send(s_client, s, (int)strlen(s), 0);
}
static void sendf(const char *fmt, ...) {
    char buf[4096];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sock_send(buf);
}
/* Read one '\n'-terminated line (blocking). Returns 0 on EOF/err. */
static int recv_line(char *out, int cap) {
    int n = 0;
    for (;;) {
        char c;
        int r = recv(s_client, &c, 1, 0);
        if (r <= 0) return 0;
        if (c == '\r') continue;
        if (c == '\n') { out[n] = 0; return 1; }
        if (n < cap - 1) out[n++] = c;
    }
}

/* ── init: listen + accept the single coordinator client ────────────────── */
void cosim_init(void) {
    const char *sp = getenv("SNES_COSIM_STRIDE");
    const char *pp = getenv("SNES_COSIM_PORT");
    const char *ap = getenv("SNES_COSIM_AUDIT");
    if (sp && atoi(sp) > 0) s_stride = (uint64_t)atoll(sp);
    if (ap && atoi(ap) > 0) s_audit_period = atoi(ap);
    const char *syp = getenv("SNES_COSIM_SYNC_PC");
    const char *isp = getenv("SNES_COSIM_ISTRIDE");
    if (syp && syp[0]) s_sync_pc16 = (uint32_t)(strtoul(syp, NULL, 0) & 0xFFFFu);
    if (isp && atoi(isp) > 0) s_istride = (uint64_t)atoll(isp);
    int port = (pp && atoi(pp) > 0) ? atoi(pp) : 4500;

#ifdef _WIN32
    WSADATA wsa; if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "[cosim] WSAStartup failed\n"); return; }
#endif
    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen == BADSOCK) { fprintf(stderr, "[cosim] socket() failed\n"); return; }
    int yes = 1; setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    if (bind(s_listen, (struct sockaddr *)&a, sizeof a) != 0) { fprintf(stderr, "[cosim] bind(%d) failed\n", port); return; }
    if (listen(s_listen, 1) != 0) { fprintf(stderr, "[cosim] listen failed\n"); return; }
    fprintf(stderr, "[cosim] listening on 127.0.0.1:%d (stride=%llu frames) — waiting for coordinator\n",
            port, (unsigned long long)s_stride);
    s_client = accept(s_listen, NULL, NULL);
    if (s_client == BADSOCK) { fprintf(stderr, "[cosim] accept failed\n"); return; }
    s_inited = 1;
    fprintf(stderr, "[cosim] coordinator connected\n");
}

/* ── ring ───────────────────────────────────────────────────────────────── */
static void ring_push(const CosimSnapshot *s) {
    CpRow *r = &s_ring[s_cp % RING];
    r->cp = s_cp; r->ruler = s->master_cycles; r->combined = s->combined;
    r->chain = s_chain; r->leader = s->last_leader_pc;
}

/* Hash-vs-byte audit (gate 4): recompute combined from a fresh snapshot and
 * compare to the chained one. Any mismatch = incremental-hash / read bug. */
static void maybe_audit(const CosimSnapshot *s) {
    if (!s_audit_period || (s_cp % s_audit_period)) return;
    CosimSnapshot v; cosim_state_snapshot(&v);
    if (v.combined != s->combined)
        sendf("AUDIT-FAIL cp=%llu a=%016llX b=%016llX\n",
              (unsigned long long)s_cp, (unsigned long long)s->combined,
              (unsigned long long)v.combined);
}

/* ── command service: park here until a `step` grants budget ────────────── */
static void serve_until_step(void) {
    char line[256];
    while (recv_line(line, sizeof line)) {
        if (!strncmp(line, "step", 4)) {
            long n = atol(line + 4);
            if (n < 1) n = 1;
            s_budget = n;
            s_pending_reply = 1;
            return;                     /* let the guest run */
        } else if (!strcmp(line, "status")) {
            sendf("cp=%llu ruler=%llu frame=%llu parked=1\n",
                  (unsigned long long)s_cp, (unsigned long long)cosim_state_ruler(),
                  (unsigned long long)s_frame);
        } else if (!strcmp(line, "chain")) {
            sendf("chain=%016llX cp=%llu ruler=%llu\n",
                  (unsigned long long)s_chain, (unsigned long long)s_cp,
                  (unsigned long long)cosim_state_ruler());
        } else if (!strcmp(line, "sub")) {
            sendf("combined=%016llX", (unsigned long long)s_last.combined);
            for (int i = 0; i < COSIM_SUB_COUNT; i++)
                sendf(" %s=%016llX", cosim_sub_names[i], (unsigned long long)s_last.sub[i]);
            sendf(" cyc=%llu mcyc=%llu pc=%06X\n",
                  (unsigned long long)s_last.cycles, (unsigned long long)s_last.master_cycles,
                  s_last.last_leader_pc);
        } else if (!strcmp(line, "cpu")) {
            char b[512]; cosim_state_dump_cpu(b, sizeof b); sendf("%s\n", b);
        } else if (!strcmp(line, "dev")) {
            char b[512]; cosim_state_dump_dev(b, sizeof b); sendf("%s\n", b);
        } else if (!strncmp(line, "window", 6)) {
            long n = atol(line + 6); if (n < 1) n = 16; if (n > RING) n = RING;
            for (long i = n - 1; i >= 0; i--) {
                if ((uint64_t)i > s_cp) continue;
                CpRow *r = &s_ring[(s_cp - i) % RING];
                sendf("win cp=%llu ruler=%llu combined=%016llX chain=%016llX pc=%06X\n",
                      (unsigned long long)r->cp, (unsigned long long)r->ruler,
                      (unsigned long long)r->combined, (unsigned long long)r->chain, r->leader);
            }
            sendf("win-end\n");
        } else if (!strncmp(line, "inject ram", 10)) {
            unsigned addr, val;
            if (sscanf(line + 10, "%x %x", &addr, &val) == 2 &&
                cosim_state_inject_ram(addr, (uint8_t)val) == 0) sendf("ok\n");
            else sendf("err\n");
        } else if (!strncmp(line, "inject reg", 10)) {
            char reg[8]; unsigned val;
            if (sscanf(line + 10, "%7s %x", reg, &val) == 2 &&
                cosim_state_inject_reg(reg, val) == 0) sendf("ok\n");
            else sendf("err\n");
        } else if (!strcmp(line, "reset")) {
            s_cp = 0; s_frame = 0; s_chain = FNV_OFF; s_budget = 0;
            memset(s_ring, 0, sizeof s_ring);
            sendf("ok\n");
        } else {
            sendf("err unknown\n");
        }
    }
    /* client hung up: let the guest free-run to exit */
    s_inited = 0;
}

/* Take a snapshot at the current checkpoint, chain it, maybe park. */
static void checkpoint(void) {
    cosim_state_snapshot(&s_last);
    s_cp++;
    s_chain = fnv(s_chain, &s_cp, sizeof s_cp);
    s_chain = fnv(s_chain, &s_last.master_cycles, sizeof s_last.master_cycles);
    s_chain = fnv(s_chain, &s_last.combined, sizeof s_last.combined);
    ring_push(&s_last);
    maybe_audit(&s_last);

    if (s_budget > 0) s_budget--;
    if (s_budget == 0) {
        if (s_pending_reply) {
            sendf("parked cp=%llu ruler=%llu chain=%016llX pc=%06X\n",
                  (unsigned long long)s_cp, (unsigned long long)s_last.master_cycles,
                  (unsigned long long)s_chain, s_last.last_leader_pc);
            s_pending_reply = 0;
        }
        serve_until_step();
    }
}

/* Called by both builds at every completed guest frame. */
void cosim_frame(void) {
    if (!s_inited) return;
    if (s_sync_pc16 != 0xFFFFFFFFu) return;   /* instruction mode: frames don't checkpoint */
    s_frame++;
    if (s_frame % s_stride) return;   /* only checkpoint every `stride` frames */
    checkpoint();
}

/* Called by both builds per interpreted opcode (interp_bridge / ref_driver).
 * Once the guest reaches the sync PC (low-16), checkpoint every s_istride
 * opcodes — instruction-granular lockstep. No-op until sync PC is set + hit. */
void cosim_insn(uint32_t pc24) {
    if (!s_inited || s_sync_pc16 == 0xFFFFFFFFu) return;
    if (!s_insn_armed) {
        if ((pc24 & 0xFFFFu) != s_sync_pc16) return;  /* bank-agnostic (LoROM $00/$80 mirror) */
        s_insn_armed = 1;
    }
    s_insn++;
    if (s_insn % s_istride) return;
    checkpoint();
}

/* Optional: pump the first park before any frame (so the coordinator can query
 * initial state / inject before frame 1). The recomp/ref may call this right
 * after cosim_init() + reset. Harmless if unused. */
void cosim_prime(void) {
    if (s_inited && s_cp == 0) checkpoint();
}

#endif /* SNES_COSIM */

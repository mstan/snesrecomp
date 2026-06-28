#ifndef CPU_DIFF_H
#define CPU_DIFF_H
#include "cpu_state.h"

/* One opcode-under-test: its recompiled function + the raw bytes interp816
 * executes, plus the (m,x) width context both run under. */
typedef struct {
    const char *name;
    uint8_t code[4];
    int len;
    RecompReturn (*fn)(CpuState *);
    uint8_t m, x;
} OpTest;

extern const OpTest g_ops[];
extern const int g_nops;
#endif

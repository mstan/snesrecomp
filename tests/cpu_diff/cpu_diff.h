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
    uint8_t wmem;  /* 1 = writes memory (store/RMW): also diff the RAM window */
    uint8_t idx;   /* 1 = index-addressed: bound X/Y so the EA stays in WRAM */
    uint8_t ind;   /* indirect: plant a pointer at the dp slot.
                    *   1 = 16-bit ptr at D+dp  ((dp), (dp),Y)
                    *   2 = 24-bit ptr at D+dp  ([dp], [dp],Y)
                    *   3 = 16-bit ptr at D+dp+X ((dp,X)) */
    uint8_t far;   /* 1 = long ($C0:FFF0-based): use the far window ($C0-$C1),
                    * which exercises ROM-bank addressing + long,X bank-carry */
} OpTest;

extern const OpTest g_ops[];
extern const int g_nops;
#endif

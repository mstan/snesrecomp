/*
 * snes_oracle_backend.h — pluggable emulator-oracle backend interface.
 *
 * Oracle builds embed one or more third-party SNES emulators inside
 * smw.exe for side-by-side comparison against the recompiled code.
 * All emu_* TCP commands route through g_active_backend — a single
 * function-pointer table — so a new backend (snes9x, bsnes, mesen)
 * drops in additively without touching the command layer.
 *
 * Only the Oracle MSBuild configuration compiles any of this; the
 * production Release|x64 binary is byte-for-byte unaffected.
 */
#ifndef SNES_ORACLE_BACKEND_H
#define SNES_ORACLE_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 65816 register snapshot. Matches snes9x's view; other backends
 * normalize to this shape. */
typedef struct {
    uint16_t a, x, y, s, d, pc;
    uint8_t  db;                /* data bank */
    uint8_t  pb;                /* program bank */
    uint8_t  p;                 /* processor status */
    uint8_t  emulation_mode;    /* 1 = 6502 emulation mode, 0 = native */
} SnesCpuRegs;

typedef struct snes_oracle_backend {
    const char *name;                                   /* "snes9x", "bsnes", ... */
    int   (*init)(const char *rom_path);                /* 0 on success, negative on failure */
    void  (*run_frame)(uint16_t joypad1, uint16_t joypad2);
    void  (*shutdown)(void);
    int   (*is_loaded)(void);                           /* 1 if ROM is live */

    /* Memory peeks. Output buffers sized by caller. */
    void    (*get_wram)(uint8_t *out_128k);             /* bank 7E:7F, 128 KB */
    uint8_t (*cpu_read)(uint32_t addr24);               /* full 24-bit bus read */
    void    (*get_cpu_regs)(SnesCpuRegs *out);
    /* get_vram / get_cgram / get_oam / get_framebuf_argb / get_ppu_regs
     * added here as the matching emu_* TCP commands ship. */
} snes_oracle_backend_t;

/* The currently-selected backend. NULL before first init, or if no
 * backend compiled in. emu_oracle_cmds dispatches through this. */
extern const snes_oracle_backend_t *g_active_backend;

/* Call once at startup (under ENABLE_ORACLE_BACKEND). Picks a default
 * backend from the compiled-in set, initializes it with rom_path,
 * and sets g_active_backend. Returns 0 on success. */
int snes_oracle_init_default(const char *rom_path);

/* Swap the active backend at runtime. Re-inits the chosen backend
 * with the ROM path captured by snes_oracle_init_default. Returns 0
 * if the named backend is compiled in and initialized successfully. */
int snes_oracle_select(const char *name);

/* The ROM path cached by snes_oracle_init_default, so emu_select can
 * re-init. NULL until init succeeds. */
const char *snes_oracle_rom_path(void);

#ifdef __cplusplus
}
#endif

#endif

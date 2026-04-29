/*
 * launcher.c — ROM discovery + CRC32 verification + cached path.
 *
 * Public API: snesrecomp_launcher_resolve_rom() in launcher.h.
 *
 * Persists the user's chosen ROM path to <exe_dir>/rom.cfg so that
 * subsequent runs skip the file picker. Designed to be called from
 * the per-game runner's main() before any ROM byte is loaded.
 */
#include "launcher.h"
#include "crc32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <commdlg.h>
#  pragma comment(lib, "comdlg32.lib")
#endif

/* ---- exe-dir helpers ---- */

static void get_exe_dir(char *out, size_t max_len) {
#ifdef _WIN32
    char exe_path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        snprintf(out, max_len, ".");
        return;
    }
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep) *(last_sep + 1) = '\0';
    snprintf(out, max_len, "%s", exe_path);
#else
    snprintf(out, max_len, "./");
#endif
}

static void get_rom_cfg_path(char *out, size_t max_len) {
    char dir[512];
    get_exe_dir(dir, sizeof(dir));
    snprintf(out, max_len, "%srom.cfg", dir);
}

/* ---- rom.cfg persistence ---- */

static void rom_cfg_read(char *path_out, size_t max_len) {
    char cfg_path[512];
    get_rom_cfg_path(cfg_path, sizeof(cfg_path));
    FILE *f = fopen(cfg_path, "r");
    if (!f) { path_out[0] = '\0'; return; }
    if (!fgets(path_out, (int)max_len, f)) path_out[0] = '\0';
    fclose(f);
    size_t len = strlen(path_out);
    while (len > 0 && (path_out[len-1] == '\n' || path_out[len-1] == '\r'))
        path_out[--len] = '\0';
}

static void rom_cfg_write(const char *rom_path) {
    char cfg_path[512];
    get_rom_cfg_path(cfg_path, sizeof(cfg_path));
    FILE *f = fopen(cfg_path, "w");
    if (!f) return;
    fprintf(f, "%s\n", rom_path);
    fclose(f);
}

/* ---- File picker ---- */

static int pick_rom_file(char *out, size_t max_len) {
#ifdef _WIN32
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    out[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = NULL;
    ofn.lpstrFilter = "SNES ROMs (*.sfc;*.smc)\0*.sfc;*.smc\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = out;
    ofn.nMaxFile    = (DWORD)max_len;
    ofn.lpstrTitle  = "Select SNES ROM";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
#else
    (void)out; (void)max_len;
    fprintf(stderr,
            "[Launcher] No ROM specified and no file picker on this platform.\n"
            "Pass the ROM path as the first argument.\n");
    return 0;
#endif
}

/* ---- CRC32 verification ---- */

/* Returns 1 if the ROM at `path` matches expected_crc (or expected_crc==0).
 * If the file is 512 bytes longer than a multiple of 32KB, treat the first
 * 512 bytes as an SMC copier header and skip them for CRC purposes. */
static int verify_rom(const char *path, uint32_t expected_crc) {
    if (expected_crc == 0) return 1;

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[Launcher] Cannot open '%s'\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return 0; }

    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    if (!data) { fclose(f); return 0; }
    size_t read = fread(data, 1, (size_t)sz, f);
    fclose(f);
    if (read != (size_t)sz) { free(data); return 0; }

    /* Strip 512-byte SMC header if present. */
    size_t hdr = ((size_t)sz % 1024 == 512) ? 512 : 0;
    uint32_t actual = crc32_compute(data + hdr, (size_t)sz - hdr);
    free(data);

    if (actual != expected_crc) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "ROM CRC32 mismatch.\n\nExpected: %08X\nGot:      %08X\n\n"
                 "Please select the correct ROM file.",
                 expected_crc, actual);
        fprintf(stderr, "[Launcher] %s\n", msg);
#ifdef _WIN32
        MessageBoxA(NULL, msg, "Wrong ROM", MB_ICONWARNING | MB_OK);
#endif
        return 0;
    }
    return 1;
}

/* ---- Public ---- */

int snesrecomp_launcher_resolve_rom(int argc, char **argv,
                                    char *out_path, size_t max_len,
                                    uint32_t expected_crc) {
    out_path[0] = '\0';

    /* (1) argv[1] override (back-compat with command-line invocation). */
    if (argc >= 2 && argv[1] && argv[1][0] != '-' && argv[1][0] != '\0') {
        strncpy(out_path, argv[1], max_len - 1);
        out_path[max_len - 1] = '\0';
        if (expected_crc != 0 && !verify_rom(out_path, expected_crc)) {
            fprintf(stderr, "[Launcher] Warning: CRC mismatch for '%s' — continuing anyway\n", out_path);
        }
        rom_cfg_write(out_path);
        printf("[Launcher] ROM: %s\n", out_path);
        return 1;
    }

    /* (2) Cached path from rom.cfg. */
    rom_cfg_read(out_path, max_len);

    /* (3) File picker loop until user provides a valid (or skip-CRC) ROM. */
    for (;;) {
        if (out_path[0] == '\0') {
            if (!pick_rom_file(out_path, max_len)) {
                fprintf(stderr, "[Launcher] No ROM selected — exiting.\n");
                out_path[0] = '\0';
                return 0;
            }
        }
        if (verify_rom(out_path, expected_crc)) {
            rom_cfg_write(out_path);
            printf("[Launcher] ROM: %s\n", out_path);
            return 1;
        }
        /* Wrong ROM — clear and re-prompt. */
        out_path[0] = '\0';
    }
}

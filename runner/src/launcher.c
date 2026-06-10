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
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <commdlg.h>
#  include <direct.h>
#  define snesrecomp_chdir _chdir
#  define snesrecomp_getcwd _getcwd
#  pragma comment(lib, "comdlg32.lib")
#else
#  include <unistd.h>
#  define snesrecomp_chdir chdir
#  define snesrecomp_getcwd getcwd
#endif
#ifdef __APPLE__
#  include <mach-o/dyld.h>
#endif

/* ---- exe-dir helpers ---- */

/* Full path of the running executable. Returns 1 on success. Platforms
 * without a query mechanism (e.g. Switch homebrew) return 0 and callers
 * fall back to cwd-relative behavior. */
static int get_exe_path(char *out, size_t max_len) {
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)max_len);
    return (n > 0 && n < max_len) ? 1 : 0;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)max_len;
    return _NSGetExecutablePath(out, &size) == 0 ? 1 : 0;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", out, max_len - 1);
    if (n <= 0) return 0;
    out[n] = '\0';
    return 1;
#else
    (void)out; (void)max_len;
    return 0;
#endif
}

/* Directory containing the executable, with trailing separator.
 * Falls back to "./" when the exe path can't be determined. */
static void get_exe_dir(char *out, size_t max_len) {
    char exe_path[1024];
    if (get_exe_path(exe_path, sizeof(exe_path))) {
        char *last_sep = NULL;
        for (char *p = exe_path; *p; p++)
            if (*p == '/' || *p == '\\') last_sep = p;
        if (last_sep) {
            *(last_sep + 1) = '\0';
            snprintf(out, max_len, "%s", exe_path);
            return;
        }
    }
    snprintf(out, max_len, "./");
}

int snesrecomp_abspath(const char *path, char *out, size_t max_len) {
    if (!path || !*path || !out || max_len == 0) return 0;
#ifdef _WIN32
    char tmp[1024];
    if (!_fullpath(tmp, path, sizeof(tmp))) return 0;
    if (strlen(tmp) >= max_len) return 0;
    strcpy(out, tmp);
    return 1;
#else
    if (path[0] == '/') {
        if (strlen(path) >= max_len) return 0;
        strcpy(out, path);
        return 1;
    }
    char cwd[1024];
    if (!snesrecomp_getcwd(cwd, sizeof(cwd))) return 0;
    if (snprintf(out, max_len, "%s/%s", cwd, path) >= (int)max_len) return 0;
    return 1;
#endif
}

/* Can we create files in `dir`? Probed by actually creating one —
 * access(W_OK) lies on Windows and inside sandboxed mounts. */
static int dir_is_writable(const char *dir) {
    char probe[1024];
    if (snprintf(probe, sizeof(probe), "%s.snesrecomp_write_probe",
                 dir) >= (int)sizeof(probe))
        return 0;
    FILE *f = fopen(probe, "wb");
    if (!f) return 0;
    fclose(f);
    remove(probe);
    return 1;
}

int snesrecomp_anchor_to_exe_dir(void) {
    char dir[1024];
    get_exe_dir(dir, sizeof(dir));
    if (dir[0] == '.' && (dir[1] == '/' || dir[1] == '\0')) {
        /* Exe path unavailable on this platform — cwd stays authoritative. */
        return 0;
    }
    if (!dir_is_writable(dir)) {
        fprintf(stderr,
                "[Launcher] Executable directory '%s' is not writable; "
                "config and saves stay in the current directory.\n", dir);
        return 0;
    }
    if (snesrecomp_chdir(dir) != 0) {
        fprintf(stderr, "[Launcher] Could not change directory to '%s'.\n", dir);
        return 0;
    }
    return 1;
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

/* ---- SHA-256 verification ---- */

/* Returns 1 if the ROM at `path` matches expected_sha256 (or NULL).
 * Same SMC-header-strip rule as CRC32. */
static int verify_rom_sha256(const char *path, const uint8_t *expected_sha256) {
    if (!expected_sha256) return 1;

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

    size_t hdr = ((size_t)sz % 1024 == 512) ? 512 : 0;
    uint8_t actual[32];
    sha256_compute(data + hdr, (size_t)sz - hdr, actual);
    free(data);

    if (memcmp(actual, expected_sha256, 32) != 0) {
        char exp_hex[65], act_hex[65];
        for (int i = 0; i < 32; i++) {
            snprintf(exp_hex + i*2, 3, "%02x", expected_sha256[i]);
            snprintf(act_hex + i*2, 3, "%02x", actual[i]);
        }
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "ROM SHA-256 mismatch.\n\nExpected:\n%s\n\nGot:\n%s\n\n"
                 "Please select the correct ROM file.",
                 exp_hex, act_hex);
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

    /* (1) argv[1] override (back-compat with command-line invocation).
     * Absolutized so the rom.cfg cache stays valid however the next
     * launch's cwd differs from this one's. */
    if (argc >= 2 && argv[1] && argv[1][0] != '-' && argv[1][0] != '\0') {
        if (!snesrecomp_abspath(argv[1], out_path, max_len)) {
            strncpy(out_path, argv[1], max_len - 1);
            out_path[max_len - 1] = '\0';
        }
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

int snesrecomp_launcher_resolve_rom_sha256(int argc, char **argv,
                                           char *out_path, size_t max_len,
                                           const uint8_t *expected_sha256) {
    out_path[0] = '\0';

    /* (1) argv[1] override (back-compat with command-line invocation).
     * Absolutized so the rom.cfg cache stays valid however the next
     * launch's cwd differs from this one's. */
    if (argc >= 2 && argv[1] && argv[1][0] != '-' && argv[1][0] != '\0') {
        if (!snesrecomp_abspath(argv[1], out_path, max_len)) {
            strncpy(out_path, argv[1], max_len - 1);
            out_path[max_len - 1] = '\0';
        }
        if (expected_sha256 && !verify_rom_sha256(out_path, expected_sha256)) {
            fprintf(stderr, "[Launcher] Warning: SHA-256 mismatch for '%s' — continuing anyway\n", out_path);
        }
        rom_cfg_write(out_path);
        printf("[Launcher] ROM: %s\n", out_path);
        return 1;
    }

    /* (2) Cached path from rom.cfg. */
    rom_cfg_read(out_path, max_len);

    /* (3) File picker loop until user provides a valid (or skip-hash) ROM. */
    for (;;) {
        if (out_path[0] == '\0') {
            if (!pick_rom_file(out_path, max_len)) {
                fprintf(stderr, "[Launcher] No ROM selected — exiting.\n");
                out_path[0] = '\0';
                return 0;
            }
        }
        if (verify_rom_sha256(out_path, expected_sha256)) {
            rom_cfg_write(out_path);
            printf("[Launcher] ROM: %s\n", out_path);
            return 1;
        }
        /* Wrong ROM — clear and re-prompt. */
        out_path[0] = '\0';
    }
}

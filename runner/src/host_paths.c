/*
 * host_paths.c — executable-relative and current-directory path policy.
 *
 * Kept separate from launcher.c because config, saves, mods, and command-line
 * paths all need this policy even when a game supplies its own ROM launcher.
 */
#include "host_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  define snesrecomp_chdir _chdir
#  define snesrecomp_getcwd _getcwd
#else
#  include <unistd.h>
#  define snesrecomp_chdir chdir
#  define snesrecomp_getcwd getcwd
#endif
#ifdef __APPLE__
#  include <mach-o/dyld.h>
#endif

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
    /* Inside an AppImage, /proc/self/exe resolves inside the read-only mount.
     * $APPIMAGE instead identifies the user-visible file beside config.ini,
     * rom.cfg, and saves. */
    const char *appimg = getenv("APPIMAGE");
    if (appimg && appimg[0]) {
        size_t len = strlen(appimg);
        if (len >= max_len) return 0;
        memcpy(out, appimg, len + 1);
        return 1;
    }
    ssize_t n = readlink("/proc/self/exe", out, max_len - 1);
    if (n <= 0) return 0;
    out[n] = '\0';
    return 1;
#else
    (void)out;
    (void)max_len;
    return 0;
#endif
}

/* Directory containing the executable, with trailing separator.
 * Falls back to "./" when the executable path cannot be determined. */
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

int snesrecomp_exe_basename(char *out, size_t max_len) {
    char exe_path[1024];
    if (!out || max_len == 0) return 0;
    out[0] = '\0';
    if (!get_exe_path(exe_path, sizeof(exe_path))) return 0;

    const char *base = exe_path;
    for (const char *p = exe_path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (!*base) return 0;

    size_t len = strlen(base);
    /* Strip a trailing ".exe" so the same name serves the CMake target on
     * every platform. Case-insensitive: Windows does not promise the case. */
    if (len > 4) {
        const char *ext = base + len - 4;
        if ((ext[0] == '.') &&
            (ext[1] == 'e' || ext[1] == 'E') &&
            (ext[2] == 'x' || ext[2] == 'X') &&
            (ext[3] == 'e' || ext[3] == 'E'))
            len -= 4;
    }
    if (len >= max_len) return 0;
    memcpy(out, base, len);
    out[len] = '\0';
    return 1;
}

int snesrecomp_exe_dir_path(const char *leaf, char *out, size_t max_len) {
    if (!leaf || !out || max_len == 0) return 0;
    char dir[1024];
    get_exe_dir(dir, sizeof(dir));
    if (dir[0] == '.' &&
        (dir[1] == '/' || dir[1] == '\\' || dir[1] == '\0'))
        return 0;
    if (snprintf(out, max_len, "%s%s", dir, leaf) >= (int)max_len) return 0;
    return 1;
}

/* Probe by creating a file: access(W_OK) is unreliable on Windows and inside
 * sandboxed mounts. */
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
    if (dir[0] == '.' &&
        (dir[1] == '/' || dir[1] == '\\' || dir[1] == '\0'))
        return 0;
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
    fprintf(stderr, "[Launcher] Config/saves anchored to '%s'.\n", dir);
    return 1;
}

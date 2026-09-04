/* Portable generate → rebuild → relaunch host for recomp-ui setup wizards. */

/* posix_spawn, fdopen, setenv: POSIX, not ISO C. A strict -std=c11 without
 * this hides them; CMake's default gnu11 does not, which is how it built
 * before, and why the guard is here rather than trusted to the flags. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE 1
#endif

#include "snesrecomp_codegen_host.h"

#include "host_paths.h"   /* snesrecomp_exe_basename, snesrecomp_exe_dir_path */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <spawn.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

/* The portable toolchain this host knows how to fetch: cmake + ninja (+ clang
 * and static SDL3 on Linux/Windows; macOS uses Xcode's clang) published as
 * one zip per platform by retcomm-toolchains. RetComM installs the same pack
 * into the same cache, so a machine that has one never downloads twice. */
#define SNESRECOMP_TC_REPO   "TechnicallyComputers/retcomm-toolchains"
#define SNESRECOMP_TC_PACK   "cmake-clang-v1"
#if defined(_WIN32)
#  define SNESRECOMP_TC_ASSET "cmake-clang-v1-windows-x64.zip"
#elif defined(__APPLE__)
#  define SNESRECOMP_TC_ASSET "cmake-clang-v1-macos-universal.zip"
#else
#  define SNESRECOMP_TC_ASSET "cmake-clang-v1-linux-x64.zip"
#endif

static const SnesrecompCodegenHostConfig* g_cfg;
static char g_project_root[1024];
static char g_cli_path[1100];
static char g_python[512];
static char g_cmake[512];
static char g_build_dir[1100];
static char g_exe_path[1100];
static char g_helper_path[1100];
static char g_cfg_dir[256];
static char g_out_dir[256];
static char g_funcs_h[256];
static char g_cmake_target[256];
static char g_display[128];
static char g_toolchain_root[1100];   /* pack root with bin/cmake, or "" */
static char g_exe_dir[1100];
static int g_ready;
static int g_relaunch_is_helper;
static int g_cfg_roots;
static int g_has_funcs_h;

static const char* cfg_or(const char* v, const char* d) {
    return (v && v[0]) ? v : d;
}

/* ── What the child processes said ──────────────────────────────────────────
 *
 * The wizard shows one line at a time and then reports "exit 1", which told a
 * developer nothing about WHICH of 150 compile steps failed. Three outlets:
 *
 *   1. Every line is echoed to the host's stderr, so a GUI launched from a
 *      terminal shows the whole build there, live.
 *   2. Every line is appended to <build>/snesrecomp_rebuild.log, for a GUI
 *      launched by double-click.
 *   3. Lines that look like errors are kept, and the failure message carries
 *      the last few of them plus the log path.
 */
static void mkdir_p(const char* path);
static int join_path(char* out, size_t cap, const char* a, const char* b);
static FILE* g_step_log;
static char g_step_log_path[1400];
#define STEP_ERR_KEEP 6
static char g_step_errs[STEP_ERR_KEEP][200];
static int g_step_err_n;

static void step_log_open(const char* step) {
    g_step_err_n = 0;
    if (!g_step_log && g_build_dir[0]) {
        mkdir_p(g_build_dir);
        if (join_path(g_step_log_path, sizeof(g_step_log_path), g_build_dir,
                      "snesrecomp_rebuild.log"))
            g_step_log = fopen(g_step_log_path, "a");
    }
    if (g_step_log) {
        fprintf(g_step_log, "\n==== %s ====\n", step);
        fflush(g_step_log);
    }
    fprintf(stderr, "snesrecomp-codegen: ==== %s ====\n", step);
}

/* One diagnosis ninja states only as a hint. Files unpacked from a zip made
 * in another time zone can sit hours in the future; ninja then regenerates
 * build.ninja forever and compiles nothing. Say what to do about it. */
static const char* step_hint_for(const char* line) {
    if (strstr(line, "still dirty after 100 tries"))
        return "Some files in this folder have timestamps in the future "
               "(unzipped from a different time zone). Fix with: "
               "find . -exec touch {} +   (in this folder), then rebuild.";
    return NULL;
}

static int line_looks_like_error(const char* line) {
    return strstr(line, "error") || strstr(line, "Error") || strstr(line, "ERROR") ||
           strstr(line, "FAILED") || strstr(line, "undefined reference") ||
           strstr(line, "fatal") || strstr(line, "No such file") ||
           strstr(line, "ninja: build stopped");
}

static void step_log_line(const char* line) {
    fprintf(stderr, "  %s\n", line);
    if (g_step_log) {
        fprintf(g_step_log, "%s\n", line);
        fflush(g_step_log);
    }
    const char* hint = step_hint_for(line);
    if (hint && g_step_err_n < STEP_ERR_KEEP)
        snprintf(g_step_errs[g_step_err_n++], sizeof(g_step_errs[0]), "%s", hint);
    if (line_looks_like_error(line)) {
        /* Keep the LAST few: the root cause tends to precede the cascade,
         * but "ninja: build stopped" and the FAILED line name the target. */
        if (g_step_err_n == STEP_ERR_KEEP) {
            memmove(g_step_errs[0], g_step_errs[1], sizeof(g_step_errs) - sizeof(g_step_errs[0]));
            g_step_err_n--;
        }
        snprintf(g_step_errs[g_step_err_n++], sizeof(g_step_errs[0]), "%s", line);
    }
}

/* "<what> failed (exit N)." plus the kept error lines and where the full log
 * is. err_cap is whatever the launcher gave us; fill it, no more. */
static void step_fail_message(char* err_msg, size_t err_cap, const char* what, int code) {
    size_t used = (size_t)snprintf(err_msg, err_cap, "%s failed (exit %d).", what, code);
    for (int i = 0; i < g_step_err_n && used + 8 < err_cap; ++i) {
        int n = snprintf(err_msg + used, err_cap - used, "\n%.160s", g_step_errs[i]);
        if (n < 0) break;
        used += (size_t)n;
        if (used >= err_cap) { used = err_cap - 1; break; }
    }
    if (g_step_log_path[0] && used + 24 < err_cap)
        snprintf(err_msg + used, err_cap - used, "\nFull log: %s", g_step_log_path);
    fprintf(stderr, "snesrecomp-codegen: %s\n", err_msg);
}

static int path_is_file(const char* path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static int path_is_dir(const char* path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int join_path(char* out, size_t cap, const char* a, const char* b) {
    size_t na = strlen(a);
    int need_slash = na > 0 && a[na - 1] != '/' && a[na - 1] != '\\';
    int n = snprintf(out, cap, "%s%s%s", a, need_slash ? "/" : "", b);
    return n > 0 && (size_t)n < cap;
}

static void mkdir_p(const char* path) {
    char tmp[1400];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = '\0';
#if defined(_WIN32)
            CreateDirectoryA(tmp, NULL);
#else
            mkdir(tmp, 0755);
#endif
            tmp[i] = c;
        }
    }
#if defined(_WIN32)
    CreateDirectoryA(tmp, NULL);
#else
    mkdir(tmp, 0755);
#endif
}

/* Remove a directory tree. Only ever pointed at directories this file created
 * inside the toolchain cache; never at anything the player owns. */
static void rmtree_path(const char* path) {
    if (!path || !path[0] || !path_is_dir(path)) return;
    char cmd[3000];
#if defined(_WIN32)
    snprintf(cmd, sizeof(cmd), "cmd.exe /C rmdir /S /Q \"%s\"", path);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", path);
#endif
    (void)system(cmd);
}

static int dirname_copy(char* out, size_t cap, const char* path) {
    size_t n = strlen(path);
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\'))
        --n;
    while (n > 0 && path[n - 1] != '/' && path[n - 1] != '\\')
        --n;
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\'))
        --n;
    if (n == 0) {
        if (cap < 2) return 0;
        out[0] = '.';
        out[1] = '\0';
        return 1;
    }
    if (n >= cap) return 0;
    memcpy(out, path, n);
    out[n] = '\0';
    return 1;
}

static int looks_like_project_root(const char* root) {
    char cli[1100], cfg[1100];
    if (!join_path(cli, sizeof(cli), root,
                   cfg_or(g_cfg->snesrecomp_cli_relpath,
                          "snesrecomp/snesrecomp_cli.py")))
        return 0;
    if (!join_path(cfg, sizeof(cfg), root,
                   cfg_or(g_cfg->seed_cfg_relpath, "recomp/bank00.cfg")))
        return 0;
    return path_is_file(cli) && path_is_file(cfg);
}

static int find_on_path(const char* name, char* out, size_t cap) {
#if defined(_WIN32)
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "where %s >nul 2>nul", name);
    if (system(cmd) == 0) {
        snprintf(out, cap, "%s", name);
        return 1;
    }
#else
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
    if (system(cmd) == 0) {
        snprintf(out, cap, "%s", name);
        return 1;
    }
#endif
    return 0;
}

static int find_python(char* out, size_t cap) {
    const char* env = getenv("PYTHON");
    if (env && env[0] && path_is_file(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }
#if defined(_WIN32)
    const char* candidates[] = { "python.exe", "python3.exe", "py.exe" };
#else
    const char* candidates[] = { "python3", "python" };
#endif
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (find_on_path(candidates[i], out, cap))
            return 1;
    }
    return 0;
}

static int find_cmake(char* out, size_t cap) {
    const char* env = getenv("CMAKE");
    if (env && env[0] && path_is_file(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }
#if defined(_WIN32)
    return find_on_path("cmake.exe", out, cap);
#else
    return find_on_path("cmake", out, cap);
#endif
}

static int discover_project_root(char* out, size_t cap) {
    const char* env_name =
        cfg_or(g_cfg->project_root_env, "SNESRECOMP_PROJECT_ROOT");
    const char* env = getenv(env_name);
    if (env && env[0] && looks_like_project_root(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }

    char start[1024];
#if defined(_WIN32)
    if (!GetCurrentDirectoryA((DWORD)sizeof(start), start))
        start[0] = '\0';
#else
    if (!getcwd(start, sizeof(start)))
        start[0] = '\0';
#endif

    /* Two starting points, in order: the working directory (a terminal launch
     * from the repo or unzip root), then the executable's own directory. The
     * second is what a double-clicked setup zip needs -- Finder and Explorer
     * hand the process a cwd that has nothing to do with where it lives, and
     * a setup host that cannot find its own tree cannot rebuild. */
    const char* starts[2];
    int n_starts = 0;
    if (start[0]) starts[n_starts++] = start;
    if (g_exe_dir[0]) starts[n_starts++] = g_exe_dir;
    for (int si = 0; si < n_starts; ++si) {
        char cur[1024];
        snprintf(cur, sizeof(cur), "%s", starts[si]);
        for (int i = 0; i < 8; ++i) {
            if (looks_like_project_root(cur)) {
                snprintf(out, cap, "%s", cur);
                return 1;
            }
            char parent[1024];
            if (!dirname_copy(parent, sizeof(parent), cur))
                break;
            if (strcmp(parent, cur) == 0)
                break;
            snprintf(cur, sizeof(cur), "%s", parent);
        }
    }
    return 0;
}

static int resolve_build_paths(void) {
    const char* env_name =
        cfg_or(g_cfg->build_dir_env, "SNESRECOMP_BUILD_DIR");
    const char* env = getenv(env_name);
    if (env && env[0])
        snprintf(g_build_dir, sizeof(g_build_dir), "%s", env);
    else if (!join_path(g_build_dir, sizeof(g_build_dir), g_project_root,
                        cfg_or(g_cfg->build_dir_name, "build")))
        return 0;
    /* The directory may not exist yet: a setup zip ships source and a setup
     * binary, never a configured build tree (CMakeCache.txt is full of the
     * packager's absolute paths). host_rebuild_game() configures it on first
     * use, so its absence is not a reason to hide the rebuild button. */

    char exe_name[300];
#if defined(_WIN32)
    snprintf(exe_name, sizeof(exe_name), "%s.exe", g_cfg->exe_basename);
#else
    snprintf(exe_name, sizeof(exe_name), "%s", g_cfg->exe_basename);
#endif
    return join_path(g_exe_path, sizeof(g_exe_path), g_build_dir, exe_name);
}


/* ── Portable toolchain ──────────────────────────────────────────────────────
 *
 * A setup zip carries source and a setup binary; the compiler, cmake, ninja
 * and python that turn the player's ROM into a playable build come from the
 * retcomm-toolchains pack. Where it may already be, in the order tried:
 *
 *   1. RETCOMM_TOOLCHAIN_DIR                 explicit override
 *   2. <exe dir>/toolchain, <root>/toolchain a pack embedded in the zip
 *   3. the RetComM cache                     %LOCALAPPDATA%/retcomm/toolchains/
 *                                            cmake-clang-v1 (Windows),
 *                                            $XDG_DATA_HOME or ~/.local/share/
 *                                            retcomm/toolchains/cmake-clang-v1
 *   4. cmake + a C compiler + python on PATH  a developer machine
 *
 * The cache is shared with RetComM and psxrecomp hosts on purpose: one 800 MB
 * download per machine, not one per game. Within a cache root, `latest/` is
 * what this host installs, `offline/` is a player-supplied zip, and any other
 * child with bin/cmake is a pack RetComM installed (e.g. v1.0.14/). */

#if defined(_WIN32)
#  define SNESRECOMP_EXE_SUFFIX ".exe"
#else
#  define SNESRECOMP_EXE_SUFFIX ""
#endif

static int json_get_string(const char* line, const char* key, char* out,
                           size_t out_cap);

static int pack_tool(const char* root, const char* name, char* out, size_t cap) {
    char rel[128];
    snprintf(rel, sizeof(rel), "bin/%s" SNESRECOMP_EXE_SUFFIX, name);
    return join_path(out, cap, root, rel) && path_is_file(out);
}

static int pack_root_has_cmake(const char* root) {
    char tmp[1400];
    return root && root[0] && pack_tool(root, "cmake", tmp, sizeof(tmp));
}

/* The pack's embedded CPython, wherever the platform packager put it. */
static int pack_python(const char* root, char* out, size_t cap) {
    static const char* rels[] = {
        "python/bin/python3", "python/bin/python3" SNESRECOMP_EXE_SUFFIX,
        "python/python" SNESRECOMP_EXE_SUFFIX, "python/python3" SNESRECOMP_EXE_SUFFIX,
        NULL
    };
    for (int i = 0; rels[i]; ++i) {
        if (join_path(out, cap, root, rels[i]) && path_is_file(out))
            return 1;
    }
    return 0;
}

static int resolve_pack_under(const char* dir, char* out, size_t cap) {
    if (!dir || !dir[0] || !path_is_dir(dir)) return 0;
    if (pack_root_has_cmake(dir)) {
        snprintf(out, cap, "%s", dir);
        return 1;
    }
    static const char* prefer[] = { "latest", "offline", NULL };
    char cand[1400];
    for (int i = 0; prefer[i]; ++i) {
        if (join_path(cand, sizeof(cand), dir, prefer[i]) &&
            pack_root_has_cmake(cand)) {
            snprintf(out, cap, "%s", cand);
            return 1;
        }
    }
    /* Any child directory that is a pack (RetComM installs as <version>/). */
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    char pattern[1400];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        if (join_path(cand, sizeof(cand), dir, fd.cFileName) &&
            pack_root_has_cmake(cand)) {
            snprintf(out, cap, "%s", cand);
            found = 1;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
#else
    DIR* d = opendir(dir);
    if (!d) return 0;
    struct dirent* ent;
    int found = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (join_path(cand, sizeof(cand), dir, ent->d_name) &&
            pack_root_has_cmake(cand)) {
            snprintf(out, cap, "%s", cand);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
#endif
}

/* Cache roots, first is where a download goes. Same set psxrecomp uses. */
static int collect_cache_bases(char bases[][1400], int max_n) {
    int n = 0;
    const char* cache = getenv("RETCOMM_TOOLCHAIN_CACHE");
    if (cache && cache[0] && n < max_n)
        snprintf(bases[n++], 1400, "%s", cache);
    const char* data = getenv("RETCOMM_DATA_HOME");
    if (data && data[0] && n < max_n &&
        join_path(bases[n], 1400, data, "toolchains/" SNESRECOMP_TC_PACK))
        ++n;
#if defined(_WIN32)
    const char* local = getenv("LOCALAPPDATA");
    if (local && local[0] && n < max_n &&
        join_path(bases[n], 1400, local, "retcomm/toolchains/" SNESRECOMP_TC_PACK))
        ++n;
#endif
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] && n < max_n &&
        join_path(bases[n], 1400, xdg, "retcomm/toolchains/" SNESRECOMP_TC_PACK))
        ++n;
    const char* home = getenv("HOME");
    if (home && home[0] && n < max_n &&
        join_path(bases[n], 1400, home,
                  ".local/share/retcomm/toolchains/" SNESRECOMP_TC_PACK))
        ++n;
    return n;
}

static int preferred_cache_root(char* out, size_t cap) {
    char bases[6][1400];
    int n = collect_cache_bases(bases, 6);
    if (n == 0) return 0;
    snprintf(out, cap, "%s", bases[0]);
    mkdir_p(out);
    return path_is_dir(out);
}

static int resolve_toolchain_root(char* out, size_t cap) {
    const char* env = getenv("RETCOMM_TOOLCHAIN_DIR");
    if (env && env[0] && resolve_pack_under(env, out, cap))
        return 1;
    char cand[1400];
    if (g_exe_dir[0] && join_path(cand, sizeof(cand), g_exe_dir, "toolchain") &&
        resolve_pack_under(cand, out, cap))
        return 1;
    if (g_project_root[0] &&
        join_path(cand, sizeof(cand), g_project_root, "toolchain") &&
        resolve_pack_under(cand, out, cap))
        return 1;
    char bases[6][1400];
    int n = collect_cache_bases(bases, 6);
    for (int i = 0; i < n; ++i) {
        if (resolve_pack_under(bases[i], out, cap))
            return 1;
    }
    return 0;
}

static void env_prepend_path(const char* dir) {
    if (!dir || !dir[0]) return;
    const char* cur = getenv("PATH");
    char merged[8192];
#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    if (cur && strstr(cur, dir)) return;
    snprintf(merged, sizeof(merged), "%s%c%s", dir, sep, cur ? cur : "");
#if defined(_WIN32)
    SetEnvironmentVariableA("PATH", merged);
    _putenv_s("PATH", merged);
#else
    setenv("PATH", merged, 1);
#endif
}

static void env_set(const char* name, const char* value) {
#if defined(_WIN32)
    SetEnvironmentVariableA(name, value);
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* Make the pack the process's toolchain: bin/ and python/ on PATH so every
 * child (python, cmake, ninja, clang) resolves to it, plus the variables the
 * pack's own env script exports for CMake. On POSIX the heavy lifting is left
 * to sourcing <pack>/env.sh in the spawned shell (see shell_wrap_argv), which
 * keeps this host correct when the pack's contract changes. */
static void activate_toolchain(void) {
    g_toolchain_root[0] = '\0';
    if (!resolve_toolchain_root(g_toolchain_root, sizeof(g_toolchain_root)))
        return;
    char dir[1400];
    if (join_path(dir, sizeof(dir), g_toolchain_root, "bin"))
        env_prepend_path(dir);
    if (join_path(dir, sizeof(dir), g_toolchain_root, "python/bin") && path_is_dir(dir))
        env_prepend_path(dir);
    if (join_path(dir, sizeof(dir), g_toolchain_root, "python") && path_is_dir(dir))
        env_prepend_path(dir);
    env_set("RETCOMM_TOOLCHAIN_DIR", g_toolchain_root);
#if defined(_WIN32)
    char tool[1400];
    if (pack_tool(g_toolchain_root, "clang", tool, sizeof(tool)))   env_set("CC", tool);
    if (pack_tool(g_toolchain_root, "clang++", tool, sizeof(tool))) env_set("CXX", tool);
    if (join_path(dir, sizeof(dir), g_toolchain_root, "deps/lib/cmake/SDL3") &&
        path_is_dir(dir))
        env_set("SDL3_DIR", dir);
    if (join_path(dir, sizeof(dir), g_toolchain_root, "deps/include/zlib.h") &&
        path_is_file(dir) &&
        join_path(dir, sizeof(dir), g_toolchain_root, "deps"))
        env_set("ZLIB_ROOT", dir);
    /* llvm-mingw occasionally ships libpython*.dll in bin/; the embedded
     * CPython must not pick those up over its own. */
    env_set("PYTHONNOUSERSITE", "1");
#endif
}

/* cmake, python, and a C compiler -- from the pack when it has them, else
 * PATH. The macOS pack deliberately ships no compiler (Xcode's clang is the
 * only one Apple supports), so on that platform the compiler check is PATH
 * regardless. Fills g_cmake / g_python as a side effect. */
static int resolve_tools(char* why, size_t why_cap) {
    activate_toolchain();
    g_cmake[0] = '\0';
    g_python[0] = '\0';
    if (g_toolchain_root[0]) {
        pack_tool(g_toolchain_root, "cmake", g_cmake, sizeof(g_cmake));
        pack_python(g_toolchain_root, g_python, sizeof(g_python));
    }
    if (!g_cmake[0] && !find_cmake(g_cmake, sizeof(g_cmake))) {
        snprintf(why, why_cap, "cmake was not found (no toolchain pack, none on PATH).");
        return 0;
    }
    if (!g_python[0] && !find_python(g_python, sizeof(g_python))) {
        snprintf(why, why_cap, "python3 was not found (no toolchain pack, none on PATH).");
        return 0;
    }
    char cc[512];
    int have_cc = (g_toolchain_root[0] && pack_tool(g_toolchain_root, "clang", cc, sizeof(cc)))
               || find_on_path("cc", cc, sizeof(cc))
               || find_on_path("clang", cc, sizeof(cc))
               || find_on_path("gcc", cc, sizeof(cc));
    if (!have_cc) {
#if defined(__APPLE__)
        snprintf(why, why_cap,
                 "No C compiler. Install the Xcode Command Line Tools: "
                 "xcode-select --install");
#else
        snprintf(why, why_cap, "No C compiler found (toolchain pack incomplete?).");
#endif
        return 0;
    }
    return 1;
}

static int read_pack_version(const char* root, char* out, size_t cap) {
    char path[1400];
    out[0] = '\0';
    if (!join_path(path, sizeof(path), root, "retcomm-toolchain.json")) return 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return json_get_string(buf, "version", out, cap);
}

/* Run a command line to completion, output discarded. Hidden window on
 * Windows: system() flashes a console over the launcher for every curl/tar. */
static int run_cmdline_wait(const char* cmdline, unsigned long* code) {
#if defined(_WIN32)
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char mutable_cmd[4096];
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    snprintf(mutable_cmd, sizeof(mutable_cmd), "%s", cmdline);
    if (!CreateProcessA(NULL, mutable_cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
        return 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD c = 1;
    GetExitCodeProcess(pi.hProcess, &c);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    *code = (unsigned long)c;
    return 1;
#else
    int rc = system(cmdline);
    if (rc == -1) return 0;
    *code = (unsigned long)(WIFEXITED(rc) ? WEXITSTATUS(rc) : 1);
    return 1;
#endif
}

static int download_file(const char* url, const char* dest, char* err, size_t cap) {
    char parent[1400], cmd[4096];
    unsigned long code = 1;
    if (!dirname_copy(parent, sizeof(parent), dest)) {
        snprintf(err, cap, "Bad download destination.");
        return 0;
    }
    mkdir_p(parent);
    remove(dest);
    /* curl ships with Windows 10+, macOS, and every mainstream distro. -L
     * follows the GitHub release redirect. */
    snprintf(cmd, sizeof(cmd),
             "curl" SNESRECOMP_EXE_SUFFIX
             " -fsSL --retry 3 --retry-delay 2 -o \"%s\" \"%s\"", dest, url);
    if (!run_cmdline_wait(cmd, &code) || code != 0) {
        snprintf(err, cap, "Toolchain download failed (curl exit %lu). "
                 "Check your network, or pick a cmake-clang-v1 zip you downloaded.",
                 code);
        return 0;
    }
    return path_is_file(dest);
}

static int extract_zip(const char* zip, const char* dest, char* err, size_t cap) {
    char cmd[3200];
    unsigned long code = 1;
    if (!path_is_file(zip)) {
        snprintf(err, cap, "Toolchain zip not found: %s", zip);
        return 0;
    }
    rmtree_path(dest);
    mkdir_p(dest);
    /* bsdtar (Windows 10+, macOS) and GNU tar both extract .zip; unzip is the
     * fallback for Linux hosts whose tar lacks libarchive. */
    snprintf(cmd, sizeof(cmd), "tar" SNESRECOMP_EXE_SUFFIX " -xf \"%s\" -C \"%s\"", zip, dest);
    if (!run_cmdline_wait(cmd, &code) || code != 0) {
        snprintf(cmd, sizeof(cmd), "unzip -q \"%s\" -d \"%s\"", zip, dest);
        if (!run_cmdline_wait(cmd, &code) || code != 0) {
            snprintf(err, cap, "Failed to extract the toolchain zip (tar/unzip).");
            return 0;
        }
    }
    char root[1400];
    if (!resolve_pack_under(dest, root, sizeof(root))) {
        snprintf(err, cap, "The zip did not contain a cmake-clang-v1 pack (no bin/cmake).");
        return 0;
    }
    return 1;
}

static int host_toolchain_is_ready(void) {
    char why[256];
    return resolve_tools(why, sizeof(why));
}

/* download: 0 = cache or zip_path only, 1 = download if missing, 2 = update. */
static int host_ensure_toolchain_with_progress(
    int download, const char* zip_path, char* err_msg, size_t err_cap,
    RecompLauncherCPrepareProgressFn on_progress, void* progress_ctx) {
    char why[256];
    const int force = (download == 2);
    if (!force && resolve_tools(why, sizeof(why)))
        return 1;

    char cache[1400];
    if (!preferred_cache_root(cache, sizeof(cache))) {
        snprintf(err_msg, err_cap,
                 "No writable toolchain cache (set RETCOMM_TOOLCHAIN_CACHE).");
        return 0;
    }

    char dest[1400];
    if (zip_path && zip_path[0]) {
        if (on_progress) on_progress(progress_ctx, 0.05f, "Installing toolchain from zip…");
        if (!join_path(dest, sizeof(dest), cache, "offline")) return 0;
        if (!extract_zip(zip_path, dest, err_msg, err_cap)) return 0;
    } else if (download) {
        char url[512], zip[1400];
        snprintf(url, sizeof(url), "https://github.com/%s/releases/latest/download/%s",
                 SNESRECOMP_TC_REPO, SNESRECOMP_TC_ASSET);
        if (!join_path(zip, sizeof(zip), cache, "dl/" SNESRECOMP_TC_ASSET)) return 0;
        if (on_progress)
            on_progress(progress_ctx, 0.05f,
                        force ? "Downloading toolchain update…"
                              : "Downloading portable cmake/clang (this is large; one time per machine)…");
        if (!download_file(url, zip, err_msg, err_cap)) return 0;
        if (on_progress) on_progress(progress_ctx, 0.70f, "Unpacking toolchain…");
        if (!join_path(dest, sizeof(dest), cache, "latest")) return 0;
        if (!extract_zip(zip, dest, err_msg, err_cap)) return 0;
        remove(zip);
    } else {
        snprintf(err_msg, err_cap,
                 "No portable toolchain found. Enable download, pick a "
                 "cmake-clang-v1 zip, or set RETCOMM_TOOLCHAIN_DIR. (%s)", why);
        return 0;
    }

    if (on_progress) on_progress(progress_ctx, 0.95f, "Checking toolchain…");
    if (!resolve_tools(why, sizeof(why))) {
        snprintf(err_msg, err_cap, "Toolchain installed but unusable: %s", why);
        return 0;
    }
    if (on_progress) on_progress(progress_ctx, 1.0f, "Toolchain ready");
    return 1;
}

static int version_cmp(const char* a, const char* b) {
    /* "v1.0.14" vs "1.0.13": strip a leading v, compare numerically. */
    if (*a == 'v' || *a == 'V') ++a;
    if (*b == 'v' || *b == 'V') ++b;
    while (*a || *b) {
        long x = strtol(a, (char**)&a, 10);
        long y = strtol(b, (char**)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') ++a;
        if (*b == '.') ++b;
        if (!*a && !*b) break;
        if (!*a || !*b) return *a ? 1 : -1;
    }
    return 0;
}

static int host_toolchain_update_available(char* local_ver, size_t local_cap,
                                           char* remote_ver, size_t remote_cap) {
    local_ver[0] = '\0';
    remote_ver[0] = '\0';
    activate_toolchain();
    if (!g_toolchain_root[0] || !read_pack_version(g_toolchain_root, local_ver, local_cap))
        return 0;
    /* Short timeouts: this runs while the wizard is opening, offline is normal. */
    char tmp[1400], cmd[2048];
    char cache[1400];
    if (!preferred_cache_root(cache, sizeof(cache))) return 0;
    if (!join_path(tmp, sizeof(tmp), cache, "dl/latest.json")) return 0;
    mkdir_p(cache);
    {
        char dl[1400];
        if (join_path(dl, sizeof(dl), cache, "dl")) mkdir_p(dl);
    }
    snprintf(cmd, sizeof(cmd),
             "curl" SNESRECOMP_EXE_SUFFIX
             " -fsSL --connect-timeout 5 --max-time 15 -o \"%s\" "
             "\"https://api.github.com/repos/%s/releases/latest\"", tmp, SNESRECOMP_TC_REPO);
    unsigned long code = 1;
    if (!run_cmdline_wait(cmd, &code) || code != 0) return 0;
    FILE* f = fopen(tmp, "rb");
    if (!f) return 0;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    remove(tmp);
    buf[n] = '\0';
    if (!json_get_string(buf, "tag_name", remote_ver, remote_cap)) return 0;
    return version_cmp(local_ver, remote_ver) < 0;
}

#if !defined(_WIN32)
/* Wrap a command so it runs with the pack's env.sh sourced. env.sh is the
 * pack's own statement of how to use it (PATH, CC/CXX, sysroot, SDL3_DIR);
 * reproducing that here would be a second copy that drifts. Without a pack
 * the wrapper is a no-op exec. argv_out must hold argc_in + 5 entries. */
static int shell_wrap_argv(char** argv_in, int argc_in, char** argv_out, int cap_out) {
    static char script[] =
        "if [ -n \"$0\" ] && [ -f \"$0/env.sh\" ]; then . \"$0/env.sh\"; fi; exec \"$@\"";
    if (argc_in + 5 > cap_out) return 0;
    int n = 0;
    argv_out[n++] = "/bin/sh";
    argv_out[n++] = "-c";
    argv_out[n++] = script;
    argv_out[n++] = g_toolchain_root;   /* $0 -- "" when there is no pack */
    for (int i = 0; i < argc_in; ++i) argv_out[n++] = argv_in[i];
    argv_out[n] = NULL;
    return 1;
}
#endif

int snesrecomp_codegen_host_sources_missing(
    const SnesrecompCodegenHostConfig* cfg) {
    if (!cfg || !cfg->cmake_target || !cfg->exe_basename)
        return 0;
    g_cfg = cfg;
    if (!g_ready && !discover_project_root(g_project_root, sizeof(g_project_root)))
        return 0;
    char marker[1100];
    if (!join_path(marker, sizeof(marker), g_project_root,
                   cfg_or(cfg->gen_marker_relpath, "src/gen/dispatch_v2.c")))
        return 1;
    return !path_is_file(marker);
}

static int json_get_string(const char* line, const char* key, char* out,
                           size_t out_cap) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(line, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_cap) {
        if (*p == '\\' && p[1]) {
            ++p;
            out[i++] = *p++;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

static int json_get_number(const char* line, const char* key, double* out) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(line, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ') ++p;
    char* end = NULL;
    double v = strtod(p, &end);
    if (end == p) return 0;
    *out = v;
    return 1;
}

static void handle_progress_line(const char* line,
                                 RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx) {
    if (!line) return;
    if (line[0] != '{') {                 /* tool chatter that is not JSONL */
        if (line[0]) step_log_line(line);
        return;
    }
    if (!on_progress) return;
    char event[64] = "";
    json_get_string(line, "event", event, sizeof(event));
    if (strcmp(event, "phase") == 0) {
        char message[240] = "";
        char phase[64] = "";
        double pct = -1.0;
        json_get_string(line, "message", message, sizeof(message));
        json_get_string(line, "phase", phase, sizeof(phase));
        if (!json_get_number(line, "pct", &pct))
            pct = -1.0;
        if (!message[0] && phase[0])
            snprintf(message, sizeof(message), "%s", phase);
        on_progress(progress_ctx, (float)pct, message[0] ? message : NULL);
    } else if (strcmp(event, "log") == 0 || strcmp(event, "error") == 0) {
        char message[240] = "";
        if (json_get_string(line, "message", message, sizeof(message))) {
            step_log_line(message);
            on_progress(progress_ctx, -1.0f, message);
        }
    }
}

#if defined(_WIN32)
static int run_generate_win(const char* rom,
                            RecompLauncherCPrepareProgressFn on_progress,
                            void* progress_ctx, char* err_msg, size_t err_cap) {
    char cmdline[4096];
    char crc_arg[96] = "";
    char sha_arg[160] = "";
    if (g_cfg->expected_crc32 && g_cfg->expected_crc32[0])
        snprintf(crc_arg, sizeof(crc_arg), " --expected-crc32 %s",
                 g_cfg->expected_crc32);
    if (g_cfg->expected_sha256 && g_cfg->expected_sha256[0])
        snprintf(sha_arg, sizeof(sha_arg), " --expected-sha256 %s",
                 g_cfg->expected_sha256);

    if (g_has_funcs_h) {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s\" \"%s\" generate --project-root \"%s\" --rom \"%s\" "
                 "--cfg-dir \"%s\" --out-dir \"%s\" --funcs-h \"%s\"%s%s%s "
                 "--json-progress",
                 g_python, g_cli_path, g_project_root, rom, g_cfg_dir, g_out_dir,
                 g_funcs_h, g_cfg_roots ? " --cfg-roots" : "", crc_arg, sha_arg);
    } else {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s\" \"%s\" generate --project-root \"%s\" --rom \"%s\" "
                 "--cfg-dir \"%s\" --out-dir \"%s\"%s%s%s --json-progress",
                 g_python, g_cli_path, g_project_root, rom, g_cfg_dir, g_out_dir,
                 g_cfg_roots ? " --cfg-roots" : "", crc_arg, sha_arg);
    }

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        snprintf(err_msg, err_cap, "CreatePipe failed.");
        return 0;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    char mutable_cmd[4096];
    snprintf(mutable_cmd, sizeof(mutable_cmd), "%s", cmdline);
    if (!CreateProcessA(NULL, mutable_cmd, NULL, NULL, TRUE, 0, NULL,
                        g_project_root, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        snprintf(err_msg, err_cap, "Failed to spawn snesrecomp generate.");
        return 0;
    }
    CloseHandle(wr);

    char buf[512];
    char line[1024];
    size_t line_len = 0;
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, NULL) && n > 0) {
        for (DWORD i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[line_len] = '\0';
                handle_progress_line(line, on_progress, progress_ctx);
                line_len = 0;
                continue;
            }
            if (line_len + 1 < sizeof(line))
                line[line_len++] = c;
        }
    }
    if (line_len) {
        line[line_len] = '\0';
        handle_progress_line(line, on_progress, progress_ctx);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code == 0) return 1;
    if (code == 3)
        snprintf(err_msg, err_cap, "ROM verification failed (wrong dump).");
    else
        step_fail_message(err_msg, err_cap, "snesrecomp generate", (int)code);
    return 0;
}
#else
static int run_generate_posix(const char* rom,
                              RecompLauncherCPrepareProgressFn on_progress,
                              void* progress_ctx, char* err_msg,
                              size_t err_cap) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(err_msg, err_cap, "pipe() failed: %s", strerror(errno));
        return 0;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    char* argv[32];
    int argc = 0;
    argv[argc++] = g_python;
    argv[argc++] = g_cli_path;
    argv[argc++] = "generate";
    argv[argc++] = "--project-root";
    argv[argc++] = g_project_root;
    argv[argc++] = "--rom";
    argv[argc++] = (char*)rom;
    argv[argc++] = "--cfg-dir";
    argv[argc++] = g_cfg_dir;
    argv[argc++] = "--out-dir";
    argv[argc++] = g_out_dir;
    if (g_has_funcs_h) {
        argv[argc++] = "--funcs-h";
        argv[argc++] = g_funcs_h;
    }
    if (g_cfg_roots)
        argv[argc++] = "--cfg-roots";
    if (g_cfg->expected_crc32 && g_cfg->expected_crc32[0]) {
        argv[argc++] = "--expected-crc32";
        argv[argc++] = (char*)g_cfg->expected_crc32;
    }
    if (g_cfg->expected_sha256 && g_cfg->expected_sha256[0]) {
        argv[argc++] = "--expected-sha256";
        argv[argc++] = (char*)g_cfg->expected_sha256;
    }
    argv[argc++] = "--json-progress";
    argv[argc] = NULL;

    char* wrapped[40];
    if (!shell_wrap_argv(argv, argc, wrapped, 40)) {
        close(pipefd[0]);
        close(pipefd[1]);
        posix_spawn_file_actions_destroy(&actions);
        snprintf(err_msg, err_cap, "Too many arguments for generate.");
        return 0;
    }
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, wrapped[0], &actions, NULL, wrapped, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        snprintf(err_msg, err_cap, "Failed to spawn snesrecomp generate: %s",
                 strerror(rc));
        return 0;
    }

    FILE* out = fdopen(pipefd[0], "r");
    if (!out) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        snprintf(err_msg, err_cap, "fdopen failed.");
        return 0;
    }
    char line[1024];
    while (fgets(line, sizeof(line), out)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        handle_progress_line(line, on_progress, progress_ctx);
    }
    fclose(out);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(err_msg, err_cap, "waitpid failed: %s", strerror(errno));
        return 0;
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (code == 0) return 1;
    if (code == 3)
        snprintf(err_msg, err_cap, "ROM verification failed (wrong dump).");
    else
        step_fail_message(err_msg, err_cap, "snesrecomp generate", code);
    return 0;
}
#endif

static int host_prepare_generate(const char* source_path, char* out_path,
                                 size_t out_cap, char* err_msg, size_t err_cap,
                                 RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx) {
    if (!g_ready) {
        snprintf(err_msg, err_cap, "Local codegen tools are not available.");
        return 0;
    }
    if (!source_path || !source_path[0]) {
        snprintf(err_msg, err_cap, "No ROM selected.");
        return 0;
    }
    {
        char why[256];
        if (!resolve_tools(why, sizeof(why))) {
            snprintf(err_msg, err_cap, "%s", why);
            return 0;
        }
    }
    step_log_open("snesrecomp generate");
    if (on_progress)
        on_progress(progress_ctx, 0.02f, "Starting snesrecomp generate…");

#if defined(_WIN32)
    if (!run_generate_win(source_path, on_progress, progress_ctx, err_msg,
                          err_cap))
        return 0;
#else
    if (!run_generate_posix(source_path, on_progress, progress_ctx, err_msg,
                            err_cap))
        return 0;
#endif

    snprintf(out_path, out_cap, "%s", source_path);
    if (on_progress)
        on_progress(progress_ctx, 1.0f, "Generate complete");
    return 1;
}

#if defined(_WIN32)
static void bat_write_set(FILE* f, const char* name, const char* value) {
    fprintf(f, "set \"%s=", name);
    for (const char* p = value; *p; ++p) {
        if (*p == '%')
            fputc('%', f);
        fputc(*p, f);
    }
    fprintf(f, "\"\r\n");
}

static int write_windows_deferred_rebuild_helper(char* err_msg, size_t err_cap) {
    if (!join_path(g_helper_path, sizeof(g_helper_path), g_build_dir,
                   "recomp_deferred_rebuild.cmd")) {
        snprintf(err_msg, err_cap, "Failed to form helper path.");
        return 0;
    }
    FILE* f = fopen(g_helper_path, "wb");
    if (!f) {
        snprintf(err_msg, err_cap, "Failed to write rebuild helper: %s",
                 g_helper_path);
        return 0;
    }
    char pid_buf[32];
    snprintf(pid_buf, sizeof(pid_buf), "%lu",
             (unsigned long)GetCurrentProcessId());
    fprintf(f, "@echo off\r\n");
    fprintf(f, "setlocal EnableExtensions\r\n");
    fprintf(f, "title %s - rebuilding\r\n", g_display);
    bat_write_set(f, "PARENT_PID", pid_buf);
    bat_write_set(f, "CMAKE", g_cmake);
    bat_write_set(f, "BUILD_DIR", g_build_dir);
    bat_write_set(f, "EXE", g_exe_path);
    bat_write_set(f, "ROOT", g_project_root);
    bat_write_set(f, "TARGET", g_cmake_target);
    bat_write_set(f, "DISPLAY", g_display);
    bat_write_set(f, "PACK", g_toolchain_root);
    fprintf(f,
            "echo Waiting for %%DISPLAY%% to exit...\r\n"
            ":waitloop\r\n"
            "tasklist /FI \"PID eq %%PARENT_PID%%\" 2>NUL | "
            "findstr /I \"%%PARENT_PID%%\" >NUL\r\n"
            "if not errorlevel 1 (\r\n"
            "  ping -n 2 127.0.0.1 >NUL\r\n"
            "  goto waitloop\r\n"
            ")\r\n"
            /* The pack's env.bat is its own statement of how to use it
             * (PATH, CC/CXX, SDL3_DIR); calling it beats a second copy. */
            "if exist \"%%PACK%%\\env.bat\" call \"%%PACK%%\\env.bat\"\r\n"
            "cd /d \"%%ROOT%%\"\r\n"
            /* A setup zip ships no build tree: configure on first rebuild. */
            "if not exist \"%%BUILD_DIR%%\\CMakeCache.txt\" (\r\n"
            "  echo Configuring...\r\n"
            "  \"%%CMAKE%%\" -S \"%%ROOT%%\" -B \"%%BUILD_DIR%%\" -G Ninja "
            "-DCMAKE_BUILD_TYPE=Release\r\n"
            "  if errorlevel 1 (\r\n"
            "    echo.\r\n"
            "    echo Configure failed. Fix the errors above, then retry.\r\n"
            "    pause\r\n"
            "    exit /b 1\r\n"
            "  )\r\n"
            ")\r\n"
            "echo Building...\r\n"
            /* The console shows the build live; the log keeps it for a
             * failure report after the window has gone. */
            "\"%%CMAKE%%\" --build \"%%BUILD_DIR%%\" --parallel "
            "--target \"%%TARGET%%\" 2>&1 | "
            "powershell -NoProfile -Command \"$input | Tee-Object -FilePath "
            "'%%BUILD_DIR%%\\snesrecomp_rebuild.log' -Append\"\r\n"
            "if errorlevel 1 (\r\n"
            "  echo.\r\n"
            "  echo Build failed. Fix the errors above, then rebuild manually.\r\n"
            "  pause\r\n"
            "  exit /b 1\r\n"
            ")\r\n"
            "echo Starting %%DISPLAY%%...\r\n"
            "start \"\" /D \"%%ROOT%%\" \"%%EXE%%\" --launcher\r\n"
            "endlocal\r\n");
    fclose(f);
    return 1;
}
#else
/* Run cmake with the given arguments (argv[0] is filled in here), streaming
 * its output to the progress callback. `what` names the step in errors. */
static int run_cmake_posix(char** cmake_args, int n_args, const char* what,
                           float pct_base,
                           RecompLauncherCPrepareProgressFn on_progress,
                           void* progress_ctx, char* err_msg, size_t err_cap) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(err_msg, err_cap, "pipe() failed: %s", strerror(errno));
        return 0;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    char* argv[24];
    int argc = 0;
    argv[argc++] = g_cmake;
    for (int i = 0; i < n_args && argc < 22; ++i) argv[argc++] = cmake_args[i];
    argv[argc] = NULL;
    char* wrapped[32];
    if (!shell_wrap_argv(argv, argc, wrapped, 32)) {
        close(pipefd[0]);
        close(pipefd[1]);
        posix_spawn_file_actions_destroy(&actions);
        snprintf(err_msg, err_cap, "Too many arguments for cmake %s.", what);
        return 0;
    }

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, wrapped[0], &actions, NULL, wrapped, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        snprintf(err_msg, err_cap, "Failed to spawn cmake %s: %s", what,
                 strerror(rc));
        return 0;
    }

    FILE* out = fdopen(pipefd[0], "r");
    if (!out) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        snprintf(err_msg, err_cap, "fdopen failed.");
        return 0;
    }
    char line[1024];
    int line_i = 0;
    while (fgets(line, sizeof(line), out)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (!line[0]) continue;
        step_log_line(line);
        if (on_progress) {
            float pct = pct_base + (float)((line_i++ % 80) / 100.0) * 0.5f;
            if (pct > 0.95f) pct = 0.95f;
            on_progress(progress_ctx, pct, line);
        }
    }
    fclose(out);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(err_msg, err_cap, "waitpid failed: %s", strerror(errno));
        return 0;
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (code == 0) return 1;
    char what_full[64];
    snprintf(what_full, sizeof(what_full), "cmake %s", what);
    step_fail_message(err_msg, err_cap, what_full, code);
    return 0;
}

static int build_dir_is_configured(void) {
    char cache[1400];
    return join_path(cache, sizeof(cache), g_build_dir, "CMakeCache.txt") &&
           path_is_file(cache);
}

/* First rebuild from a setup zip: there is no build tree yet, so configure
 * one. Ninja when the pack (or PATH) has it -- it is what the pack ships and
 * what CI used -- else CMake's default generator. Release, because a player is
 * not debugging the framework. */
static int run_cmake_configure_posix(RecompLauncherCPrepareProgressFn on_progress,
                                     void* progress_ctx, char* err_msg,
                                     size_t err_cap) {
    char ninja[1400];
    int have_ninja = (g_toolchain_root[0] &&
                      pack_tool(g_toolchain_root, "ninja", ninja, sizeof(ninja)))
                  || find_on_path("ninja", ninja, sizeof(ninja));
    char make_program[1500];
    snprintf(make_program, sizeof(make_program), "-DCMAKE_MAKE_PROGRAM=%s", ninja);
    char* args[12];
    int n = 0;
    args[n++] = "-S"; args[n++] = g_project_root;
    args[n++] = "-B"; args[n++] = g_build_dir;
    args[n++] = "-DCMAKE_BUILD_TYPE=Release";
    if (have_ninja) {
        args[n++] = "-G"; args[n++] = "Ninja";
        if (g_toolchain_root[0]) args[n++] = make_program;
    }
    mkdir_p(g_build_dir);
    step_log_open("cmake configure");
    return run_cmake_posix(args, n, "configure", 0.05f, on_progress, progress_ctx,
                           err_msg, err_cap);
}

static int run_cmake_build_posix(RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx, char* err_msg,
                                 size_t err_cap) {
    char* args[] = { "--build", g_build_dir, "--parallel", "--target", g_cmake_target };
    step_log_open("cmake --build");
    return run_cmake_posix(args, 5, "--build", 0.30f, on_progress, progress_ctx,
                           err_msg, err_cap);
}
#endif

static int host_rebuild_game(const char* rom_path, char* out_exe_path,
                             size_t out_cap, char* err_msg, size_t err_cap,
                             RecompLauncherCPrepareProgressFn on_progress,
                             void* progress_ctx) {
    (void)rom_path;
    g_relaunch_is_helper = 0;
    if (!g_ready || !g_build_dir[0]) {
        snprintf(err_msg, err_cap, "Project tree is not available.");
        return 0;
    }
    /* The wizard's toolchain page normally ran first; this is the fallback
     * for a skipped page or a cache pruned since -- fetch rather than fail. */
    char why[256];
    if (!resolve_tools(why, sizeof(why))) {
        if (on_progress)
            on_progress(progress_ctx, 0.02f, "Toolchain missing — fetching…");
        if (!host_ensure_toolchain_with_progress(1, NULL, err_msg, err_cap,
                                                 on_progress, progress_ctx))
            return 0;
    }
    if (!g_cmake[0]) {
        snprintf(err_msg, err_cap, "CMake build environment is not available.");
        return 0;
    }

#if defined(_WIN32)
    if (on_progress)
        on_progress(progress_ctx, 0.4f,
                    "Scheduling Windows rebuild after exit…");
    if (!write_windows_deferred_rebuild_helper(err_msg, err_cap))
        return 0;
    g_relaunch_is_helper = 1;
    snprintf(out_exe_path, out_cap, "%s", g_helper_path);
    if (on_progress)
        on_progress(progress_ctx, 1.0f,
                    "Exiting so Windows can rebuild safely…");
    return 1;
#else
    if (!build_dir_is_configured()) {
        if (on_progress)
            on_progress(progress_ctx, 0.05f, "Configuring build (first time)…");
        if (!run_cmake_configure_posix(on_progress, progress_ctx, err_msg, err_cap))
            return 0;
    }
    if (on_progress)
        on_progress(progress_ctx, 0.30f, "Starting cmake --build…");
    if (!run_cmake_build_posix(on_progress, progress_ctx, err_msg, err_cap))
        return 0;
    if (!path_is_file(g_exe_path)) {
        snprintf(err_msg, err_cap, "Build succeeded but binary missing: %s",
                 g_exe_path);
        return 0;
    }
    snprintf(out_exe_path, out_cap, "%s", g_exe_path);
    if (on_progress)
        on_progress(progress_ctx, 1.0f, "Build complete");
    return 1;
#endif
}

void snesrecomp_codegen_host_relaunch_or_exit(const char* rom_path) {
    char exe[512];
    if (!recomp_launcher_relaunch_exe(exe, sizeof(exe)) || !exe[0]) {
        fprintf(stderr, "snesrecomp-codegen: relaunch requested but no path\n");
        exit(1);
    }
    if (rom_path && rom_path[0]) {
        FILE* rc = fopen("rom.cfg", "w");
        if (rc) {
            fprintf(rc, "%s\n", rom_path);
            fclose(rc);
        }
    }

#if defined(_WIN32)
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char cmd[1536];
        DWORD flags = 0;
        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);
        if (g_relaunch_is_helper) {
            fprintf(stderr,
                    "snesrecomp-codegen: starting deferred rebuild helper\n");
            snprintf(cmd, sizeof(cmd), "cmd.exe /C \"%s\"", exe);
            flags = CREATE_NEW_CONSOLE;
        } else {
            fprintf(stderr, "snesrecomp-codegen: relaunching %s\n", exe);
            snprintf(cmd, sizeof(cmd), "\"%s\" --launcher", exe);
        }
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, flags, NULL,
                            g_project_root, &si, &pi)) {
            fprintf(stderr, "snesrecomp-codegen: CreateProcess failed\n");
            exit(1);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
#else
    {
        fprintf(stderr, "snesrecomp-codegen: relaunching %s\n", exe);
        char* args[] = { exe, "--launcher", NULL };
        execv(exe, args);
        perror("snesrecomp-codegen: execv failed");
        exit(1);
    }
#endif
}

void snesrecomp_codegen_host_autowire(RecompLauncherCGameInfo* gi,
                                      const char* display_name) {
    /* Static: apply() stores the pointer in g_cfg and dereferences it for the
     * life of the launcher, so this must outlive the call. */
    static SnesrecompCodegenHostConfig cfg;
    static char exe_name[256];

    if (!gi) return;
    if (!snesrecomp_exe_basename(exe_name, sizeof(exe_name))) return;

    memset(&cfg, 0, sizeof(cfg));
    cfg.display_name = display_name;
    /* The CMake target and the relaunch binary are the same name by
     * construction: the scaffolder names the executable after the target.
     * Every other field stays NULL so apply()'s defaults own the layout. */
    cfg.cmake_target = exe_name;
    cfg.exe_basename = exe_name;
    /* Game ports seed generation from bank .cfg roots; the scaffolder emits
     * them, so this is the scaffold default rather than a per-title choice. */
    cfg.cfg_roots = 1;

    snesrecomp_codegen_host_apply(gi, &cfg);
}

void snesrecomp_codegen_host_apply(RecompLauncherCGameInfo* gi,
                                   const SnesrecompCodegenHostConfig* cfg) {
    if (!gi || !cfg || !cfg->cmake_target || !cfg->exe_basename)
        return;

    g_cfg = cfg;
    g_ready = 0;
    g_relaunch_is_helper = 0;
    g_project_root[0] = '\0';
    g_cli_path[0] = '\0';
    g_python[0] = '\0';
    g_cmake[0] = '\0';
    g_build_dir[0] = '\0';
    g_exe_path[0] = '\0';
    g_helper_path[0] = '\0';

    snprintf(g_display, sizeof(g_display), "%s",
             cfg_or(cfg->display_name, "Game"));
    snprintf(g_cfg_dir, sizeof(g_cfg_dir), "%s",
             cfg_or(cfg->cfg_dir, "recomp"));
    snprintf(g_out_dir, sizeof(g_out_dir), "%s",
             cfg_or(cfg->out_dir, "src/gen"));
    /* recomp/funcs.h is a scaffold convention like recomp/ and src/gen/, and
     * the generated C #includes it -- so a generate that skips --funcs-h
     * produces a tree that cannot compile ("funcs.h file not found" on the
     * first bank), which is exactly what the zero-config autowire path did
     * when this had no default. An explicit empty string still opts out. */
    if (cfg->funcs_h && !cfg->funcs_h[0]) {
        g_has_funcs_h = 0;
        g_funcs_h[0] = '\0';
    } else {
        g_has_funcs_h = 1;
        snprintf(g_funcs_h, sizeof(g_funcs_h), "%s",
                 cfg_or(cfg->funcs_h, "recomp/funcs.h"));
    }
    snprintf(g_cmake_target, sizeof(g_cmake_target), "%s", cfg->cmake_target);
    g_cfg_roots = cfg->cfg_roots != 0;

    g_exe_dir[0] = '\0';
    if (!snesrecomp_exe_dir_path(".", g_exe_dir, sizeof(g_exe_dir)))
        g_exe_dir[0] = '\0';

    if (!discover_project_root(g_project_root, sizeof(g_project_root)))
        return;
    if (!join_path(g_cli_path, sizeof(g_cli_path), g_project_root,
                   cfg_or(cfg->snesrecomp_cli_relpath,
                          "snesrecomp/snesrecomp_cli.py")))
        return;
    if (!path_is_file(g_cli_path))
        return;

    /* Tools are resolved again right before generate and rebuild, because the
     * wizard's toolchain page may install them after this runs. A missing
     * python here is therefore not a reason to leave the wizard dark -- that
     * page exists precisely for the machine that has nothing yet. */
    {
        char why[256];
        (void)resolve_tools(why, sizeof(why));
    }

    g_ready = 1;
    /* The first-run wizard is opt-in per host (zero-init keeps a launcher
     * dark). This host IS the self-build flow, so it opts in -- and without
     * this line none of the fields below are ever shown. */
    gi->setup_wizard_supported = 1;
    gi->setup_needs_toolchain = 1;
    gi->toolchain_is_ready = host_toolchain_is_ready;
    gi->ensure_toolchain_with_progress = host_ensure_toolchain_with_progress;
    gi->toolchain_update_available = host_toolchain_update_available;

    gi->prepare_with_progress = host_prepare_generate;
    gi->prepare_use_selected_rom = 1;
    gi->prepare_section_title = "2. Generate C sources & rebuild";
    gi->prepare_busy_status = "Generating sources…";
    gi->prepare_success_status = "Sources ready — building…";

    /* Rebuild is always offered once the project tree is known: the build
     * directory is created on first use and the toolchain is fetched on
     * demand, so neither being absent now is a reason to demote the button. */
    const int can_rebuild = resolve_build_paths();
    if (can_rebuild) {
        gi->prepare_disc_label = "Generate & rebuild…";
#if defined(_WIN32)
        gi->prepare_disc_note =
            cfg->prepare_note_windows
                ? cfg->prepare_note_windows
                : "Regenerates sources with the local snesrecomp SDK, then "
                  "quits and rebuilds via a helper so the running .exe is not "
                  "locked. You must legally own this ROM.";
        gi->rebuild_busy_status = "Scheduling rebuild…";
        gi->rebuild_success_status =
            "Exiting for Windows rebuild — a console will finish the build…";
#else
        gi->prepare_disc_note =
            cfg->prepare_note
                ? cfg->prepare_note
                : "Regenerates sources with the local snesrecomp SDK, then "
                  "runs cmake --build and restarts into the new binary. You "
                  "must legally own this ROM.";
        gi->rebuild_busy_status = "Building game…";
        gi->rebuild_success_status = "Build complete — restarting…";
#endif
        gi->rebuild_with_progress = host_rebuild_game;
        gi->rebuild_after_prepare = 1;
        gi->relaunch_after_rebuild = 1;
    } else {
        gi->prepare_disc_label = "Generate sources…";
        gi->prepare_disc_note =
            cfg->prepare_note_no_cmake
                ? cfg->prepare_note_no_cmake
                : "Regenerates sources with the local snesrecomp SDK. "
                  "CMake/build dir not found — rebuild manually with "
                  "cmake --build, then relaunch.";
        gi->prepare_success_status =
            "Sources generated. Rebuild manually, then relaunch.";
    }

    const char* force_env =
        cfg_or(cfg->force_setup_env, "SNESRECOMP_FORCE_SETUP");
    const char* force = getenv(force_env);
    if (snesrecomp_codegen_host_sources_missing(cfg) ||
        (force && force[0] && force[0] != '0')) {
        gi->needs_setup = 1;
        gi->prepare_required_before_continue = 1;
    }
}

/*
 * host_report.c — always-on host-side crash/diagnostic capture.
 *
 * See host_report.h for the module contract. Ships in every build
 * configuration (Production included): the point is that when a user
 * we can't reproduce hits a crash, the artifacts next to their exe —
 * last_run_report.json + crash_report_*.json + crash_minidump_*.dmp —
 * carry enough to diagnose offline:
 *
 *   - breadcrumb ring: which boot stage was reached, with timings and
 *     device parameters (the "crashed before the Capcom logo" class of
 *     report becomes "crashed between SnesInit and audio-device open").
 *   - module list with load bases: host stack PCs from post_mortem's
 *     StackWalk64 become exe+offset resolvable against archived PDBs
 *     even though ASLR randomizes every run and users have no symbols.
 *   - minidump: full debugger-openable artifact, written first on the
 *     SEH path (most robust — before any JSON serialization runs).
 *
 * Design rules honored here:
 *   - always-on ring, queried at dump time (never "armed" at probe
 *     time) — matches the ring-buffer observability model used by the
 *     guest-side rings.
 *   - recorder is dumb and allocation-free on the crash path; dbghelp
 *     is resolved dynamically so no game build config needs new libs.
 */

#include "host_report.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#else
#include <pthread.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__linux__)
#include <link.h>
#endif
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#endif
#endif

#include <SDL.h>

/* ── Ring storage ───────────────────────────────────────────────────── */

#define HOST_BC_CAP 160
#define HOST_BC_MSG 192

typedef struct HostBreadcrumb {
    uint32_t t_ms;               /* monotonic ms since first breadcrumb */
    char     msg[HOST_BC_MSG];
} HostBreadcrumb;

static HostBreadcrumb g_bc_ring[HOST_BC_CAP];
static uint32_t g_bc_write_idx;          /* total writes; ring pos = idx % CAP */

static char g_game_name[64]     = "unknown";
static char g_build_version[64] = "dev";
static char g_fatal_msg[512];
static int  g_has_fatal;

/* ── Lock (lazy-init, same pattern as post_mortem.c) ────────────────── */

#ifdef _WIN32
static CRITICAL_SECTION g_hr_mutex;
static volatile LONG    g_hr_mutex_init = 0;
static void hr_lock(void) {
    if (InterlockedCompareExchange(&g_hr_mutex_init, 1, 0) == 0)
        InitializeCriticalSection(&g_hr_mutex);
    EnterCriticalSection(&g_hr_mutex);
}
static void hr_unlock(void) { LeaveCriticalSection(&g_hr_mutex); }
#else
static pthread_mutex_t g_hr_mutex = PTHREAD_MUTEX_INITIALIZER;
static void hr_lock(void)   { pthread_mutex_lock(&g_hr_mutex); }
static void hr_unlock(void) { pthread_mutex_unlock(&g_hr_mutex); }
#endif

/* ── Monotonic clock (SDL may not be initialized yet) ───────────────── */

static uint64_t mono_ms_raw(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / (f.QuadPart / 1000));
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static uint32_t mono_ms(void) {
    static uint64_t base;             /* anchored on first breadcrumb */
    uint64_t now = mono_ms_raw();
    if (base == 0) base = now;
    return (uint32_t)(now - base);
}

/* ── JSON string escape (local copy; post_mortem.c keeps its own) ───── */

static void hr_json_escape(const char *src, char *out, size_t cap) {
    if (cap == 0) return;
    size_t o = 0;
    if (src == NULL) { out[0] = 0; return; }
    for (const char *p = src; *p && o + 6 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' || c == '"') {
            out[o++] = '\\'; out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\'; out[o++] = 'n';
        } else if (c == '\r') {
            out[o++] = '\\'; out[o++] = 'r';
        } else if (c == '\t') {
            out[o++] = '\\'; out[o++] = 't';
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

/* ── Public: init / breadcrumbs / fatal ─────────────────────────────── */

void host_report_init(const char *game_name, const char *build_version) {
    if (game_name)     snprintf(g_game_name, sizeof(g_game_name), "%s", game_name);
    if (build_version) snprintf(g_build_version, sizeof(g_build_version), "%s", build_version);
    host_report_breadcrumb("host_report: %s %s", g_game_name, g_build_version);
}

static void breadcrumb_v(const char *fmt, va_list ap) {
    char msg[HOST_BC_MSG];
    vsnprintf(msg, sizeof(msg), fmt, ap);

    hr_lock();
    HostBreadcrumb *bc = &g_bc_ring[g_bc_write_idx % HOST_BC_CAP];
    bc->t_ms = mono_ms();
    snprintf(bc->msg, sizeof(bc->msg), "%s", msg);
    g_bc_write_idx++;
    hr_unlock();

    fprintf(stderr, "[host +%u.%03us] %s\n", bc->t_ms / 1000, bc->t_ms % 1000, msg);
}

void host_report_breadcrumb(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    breadcrumb_v(fmt, ap);
    va_end(ap);
}

void host_report_fatal(const char *msg) {
    hr_lock();
    snprintf(g_fatal_msg, sizeof(g_fatal_msg), "%s", msg ? msg : "(null)");
    g_has_fatal = 1;
    hr_unlock();
    host_report_breadcrumb("FATAL: %s", msg ? msg : "(null)");
}

int host_report_has_fatal(void) { return g_has_fatal; }

/* ── Host environment serialization ─────────────────────────────────── */

static void dump_build_json(FILE *f) {
    char esc_game[128], esc_ver[128];
    hr_json_escape(g_game_name, esc_game, sizeof(esc_game));
    hr_json_escape(g_build_version, esc_ver, sizeof(esc_ver));
    uint32_t pe_timestamp = 0;
#ifdef _WIN32
    /* PE link timestamp uniquely identifies the exe build even when the
     * version stamp says "dev". */
    {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
        if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const IMAGE_NT_HEADERS *nt =
                (const IMAGE_NT_HEADERS *)((const uint8_t *)dos + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                pe_timestamp = nt->FileHeader.TimeDateStamp;
        }
    }
#endif
    fprintf(f,
        "  \"build\": {\"game\": \"%s\", \"version\": \"%s\", \"pe_timestamp\": %u},\n",
        esc_game, esc_ver, pe_timestamp);
}

static void dump_cpu_brand(char *out, size_t cap) {
    out[0] = 0;
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    int regs[4];
    char brand[49];
    __cpuid(regs, 0x80000000);
    if ((unsigned)regs[0] >= 0x80000004) {
        for (int i = 0; i < 3; i++) {
            __cpuid(regs, 0x80000002 + i);
            memcpy(brand + i * 16, regs, 16);
        }
        brand[48] = 0;
        snprintf(out, cap, "%s", brand);
    }
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    unsigned int a, b, c, d;
    char brand[49];
    if (__get_cpuid(0x80000000, &a, &b, &c, &d) && a >= 0x80000004) {
        unsigned int *p = (unsigned int *)brand;
        for (unsigned int i = 0; i < 3; i++) {
            __get_cpuid(0x80000002 + i, &p[i*4], &p[i*4+1], &p[i*4+2], &p[i*4+3]);
        }
        brand[48] = 0;
        snprintf(out, cap, "%s", brand);
    }
#else
    (void)cap;
#endif
}

static void dump_os_json(FILE *f) {
#ifdef _WIN32
    /* RtlGetVersion tells the truth regardless of manifest, unlike
     * GetVersionEx. Resolved dynamically from ntdll. */
    DWORD major = 0, minor = 0, build = 0;
    {
        typedef LONG (WINAPI *pRtlGetVersion_t)(PRTL_OSVERSIONINFOW);
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        pRtlGetVersion_t pRtlGetVersion = ntdll
            ? (pRtlGetVersion_t)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
        if (pRtlGetVersion) {
            RTL_OSVERSIONINFOW vi;
            memset(&vi, 0, sizeof(vi));
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (pRtlGetVersion(&vi) == 0) {
                major = vi.dwMajorVersion;
                minor = vi.dwMinorVersion;
                build = vi.dwBuildNumber;
            }
        }
    }
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms;
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    char brand[64], esc_brand[128];
    dump_cpu_brand(brand, sizeof(brand));
    hr_json_escape(brand, esc_brand, sizeof(esc_brand));
    fprintf(f,
        "  \"os\": {\"platform\": \"windows\", \"version\": \"%lu.%lu.%lu\","
        " \"arch\": %u, \"cpus\": %lu, \"cpu_brand\": \"%s\","
        " \"ram_total_mb\": %llu, \"ram_avail_mb\": %llu},\n",
        (unsigned long)major, (unsigned long)minor, (unsigned long)build,
        (unsigned)si.wProcessorArchitecture,
        (unsigned long)si.dwNumberOfProcessors, esc_brand,
        (unsigned long long)(ms.ullTotalPhys >> 20),
        (unsigned long long)(ms.ullAvailPhys >> 20));
#else
    struct utsname un;
    char brand[64], esc_brand[128];
    dump_cpu_brand(brand, sizeof(brand));
    hr_json_escape(brand, esc_brand, sizeof(esc_brand));
    if (uname(&un) == 0) {
        char esc_sys[128], esc_rel[128], esc_mach[128];
        hr_json_escape(un.sysname, esc_sys, sizeof(esc_sys));
        hr_json_escape(un.release, esc_rel, sizeof(esc_rel));
        hr_json_escape(un.machine, esc_mach, sizeof(esc_mach));
        fprintf(f,
            "  \"os\": {\"platform\": \"%s\", \"version\": \"%s\","
            " \"arch\": \"%s\", \"cpu_brand\": \"%s\"},\n",
            esc_sys, esc_rel, esc_mach, esc_brand);
    } else {
        fprintf(f, "  \"os\": {\"platform\": \"posix\", \"cpu_brand\": \"%s\"},\n",
                esc_brand);
    }
#endif
}

static void dump_sdl_json(FILE *f) {
    SDL_version compiled, linked;
    SDL_VERSION(&compiled);
    SDL_GetVersion(&linked);
    const char *vd = SDL_GetCurrentVideoDriver();   /* NULL before/without init */
    const char *ad = SDL_GetCurrentAudioDriver();
    char esc_vd[64], esc_ad[64];
    hr_json_escape(vd ? vd : "(none)", esc_vd, sizeof(esc_vd));
    hr_json_escape(ad ? ad : "(none)", esc_ad, sizeof(esc_ad));
    fprintf(f,
        "  \"sdl\": {\"compiled\": \"%u.%u.%u\", \"linked\": \"%u.%u.%u\","
        " \"video_driver\": \"%s\", \"audio_driver\": \"%s\"},\n",
        compiled.major, compiled.minor, compiled.patch,
        linked.major, linked.minor, linked.patch, esc_vd, esc_ad);
}

#ifdef _WIN32
/* K32* module-enumeration entry points live in kernel32 on Win7+;
 * resolving them dynamically avoids a psapi link dependency across
 * every game's two build systems. */
typedef BOOL  (WINAPI *pEnumProcessModules_t)(HANDLE, HMODULE *, DWORD, LPDWORD);
typedef struct HR_MODULEINFO { LPVOID lpBaseOfDll; DWORD SizeOfImage; LPVOID EntryPoint; } HR_MODULEINFO;
typedef BOOL  (WINAPI *pGetModuleInformation_t)(HANDLE, HMODULE, HR_MODULEINFO *, DWORD);
#elif defined(__linux__)
typedef struct HrPhdrState { FILE *f; int emitted; } HrPhdrState;
static int hr_phdr_cb(struct dl_phdr_info *info, size_t size, void *data) {
    HrPhdrState *st = (HrPhdrState *)data;
    (void)size;
    char esc_path[1024];
    hr_json_escape(info->dlpi_name && info->dlpi_name[0]
                       ? info->dlpi_name : "(main)", esc_path, sizeof(esc_path));
    fprintf(st->f, "%s\n    {\"base\": %llu, \"path\": \"%s\"}",
            (st->emitted ? "," : ""),
            (unsigned long long)info->dlpi_addr, esc_path);
    st->emitted++;
    return 0;
}
#endif

static void dump_modules_json(FILE *f) {
#ifdef _WIN32
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    pEnumProcessModules_t pEnum = k32
        ? (pEnumProcessModules_t)GetProcAddress(k32, "K32EnumProcessModules") : NULL;
    pGetModuleInformation_t pInfo = k32
        ? (pGetModuleInformation_t)GetProcAddress(k32, "K32GetModuleInformation") : NULL;
    if (!pEnum || !pInfo) {
        fprintf(f, "  \"modules\": [],\n");
        return;
    }
    HMODULE mods[256];
    DWORD needed = 0;
    HANDLE proc = GetCurrentProcess();
    fprintf(f, "  \"modules\": [");
    if (pEnum(proc, mods, sizeof(mods), &needed)) {
        int n = (int)(needed / sizeof(HMODULE));
        if (n > (int)(sizeof(mods) / sizeof(mods[0])))
            n = (int)(sizeof(mods) / sizeof(mods[0]));
        int emitted = 0;
        for (int i = 0; i < n; i++) {
            char path[512];
            HR_MODULEINFO mi;
            memset(&mi, 0, sizeof(mi));
            if (!GetModuleFileNameA(mods[i], path, sizeof(path)))
                path[0] = 0;
            if (!pInfo(proc, mods[i], &mi, sizeof(mi)))
                continue;
            char esc_path[1024];
            hr_json_escape(path, esc_path, sizeof(esc_path));
            fprintf(f,
                "%s\n    {\"base\": %llu, \"size\": %lu, \"path\": \"%s\"}",
                (emitted ? "," : ""),
                (unsigned long long)(uintptr_t)mi.lpBaseOfDll,
                (unsigned long)mi.SizeOfImage, esc_path);
            emitted++;
        }
    }
    fprintf(f, "\n  ],\n");
#elif defined(__linux__)
    /* dl_iterate_phdr gives every loaded object's base — same ASLR
     * resolution story as the Windows module list. */
    fprintf(f, "  \"modules\": [");
    HrPhdrState st = { f, 0 };
    dl_iterate_phdr(hr_phdr_cb, &st);
    fprintf(f, "\n  ],\n");
#else
    fprintf(f, "  \"modules\": [],\n");
#endif
}

static void dump_breadcrumbs_json(FILE *f) {
    hr_lock();
    uint32_t widx = g_bc_write_idx;
    uint32_t n = widx < HOST_BC_CAP ? widx : HOST_BC_CAP;
    fprintf(f, "  \"breadcrumbs\": {\"write_idx\": %u, \"events\": [", widx);
    for (uint32_t i = 0; i < n; i++) {
        const HostBreadcrumb *bc = &g_bc_ring[(widx - n + i) % HOST_BC_CAP];
        char esc[HOST_BC_MSG * 2];
        hr_json_escape(bc->msg, esc, sizeof(esc));
        fprintf(f, "%s\n    {\"t_ms\": %u, \"msg\": \"%s\"}",
                (i ? "," : ""), bc->t_ms, esc);
    }
    fprintf(f, "\n  ]},\n");
    if (g_has_fatal) {
        char esc[1024];
        hr_json_escape(g_fatal_msg, esc, sizeof(esc));
        fprintf(f, "  \"fatal\": \"%s\",\n", esc);
    }
    hr_unlock();
}

void host_report_dump_json(FILE *f) {
    dump_build_json(f);
    dump_os_json(f);
    dump_sdl_json(f);
    dump_modules_json(f);
    dump_breadcrumbs_json(f);
}

/* ── Crash artifacts ────────────────────────────────────────────────── */

static void utc_stamp(char *out, size_t cap) {
    time_t tt = time(NULL);
    struct tm tmbuf;
#ifdef _WIN32
    if (gmtime_s(&tmbuf, &tt) != 0) { snprintf(out, cap, "unknown"); return; }
#else
    if (gmtime_r(&tt, &tmbuf) == NULL) { snprintf(out, cap, "unknown"); return; }
#endif
    strftime(out, cap, "%Y%m%d_%H%M%S", &tmbuf);
}

const char *host_report_write_minidump(void *seh_info) {
#ifdef _WIN32
    /* MiniDumpWriteDump prototype, resolved dynamically. */
    typedef BOOL (WINAPI *pMiniDumpWriteDump_t)(
        HANDLE, DWORD, HANDLE, int, void *, void *, void *);
    /* MINIDUMP_EXCEPTION_INFORMATION layout (dbghelp.h) — declared
     * locally so this file doesn't need dbghelp.h/link at build time.
     * dbghelp.h declares the MINIDUMP_* structs under 4-byte packing
     * (pshpack4.h); without it, x64 padding puts ExceptionPointers at
     * offset 8 instead of 4 and MiniDumpWriteDump reads garbage (fails,
     * verified by the crash drill). */
#pragma pack(push, 4)
    typedef struct {
        DWORD ThreadId;
        EXCEPTION_POINTERS *ExceptionPointers;
        BOOL  ClientPointers;
    } HR_MINIDUMP_EXCEPTION_INFORMATION;
#pragma pack(pop)

    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    pMiniDumpWriteDump_t pWrite = dbghelp
        ? (pMiniDumpWriteDump_t)GetProcAddress(dbghelp, "MiniDumpWriteDump") : NULL;
    if (!pWrite)
        return NULL;

    static char path[128];
    char stamp[32];
    utc_stamp(stamp, sizeof(stamp));
    snprintf(path, sizeof(path), "crash_minidump_%s.dmp", stamp);

    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return NULL;

    HR_MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = (EXCEPTION_POINTERS *)seh_info;
    mei.ClientPointers = FALSE;

    /* MiniDumpWithIndirectlyReferencedMemory (0x40) +
     * MiniDumpScanMemory (0x10) + MiniDumpWithThreadInfo (0x1000) +
     * MiniDumpWithUnloadedModules (0x20): full thread stacks + the
     * memory they reference, a few MB — rich enough to debug, small
     * enough for a GitHub attachment. */
    int dump_type = 0x40 | 0x10 | 0x1000 | 0x20;
    BOOL ok = pWrite(GetCurrentProcess(), GetCurrentProcessId(), file,
                     dump_type, seh_info ? &mei : NULL, NULL, NULL);
    CloseHandle(file);
    if (!ok) {
        DeleteFileA(path);
        return NULL;
    }
    host_report_breadcrumb("minidump written: %s", path);
    return path;
#else
    (void)seh_info;
    return NULL;
#endif
}

const char *host_report_preserve_crash_copy(const char *src_path) {
    static char dst[128];
    char stamp[32];
    utc_stamp(stamp, sizeof(stamp));
    snprintf(dst, sizeof(dst), "crash_report_%s.json", stamp);

    FILE *in = fopen(src_path, "rb");
    if (!in)
        return NULL;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return NULL;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return dst;
}

/* ── Crash drill ────────────────────────────────────────────────────── */

extern void Die(const char *error);

void host_report_crash_test_tick(void) {
    static int armed = -1, ticks = 0;
    static const char *mode;
    if (armed < 0) {
        mode = getenv("SNESRECOMP_CRASH_TEST");
        armed = (mode && mode[0]) ? 1 : 0;
    }
    if (!armed)
        return;
    if (++ticks < 120)
        return;
    host_report_breadcrumb("SNESRECOMP_CRASH_TEST firing (mode=%s)", mode);
    if (strcmp(mode, "die") == 0)
        Die("SNESRECOMP_CRASH_TEST=die drill");
    *(volatile int *)0 = 0x0DEAD;   /* deliberate AV -> SEH pipeline */
}

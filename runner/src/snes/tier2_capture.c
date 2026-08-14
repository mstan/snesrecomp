#include "tier2_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <process.h>
#define tier2_getpid _getpid
#else
#include <unistd.h>
#define tier2_getpid getpid
#endif

#define TIER2_CAPTURE_PATH_CAP 512

static char s_manifest_path[TIER2_CAPTURE_PATH_CAP];
static char s_journal_path[TIER2_CAPTURE_PATH_CAP];
static char s_capture_id[128];
static FILE *s_journal;
static int s_paths_ready;
static int s_close_registered;
static int s_announced;
static int s_verbose_checked;
static int s_verbose;

static int tier2_verbose(void) {
    if (!s_verbose_checked) {
        const char *value = getenv("SNESRECOMP_TIER2_VERBOSE");
        s_verbose = value && *value && *value != '0';
        s_verbose_checked = 1;
    }
    return s_verbose;
}

static void sanitize_romid(const char *title, char *out, size_t cap) {
    size_t n = 0;
    if (!title || !*title) title = "unknown";
    for (const char *p = title; *p && n + 1 < cap; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        out[n++] = ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                     ? (char)c : '_';
    }
    out[n] = '\0';
}

static void sanitize_json(const char *src, char *out, size_t cap) {
    size_t n = 0;
    if (!src) src = "unknown";
    for (const char *p = src; *p && n + 1 < cap; ++p) {
        unsigned char c = (unsigned char)*p;
        out[n++] = (c == '"' || c == '\\' || c < 0x20) ? '_' : (char)c;
    }
    out[n] = '\0';
}

static void derive_journal_path(const char *manifest) {
    size_t n = strlen(manifest);
    const char *suffix = ".jsonl";
    if (n >= 5 && strcmp(manifest + n - 5, ".json") == 0) {
        n -= 5;
    }
    if (n + strlen(suffix) + 1 > sizeof s_journal_path) {
        fprintf(stderr, "[tier2] manifest path is too long for journal suffix\n");
        s_journal_path[0] = '\0';
        return;
    }
    memcpy(s_journal_path, manifest, n);
    memcpy(s_journal_path + n, suffix, strlen(suffix) + 1);
}

static void init_paths(const char *rom_title) {
    if (s_paths_ready) return;
    s_paths_ready = 1;

    char romid[64];
    sanitize_romid(rom_title, romid, sizeof romid);
    time_t now = time(NULL);
    struct tm utc;
#ifdef _WIN32
    if (gmtime_s(&utc, &now) != 0) memset(&utc, 0, sizeof utc);
#else
    if (gmtime_r(&now, &utc) == NULL) memset(&utc, 0, sizeof utc);
#endif
    char stamp[32];
    if (strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &utc) == 0)
        snprintf(stamp, sizeof stamp, "%lld", (long long)now);
    snprintf(s_capture_id, sizeof s_capture_id, "%s_%s_p%ld",
             romid, stamp, (long)tier2_getpid());

    const char *manifest = getenv("SNESRECOMP_TIER2_MANIFEST");
    if (manifest && *manifest)
        snprintf(s_manifest_path, sizeof s_manifest_path, "%s", manifest);
    else
        snprintf(s_manifest_path, sizeof s_manifest_path,
                 "tier2_%s.json", s_capture_id);

    const char *journal = getenv("SNESRECOMP_TIER2_JOURNAL");
    if (journal && *journal)
        snprintf(s_journal_path, sizeof s_journal_path, "%s", journal);
    else
        derive_journal_path(s_manifest_path);
}

const char *tier2_capture_manifest_path(const char *rom_title) {
    init_paths(rom_title);
    return s_manifest_path;
}

const char *tier2_capture_journal_path(const char *rom_title) {
    init_paths(rom_title);
    return s_journal_path;
}

void tier2_capture_close(void) {
    if (s_journal) {
        if (fclose(s_journal) != 0)
            fprintf(stderr, "[tier2] failed to close dispatch-miss journal: %s\n",
                    s_journal_path);
        s_journal = NULL;
    }
}

int tier2_capture_append_discovery(const char *rom_title,
                                   uint32_t site_pc24,
                                   uint32_t target_pc24,
                                   const char *entry_mx,
                                   const char *site_kind,
                                   int outcome,
                                   int32_t frame) {
    init_paths(rom_title);
    if (!s_journal) {
        s_journal = fopen(s_journal_path, "a");
        if (!s_journal) {
            fprintf(stderr, "[tier2] cannot append dispatch-miss journal: %s\n",
                    s_journal_path);
            return 0;
        }
        if (!s_close_registered) {
            s_close_registered = 1;
            atexit(tier2_capture_close);
        }
        if (!s_announced) {
            s_announced = 1;
            if (tier2_verbose())
                fprintf(stderr,
                        "[tier2] append-only dispatch-miss journal: %s\n",
                        s_journal_path);
        }
    }

    char title[128];
    sanitize_json(rom_title, title, sizeof title);
    if (fprintf(s_journal,
            "{\"schema\":\"snesrecomp tier2 discovery v1\","
            "\"capture_id\":\"%s\",\"rom_title\":\"%s\","
            "\"site_pc24\":\"0x%06X\",\"target_pc24\":\"0x%06X\","
            "\"entry_mx\":\"%s\",\"site_kind\":\"%s\","
            "\"clean_hits\":%d,\"bail_hits\":%d,\"outcome_pending\":%s,"
            "\"first_frame\":%d,\"last_frame\":%d}\n",
            s_capture_id, title,
            (unsigned)(site_pc24 & 0xFFFFFFu),
            (unsigned)(target_pc24 & 0xFFFFFFu),
            entry_mx, site_kind, outcome > 0 ? 1 : 0, outcome == 0 ? 1 : 0,
            outcome < 0 ? "true" : "false",
            (int)frame, (int)frame) < 0 || fflush(s_journal) != 0) {
        fprintf(stderr, "[tier2] failed appending dispatch-miss journal: %s\n",
                s_journal_path);
        clearerr(s_journal);
        return 0;
    }
    return 1;
}

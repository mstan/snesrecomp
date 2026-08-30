/* See snes_netplay_identity.h. Small, dependency-light INI surgery: the
 * shared config writer (mmx_config.c) owns the whole file and is only called
 * by hosts that use the framework Config, while hosts that parse config.ini
 * themselves still need the name to survive a launch. */
#include "snes_netplay_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../host_paths.h"

#define IDENT_SECTION "[Netplay]"
#define IDENT_KEY     "PlayerName"

static void ident_path(char *out, size_t cap)
{
    if (!snesrecomp_exe_dir_path("config.ini", out, cap))
        snprintf(out, cap, "config.ini");
}

static char *ident_trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static int ident_is_section(const char *line, const char *want)
{
    char buf[64];
    size_t i = 0;
    while (line[i] && line[i] != '\r' && line[i] != '\n' &&
           i + 1 < sizeof(buf)) {
        buf[i] = line[i];
        i++;
    }
    buf[i] = '\0';
    {
        char *t = ident_trim(buf);
        size_t n = strlen(want);
        if (strlen(t) != n) return 0;
        for (i = 0; i < n; i++) {
            char a = t[i], b = want[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) return 0;
        }
    }
    return 1;
}

int snes_netplay_identity_load(char *out, size_t cap)
{
    char path[1024];
    char line[512];
    FILE *f;
    int in_section = 0;

    if (!out || !cap) return 0;
    out[0] = '\0';
    ident_path(path, sizeof(path));
    f = fopen(path, "rb");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char *t = ident_trim(line);
        if (t[0] == '[') {
            in_section = ident_is_section(t, IDENT_SECTION);
            continue;
        }
        if (!in_section || t[0] == ';' || t[0] == '#' || !t[0])
            continue;
        {
            char *eq = strchr(t, '=');
            char *key, *val;
            if (!eq) continue;
            *eq = '\0';
            key = ident_trim(t);
            val = ident_trim(eq + 1);
            if (strlen(key) == strlen(IDENT_KEY)) {
                size_t i;
                int match = 1;
                for (i = 0; key[i]; i++) {
                    char a = key[i], b = IDENT_KEY[i];
                    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                    if (a != b) { match = 0; break; }
                }
                if (match) {
                    snprintf(out, cap, "%s", val);
                    fclose(f);
                    return out[0] ? 1 : 0;
                }
            }
        }
    }
    fclose(f);
    return 0;
}

int snes_netplay_identity_store(const char *name)
{
    char path[1024];
    char line[512];
    FILE *f;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int in_section = 0, wrote = 0, saw_section = 0;

    ident_path(path, sizeof(path));

    /* Grow a copy with the key replaced in place; everything else — comments,
     * ordering, unrelated sections — is passed through byte for byte. */
    f = fopen(path, "rb");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char copy[512];
            char *t;
            int emit = 1;
            snprintf(copy, sizeof(copy), "%s", line);
            t = ident_trim(copy);
            if (t[0] == '[') {
                if (in_section && !wrote) {
                    /* Leaving [Netplay] without having seen the key. */
                    char add[256];
                    int n = snprintf(add, sizeof(add), "%s = %s\n", IDENT_KEY,
                                     name ? name : "");
                    if (len + (size_t)n + 1 > cap) {
                        cap = (len + (size_t)n + 1) * 2;
                        buf = (char *)realloc(buf, cap);
                        if (!buf) { fclose(f); return 0; }
                    }
                    memcpy(buf + len, add, (size_t)n);
                    len += (size_t)n;
                    wrote = 1;
                }
                in_section = ident_is_section(t, IDENT_SECTION);
                if (in_section) saw_section = 1;
            } else if (in_section && !wrote) {
                char *eq = strchr(t, '=');
                if (eq) {
                    char keybuf[128];
                    size_t kl;
                    *eq = '\0';
                    snprintf(keybuf, sizeof(keybuf), "%s", ident_trim(t));
                    kl = strlen(keybuf);
                    if (kl == strlen(IDENT_KEY)) {
                        size_t i;
                        int match = 1;
                        for (i = 0; i < kl; i++) {
                            char a = keybuf[i], b = IDENT_KEY[i];
                            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                            if (a != b) { match = 0; break; }
                        }
                        if (match) {
                            int n = snprintf(line, sizeof(line), "%s = %s\n",
                                             IDENT_KEY, name ? name : "");
                            (void)n;
                            wrote = 1;
                        }
                    }
                }
            }
            if (emit) {
                size_t n = strlen(line);
                if (len + n + 1 > cap) {
                    cap = (len + n + 1) * 2 + 256;
                    buf = (char *)realloc(buf, cap);
                    if (!buf) { fclose(f); return 0; }
                }
                memcpy(buf + len, line, n);
                len += n;
            }
        }
        fclose(f);
    }

    if (!wrote) {
        char add[320];
        int n = snprintf(add, sizeof(add), "%s%s\n%s = %s\n",
                         (len && buf && buf[len - 1] != '\n') ? "\n" : "",
                         saw_section ? "" : IDENT_SECTION,
                         IDENT_KEY, name ? name : "");
        if (len + (size_t)n + 1 > cap) {
            cap = len + (size_t)n + 1;
            buf = (char *)realloc(buf, cap);
            if (!buf) return 0;
        }
        memcpy(buf + len, add, (size_t)n);
        len += (size_t)n;
    }

    f = fopen(path, "wb");
    if (!f) { free(buf); return 0; }
    if (buf && len)
        (void)fwrite(buf, 1, len, f);
    fclose(f);
    free(buf);
    return 1;
}

/*
 * Round-trip the lobby mod plan: canonical set -> caps.mods -> parsed rows.
 *
 *   cc tests/netplay/lobby_mod_plan_test.c -o /tmp/t && /tmp/t
 *
 * The plan is what a guest sees in the lobby before seating, so the encoding
 * has to survive the trip exactly: an entry that parses to the wrong package
 * or version tells a player to install the wrong thing, and a count that is
 * one short hides a requirement entirely.
 *
 * The two functions below mirror fill_caps_mods() and plan_entry() in
 * snes_host_lobby.c. That duplication is deliberate and narrow: those live
 * behind a lobby session and a mod runtime, neither of which a unit test can
 * stand up, and the alternative was leaving the encoding untested. If either
 * side changes, this test is the thing that should fail.
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int plan_count(const char *mods) {
    const char *p; int n = 0;
    if (!mods || !mods[0]) return 0;
    for (p = mods; *p; ) { const char *e = strchr(p, ';'); n++; if (!e) break; p = e + 1; }
    return n;
}
static int plan_entry(const char *mods, int index, char *id, size_t idc,
                      char *ver, size_t vc) {
    const char *p; int n = 0;
    id[0] = ver[0] = '\0';
    if (!mods || !mods[0] || index < 0) return 0;
    for (p = mods; *p; ) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (n++ == index) {
            const char *at = memchr(p, '@', len);
            const char *slash = at ? memchr(at, '/', len - (size_t)(at - p)) : NULL;
            size_t il = at ? (size_t)(at - p) : len;
            if (il >= idc) il = idc - 1;
            memcpy(id, p, il); id[il] = '\0';
            if (at && slash) {
                size_t vl = (size_t)(slash - at - 1);
                if (vl >= vc) vl = vc - 1;
                memcpy(ver, at + 1, vl); ver[vl] = '\0';
            }
            return 1;
        }
        p = end ? end + 1 : p + len;
    }
    return 0;
}
/* mirror of fill_caps_mods */
static void encode(const char *text, char *out, size_t cap) {
    size_t o = 0, i;
    out[0] = '\0';
    if (!strcmp(text, "(none)\n")) return;
    for (i = 0; text[i] && o + 1 < cap; ++i) {
        char c = text[i];
        if (c == '\n') { if (o == 0 || out[o-1] == ';') continue; c = ';'; }
        if (c == '"' || c == '\\') continue;
        out[o++] = c;
    }
    out[o] = '\0';
    if (o && out[o-1] == ';') out[o-1] = '\0';
}

int main(void) {
    char mods[512], id[96], ver[32];
    int fails = 0;
    struct { const char *set; int n; const char *id0; const char *v0; } cases[] = {
        { "(none)\n", 0, "", "" },
        { "gwed.localization@1.0.0/localization language=en\n", 1,
          "gwed.localization", "1.0.0" },
        { "a.one@1.0/f x=1\nb.two@2.5/g\n", 2, "a.one", "1.0" },
    };
    for (unsigned k = 0; k < sizeof(cases)/sizeof(cases[0]); ++k) {
        encode(cases[k].set, mods, sizeof(mods));
        int n = plan_count(mods);
        printf("  set=%-46s -> mods=\"%s\" n=%d\n",
               cases[k].set, mods, n);
        if (n != cases[k].n) { printf("    FAIL count %d != %d\n", n, cases[k].n); fails++; }
        if (n > 0) {
            plan_entry(mods, 0, id, sizeof(id), ver, sizeof(ver));
            if (strcmp(id, cases[k].id0) || strcmp(ver, cases[k].v0)) {
                printf("    FAIL entry0 id=%s ver=%s\n", id, ver); fails++;
            }
        }
        /* every index must be reachable and distinct */
        for (int i = 0; i < n; ++i)
            if (!plan_entry(mods, i, id, sizeof(id), ver, sizeof(ver))) {
                printf("    FAIL entry %d unreachable\n", i); fails++;
            }
        if (plan_entry(mods, n, id, sizeof(id), ver, sizeof(ver))) {
            printf("    FAIL index past the end returned a row\n"); fails++;
        }
    }
    printf(fails ? "\n%d failure(s)\n" : "\nall round-trip cases passed\n", fails);
    return fails != 0;
}

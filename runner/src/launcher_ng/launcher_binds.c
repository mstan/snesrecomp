// launcher_binds.c — real binding persistence (keybinds.ini + config.ini [KeyMap]).

#include "launcher_binds.h"
#include "launcher_sdlcompat.h"   // SDL header (2 or 3)
#include "keybinds.h"             // engine keyboard-binding store

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MSVC spells these differently than POSIX.
#ifdef _MSC_VER
  #define strtok_r     strtok_s
  #define strncasecmp  _strnicmp
  #define strdup       _strdup
#endif

const char* g_launcher_config_path = NULL;

// LngButton -> keybinds button index. keybinds order is
// a,b,x,y,l,r,start,select,up,down,left,right (see keybinds.h).
static const int kKbIndex[LNG_BTN_COUNT] = {
    /* UP    */ 8, /* DOWN  */ 9, /* LEFT  */ 10, /* RIGHT */ 11,
    /* A     */ 0, /* B     */ 1, /* X     */ 2,  /* Y     */ 3,
    /* L     */ 4, /* R     */ 5, /* START */ 6,  /* SELECT*/ 7,
};

// The engine's config.ini [KeyMap] keys, in LngHotkey order.
static const char* kHotkeyKey[LNG_HK_COUNT] = {
    "Fullscreen", "Reset", "Pause", "PauseDimmed", "Turbo",
    "WindowBigger", "WindowSmaller", "VolumeUp", "VolumeDown",
    "DisplayPerf", "ToggleRenderer"
};
// Built-in defaults (shown when config.ini has no line; "" = unbound).
static const char* kHotkeyDef[LNG_HK_COUNT] = {
    "Alt+Return", "Ctrl+R", "Shift+P", "P", "Tab",
    "", "", "", "", "F", "R"
};

static void copy_str(char* d, size_t cap, const char* s) {
    if (!d || !cap) return;
    if (!s) { d[0] = 0; return; }
    size_t n = strlen(s); if (n >= cap) n = cap - 1;
    memcpy(d, s, n); d[n] = 0;
}

static const char* scancode_label(SDL_Scancode sc) {
    if (sc == SDL_SCANCODE_UNKNOWN) return "(unbound)";
    const char* n = SDL_GetScancodeName(sc);
    return (n && n[0]) ? n : "(unbound)";
}

static void reload_player_display(LauncherModel* m, int player) {
    for (int b = 0; b < LNG_BTN_COUNT; ++b) {
        SDL_Scancode sc = keybinds_get_button(player, kKbIndex[b]);
        copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]), scancode_label(sc));
    }
}

// ---- config.ini [KeyMap] surgical read/write (ported from the RmlUi launcher) --

static int ieq(const char* a, size_t alen, const char* b) {
    size_t bl = strlen(b);
    if (alen != bl) return 0;
    for (size_t i = 0; i < alen; ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

static char* read_whole(const char* path, long* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) { *out_len = 0; return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)n + 1);
    if (buf) { *out_len = (long)fread(buf, 1, (size_t)n, f); buf[*out_len] = 0; }
    fclose(f);
    return buf;
}

static const char* config_path(void) {
    return (g_launcher_config_path && g_launcher_config_path[0])
             ? g_launcher_config_path : "config.ini";
}

// Fill m->hotkeys[] from config.ini [KeyMap] (or defaults where absent).
static void reload_hotkey_display(LauncherModel* m) {
    for (int h = 0; h < LNG_HK_COUNT; ++h)
        copy_str(m->hotkeys[h], sizeof(m->hotkeys[h]), kHotkeyDef[h]);

    long len = 0; char* text = read_whole(config_path(), &len);
    if (!text) return;

    int in_keymap = 0;
    char* save = NULL;
    for (char* line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        size_t l = strlen(p);
        while (l && (p[l-1] == '\r' || p[l-1] == ' ' || p[l-1] == '\t')) p[--l] = 0;
        if (!*p || *p == '#') continue;
        if (*p == '[') {
            char* close = strchr(p, ']');
            size_t sl = close ? (size_t)(close - p - 1) : strlen(p + 1);
            in_keymap = ieq(p + 1, sl, "KeyMap");
            continue;
        }
        if (!in_keymap) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        char* ke = eq; while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t')) --ke;
        size_t klen = (size_t)(ke - p);
        char* v = eq + 1; while (*v == ' ' || *v == '\t') ++v;
        char* hash = strchr(v, '#'); if (hash) *hash = 0;
        size_t vl = strlen(v); while (vl && (v[vl-1] == ' ' || v[vl-1] == '\t')) v[--vl] = 0;
        for (int h = 0; h < LNG_HK_COUNT; ++h)
            if (ieq(p, klen, kHotkeyKey[h])) { copy_str(m->hotkeys[h], sizeof(m->hotkeys[h]), v); break; }
    }
    free(text);
}

// Does `line` (leading ws / optional '#') assign `key`? Mirrors config.c.
static int line_is_key(const char* line, const char* key) {
    const char* i = line;
    while (*i == ' ' || *i == '\t') ++i;
    if (*i == '#') { ++i; while (*i == ' ' || *i == '\t') ++i; }
    size_t kl = strlen(key);
    if (strncasecmp(i, key, kl) != 0) return 0;
    i += kl;
    while (*i == ' ' || *i == '\t') ++i;
    return *i == '=';
}

// Surgically set "Key = value" inside [KeyMap], preserving all other lines.
static void keymap_write(const char* key, const char* value) {
    const char* path = config_path();
    long len = 0; char* text = read_whole(path, &len);
    /* split into a growable line array, PRESERVING blank lines (strtok would
     * collapse them, losing the user's config formatting). */
    int cap = 64, n = 0;
    char** lines = (char**)malloc(sizeof(char*) * cap);
    if (text) {
        char* start = text;
        for (long i = 0; i <= len; ++i) {
            if (i == len || text[i] == '\n') {
                char* end = text + i;
                if (end > start && end[-1] == '\r') end[-1] = 0;
                else if (i < len) text[i] = 0;
                else text[i] = 0;
                if (i == len && start == text + len) break;  // no trailing empty
                if (n == cap) { cap *= 2; lines = (char**)realloc(lines, sizeof(char*) * cap); }
                lines[n++] = strdup(start);
                start = text + i + 1;
            }
        }
    }
    char assign[128];
    snprintf(assign, sizeof(assign), "%s = %s", key, value ? value : "");

    /* locate [KeyMap] body [start,end) */
    int ks = -1, ke = -1;
    for (int i = 0; i < n; ++i) {
        const char* p = lines[i]; while (*p == ' ' || *p == '\t') ++p;
        if (*p != '[') continue;
        const char* close = strchr(p, ']');
        size_t sl = close ? (size_t)(close - p - 1) : strlen(p + 1);
        if (ks >= 0) { ke = i; break; }
        if (ieq(p + 1, sl, "KeyMap")) ks = i + 1;
    }
    if (ks >= 0 && ke < 0) ke = n;

    if (ks < 0) {
        if (n == cap) { cap += 4; lines = (char**)realloc(lines, sizeof(char*) * cap); }
        if (n && lines[n-1][0]) lines[n++] = strdup("");
        if (n == cap) { cap += 4; lines = (char**)realloc(lines, sizeof(char*) * cap); }
        lines[n++] = strdup("[KeyMap]");
        if (n == cap) { cap += 4; lines = (char**)realloc(lines, sizeof(char*) * cap); }
        lines[n++] = strdup(assign);
    } else {
        int hit = -1;
        for (int i = ks; i < ke; ++i) if (line_is_key(lines[i], key)) { hit = i; break; }
        if (hit >= 0) { free(lines[hit]); lines[hit] = strdup(assign); }
        else {
            int at = ke;
            while (at > ks) { const char* p = lines[at-1]; while (*p==' '||*p=='\t') ++p; if (*p) break; --at; }
            if (n == cap) { cap += 4; lines = (char**)realloc(lines, sizeof(char*) * cap); }
            for (int i = n; i > at; --i) lines[i] = lines[i-1];
            lines[at] = strdup(assign); ++n;
        }
    }

    FILE* f = fopen(path, "wb");
    if (f) { for (int i = 0; i < n; ++i) { fputs(lines[i], f); fputc('\n', f); } fclose(f); }
    for (int i = 0; i < n; ++i) free(lines[i]);
    free(lines); free(text);
}

// Format SDL keycode + mods the way config.c's ParseKeyArray reads back.
static void format_hotkey(int keycode, int kmod, char* out, size_t cap) {
    out[0] = 0;
    if (keycode == 0) return;              // unbound
    char buf[96]; buf[0] = 0;
    if (kmod & SDL_KMOD_CTRL)  strncat(buf, "Ctrl+",  sizeof(buf)-strlen(buf)-1);
    if (kmod & SDL_KMOD_ALT)   strncat(buf, "Alt+",   sizeof(buf)-strlen(buf)-1);
    if (kmod & SDL_KMOD_SHIFT) strncat(buf, "Shift+", sizeof(buf)-strlen(buf)-1);
    const char* kn = SDL_GetKeyName((SDL_Keycode)keycode);
    if (!kn || !kn[0]) return;
    strncat(buf, kn, sizeof(buf)-strlen(buf)-1);
    copy_str(out, cap, buf);
}

// ---- public API -------------------------------------------------------------

void launcher_binds_load(LauncherModel* m, const char* config_path_in) {
    g_launcher_config_path = config_path_in;
    keybinds_init(NULL);               // load/generate keybinds.ini (exe-anchored)
    reload_player_display(m, 1);
    reload_player_display(m, 2);
    reload_hotkey_display(m);
}

void launcher_binds_set_button(LauncherModel* m, int player, LngButton b, int scancode) {
    if (b < 0 || b >= LNG_BTN_COUNT || player < 1 || player > 2) return;
    keybinds_set_button(player, kKbIndex[b], (SDL_Scancode)scancode);
    keybinds_save();
    copy_str(m->binds[player - 1][b], sizeof(m->binds[player - 1][b]),
             scancode_label((SDL_Scancode)scancode));
}

void launcher_binds_reset_player(LauncherModel* m, int player) {
    if (player < 1 || player > 2) return;
    keybinds_reset_player(player);
    keybinds_save();
    reload_player_display(m, player);
}

void launcher_binds_set_hotkey(LauncherModel* m, LngHotkey h, int keycode, int kmod) {
    if (h < 0 || h >= LNG_HK_COUNT) return;
    char val[64];
    format_hotkey(keycode, kmod, val, sizeof(val));
    keymap_write(kHotkeyKey[h], val);
    copy_str(m->hotkeys[h], sizeof(m->hotkeys[h]), val[0] ? val : "(unbound)");
}

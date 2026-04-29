/*
 * keybinds.c — SNES controller keybinds, INI-driven.
 *
 * INI lives next to the exe as keybinds.ini. Auto-generated with sane
 * defaults when missing. Edit and restart to apply.
 */
#include "keybinds.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stddef.h>

/* ── Defaults ─────────────────────────────────────────────────────────────── */
/*
 * Defaults match the SMW/Zelda3-HLE legacy layout:
 *   D-pad: Arrow keys
 *   Select: RShift     Start: Return
 *   Y/B (face L/D):    A / Z
 *   X/A (face U/R):    S / X
 *   L/R (shoulders):   C / V
 */

static KeyBinds s_binds = {
    .p1 = {
        .a      = SDL_SCANCODE_X,
        .b      = SDL_SCANCODE_Z,
        .x      = SDL_SCANCODE_S,
        .y      = SDL_SCANCODE_A,
        .l      = SDL_SCANCODE_C,
        .r      = SDL_SCANCODE_V,
        .start  = SDL_SCANCODE_RETURN,
        .select = SDL_SCANCODE_RSHIFT,
        .up     = SDL_SCANCODE_UP,
        .down   = SDL_SCANCODE_DOWN,
        .left   = SDL_SCANCODE_LEFT,
        .right  = SDL_SCANCODE_RIGHT,
    },
    /* Player 2 unbound by default. Add bindings in the INI to enable. */
    .p2 = {
        .a = SDL_SCANCODE_UNKNOWN, .b = SDL_SCANCODE_UNKNOWN,
        .x = SDL_SCANCODE_UNKNOWN, .y = SDL_SCANCODE_UNKNOWN,
        .l = SDL_SCANCODE_UNKNOWN, .r = SDL_SCANCODE_UNKNOWN,
        .start = SDL_SCANCODE_UNKNOWN, .select = SDL_SCANCODE_UNKNOWN,
        .up    = SDL_SCANCODE_UNKNOWN, .down  = SDL_SCANCODE_UNKNOWN,
        .left  = SDL_SCANCODE_UNKNOWN, .right = SDL_SCANCODE_UNKNOWN,
    },
};

typedef struct {
    const char *name;
    size_t      offset;  /* offset into PlayerBinds */
} ButtonDef;

static const ButtonDef s_buttons[] = {
    { "a",      offsetof(PlayerBinds, a)      },
    { "b",      offsetof(PlayerBinds, b)      },
    { "x",      offsetof(PlayerBinds, x)      },
    { "y",      offsetof(PlayerBinds, y)      },
    { "l",      offsetof(PlayerBinds, l)      },
    { "r",      offsetof(PlayerBinds, r)      },
    { "start",  offsetof(PlayerBinds, start)  },
    { "select", offsetof(PlayerBinds, select) },
    { "up",     offsetof(PlayerBinds, up)     },
    { "down",   offsetof(PlayerBinds, down)   },
    { "left",   offsetof(PlayerBinds, left)   },
    { "right",  offsetof(PlayerBinds, right)  },
    { NULL, 0 }
};

/* ── INI parsing helpers ──────────────────────────────────────────────────── */

static void trim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n-1])) s[--n] = '\0';
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static SDL_Scancode name_to_scancode(const char *name) {
    if (!name || !*name) return SDL_SCANCODE_UNKNOWN;
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc != SDL_SCANCODE_UNKNOWN) return sc;
    /* Normalise common short aliases. */
    char buf[32];
    size_t i = 0;
    for (; name[i] && i < sizeof(buf) - 1; i++) buf[i] = (char)tolower((unsigned char)name[i]);
    buf[i] = '\0';
    if (!strcmp(buf, "enter") || !strcmp(buf, "return")) return SDL_SCANCODE_RETURN;
    if (!strcmp(buf, "tab"))                              return SDL_SCANCODE_TAB;
    if (!strcmp(buf, "space"))                            return SDL_SCANCODE_SPACE;
    if (!strcmp(buf, "lshift"))                           return SDL_SCANCODE_LSHIFT;
    if (!strcmp(buf, "rshift"))                           return SDL_SCANCODE_RSHIFT;
    if (!strcmp(buf, "lctrl"))                            return SDL_SCANCODE_LCTRL;
    if (!strcmp(buf, "rctrl"))                            return SDL_SCANCODE_RCTRL;
    if (!strcmp(buf, "backslash"))                        return SDL_SCANCODE_BACKSLASH;
    if (!strcmp(buf, "escape") || !strcmp(buf, "esc"))    return SDL_SCANCODE_ESCAPE;
    if (!strcmp(buf, "backspace"))                        return SDL_SCANCODE_BACKSPACE;
    if (!strcmp(buf, "none") || !strcmp(buf, ""))         return SDL_SCANCODE_UNKNOWN;
    return SDL_SCANCODE_UNKNOWN;
}

static const char *scancode_to_name(SDL_Scancode sc) {
    if (sc == SDL_SCANCODE_UNKNOWN) return "None";
    const char *name = SDL_GetScancodeName(sc);
    return (name && name[0]) ? name : "None";
}

/* ── File I/O ─────────────────────────────────────────────────────────────── */

static char s_ini_path[512] = {0};

static void derive_ini_path(const char *exe_path) {
    if (!exe_path || !*exe_path) {
        strcpy(s_ini_path, "keybinds.ini");
        return;
    }
    const char *slash = NULL;
    for (const char *p = exe_path; *p; p++)
        if (*p == '/' || *p == '\\') slash = p;
    if (slash) {
        size_t dir_len = (size_t)(slash - exe_path) + 1;
        if (dir_len + 13 < sizeof(s_ini_path)) {
            memcpy(s_ini_path, exe_path, dir_len);
            strcpy(s_ini_path + dir_len, "keybinds.ini");
            return;
        }
    }
    strcpy(s_ini_path, "keybinds.ini");
}

static void write_player_section(FILE *f, const char *section, const PlayerBinds *pb) {
    fprintf(f, "[%s]\n", section);
    for (const ButtonDef *bd = s_buttons; bd->name; bd++) {
        SDL_Scancode sc = *(const SDL_Scancode *)((const char *)pb + bd->offset);
        fprintf(f, "%-7s = %s\n", bd->name, scancode_to_name(sc));
    }
    fprintf(f, "\n");
}

static void write_defaults(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "# SNES Controller Keybinds\n"
        "# Edit values to customize. Restart the game to apply.\n"
        "# Use SDL key names. Common: A B C ... Z, 0-9, F1-F12, Up Down Left Right,\n"
        "# Return, Tab, Space, Left Shift, Right Shift, Left Ctrl, Right Ctrl,\n"
        "# Backspace, Escape, Backslash. Use \"None\" to leave a button unbound.\n"
        "#\n"
        "# Player 2 is unbound by default — fill in keys to enable a second\n"
        "# keyboard player.\n"
        "\n");
    write_player_section(f, "player1", &s_binds.p1);
    write_player_section(f, "player2", &s_binds.p2);
    fclose(f);
    printf("[Keybinds] Generated %s\n", path);
}

static void load_ini(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    PlayerBinds *current = NULL;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) *end = '\0';
            const char *section = line + 1;
            current = NULL;
            if (!strcmp(section, "player1"))      current = &s_binds.p1;
            else if (!strcmp(section, "player2")) current = &s_binds.p2;
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        trim(key); trim(val);
        for (char *c = key; *c; c++) *c = (char)tolower((unsigned char)*c);

        if (!current) continue;

        for (const ButtonDef *bd = s_buttons; bd->name; bd++) {
            if (!strcmp(key, bd->name)) {
                SDL_Scancode sc = name_to_scancode(val);
                *(SDL_Scancode *)((char *)current + bd->offset) = sc;
                break;
            }
        }
    }
    fclose(f);
    printf("[Keybinds] Loaded %s\n", path);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void keybinds_init(const char *exe_path) {
    derive_ini_path(exe_path);
    FILE *test = fopen(s_ini_path, "r");
    if (test) {
        fclose(test);
        load_ini(s_ini_path);
    } else {
        write_defaults(s_ini_path);
    }
}

const KeyBinds *keybinds_get(void) {
    return &s_binds;
}

/* SNES joypad bitmask layout — matches $4218/$4219 (low/high byte) packing.
 * Returns 0 if either `keys` is NULL or player is out of range. */
uint16_t keybinds_read_player(const uint8_t *keys, int player) {
    if (!keys) return 0;
    const PlayerBinds *pb = (player == 2) ? &s_binds.p2 : &s_binds.p1;
    uint16_t b = 0;
    if (pb->r      != SDL_SCANCODE_UNKNOWN && keys[pb->r])      b |= 0x0001;
    if (pb->l      != SDL_SCANCODE_UNKNOWN && keys[pb->l])      b |= 0x0002;
    if (pb->x      != SDL_SCANCODE_UNKNOWN && keys[pb->x])      b |= 0x0004;
    if (pb->a      != SDL_SCANCODE_UNKNOWN && keys[pb->a])      b |= 0x0008;
    if (pb->right  != SDL_SCANCODE_UNKNOWN && keys[pb->right])  b |= 0x0010;
    if (pb->left   != SDL_SCANCODE_UNKNOWN && keys[pb->left])   b |= 0x0020;
    if (pb->down   != SDL_SCANCODE_UNKNOWN && keys[pb->down])   b |= 0x0040;
    if (pb->up     != SDL_SCANCODE_UNKNOWN && keys[pb->up])     b |= 0x0080;
    if (pb->start  != SDL_SCANCODE_UNKNOWN && keys[pb->start])  b |= 0x0100;
    if (pb->select != SDL_SCANCODE_UNKNOWN && keys[pb->select]) b |= 0x0200;
    if (pb->y      != SDL_SCANCODE_UNKNOWN && keys[pb->y])      b |= 0x0400;
    if (pb->b      != SDL_SCANCODE_UNKNOWN && keys[pb->b])      b |= 0x0800;
    return b;
}

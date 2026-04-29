#pragma once
#include <stdint.h>
#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SNES controller keybinds — INI-driven, generated next to the exe on
 * first run.
 *
 * Layout matches the SNES gamepad: A, B, X, Y, L, R, Start, Select, and
 * the d-pad. Two players. Each button maps to one SDL_Scancode.
 *
 * The button bitmask returned by keybinds_read_player() uses the same
 * 12-bit layout as SMW's $4218/$4219 joypad register pair, low byte first:
 *
 *   bit  0: R              (high byte, $4219 bit 4)
 *   bit  1: L              (high byte, $4219 bit 5)
 *   bit  2: X              (high byte, $4219 bit 6)
 *   bit  3: A              (high byte, $4219 bit 7)
 *   bit  4: Right          (low byte,  $4218 bit 0)
 *   bit  5: Left           (low byte,  $4218 bit 1)
 *   bit  6: Down           (low byte,  $4218 bit 2)
 *   bit  7: Up             (low byte,  $4218 bit 3)
 *   bit  8: Start          (low byte,  $4218 bit 4)
 *   bit  9: Select         (low byte,  $4218 bit 5)
 *   bit 10: Y              (low byte,  $4218 bit 6)
 *   bit 11: B              (low byte,  $4218 bit 7)
 *
 * Per-game runners that prefer a different layout can ignore the
 * bitmask and read individual buttons from PlayerBinds directly.
 */

typedef struct {
    SDL_Scancode a, b, x, y;
    SDL_Scancode l, r;
    SDL_Scancode start, select;
    SDL_Scancode up, down, left, right;
} PlayerBinds;

typedef struct {
    PlayerBinds p1;
    PlayerBinds p2;
} KeyBinds;

/* Initialize keybinds from <exe_dir>/keybinds.ini. Generates a default
 * file if one doesn't exist. exe_path may be NULL or argv[0]. */
void keybinds_init(const char *exe_path);

/* Get current keybind configuration (read-only view). */
const KeyBinds *keybinds_get(void);

/* Build a 12-bit SNES button bitmask for the given player (1 or 2)
 * from the SDL keyboard state. See header docstring for bit layout. */
uint16_t keybinds_read_player(const uint8_t *keys, int player);

#ifdef __cplusplus
}
#endif

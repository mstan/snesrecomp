// launcher_input.h — live gamepad enumeration for the launcher (shared core).
//
// Polls SDL for currently-connected gamepads every frame, so a controller
// turned on AFTER the launcher opens shows up without a relaunch (and one
// turned off disappears). Game-agnostic; both backends use it to populate the
// per-player input-source dropdown with real device names.

#ifndef LAUNCHER_NG_INPUT_H
#define LAUNCHER_NG_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LNG_MAX_PADS 8

typedef struct {
    uint32_t id;        // SDL_JoystickID (stable while connected)
    char     name[64];  // e.g. "PS5 Controller", "Xbox Series Controller"
} LauncherPad;

// Fill `out` (capacity `max`) with the gamepads SDL currently sees. Returns the
// count. Cheap enough to call once per frame; reflects hot-plug because SDL's
// device list is refreshed by the event pump the backend already runs.
int launcher_input_poll(LauncherPad* out, int max);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_INPUT_H

// launcher_input.c — SDL3 gamepad enumeration.

#include "launcher_input.h"

#include <SDL3/SDL.h>
#include <stdio.h>

int launcher_input_poll(LauncherPad* out, int max) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);   // gamepads (mapped) only
    int n = 0;
    if (ids) {
        for (int i = 0; i < count && n < max; ++i) {
            out[n].id = (uint32_t)ids[i];
            const char* nm = SDL_GetGamepadNameForID(ids[i]);
            snprintf(out[n].name, sizeof(out[n].name), "%s", nm ? nm : "Gamepad");
            ++n;
        }
        SDL_free(ids);
    }
    return n;
}

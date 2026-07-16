// launcher_input.c — live gamepad enumeration (SDL2 + SDL3).

#include "launcher_input.h"
#include "launcher_platform.h"   // pulls the right SDL header for this build

#include <stdio.h>

int launcher_input_poll(LauncherPad* out, int max) {
    int n = 0;

#if defined(LNG_SDL3)
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids) {
        for (int i = 0; i < count && n < max; ++i) {
            out[n].id = (uint32_t)ids[i];
            const char* nm = SDL_GetGamepadNameForID(ids[i]);
            snprintf(out[n].name, sizeof(out[n].name), "%s", nm ? nm : "Gamepad");
            ++n;
        }
        SDL_free(ids);
    }
#else
    // SDL2 uses a device-index model. SDL_NumJoysticks() re-scans on each call
    // (fed by SDL's event pump), so a pad powered on after launch shows up here
    // without a relaunch — same hot-plug behaviour as the SDL3 path.
    const int count = SDL_NumJoysticks();
    for (int i = 0; i < count && n < max; ++i) {
        if (!SDL_IsGameController(i)) continue;   // mapped gamepads only
        // Report the stable instance id, not the volatile device index, so a
        // selection survives other pads connecting/disconnecting.
        SDL_Joystick* js = SDL_JoystickOpen(i);
        SDL_JoystickID inst = js ? SDL_JoystickInstanceID(js) : -1;
        if (js) SDL_JoystickClose(js);
        if (inst < 0) continue;

        out[n].id = (uint32_t)inst;
        const char* nm = SDL_GameControllerNameForIndex(i);
        snprintf(out[n].name, sizeof(out[n].name), "%s", nm ? nm : "Gamepad");
        ++n;
    }
#endif

    return n;
}

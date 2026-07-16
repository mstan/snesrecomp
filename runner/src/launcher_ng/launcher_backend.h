// launcher_backend.h — the single seam between the shared core and a renderer.
//
// Exactly one backend (Dear ImGui *or* Clay) is compiled into a given
// executable; both provide this same entry point. It owns the event/render loop
// (event handling is unavoidably backend-specific) but drives the shared
// LauncherModel via its mutators, so behavior is identical across backends.

#ifndef LAUNCHER_NG_BACKEND_H
#define LAUNCHER_NG_BACKEND_H

#include "launcher_model.h"
#include "launcher_platform.h"
#include "launcher_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

// Runs the launcher to completion. Returns the chosen LngAction
// (LNG_ACTION_LAUNCH or LNG_ACTION_QUIT). Reads/writes m in place.
LngAction launcher_backend_run(LauncherPlatform* p,
                               LauncherModel* m,
                               const LauncherTheme* th);

// Human name of the compiled-in backend, for window titles / logs.
const char* launcher_backend_name(void);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_BACKEND_H

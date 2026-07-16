// launcher_panels.h — the launcher's MODULE system.
//
// Every section of the launcher is a self-contained panel module: a stable id,
// a title, the view it belongs to, a draw function, and an `available`
// predicate that decides whether the game gets that module at all.
//
// The launcher does NOT hardcode a layout. It walks the registry, keeps the
// modules this game supports, and lays them out. That is what makes the UI
// per-game composable:
//
//   Mega Man X : GAME + CONTROLLERS(1P)            (no SAVES, no MSU-1, no widescreen)
//   Zelda      : GAME + CONTROLLERS(2P) + SAVES + MSU-1 + WIDESCREEN
//
// Adding a game-specific module = add one LauncherPanel to the registry with an
// `available` predicate. No layout code changes, nothing to un-hardcode later.
//
// Panels are game-AGNOSTIC: they read capability from the model (which is built
// from the game's C-ABI struct), never from a game name.

#ifndef LAUNCHER_NG_PANELS_H
#define LAUNCHER_NG_PANELS_H

#include "launcher_model.h"
#include "launcher_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LauncherPanel LauncherPanel;

// Draw the panel's body. The framework has already opened the card + eyebrow.
typedef void (*LauncherPanelDrawFn)(LauncherModel* m, const LauncherTheme* th);

// Return false to omit the module entirely for this game.
typedef bool (*LauncherPanelAvailFn)(const LauncherModel* m);

typedef enum {
    LNG_SLOT_MAIN = 0,   // primary column
    LNG_SLOT_SIDE,       // secondary column
    LNG_SLOT_WIDE,       // spans the full width
} LngPanelSlot;

struct LauncherPanel {
    const char*          id;        // stable id, e.g. "game"
    const char*          title;     // eyebrow text, e.g. "GAME"
    LngView              view;      // which view it appears in
    LngPanelSlot         slot;      // where it wants to sit
    float                weight;    // relative height hint within its column
    LauncherPanelAvailFn available;  // NULL => always available
    LauncherPanelDrawFn  draw;
};

// The registry for the current build. Returns a NULL-terminated array.
const LauncherPanel* launcher_panels_all(void);

// True when this panel should be shown for this game.
bool launcher_panel_available(const LauncherPanel* p, const LauncherModel* m);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_PANELS_H

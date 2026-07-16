// launcher_debug.h — scriptable UI automation + screenshot capture.
//
// Lets an agent (or CI) drive the launcher headlessly-ish and SEE the result,
// instead of guessing at layout from source. Shared by both backends so a
// single script exercises either one identically.
//
// Driven by the LNG_SCRIPT env var: a ';'-separated command list, executed one
// command per frame.
//
//   view:dashboard|settings|controller   switch view (no clicking required)
//   player:0|1                           which player the controller view edits
//   size:WxH                             resize the window (tests live reflow)
//   click:X,Y                            synthetic click at logical coords
//   key:escape                           synthetic key press
//   wait:N                               idle N frames (let layout settle)
//   shot:PATH                            write a PNG of the current frame
//   quit                                 exit the launcher
//
// Example:
//   LNG_SCRIPT="wait:5;shot:a.png;view:settings;wait:5;shot:b.png;quit"
//
// Screenshots capture the real framebuffer (glReadPixels), so what lands in the
// PNG is exactly what a user would see, at whatever DPI scale is in effect.

#ifndef LAUNCHER_NG_DEBUG_H
#define LAUNCHER_NG_DEBUG_H

#include "launcher_model.h"
#include "launcher_platform.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// True when LNG_SCRIPT is set (script mode active).
bool launcher_debug_active(void);

// Parse LNG_SCRIPT. Safe to call when unset (script mode simply stays off).
void launcher_debug_init(void);

// Advance the script by one frame. Call once per frame AFTER rendering and
// BEFORE presenting, so `shot:` captures the completed frame. Sets
// m->action = LNG_ACTION_QUIT when the script ends or hits `quit`.
void launcher_debug_step(LauncherPlatform* p, LauncherModel* m);

// Write the current GL framebuffer to a PNG (RGB, row-flipped).
bool launcher_capture_png(const char* path, int w, int h);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_DEBUG_H

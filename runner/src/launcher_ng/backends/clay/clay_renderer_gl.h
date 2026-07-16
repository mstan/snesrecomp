// clay_renderer_gl.h — a compact Clay -> OpenGL 3.3 renderer with stb_truetype
// text. This file is the concrete cost of choosing Clay: unlike ImGui, Clay
// ships no renderer or text stack, so we provide both. Kept backend-local (not
// in the shared core) because it is Clay-specific.
//
// Clay works in PHYSICAL PIXELS here: the backend feeds it pixel dimensions and
// pixel font sizes (logical * display_scale), so text bakes crisply at the
// target DPI and the renderer draws 1:1 into the framebuffer.

#ifndef LAUNCHER_NG_CLAY_RENDERER_GL_H
#define LAUNCHER_NG_CLAY_RENDERER_GL_H

#include "third_party/clay.h"

#ifdef __cplusplus
extern "C" {
#endif

// Font ids used in CLAY_TEXT_CONFIG(.fontId = ...).
enum { LNG_FONT_BODY = 0, LNG_FONT_TITLE = 1, LNG_FONT_SMALL = 2, LNG_FONT_COUNT = 3 };

// Load GL functions, compile the shader, create buffers. Call once with a
// current GL context.
bool clay_gl_init(void);

// (Re)bake the three glyph atlases at the given pixel sizes. Call on start and
// whenever the display scale changes.
void clay_gl_rebake_fonts(const char* font_path,
                          float body_px, float title_px, float small_px);

// Clay text measurement callback (Clay_SetMeasureTextFunction).
Clay_Dimensions clay_gl_measure_text(Clay_StringSlice text,
                                     Clay_TextElementConfig* config, void* userData);

// Draw a full Clay frame into an fb_w x fb_h framebuffer.
void clay_gl_render(Clay_RenderCommandArray commands, int fb_w, int fb_h);

void clay_gl_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_CLAY_RENDERER_GL_H

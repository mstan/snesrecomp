// launcher_gl.h — tiny image->GL-texture loader shared by both backends.
//
// Uses stb_image (public domain) to decode TGA/PNG and uploads an RGBA texture
// via GL 1.1 calls (available from opengl32 / libGL). Backend-agnostic: the
// ImGui backend feeds the id to ImGui::Image; the Clay renderer will bind it
// directly.

#ifndef LAUNCHER_NG_GL_H
#define LAUNCHER_NG_GL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned int id;   // GL texture name (0 = not loaded)
    int          w, h;
} LauncherTexture;

// Load an image file into a freshly created GL texture (linear filtered,
// clamped). Returns a texture with id==0 on failure. Requires a current GL
// context.
LauncherTexture launcher_texture_load(const char* path);

// As above, but for images that carry no alpha channel (e.g. 24-bit TGA art
// with a flat backdrop baked in): samples the top-left pixel and makes every
// pixel within `tolerance` (0-255 per channel) fully transparent. Keeps the
// art asset-agnostic instead of hand-editing each game's images.
LauncherTexture launcher_texture_load_colorkey(const char* path, int tolerance);

void launcher_texture_free(LauncherTexture* t);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_GL_H

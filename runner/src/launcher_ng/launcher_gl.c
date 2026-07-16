// launcher_gl.c — image->GL-texture loader (stb_image + GL 1.1).

#include "launcher_gl.h"

#include <SDL3/SDL_opengl.h>   // GL types + glGenTextures/glTexImage2D (GL 1.1)

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F   // GL 1.2 token; runtime driver supports it
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_TGA
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "third_party/stb_image.h"

// Shared upload path. `colorkey_tol` < 0 disables color-keying.
static LauncherTexture load_impl(const char* path, int colorkey_tol) {
    LauncherTexture t = { 0, 0, 0 };
    int w = 0, h = 0, comp = 0;
    unsigned char* pixels = stbi_load(path, &w, &h, &comp, 4); // force RGBA
    if (!pixels) return t;

    if (colorkey_tol >= 0 && w > 0 && h > 0) {
        // Key out the backdrop using the top-left pixel as the reference.
        const unsigned char kr = pixels[0], kg = pixels[1], kb = pixels[2];
        for (int i = 0; i < w * h; ++i) {
            unsigned char* p = pixels + i * 4;
            int dr = (int)p[0] - kr, dg = (int)p[1] - kg, db = (int)p[2] - kb;
            if (dr < 0) dr = -dr;  if (dg < 0) dg = -dg;  if (db < 0) db = -db;
            if (dr <= colorkey_tol && dg <= colorkey_tol && db <= colorkey_tol)
                p[3] = 0;   // transparent
        }
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);

    t.id = (unsigned int)tex;
    t.w = w; t.h = h;
    return t;
}

LauncherTexture launcher_texture_load(const char* path) {
    return load_impl(path, -1);
}

LauncherTexture launcher_texture_load_colorkey(const char* path, int tolerance) {
    return load_impl(path, tolerance < 0 ? 0 : tolerance);
}

void launcher_texture_free(LauncherTexture* t) {
    if (t && t->id) {
        GLuint id = (GLuint)t->id;
        glDeleteTextures(1, &id);
        t->id = 0;
    }
}

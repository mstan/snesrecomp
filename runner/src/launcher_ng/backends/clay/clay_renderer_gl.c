// clay_renderer_gl.c — Clay -> OpenGL 3.3 renderer + stb_truetype text.

#include "clay_renderer_gl.h"

#include "third_party/gl_core_3_1.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- GL program + buffers ---------------------------------------------------
static GLuint g_prog, g_vao, g_vbo, g_white;
static GLint  g_uView, g_uTex;

// Every draw is: vertex (pos.xy, uv.xy, rgba) * texture. Solid fills bind a 1x1
// white texture; text binds a glyph atlas (white RGB + coverage alpha); images
// bind their RGBA texture. One shader, one path.
static const char* kVS =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;layout(location=1) in vec2 aUV;layout(location=2) in vec4 aCol;\n"
    "uniform vec2 uView;out vec2 vUV;out vec4 vCol;\n"
    "void main(){vUV=aUV;vCol=aCol;gl_Position=vec4((aPos.x/uView.x)*2.0-1.0,1.0-(aPos.y/uView.y)*2.0,0,1);}\n";
static const char* kFS =
    "#version 330 core\n"
    "in vec2 vUV;in vec4 vCol;uniform sampler2D uTex;out vec4 o;\n"
    "void main(){o=vCol*texture(uTex,vUV);}\n";

// ---- glyph atlases ----------------------------------------------------------
typedef struct {
    GLuint          tex;
    stbtt_bakedchar chars[96];      // ASCII 32..126
    int             bw, bh;
    float           px_height;
    float           ascent_px;
} GlyphAtlas;

static GlyphAtlas    g_atlas[LNG_FONT_COUNT];
static unsigned char* g_ttf = NULL;   // font file bytes, kept for re-bake

// ---- vertex scratch buffer --------------------------------------------------
static float g_vb[1 << 16];
static int   g_vn;   // float count

static void reset_vb(void) { g_vn = 0; }
static void vtx(float x, float y, float u, float v, const float c[4]) {
    if (g_vn + 8 > (int)(sizeof(g_vb) / sizeof(float))) return;
    g_vb[g_vn++] = x; g_vb[g_vn++] = y; g_vb[g_vn++] = u; g_vb[g_vn++] = v;
    g_vb[g_vn++] = c[0]; g_vb[g_vn++] = c[1]; g_vb[g_vn++] = c[2]; g_vb[g_vn++] = c[3];
}
static void quad_uv(float x, float y, float w, float h,
                    float u0, float v0, float u1, float v1, const float c[4]) {
    vtx(x,   y,   u0, v0, c); vtx(x+w, y,   u1, v0, c); vtx(x+w, y+h, u1, v1, c);
    vtx(x,   y,   u0, v0, c); vtx(x+w, y+h, u1, v1, c); vtx(x,   y+h, u0, v1, c);
}
static void quad(float x, float y, float w, float h, const float c[4]) {
    quad_uv(x, y, w, h, 0, 0, 0, 0, c);   // white tex -> solid
}
static void corner_fan(float cx, float cy, float r, float a0, const float c[4]) {
    const int N = 6;
    for (int i = 0; i < N; ++i) {
        float t0 = a0 + (float)M_PI * 0.5f * i / N;
        float t1 = a0 + (float)M_PI * 0.5f * (i + 1) / N;
        vtx(cx, cy, 0, 0, c);
        vtx(cx + cosf(t0) * r, cy - sinf(t0) * r, 0, 0, c);
        vtx(cx + cosf(t1) * r, cy - sinf(t1) * r, 0, 0, c);
    }
}
static void rrect(float x, float y, float w, float h, float r, const float c[4]) {
    if (r < 0.5f) { quad(x, y, w, h, c); return; }
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    quad(x + r, y,     w - 2*r, h,       c);   // center column
    quad(x,     y + r, r,       h - 2*r, c);   // left
    quad(x + w - r, y + r, r,   h - 2*r, c);   // right
    corner_fan(x + r,     y + r,     r, (float)M_PI * 0.5f, c);   // TL
    corner_fan(x + w - r, y + r,     r, 0.0f,               c);   // TR
    corner_fan(x + w - r, y + h - r, r, (float)M_PI * 1.5f, c);   // BR
    corner_fan(x + r,     y + h - r, r, (float)M_PI,        c);   // BL
}
static void ncol(Clay_Color k, float out[4]) {
    out[0] = k.r/255.f; out[1] = k.g/255.f; out[2] = k.b/255.f; out[3] = k.a/255.f;
}

static void flush(GLuint tex) {
    if (g_vn == 0) return;
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, g_vn * sizeof(float), g_vb, GL_STREAM_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glDrawArrays(GL_TRIANGLES, 0, g_vn / 8);
    reset_vb();
}

// ---- shader helpers ---------------------------------------------------------
static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, NULL, log);
               fprintf(stderr, "[clay-gl] shader: %s\n", log); }
    return s;
}

bool clay_gl_init(void) {
    if (ogl_LoadFunctions() == ogl_LOAD_FAILED) {
        fprintf(stderr, "[clay-gl] ogl_LoadFunctions failed\n");
        return false;
    }
    GLuint vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, kFS);
    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs); glAttachShader(g_prog, fs);
    glBindAttribLocation(g_prog, 0, "aPos");
    glBindAttribLocation(g_prog, 1, "aUV");
    glBindAttribLocation(g_prog, 2, "aCol");
    glLinkProgram(g_prog);
    glDeleteShader(vs); glDeleteShader(fs);
    g_uView = glGetUniformLocation(g_prog, "uView");
    g_uTex  = glGetUniformLocation(g_prog, "uTex");

    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(4*sizeof(float)));
    glBindVertexArray(0);

    unsigned char white[4] = { 255, 255, 255, 255 };
    glGenTextures(1, &g_white);
    glBindTexture(GL_TEXTURE_2D, g_white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    return true;
}

static unsigned char* read_file(const char* path, long* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)malloc(n);
    if (buf) *out_len = (long)fread(buf, 1, n, f);
    fclose(f);
    return buf;
}

static void bake_one(GlyphAtlas* a, float px) {
    const int W = 512, H = 512;
    unsigned char* mono = (unsigned char*)calloc((size_t)W * H, 1);
    stbtt_BakeFontBitmap(g_ttf, 0, px, mono, W, H, 32, 96, a->chars);
    a->bw = W; a->bh = H; a->px_height = px;

    stbtt_fontinfo info;
    if (stbtt_InitFont(&info, g_ttf, 0)) {
        int asc, desc, gap; stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
        a->ascent_px = asc * stbtt_ScaleForPixelHeight(&info, px);
    } else {
        a->ascent_px = px * 0.8f;
    }

    unsigned char* rgba = (unsigned char*)malloc((size_t)W * H * 4);
    for (int i = 0; i < W * H; ++i) {
        rgba[i*4+0] = 255; rgba[i*4+1] = 255; rgba[i*4+2] = 255; rgba[i*4+3] = mono[i];
    }
    if (!a->tex) glGenTextures(1, &a->tex);
    glBindTexture(GL_TEXTURE_2D, a->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba); free(mono);
}

void clay_gl_rebake_fonts(const char* font_path,
                          float body_px, float title_px, float small_px) {
    if (!g_ttf) { long n = 0; g_ttf = read_file(font_path, &n); }
    if (!g_ttf) { fprintf(stderr, "[clay-gl] font load failed: %s\n", font_path); return; }
    bake_one(&g_atlas[LNG_FONT_BODY],  body_px);
    bake_one(&g_atlas[LNG_FONT_TITLE], title_px);
    bake_one(&g_atlas[LNG_FONT_SMALL], small_px);
}

Clay_Dimensions clay_gl_measure_text(Clay_StringSlice text,
                                     Clay_TextElementConfig* config, void* userData) {
    (void)userData;
    int fid = config ? config->fontId : LNG_FONT_BODY;
    if (fid < 0 || fid >= LNG_FONT_COUNT) fid = LNG_FONT_BODY;
    GlyphAtlas* a = &g_atlas[fid];
    float x = 0, y = 0;
    for (int i = 0; i < text.length; ++i) {
        unsigned char ch = (unsigned char)text.chars[i];
        if (ch < 32 || ch > 126) ch = ' ';
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(a->chars, a->bw, a->bh, ch - 32, &x, &y, &q, 1);
    }
    Clay_Dimensions d; d.width = x; d.height = a->px_height; return d;
}

static void draw_text(const Clay_RenderCommand* cmd) {
    const Clay_TextRenderData* t = &cmd->renderData.text;
    int fid = t->fontId; if (fid < 0 || fid >= LNG_FONT_COUNT) fid = LNG_FONT_BODY;
    GlyphAtlas* a = &g_atlas[fid];
    float col[4]; ncol(t->textColor, col);
    float x = cmd->boundingBox.x;
    float y = cmd->boundingBox.y + a->ascent_px;   // baseline
    reset_vb();
    for (int i = 0; i < t->stringContents.length; ++i) {
        unsigned char ch = (unsigned char)t->stringContents.chars[i];
        if (ch < 32 || ch > 126) ch = ' ';
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(a->chars, a->bw, a->bh, ch - 32, &x, &y, &q, 1);
        quad_uv(q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0, q.s0, q.t0, q.s1, q.t1, col);
    }
    flush(a->tex);
}

void clay_gl_render(Clay_RenderCommandArray commands, int fb_w, int fb_h) {
    glViewport(0, 0, fb_w, fb_h);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(g_prog);
    glUniform2f(g_uView, (float)fb_w, (float)fb_h);
    glUniform1i(g_uTex, 0);
    glBindVertexArray(g_vao);

    for (int i = 0; i < commands.length; ++i) {
        Clay_RenderCommand* cmd = Clay_RenderCommandArray_Get(&commands, i);
        Clay_BoundingBox b = cmd->boundingBox;
        switch (cmd->commandType) {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                float c[4]; ncol(cmd->renderData.rectangle.backgroundColor, c);
                reset_vb();
                rrect(b.x, b.y, b.width, b.height,
                      cmd->renderData.rectangle.cornerRadius.topLeft, c);
                flush(g_white);
            } break;
            case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                float c[4]; ncol(cmd->renderData.border.color, c);
                Clay_BorderWidth w = cmd->renderData.border.width;
                reset_vb();
                if (w.top)    quad(b.x, b.y, b.width, w.top, c);
                if (w.bottom) quad(b.x, b.y + b.height - w.bottom, b.width, w.bottom, c);
                if (w.left)   quad(b.x, b.y, w.left, b.height, c);
                if (w.right)  quad(b.x + b.width - w.right, b.y, w.right, b.height, c);
                flush(g_white);
            } break;
            case CLAY_RENDER_COMMAND_TYPE_TEXT:
                draw_text(cmd);
                break;
            case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
                float c[4]; ncol(cmd->renderData.image.backgroundColor, c);
                if (c[0]==0 && c[1]==0 && c[2]==0 && c[3]==0) { c[0]=c[1]=c[2]=c[3]=1.f; }
                GLuint tex = (GLuint)(size_t)cmd->renderData.image.imageData;
                reset_vb();
                quad_uv(b.x, b.y, b.width, b.height, 0, 0, 1, 1, c);
                flush(tex ? tex : g_white);
            } break;
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
                glEnable(GL_SCISSOR_TEST);
                glScissor((int)b.x, fb_h - (int)(b.y + b.height),
                          (int)b.width, (int)b.height);
                break;
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
                glDisable(GL_SCISSOR_TEST);
                break;
            default: break;
        }
    }
    glBindVertexArray(0);
    glDisable(GL_SCISSOR_TEST);
}

void clay_gl_shutdown(void) {
    for (int i = 0; i < LNG_FONT_COUNT; ++i)
        if (g_atlas[i].tex) glDeleteTextures(1, &g_atlas[i].tex);
    if (g_white) glDeleteTextures(1, &g_white);
    if (g_vbo)   glDeleteBuffers(1, &g_vbo);
    if (g_vao)   glDeleteVertexArrays(1, &g_vao);
    if (g_prog)  glDeleteProgram(g_prog);
    free(g_ttf); g_ttf = NULL;
}

/*
 * opengl.c — the shared desktop OpenGL presenter.
 *
 * util.h has declared `OpenGLRenderer_Create(struct RendererFuncs *)` for a
 * long time, and mmx23_host_main.inc calls it, but the framework shipped no
 * implementation: every port carried its own copy. Seven of them did, at
 * ~225 lines each, differing only in the name of a per-game viewport helper.
 *
 * That is the shape a shared fix cannot reach, and the cost was already on
 * the board: one port's copy computed
 *
 *     int viewport_y = (viewport_height - viewport_height) >> 1;
 *
 * which is zero for every window size, so its picture never centred
 * vertically. Six other copies had the line right. Nothing could tell them
 * apart because nothing compared them.
 *
 * Presentation policy comes from the framework's own config (`display_aspect`,
 * `ignore_aspect_ratio`, `linear_filtering`, `shader`) via
 * SnesDisplayAspect_ComputeViewport, which already handles all three pixel
 * aspects, integer scaling and seam suppression — the per-game helpers this
 * replaces mostly hardcoded 7:6.
 *
 * Vendored originally from snesrev/smw's src/opengl.c (MIT, (c) 2023 snesrev,
 * (c) 2021 elzo_d); see THIRD_PARTY_ATTRIBUTION.md.
 */

#include "gl_core_3_1.h"
#include "sdl_compat.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>

#include "types.h"
#include "util.h"
#include "glsl_shader.h"
#include "config.h"
#include "display_aspect.h"

#define CODE(...) #__VA_ARGS__

static SDL_Window *g_window;
static uint8 *g_screen_buffer;
static size_t g_screen_buffer_size;
static int g_draw_width, g_draw_height;
static unsigned int g_program, g_VAO;
static GlTextureWithSize g_texture;
static GlslShader *g_glsl_shader;

/* -1 = decide from config at init. A host that paces presentation itself
 * (an FPS cap, a simulation/presentation split) sets 0 so the swap does not
 * also block; one that wants the driver to pace it sets 1. */
static int g_vsync_override = -1;

void snesrecomp_opengl_set_vsync(int enable) {
  g_vsync_override = enable ? 1 : 0;
}

static void GL_APIENTRY MessageCallback(GLenum source,
                GLenum type,
                GLuint id,
                GLenum severity,
                GLsizei length,
                const GLchar *message,
                const void *userParam) {
  (void)source; (void)id; (void)length; (void)userParam;
  if (type == GL_DEBUG_TYPE_OTHER)
    return;

  fprintf(stderr, "GL CALLBACK: %s type = 0x%x, severity = 0x%x, message = %s\n",
          (type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : ""),
          type, severity, message);
  if (type == GL_DEBUG_TYPE_ERROR)
    Die("OpenGL error!\n");
}

static bool OpenGLRenderer_Init(SDL_Window *window) {
  g_window = window;
  SDL_GLContext context = SDL_GL_CreateContext(window);
  (void)context;

  SDL_GL_SetSwapInterval(g_vsync_override >= 0
                             ? g_vsync_override
                             : (g_config.disable_frame_delay ? 0 : 1));
  ogl_LoadFunctions();

  if (!ogl_IsVersionGEQ(3, 3))
    Die("You need OpenGL 3.3");

  if (kDebugFlag) {
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(MessageCallback, 0);
  }

  glGenTextures(1, &g_texture.gl_texture);

  static const float kVertices[] = {
    // positions          // texture coords
    -1.0f,  1.0f, 0.0f,   0.0f, 0.0f, // top left
    -1.0f, -1.0f, 0.0f,   0.0f, 1.0f, // bottom left
     1.0f,  1.0f, 0.0f,   1.0f, 0.0f, // top right
     1.0f, -1.0f, 0.0f,   1.0f, 1.0f, // bottom right
  };

  // create a vertex buffer object
  unsigned int vbo;
  glGenBuffers(1, &vbo);

  // vertex array object
  glGenVertexArrays(1, &g_VAO);
  // 1. bind Vertex Array Object
  glBindVertexArray(g_VAO);
  // 2. copy our vertices array in a buffer for OpenGL to use
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);
  // position attribute
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
  glEnableVertexAttribArray(0);
  // texture coord attribute
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
  glEnableVertexAttribArray(1);

  // vertex shader
  const GLchar *vs_code = "#version 330 core\n" CODE(
  layout(location = 0) in vec3 aPos;
  layout(location = 1) in vec2 aTexCoord;
  out vec2 TexCoord;
  void main(void) {
    gl_Position = vec4(aPos, 1.0);
    TexCoord = vec2(aTexCoord.x, aTexCoord.y);
  }
);

  unsigned int vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &vs_code, NULL);
  glCompileShader(vs);

  int success;
  char infolog[512];
  glGetShaderiv(vs, GL_COMPILE_STATUS, &success);
  if (!success) {
    glGetShaderInfoLog(vs, 512, NULL, infolog);
    printf("%s\n", infolog);
  }

  // fragment shader
  const GLchar *fs_code = "#version 330 core\n" CODE(
  out vec4 FragColor;
  in vec2 TexCoord;
  // texture samplers
  uniform sampler2D texture1;
  void main(void) {
    FragColor = texture(texture1, TexCoord);
  }
);

  unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, &fs_code, NULL);
  glCompileShader(fs);

  glGetShaderiv(fs, GL_COMPILE_STATUS, &success);
  if (!success) {
    glGetShaderInfoLog(fs, 512, NULL, infolog);
    printf("%s\n", infolog);
  }

  // create program
  int program = g_program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glGetProgramiv(program, GL_LINK_STATUS, &success);

  if (!success) {
    glGetProgramInfoLog(program, 512, NULL, infolog);
    printf("%s\n", infolog);
  }

  if (g_config.shader)
    g_glsl_shader = GlslShader_CreateFromFile(g_config.shader);

  return true;
}

static void OpenGLRenderer_Destroy(void) {
}

static void OpenGLRenderer_GetOutputSize(int *width, int *height) {
  snesrecomp_sdl_get_drawable_size(g_window, width, height);
}

static void OpenGLRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  size_t size = (size_t)width * (size_t)height;

  if (size > g_screen_buffer_size) {
    g_screen_buffer_size = size;
    free(g_screen_buffer);
    g_screen_buffer = (uint8 *)malloc(size * 4);
  }

  g_draw_width = width;
  g_draw_height = height;
  *pixels = g_screen_buffer;
  *pitch = width * 4;
}

static void OpenGLRenderer_EndDraw(void) {
  int drawable_width, drawable_height;

  snesrecomp_sdl_get_drawable_size(g_window, &drawable_width, &drawable_height);

  /* The framework's own aspect maths, rather than a per-game copy: it knows
   * all three pixel aspects the config offers, and centres on both axes. */
  SnesDisplayViewport viewport;
  SnesDisplayAspect_ComputeViewport(
      g_draw_width, g_draw_height, drawable_width, drawable_height,
      SnesDisplayAspect_Clamp(g_config.display_aspect),
      g_config.ignore_aspect_ratio, false, &viewport);

  glBindTexture(GL_TEXTURE_2D, g_texture.gl_texture);
  if (g_draw_width == g_texture.width && g_draw_height == g_texture.height) {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_draw_width, g_draw_height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, g_screen_buffer);
  } else {
    g_texture.width = g_draw_width;
    g_texture.height = g_draw_height;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_draw_width, g_draw_height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, g_screen_buffer);
  }

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (g_glsl_shader == NULL) {
    glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
    glUseProgram(g_program);
    int filter = g_config.linear_filtering ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glBindVertexArray(g_VAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  } else {
    GlslShader_Render(g_glsl_shader, &g_texture, viewport.x, viewport.y,
                      viewport.width, viewport.height);
  }

  SDL_GL_SwapWindow(g_window);
}

static const struct RendererFuncs kOpenGLRendererFuncs = {
  &OpenGLRenderer_Init,
  &OpenGLRenderer_Destroy,
  &OpenGLRenderer_GetOutputSize,
  &OpenGLRenderer_BeginDraw,
  &OpenGLRenderer_EndDraw,
};

void OpenGLRenderer_Create(struct RendererFuncs *funcs) {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  *funcs = kOpenGLRendererFuncs;
}

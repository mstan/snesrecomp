/*
 * Shared SDL2/SDL3 include boundary for desktop SNESRecomp hosts.
 *
 * SDL3 builds use its transitional old-name aliases for source-compatible
 * constants and types while host code moves to the SDL3 data model. API calls
 * whose signatures changed are handled explicitly at their call sites.
 */
#ifndef SNESRECOMP_DESKTOP_SDL_COMPAT_H
#define SNESRECOMP_DESKTOP_SDL_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

#if SNESRECOMP_SDL3
#define SDL_ENABLE_OLD_NAMES
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#else
#include <SDL.h>
#endif

#define SNESRECOMP_SDL_MIX_MAXVOLUME 128

#if SNESRECOMP_SDL3
#define SNESRECOMP_SDL_EVENT_DEVICE(event) ((event).gdevice.which)
#define SNESRECOMP_SDL_EVENT_AXIS_DEVICE(event) ((event).gaxis.which)
#define SNESRECOMP_SDL_EVENT_AXIS(event) ((event).gaxis.axis)
#define SNESRECOMP_SDL_EVENT_AXIS_VALUE(event) ((event).gaxis.value)
#define SNESRECOMP_SDL_EVENT_BUTTON_DEVICE(event) ((event).gbutton.which)
#define SNESRECOMP_SDL_EVENT_BUTTON(event) ((event).gbutton.button)
#define SNESRECOMP_SDL_EVENT_KEY(event) ((event).key.key)
#define SNESRECOMP_SDL_EVENT_MOD(event) ((event).key.mod)
#else
#define SNESRECOMP_SDL_EVENT_DEVICE(event) ((event).cdevice.which)
#define SNESRECOMP_SDL_EVENT_AXIS_DEVICE(event) ((event).caxis.which)
#define SNESRECOMP_SDL_EVENT_AXIS(event) ((event).caxis.axis)
#define SNESRECOMP_SDL_EVENT_AXIS_VALUE(event) ((event).caxis.value)
#define SNESRECOMP_SDL_EVENT_BUTTON_DEVICE(event) ((event).cbutton.which)
#define SNESRECOMP_SDL_EVENT_BUTTON(event) ((event).cbutton.button)
#define SNESRECOMP_SDL_EVENT_KEY(event) ((event).key.keysym.sym)
#define SNESRECOMP_SDL_EVENT_MOD(event) ((event).key.keysym.mod)
#endif

static inline bool snesrecomp_sdl_init(Uint32 flags) {
#if SNESRECOMP_SDL3
  return SDL_Init(flags);
#else
  return SDL_Init(flags) == 0;
#endif
}

static inline SDL_Window *snesrecomp_sdl_create_window(
    const char *title, int width, int height, SDL_WindowFlags flags) {
#if SNESRECOMP_SDL3
  return SDL_CreateWindow(title, width, height, flags);
#else
  return SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED,
                          SDL_WINDOWPOS_UNDEFINED, width, height, flags);
#endif
}

static inline bool snesrecomp_sdl_get_display_usable_bounds(
    SDL_Window *window, SDL_Rect *bounds) {
#if SNESRECOMP_SDL3
  SDL_DisplayID display = SDL_GetDisplayForWindow(window);
  return display != 0 && SDL_GetDisplayUsableBounds(display, bounds);
#else
  int display = SDL_GetWindowDisplayIndex(window);
  if (display < 0) display = 0;
  return SDL_GetDisplayUsableBounds(display, bounds) == 0;
#endif
}

static inline bool snesrecomp_sdl_get_window_borders_size(
    SDL_Window *window, int *top, int *left, int *bottom, int *right) {
#if SNESRECOMP_SDL3
  return SDL_GetWindowBordersSize(window, top, left, bottom, right);
#else
  return SDL_GetWindowBordersSize(window, top, left, bottom, right) == 0;
#endif
}

static inline void snesrecomp_sdl_get_global_mouse_state(int *x, int *y) {
#if SNESRECOMP_SDL3
  float fx = 0.0f, fy = 0.0f;
  SDL_GetGlobalMouseState(&fx, &fy);
  *x = (int)fx;
  *y = (int)fy;
#else
  SDL_GetGlobalMouseState(x, y);
#endif
}

static inline bool snesrecomp_sdl_lock_mutex(SDL_mutex *mutex) {
#if SNESRECOMP_SDL3
  SDL_LockMutex(mutex);
  return true;
#else
  return SDL_LockMutex(mutex) == 0;
#endif
}

static inline SDL_Renderer *snesrecomp_sdl_create_renderer(
    SDL_Window *window, bool software, bool vsync) {
#if SNESRECOMP_SDL3
  SDL_Renderer *renderer =
      SDL_CreateRenderer(window, software ? "software" : NULL);
  if (renderer && !software) SDL_SetRenderVSync(renderer, vsync ? 1 : 0);
  return renderer;
#else
  return SDL_CreateRenderer(
      window, -1, software ? SDL_RENDERER_SOFTWARE
                           : SDL_RENDERER_ACCELERATED |
                                 (vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
#endif
}

static inline const char *snesrecomp_sdl_renderer_name(
    SDL_Renderer *renderer) {
#if SNESRECOMP_SDL3
  return SDL_GetRendererName(renderer);
#else
  SDL_RendererInfo info;
  return SDL_GetRendererInfo(renderer, &info) == 0 ? info.name : NULL;
#endif
}

static inline int snesrecomp_sdl_get_render_vsync(SDL_Renderer *renderer) {
#if SNESRECOMP_SDL3
  int vsync = 0;
  return SDL_GetRenderVSync(renderer, &vsync) ? vsync : -2;
#else
  SDL_RendererInfo info;
  if (SDL_GetRendererInfo(renderer, &info) != 0) return -2;
  return (info.flags & SDL_RENDERER_PRESENTVSYNC) ? 1 : 0;
#endif
}

static inline bool snesrecomp_sdl_set_render_logical_size(
    SDL_Renderer *renderer, int width, int height) {
#if SNESRECOMP_SDL3
  return SDL_SetRenderLogicalPresentation(
      renderer, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
#else
  return SDL_RenderSetLogicalSize(renderer, width, height) == 0;
#endif
}

static inline bool snesrecomp_sdl_get_render_output_size(
    SDL_Renderer *renderer, int *width, int *height) {
#if SNESRECOMP_SDL3
  /* SDL_GetRenderOutputSize, NOT SDL_GetCurrentRenderOutputSize: this must be
   * the TRUE physical output size, ignoring logical size and presentation, to
   * match SDL2's SDL_GetRendererOutputSize.
   *
   * The "Current" variant applies logical-presentation adjustments, so a game
   * that calls SDL_SetRenderLogicalPresentation gets its own logical size back
   * instead of the real window. That silently breaks any drawable-driven
   * decision: A Link to the Past's adaptive widescreen derives its column count
   * from this aspect, so it read 256x224, computed zero extra columns, and stayed
   * pinned to 4:3 with no error and no log line. Games using an explicit viewport
   * (Mega Man X, SMW) never set a logical presentation and so never saw it. */
  return SDL_GetRenderOutputSize(renderer, width, height);
#else
  return SDL_GetRendererOutputSize(renderer, width, height) == 0;
#endif
}

static inline bool snesrecomp_sdl_lock_texture(
    SDL_Texture *texture, const SDL_Rect *rect, void **pixels, int *pitch) {
#if SNESRECOMP_SDL3
  return SDL_LockTexture(texture, rect, pixels, pitch);
#else
  return SDL_LockTexture(texture, rect, pixels, pitch) == 0;
#endif
}

static inline bool snesrecomp_sdl_get_texture_size(
    SDL_Texture *texture, int *width, int *height) {
#if SNESRECOMP_SDL3
  float fwidth = 0.0f, fheight = 0.0f;
  bool ok = SDL_GetTextureSize(texture, &fwidth, &fheight);
  *width = (int)fwidth;
  *height = (int)fheight;
  return ok;
#else
  return SDL_QueryTexture(
             texture, NULL, NULL, width, height) == 0;
#endif
}

/* SDL3 grew a modifier-state out-parameter on this; SDL2 takes the keycode
 * alone. Callers that only want "which physical key is this" pass neither. */
static inline SDL_Scancode snesrecomp_sdl_scancode_from_key(SDL_Keycode key) {
#if SNESRECOMP_SDL3
  return SDL_GetScancodeFromKey(key, NULL);
#else
  return SDL_GetScancodeFromKey(key);
#endif
}

static inline void snesrecomp_sdl_render_texture(
    SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *source,
    const SDL_Rect *destination) {
#if SNESRECOMP_SDL3
  SDL_FRect fsource, fdestination;
  const SDL_FRect *source_ptr = NULL;
  const SDL_FRect *destination_ptr = NULL;
  if (source) {
    fsource.x = (float)source->x;
    fsource.y = (float)source->y;
    fsource.w = (float)source->w;
    fsource.h = (float)source->h;
    source_ptr = &fsource;
  }
  if (destination) {
    fdestination.x = (float)destination->x;
    fdestination.y = (float)destination->y;
    fdestination.w = (float)destination->w;
    fdestination.h = (float)destination->h;
    destination_ptr = &fdestination;
  }
  SDL_RenderTexture(
      renderer, texture, source_ptr, destination_ptr);
#else
  SDL_RenderCopy(renderer, texture, source, destination);
#endif
}

static inline void snesrecomp_sdl_set_texture_linear(
    SDL_Texture *texture, bool linear) {
#if SNESRECOMP_SDL3
  SDL_SetTextureScaleMode(
      texture, linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
#else
  (void)texture;
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, linear ? "best" : "nearest");
#endif
}

static inline void snesrecomp_sdl_set_texture_opaque(SDL_Texture *texture) {
  /* SNES framebuffers store RGB in ARGB8888 but leave the alpha byte zero.
   * They are complete opaque frames, not alpha-composited overlays. SDL3
   * otherwise blends those pixels as transparent and presents only the
   * renderer's black clear color. */
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
}

static inline void snesrecomp_sdl_get_drawable_size(
    SDL_Window *window, int *width, int *height) {
#if SNESRECOMP_SDL3
  SDL_GetWindowSizeInPixels(window, width, height);
#else
  SDL_GL_GetDrawableSize(window, width, height);
#endif
}

static inline void snesrecomp_sdl_set_fullscreen(
    SDL_Window *window, bool enabled) {
#if SNESRECOMP_SDL3
  SDL_SetWindowFullscreen(window, enabled);
#else
  SDL_SetWindowFullscreen(
      window, enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
#endif
}

static inline void snesrecomp_sdl_show_cursor(bool visible) {
#if SNESRECOMP_SDL3
  if (visible) SDL_ShowCursor();
  else SDL_HideCursor();
#else
  SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
#endif
}

static inline const uint8_t *snesrecomp_sdl_get_keyboard_state(void) {
#if SNESRECOMP_SDL3
  return (const uint8_t *)SDL_GetKeyboardState(NULL);
#else
  return SDL_GetKeyboardState(NULL);
#endif
}

/* SDL3 dropped the desktop/exclusive fullscreen flag split; both map to
 * SDL_WINDOW_FULLSCREEN and SDL_SetWindowFullscreen(window, bool). */
#if SNESRECOMP_SDL3
#define SNESRECOMP_SDL_WINDOW_FULLSCREEN_DESKTOP SDL_WINDOW_FULLSCREEN
#else
#define SNESRECOMP_SDL_WINDOW_FULLSCREEN_DESKTOP SDL_WINDOW_FULLSCREEN_DESKTOP
#endif

static inline void snesrecomp_sdl_pause_audio_device(SDL_AudioDeviceID id,
                                                    bool pause) {
#if SNESRECOMP_SDL3
  if (pause)
    SDL_PauseAudioDevice(id);
  else
    SDL_ResumeAudioDevice(id);
#else
  SDL_PauseAudioDevice(id, pause ? 1 : 0);
#endif
}

/* volume_0_128 matches the historical SDL2 mixer scale (max 128). */
static inline void snesrecomp_sdl_mix_audio(Uint8 *dst, const Uint8 *src,
                                           int len, int volume_0_128) {
#if SNESRECOMP_SDL3
  SDL_MixAudio(dst, src, SDL_AUDIO_S16, (Uint32)len,
               (float)volume_0_128 / (float)SNESRECOMP_SDL_MIX_MAXVOLUME);
#else
  SDL_MixAudioFormat(dst, src, AUDIO_S16, len, volume_0_128);
#endif
}

static inline void snesrecomp_sdl_fill_rect(SDL_Renderer *renderer) {
#if SNESRECOMP_SDL3
  SDL_RenderFillRect(renderer, NULL);
#else
  SDL_RenderFillRect(renderer, NULL);
#endif
}

#endif

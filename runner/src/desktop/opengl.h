#pragma once

/* The shared desktop OpenGL presenter (desktop/opengl.c). util.h declares
 * OpenGLRenderer_Create itself; this header carries only the presentation
 * policy a host may need to override. */

#ifdef __cplusplus
extern "C" {
#endif

/* Force the swap interval instead of deriving it from config.
 *
 * Call BEFORE the renderer is created. A host that paces presentation on its
 * own clock (an FPS cap, or a split simulation/presentation rate) passes 0,
 * so SDL_GL_SwapWindow does not block on the display as well and fight that
 * pacing. Left unset, the presenter vsyncs unless config's DisableFrameDelay
 * is on. */
void snesrecomp_opengl_set_vsync(int enable);

#ifdef __cplusplus
}
#endif

/*
 * fiber_compat.c — POSIX implementation of the Win32 Fiber API used by
 * cooperative game task schedulers. See fiber_compat.h. Compiled to nothing
 * on Windows, where the native Fiber API is used directly.
 *
 * Two backends:
 *  - ucontext (macOS/glibc): true stack switching, zero threads.
 *  - pthread (Android): bionic ships the ucontext types but NOT
 *    getcontext/makecontext/swapcontext, so each fiber is a parked thread
 *    and SwitchToFiber is a condvar handoff. A frame-model host switches
 *    fibers a couple of times per frame, so the handoff cost does not
 *    matter; a scheduler that switches per guest task should measure it.
 *
 * The Android backend came from SuperMetroidRecomp, which needed it to run
 * its single-fiber WaitForNMI frame model on arm64. It was a private copy of
 * this file with one extra #ifdef -- the shape that, repeated per port,
 * cannot inherit a fix (recomp-ai-rules/PRINCIPLES.md).
 */
#ifndef _WIN32
#ifdef __ANDROID__

#include "fiber_compat.h"
#include <pthread.h>
#include <stdlib.h>

typedef struct Fiber {
    pthread_t       thread;
    pthread_cond_t  cv;
    int             scheduled;  /* protected by g_fiber_lock */
    FiberProc       entry;
    void           *param;
    int             started;    /* thread created (CreateFiber fibers only) */
    size_t          stack_size;
} Fiber;

static pthread_mutex_t g_fiber_lock = PTHREAD_MUTEX_INITIALIZER;

/* The fiber currently executing on this thread group. Only ever mutated with
 * g_fiber_lock held, inside SwitchToFiber. */
static Fiber *g_current_fiber = NULL;

static void *fiber_thread_main(void *arg) {
    Fiber *self = (Fiber *)arg;
    pthread_mutex_lock(&g_fiber_lock);
    while (!self->scheduled)
        pthread_cond_wait(&self->cv, &g_fiber_lock);
    pthread_mutex_unlock(&g_fiber_lock);
    self->entry(self->param);
    /* Fiber entries never fall off the end (they loop SwitchToFiber back to
     * the scheduler). If one ever does, there is no safe continuation. */
    abort();
    return NULL;
}

void *ConvertThreadToFiber(void *param) {
    (void)param;
    Fiber *f = (Fiber *)calloc(1, sizeof(Fiber));
    if (!f) return NULL;
    pthread_cond_init(&f->cv, NULL);
    f->scheduled = 1;   /* it is running right now */
    f->started = 1;
    g_current_fiber = f;
    return f;
}

void *CreateFiber(size_t stack_size, FiberProc entry, void *param) {
    Fiber *f = (Fiber *)calloc(1, sizeof(Fiber));
    if (!f) return NULL;
    pthread_cond_init(&f->cv, NULL);
    f->entry = entry;
    f->param = param;
    f->stack_size = stack_size < 256 * 1024 ? 256 * 1024 : stack_size;
    /* Thread creation is deferred to the first switch so a fiber that is
     * created but never entered costs nothing but this struct. */
    return f;
}

void SwitchToFiber(void *fiber) {
    Fiber *target = (Fiber *)fiber;
    if (!target || target == g_current_fiber) return;
    Fiber *self = g_current_fiber;

    pthread_mutex_lock(&g_fiber_lock);
    if (!target->started) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, target->stack_size);
        if (pthread_create(&target->thread, &attr, fiber_thread_main,
                           target) != 0) {
            pthread_attr_destroy(&attr);
            pthread_mutex_unlock(&g_fiber_lock);
            abort();    /* mirrors the desktop backends: switch cannot fail */
        }
        pthread_attr_destroy(&attr);
        target->started = 1;
    }
    g_current_fiber = target;
    self->scheduled = 0;
    target->scheduled = 1;
    pthread_cond_signal(&target->cv);
    while (!self->scheduled)
        pthread_cond_wait(&self->cv, &g_fiber_lock);
    pthread_mutex_unlock(&g_fiber_lock);
}

void DeleteFiber(void *fiber) {
    Fiber *f = (Fiber *)fiber;
    if (!f) return;
    /* Deleting a parked fiber's thread safely would need a cancellation
     * handshake; SM only deletes fibers at process teardown, so leak the
     * parked thread and free the bookkeeping. */
    pthread_cond_destroy(&f->cv);
    free(f);
}

unsigned long GetLastError(void) { return 0; }

#else /* !__ANDROID__: ucontext backend */

#define _XOPEN_SOURCE 600   /* expose ucontext on macOS/glibc — must precede includes */

#include "fiber_compat.h"
#include <ucontext.h>
#include <stdlib.h>

typedef struct Fiber {
    ucontext_t ctx;
    FiberProc  entry;
    void      *param;
    void      *stack;     /* NULL for a ConvertThreadToFiber handle */
} Fiber;

/* The fiber currently executing on this thread. Updated by SwitchToFiber
 * before the context swap, so a freshly-started fiber's trampoline sees
 * itself here. */
static Fiber *g_current_fiber = NULL;

static void fiber_trampoline(void) {
    Fiber *self = g_current_fiber;
    if (self && self->entry)
        self->entry(self->param);
    /* MMX fiber entries never fall off the end (they loop SwitchToFiber back
     * to the scheduler). If one ever does, there is no safe continuation. */
    abort();
}

void *ConvertThreadToFiber(void *param) {
    (void)param;
    Fiber *f = (Fiber *)calloc(1, sizeof(Fiber));
    if (!f) return NULL;
    /* No stack of its own — it rides the thread's existing stack. The ctx is
     * populated by the first swapcontext that switches away from it. */
    g_current_fiber = f;
    return f;
}

void *CreateFiber(size_t stack_size, FiberProc entry, void *param) {
    Fiber *f = (Fiber *)calloc(1, sizeof(Fiber));
    if (!f) return NULL;
    f->stack = malloc(stack_size);
    if (!f->stack) { free(f); return NULL; }
    f->entry = entry;
    f->param = param;
    if (getcontext(&f->ctx) != 0) { free(f->stack); free(f); return NULL; }
    f->ctx.uc_stack.ss_sp   = f->stack;
    f->ctx.uc_stack.ss_size = stack_size;
    f->ctx.uc_link          = NULL;
    makecontext(&f->ctx, fiber_trampoline, 0);
    return f;
}

void SwitchToFiber(void *fiber) {
    Fiber *target = (Fiber *)fiber;
    if (!target || target == g_current_fiber) return;
    Fiber *prev = g_current_fiber;
    g_current_fiber = target;
    swapcontext(&prev->ctx, &target->ctx);
}

void DeleteFiber(void *fiber) {
    Fiber *f = (Fiber *)fiber;
    if (!f) return;
    free(f->stack);   /* free(NULL) is fine for thread-origin fibers */
    free(f);
}

unsigned long GetLastError(void) { return 0; }

#endif /* __ANDROID__ */
#endif /* !_WIN32 */

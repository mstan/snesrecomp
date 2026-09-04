/* See guarded_patch.h. */
#include "guarded_patch.h"

#include <string.h>

static GuardedPatch *s_registry[GUARDED_PATCH_MAX_REGISTERED];
static uint32_t s_registered;
static int s_suspend_depth;

GuardedPatchStatus guarded_patch_init(GuardedPatch *patch, const char *name,
                                      uint8_t *target, size_t size,
                                      const uint8_t *expected,
                                      const uint8_t *replacement) {
  if (!patch) return kGuardedPatch_Invalid;
  memset(patch, 0, sizeof(*patch));
  if (!target || size == 0 || size > GUARDED_PATCH_MAX_BYTES || !expected ||
      !replacement) {
    patch->last_status = kGuardedPatch_Invalid;
    return kGuardedPatch_Invalid;
  }
  patch->name = name ? name : "guarded_patch";
  patch->target = target;
  patch->size = size;
  memcpy(patch->expected, expected, size);
  memcpy(patch->replacement, replacement, size);
  patch->last_status = kGuardedPatch_Ok;
  return kGuardedPatch_Ok;
}

static int valid(const GuardedPatch *patch) {
  return patch && patch->target && patch->size > 0 &&
         patch->size <= GUARDED_PATCH_MAX_BYTES;
}

GuardedPatchStatus guarded_patch_apply(GuardedPatch *patch) {
  if (!valid(patch)) return kGuardedPatch_Invalid;
  if (patch->applied && !patch->suspended) {
    patch->last_status = kGuardedPatch_AlreadyApplied;
    return kGuardedPatch_AlreadyApplied;
  }
  if (memcmp(patch->target, patch->expected, patch->size) != 0) {
    patch->mismatch_count++;
    patch->last_status = kGuardedPatch_ExpectedMismatch;
    return kGuardedPatch_ExpectedMismatch;
  }
  memcpy(patch->target, patch->replacement, patch->size);
  patch->applied = 1;
  patch->suspended = 0;
  patch->apply_count++;
  patch->last_status = kGuardedPatch_Ok;
  return kGuardedPatch_Ok;
}

GuardedPatchStatus guarded_patch_revert(GuardedPatch *patch) {
  if (!valid(patch)) return kGuardedPatch_Invalid;
  if (!patch->applied || patch->suspended) {
    patch->last_status = kGuardedPatch_NotApplied;
    return kGuardedPatch_NotApplied;
  }
  GuardedPatchStatus status = kGuardedPatch_Ok;
  if (memcmp(patch->target, patch->replacement, patch->size) != 0) {
    patch->mismatch_count++;
    status = kGuardedPatch_PatchMismatch;
  }
  memcpy(patch->target, patch->expected, patch->size);
  patch->applied = 0;
  patch->suspended = 0;
  patch->last_status = status;
  return status;
}

GuardedPatchStatus guarded_patch_verify(GuardedPatch *patch) {
  if (!valid(patch)) return kGuardedPatch_Invalid;
  const uint8_t *want =
      (patch->applied && !patch->suspended) ? patch->replacement
                                            : patch->expected;
  GuardedPatchStatus status = kGuardedPatch_Ok;
  if (memcmp(patch->target, want, patch->size) != 0) {
    patch->mismatch_count++;
    status = (patch->applied && !patch->suspended)
                 ? kGuardedPatch_PatchMismatch
                 : kGuardedPatch_ExpectedMismatch;
  }
  patch->last_status = status;
  return status;
}

GuardedPatchStatus guarded_patch_register(GuardedPatch *patch) {
  if (!valid(patch)) return kGuardedPatch_Invalid;
  for (uint32_t i = 0; i < s_registered; i++)
    if (s_registry[i] == patch) return kGuardedPatch_Ok;
  if (s_registered >= GUARDED_PATCH_MAX_REGISTERED)
    return kGuardedPatch_RegistryFull;
  s_registry[s_registered++] = patch;
  return kGuardedPatch_Ok;
}

void guarded_patch_unregister(GuardedPatch *patch) {
  for (uint32_t i = 0; i < s_registered; i++) {
    if (s_registry[i] == patch) {
      s_registry[i] = s_registry[s_registered - 1];
      s_registry[--s_registered] = NULL;
      return;
    }
  }
}

void guarded_patch_suspend_all(void) {
  if (s_suspend_depth++ != 0) return;
  for (uint32_t i = 0; i < s_registered; i++) {
    GuardedPatch *p = s_registry[i];
    if (!p->applied || p->suspended) continue;
    if (memcmp(p->target, p->replacement, p->size) != 0) {
      p->mismatch_count++;
      p->last_status = kGuardedPatch_PatchMismatch;
    }
    memcpy(p->target, p->expected, p->size);
    p->suspended = 1;
  }
}

void guarded_patch_resume_all(void) {
  if (s_suspend_depth == 0) return;
  if (--s_suspend_depth != 0) return;
  for (uint32_t i = 0; i < s_registered; i++) {
    GuardedPatch *p = s_registry[i];
    if (!p->applied || !p->suspended) continue;
    if (memcmp(p->target, p->expected, p->size) != 0) {
      /* The guest changed the original bytes while suspended; leave them
       * and drop the patch so we never clobber live guest state. */
      p->mismatch_count++;
      p->last_status = kGuardedPatch_ExpectedMismatch;
      p->applied = 0;
      p->suspended = 0;
      continue;
    }
    memcpy(p->target, p->replacement, p->size);
    p->suspended = 0;
    p->last_status = kGuardedPatch_Ok;
  }
}

void guarded_patch_revert_all(void) {
  for (uint32_t i = 0; i < s_registered; i++) {
    GuardedPatch *p = s_registry[i];
    if (p->applied && !p->suspended) guarded_patch_revert(p);
    p->applied = 0;
    p->suspended = 0;
  }
  s_suspend_depth = 0;
}

uint32_t guarded_patch_registered_count(void) { return s_registered; }

const GuardedPatch *guarded_patch_registered_at(uint32_t index) {
  return index < s_registered ? s_registry[index] : NULL;
}

const char *guarded_patch_status_name(GuardedPatchStatus status) {
  switch (status) {
  case kGuardedPatch_Ok: return "ok";
  case kGuardedPatch_AlreadyApplied: return "already-applied";
  case kGuardedPatch_NotApplied: return "not-applied";
  case kGuardedPatch_ExpectedMismatch: return "expected-mismatch";
  case kGuardedPatch_PatchMismatch: return "patch-mismatch";
  case kGuardedPatch_Invalid: return "invalid";
  case kGuardedPatch_RegistryFull: return "registry-full";
  }
  return "unknown";
}

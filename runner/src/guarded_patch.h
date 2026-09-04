/* Byte-match-guarded patches for trusted, statically linked mods.
 *
 * A guarded patch replaces a small run of bytes in a host-owned buffer (the
 * loaded cartridge ROM image, WRAM, coprocessor RAM, CGRAM ...) only when the
 * bytes currently there equal the expected original. It remembers what it
 * replaced, can verify the patched bytes are still intact, and can revert.
 * A registry lets a host suspend every applied patch around a window where
 * the guest or a save-state must observe the original bytes, then resume.
 *
 * This is the shape used ad hoc by several titles (save / tint / restore of
 * CGRAM and Super FX RAM in Star Fox); the module makes the discipline
 * reusable and loud: every mismatch is reported through the status enum and
 * counted, never silently skipped.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GUARDED_PATCH_MAX_BYTES 256u
#define GUARDED_PATCH_MAX_REGISTERED 64u

typedef enum GuardedPatchStatus {
  kGuardedPatch_Ok = 0,
  kGuardedPatch_AlreadyApplied,
  kGuardedPatch_NotApplied,
  kGuardedPatch_ExpectedMismatch, /* target bytes are not the original */
  kGuardedPatch_PatchMismatch,    /* someone overwrote the patched bytes */
  kGuardedPatch_Invalid,          /* bad arguments */
  kGuardedPatch_RegistryFull,
} GuardedPatchStatus;

typedef struct GuardedPatch {
  const char *name;         /* for diagnostics, static storage */
  uint8_t *target;          /* first byte to patch */
  size_t size;              /* 1..GUARDED_PATCH_MAX_BYTES */
  uint8_t expected[GUARDED_PATCH_MAX_BYTES];
  uint8_t replacement[GUARDED_PATCH_MAX_BYTES];
  int applied;
  int suspended;            /* reverted temporarily by suspend_all */
  uint32_t apply_count;
  uint32_t mismatch_count;  /* every Expected/Patch mismatch observed */
  GuardedPatchStatus last_status;
} GuardedPatch;

/* Initialise a patch description. Copies expected/replacement (size bytes). */
GuardedPatchStatus guarded_patch_init(GuardedPatch *patch, const char *name,
                                      uint8_t *target, size_t size,
                                      const uint8_t *expected,
                                      const uint8_t *replacement);

/* Apply when the target still holds the expected bytes. */
GuardedPatchStatus guarded_patch_apply(GuardedPatch *patch);

/* Restore the expected bytes. Reports PatchMismatch (and still restores)
 * if the patched bytes were changed underneath us. */
GuardedPatchStatus guarded_patch_revert(GuardedPatch *patch);

/* Check the target holds what the patch state implies. */
GuardedPatchStatus guarded_patch_verify(GuardedPatch *patch);

/* Registry: patches added here participate in suspend/resume/revert_all.
 * The registry stores pointers; the caller keeps the patch objects alive
 * (static storage in a mod is the expected use). */
GuardedPatchStatus guarded_patch_register(GuardedPatch *patch);
void guarded_patch_unregister(GuardedPatch *patch);

/* Temporarily revert every applied patch (e.g. before a save-state write or
 * a cosim WRAM compare) and re-apply afterwards. Nested calls are counted. */
void guarded_patch_suspend_all(void);
void guarded_patch_resume_all(void);

/* Revert every applied registered patch permanently (feature disabled,
 * runtime reset). */
void guarded_patch_revert_all(void);

/* Diagnostics. */
uint32_t guarded_patch_registered_count(void);
const GuardedPatch *guarded_patch_registered_at(uint32_t index);
const char *guarded_patch_status_name(GuardedPatchStatus status);

#ifdef __cplusplus
}
#endif

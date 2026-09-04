/* Unit tests for runner/src/guarded_patch.{c,h}. */
#include "guarded_patch.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      g_failures++;                                                            \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
    }                                                                          \
  } while (0)

int main(void) {
  uint8_t rom[64];
  for (int i = 0; i < 64; i++) rom[i] = (uint8_t)i;
  const uint8_t expected[4] = {10, 11, 12, 13};
  const uint8_t replacement[4] = {0xa1, 0xac, 0, 0};
  GuardedPatch p;
  CHECK(guarded_patch_init(&p, "ship", rom + 10, 4, expected, replacement) ==
        kGuardedPatch_Ok);
  CHECK(guarded_patch_init(&p, "bad", NULL, 4, expected, replacement) ==
        kGuardedPatch_Invalid);
  CHECK(guarded_patch_init(&p, "bad", rom, 0, expected, replacement) ==
        kGuardedPatch_Invalid);
  CHECK(guarded_patch_init(&p, "ship", rom + 10, 4, expected, replacement) ==
        kGuardedPatch_Ok);

  /* Apply / verify / revert round trip. */
  CHECK(guarded_patch_verify(&p) == kGuardedPatch_Ok);
  CHECK(guarded_patch_apply(&p) == kGuardedPatch_Ok);
  CHECK(memcmp(rom + 10, replacement, 4) == 0 && rom[9] == 9 && rom[14] == 14);
  CHECK(guarded_patch_apply(&p) == kGuardedPatch_AlreadyApplied);
  CHECK(guarded_patch_verify(&p) == kGuardedPatch_Ok);
  CHECK(guarded_patch_revert(&p) == kGuardedPatch_Ok);
  CHECK(memcmp(rom + 10, expected, 4) == 0);
  CHECK(guarded_patch_revert(&p) == kGuardedPatch_NotApplied);
  CHECK(p.apply_count == 1 && p.mismatch_count == 0);

  /* Guard: original bytes changed -> refuse. */
  rom[11] = 0xee;
  CHECK(guarded_patch_apply(&p) == kGuardedPatch_ExpectedMismatch);
  CHECK(!p.applied && p.mismatch_count == 1);
  rom[11] = 11;

  /* Tamper detection while applied. */
  CHECK(guarded_patch_apply(&p) == kGuardedPatch_Ok);
  rom[12] = 0x77;
  CHECK(guarded_patch_verify(&p) == kGuardedPatch_PatchMismatch);
  CHECK(guarded_patch_revert(&p) == kGuardedPatch_PatchMismatch);
  CHECK(memcmp(rom + 10, expected, 4) == 0); /* still restored */
  CHECK(p.mismatch_count == 3);

  /* Registry + suspend/resume. */
  GuardedPatch q;
  const uint8_t exp2[2] = {40, 41}, rep2[2] = {0, 0};
  CHECK(guarded_patch_init(&q, "flag", rom + 40, 2, exp2, rep2) ==
        kGuardedPatch_Ok);
  CHECK(guarded_patch_register(&p) == kGuardedPatch_Ok);
  CHECK(guarded_patch_register(&p) == kGuardedPatch_Ok); /* idempotent */
  CHECK(guarded_patch_register(&q) == kGuardedPatch_Ok);
  CHECK(guarded_patch_registered_count() == 2);
  CHECK(guarded_patch_apply(&p) == kGuardedPatch_Ok);
  CHECK(guarded_patch_apply(&q) == kGuardedPatch_Ok);
  guarded_patch_suspend_all();
  CHECK(memcmp(rom + 10, expected, 4) == 0 && rom[40] == 40 && rom[41] == 41);
  CHECK(p.applied && p.suspended);
  guarded_patch_suspend_all(); /* nested */
  guarded_patch_resume_all();
  CHECK(p.suspended);          /* still suspended at depth 1 */
  guarded_patch_resume_all();
  CHECK(!p.suspended && memcmp(rom + 10, replacement, 4) == 0 && rom[40] == 0);

  /* Guest rewrote the original while suspended: the patch drops out. */
  guarded_patch_suspend_all();
  rom[40] = 99;
  guarded_patch_resume_all();
  CHECK(!q.applied && rom[40] == 99 && q.last_status == kGuardedPatch_ExpectedMismatch);
  CHECK(p.applied && memcmp(rom + 10, replacement, 4) == 0);

  /* Revert all. */
  guarded_patch_revert_all();
  CHECK(!p.applied && memcmp(rom + 10, expected, 4) == 0);
  guarded_patch_unregister(&q);
  CHECK(guarded_patch_registered_count() == 1 &&
        guarded_patch_registered_at(0) == &p);
  guarded_patch_unregister(&p);
  CHECK(guarded_patch_registered_count() == 0);
  CHECK(strcmp(guarded_patch_status_name(kGuardedPatch_PatchMismatch),
               "patch-mismatch") == 0);

  if (g_failures) {
    printf("guarded_patch tests: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("guarded_patch tests passed\n");
  return 0;
}

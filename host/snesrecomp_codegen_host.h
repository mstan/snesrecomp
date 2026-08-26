/* Portable recomp-ui setup host: ROM → generate → cmake rebuild → relaunch.
 *
 * Games compile snesrecomp_codegen_host.c, fill SnesrecompCodegenHostConfig,
 * and call snesrecomp_codegen_host_apply() when building RecompLauncherCGameInfo.
 *
 * Requires recomp_launcher.h on the include path (recomp-ui submodule).
 */
#ifndef SNESRECOMP_CODEGEN_HOST_H
#define SNESRECOMP_CODEGEN_HOST_H

#include "recomp_launcher.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnesrecompCodegenHostConfig {
    /* Display name for notes / Windows helper console title. */
    const char* display_name;

    /* Env vars (optional). Defaults: SNESRECOMP_PROJECT_ROOT / SNESRECOMP_BUILD_DIR /
     * SNESRECOMP_FORCE_SETUP when NULL. */
    const char* project_root_env;
    const char* build_dir_env;
    const char* force_setup_env;

    /* Paths relative to project root. NULL → snesrecomp defaults below. */
    const char* snesrecomp_cli_relpath; /* default: snesrecomp/snesrecomp_cli.py */
    const char* seed_cfg_relpath;       /* default: recomp/bank00.cfg (root probe) */
    const char* cfg_dir;                /* default: recomp */
    const char* out_dir;                /* default: src/gen */
    const char* funcs_h;                /* default: recomp/funcs.h (NULL = skip sync) */
    const char* gen_marker_relpath;     /* default: src/gen/dispatch_v2.c */
    const char* build_dir_name;         /* default: build */

    /* CMake / binary identity (required for auto-rebuild). */
    const char* cmake_target;           /* e.g. MetalWarriorsSNESRecomp */
    const char* exe_basename;           /* no .exe; e.g. MetalWarriorsSNESRecomp */

    /* Optional ROM digests for generate --expected-* (NULL = skip). */
    const char* expected_crc32;
    const char* expected_sha256;

    /* When non-zero, pass --cfg-roots to generate (typical for game ports). */
    int cfg_roots;

    /* Optional UI copy overrides (NULL → generic defaults). */
    const char* prepare_note;
    const char* prepare_note_windows;
    const char* prepare_note_no_cmake;
} SnesrecompCodegenHostConfig;

/* Wire prepare/rebuild/relaunch callbacks onto gi when tools are discoverable. */
void snesrecomp_codegen_host_apply(RecompLauncherCGameInfo* gi,
                                   const SnesrecompCodegenHostConfig* cfg);

/* True when gen_marker is missing under the discovered project root. */
int snesrecomp_codegen_host_sources_missing(
    const SnesrecompCodegenHostConfig* cfg);

/* After run_window returns RECOMP_LAUNCHER_RESULT_RELAUNCH. Does not return
 * on success (exec / spawn helper + exit). */
void snesrecomp_codegen_host_relaunch_or_exit(const char* rom_path);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_CODEGEN_HOST_H */

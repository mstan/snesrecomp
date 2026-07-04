#pragma once

/* host_report.h — always-on host-side crash/diagnostic capture.
 *
 * Game-agnostic companion to each game's post_mortem.c. Ships in ALL
 * build configurations, including Production: this is the module that
 * makes a user's "it crashed on my machine" report actionable without
 * a repro on ours. It is a recorder + serializer only — no validators,
 * no guest-state knowledge (that stays in post_mortem.c).
 *
 * What it owns:
 *   - An always-on breadcrumb ring (boot stages, device parameters,
 *     heartbeats, fatal errors) — queried at dump time, never armed.
 *   - Host environment serialization: build/version stamp, OS version,
 *     CPU/memory, SDL drivers, loaded modules with base addresses (so
 *     host stack PCs are resolvable under ASLR against archived PDBs).
 *   - Minidump writing on the SEH path (Windows; dbghelp resolved
 *     dynamically so no build-system link changes are needed).
 *   - Crash-copy preservation: last_run_report.json is overwritten by
 *     every run's atexit dump, so crash-path dumps are copied to a
 *     timestamped crash_report_*.json that later runs never touch.
 *
 * Threading: the ring is mutex-guarded; breadcrumbs may be logged from
 * any thread (e.g. the audio callback's first-invocation marker).
 */

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Record the game name + build version for the report header. Call
 * first thing in main(), before any other host_report use. `version`
 * is the release stamp baked in at build time (see
 * SNESRECOMP_BUILD_VERSION); pass "dev" for local builds. */
void host_report_init(const char *game_name, const char *build_version);

/* Append a printf-style breadcrumb to the always-on ring (and echo to
 * stderr for console builds). Cheap; called at boot stages, device
 * setup, savestate events, and as a low-rate heartbeat. */
void host_report_breadcrumb(const char *fmt, ...);

/* Record a fatal error message (Die() path). Also breadcrumbs it. The
 * post-mortem preserves a timestamped crash copy when a fatal message
 * is present, even on the atexit path. */
void host_report_fatal(const char *msg);
int  host_report_has_fatal(void);

/* Serialize the host section into an in-progress JSON object. Emits
 * complete `"key": {...},` members (build, os, sdl, modules,
 * breadcrumbs) and expects the caller to be between members. */
void host_report_dump_json(FILE *f);

/* Write a minidump next to the exe (Windows SEH path; `seh_info` is
 * EXCEPTION_POINTERS*). Returns the filename written, or NULL
 * (non-Windows, or dbghelp unavailable). */
const char *host_report_write_minidump(void *seh_info);

/* Copy `src_path` (the just-written report) to a timestamped
 * crash_report_*.json that subsequent runs never overwrite. Returns
 * the destination filename, or NULL on failure. */
const char *host_report_preserve_crash_copy(const char *src_path);

/* Dev/support crash-drill: if SNESRECOMP_CRASH_TEST is set in the
 * environment, deliberately fault (env value "seh", default) or
 * Die (env value "die") so the whole capture pipeline — minidump,
 * report, crash copy — can be exercised end-to-end without a real
 * bug. Call once per frame from the main loop; inert unless the env
 * var is set. Fires on frame 120 so boot breadcrumbs exist first. */
void host_report_crash_test_tick(void);

#ifdef __cplusplus
}
#endif

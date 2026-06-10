#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * snesrecomp_launcher_resolve_rom
 *
 * Interactively resolve the path to a SNES ROM the recompiled game can use.
 *
 * Resolution order:
 *   1. argv[1] if present and not a flag (starts with '-').
 *   2. Cached path in <exe_dir>/rom.cfg.
 *   3. Win32 Open File dialog (".sfc / .smc"). Other platforms: error.
 *
 * If expected_crc != 0, the resolved file is CRC32-verified. On mismatch,
 * the user is prompted again (file picker re-opens). When using argv[1],
 * a CRC mismatch warns but does not re-prompt — backwards-compatible
 * with command-line invocation.
 *
 * The 512-byte SMC copier header (older copy-tool format) is auto-detected
 * by file size and stripped before CRC verification, so headered and
 * unheadered copies of the same ROM yield the same CRC.
 *
 * On success: writes the resolved absolute path into out_path and persists
 * it to rom.cfg next to the exe. Returns 1.
 *
 * On user cancel (no ROM selected) or repeated CRC failure with cancel:
 * out_path[0] = '\0' and returns 0.
 *
 * Caller is expected to call this from main() before any ROM byte is
 * touched. Game-specific runners should pass their known good CRC32, or
 * 0 to skip verification.
 */
int snesrecomp_launcher_resolve_rom(int argc, char **argv,
                                    char *out_path, size_t max_len,
                                    uint32_t expected_crc);

/*
 * snesrecomp_launcher_resolve_rom_sha256
 *
 * Same contract as snesrecomp_launcher_resolve_rom but verifies via SHA-256
 * instead of CRC32. Use this when the canonical hash you want to pin against
 * comes from a project that publishes SHA-256 (e.g. snesrev/zelda3).
 *
 * `expected_sha256` is a pointer to a 32-byte big-endian SHA-256 digest, or
 * NULL to skip verification.
 *
 * Same SMC-copier-header handling as the CRC32 variant.
 */
int snesrecomp_launcher_resolve_rom_sha256(int argc, char **argv,
                                           char *out_path, size_t max_len,
                                           const uint8_t *expected_sha256);

/*
 * snesrecomp_anchor_to_exe_dir
 *
 * chdir() to the directory containing the running executable, so every
 * relative path the runner opens (game ini, keybinds.ini, rom.cfg, the
 * saves/ directory) resolves next to the exe — regardless of which cwd
 * the process was launched from. This is the whole config-discovery
 * policy: the config is the ini next to the exe, nothing else.
 *
 * Call it first thing in main(), before any file is opened. If any
 * command-line argument carries a relative path, absolutize it with
 * snesrecomp_abspath() BEFORE calling this, since the anchor changes
 * what relative paths mean.
 *
 * If the exe directory is not writable (AppImage squashfs mount, locked
 * system install) or can't be determined (no OS query mechanism), the
 * cwd is left untouched so saves and config land somewhere writable.
 * Returns 1 if cwd is now the exe directory, 0 if it was left alone.
 */
int snesrecomp_anchor_to_exe_dir(void);

/*
 * snesrecomp_abspath
 *
 * Resolve `path` against the CURRENT working directory into an absolute
 * path in `out`. Works for not-yet-existing files. Returns 1 on success;
 * on failure `out` is unspecified and the caller should keep the
 * original path.
 */
int snesrecomp_abspath(const char *path, char *out, size_t max_len);

#ifdef __cplusplus
}
#endif

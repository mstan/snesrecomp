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

#ifdef __cplusplus
}
#endif

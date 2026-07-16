// launcher_files.c — native ROM picker via tinyfiledialogs (zlib).

#include "launcher_files.h"

#include "third_party/tinyfiledialogs.h"

#include <stdio.h>
#include <string.h>

bool launcher_pick_rom(char* out_path, size_t out_cap) {
    if (!out_path || out_cap == 0) return false;
    out_path[0] = '\0';

    static const char* kPatterns[] = { "*.sfc", "*.smc", "*.fig", "*.swc" };

    // tinyfd returns a pointer to its own static buffer, or NULL on cancel.
    const char* sel = tinyfd_openFileDialog(
        "Select SNES ROM",
        "",                                    // default path/file
        (int)(sizeof(kPatterns) / sizeof(kPatterns[0])),
        kPatterns,
        "SNES ROM (.sfc .smc .fig .swc)",
        0);                                    // single select
    if (!sel || !sel[0]) return false;

    snprintf(out_path, out_cap, "%s", sel);
    return true;
}

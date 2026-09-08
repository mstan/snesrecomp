/*
 * secret_path_test.c — where the account secret lives, and that moving it
 * does not sign anyone out.
 *
 * The bug this guards: secret_path() defaulted to the bare relative name
 * "netplay_secret", resolved against the CURRENT WORKING DIRECTORY. The same
 * installed build therefore found its login only when launched from the same
 * place, and a rebuild run from elsewhere looked like a lost credential. The
 * fix anchors it to a host-supplied absolute path — which is only safe if an
 * existing CWD-relative file is migrated rather than abandoned.
 */

#include "recomp_net/auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static void check(int ok, const char *what) {
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static int file_has(const char *path, const char *needle) {
    char buf[512];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return strstr(buf, needle) != NULL;
}

int main(void) {
    char tmpl[] = "/tmp/rnet_secret_test_XXXXXX";
    const char *dir = mkdtemp(tmpl);
    char newpath[512];
    if (!dir) { printf("mkdtemp failed\n"); return 1; }
    if (chdir(dir) != 0) { printf("chdir failed\n"); return 1; }

    /* A previous run's credential, sitting in the working directory. */
    {
        FILE *f = fopen("netplay_secret", "wb");
        fprintf(f, "player-abc secret-xyz\n");
        fclose(f);
    }
    snprintf(newpath, sizeof(newpath), "%s/exe_dir_secret", dir);

    /* Point at the new (host-anchored) location, then init. */
    rnet_account_set_secret_path(newpath);
    rnet_account_init("ws://127.0.0.1:8765");

    check(file_has(newpath, "player-abc"),
          "a legacy CWD-relative secret is migrated to the configured path");
    check(file_has(newpath, "secret-xyz"), "and carries the secret itself");
    check(file_has("netplay_secret", "player-abc"),
          "the original is left in place — a credential is not deleted on the "
          "strength of an unconfirmed write");

    printf(failures ? "FAILED\n" : "PASSED\n");
    return failures;
}

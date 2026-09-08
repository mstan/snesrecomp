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

/* Child mode: a key, a server that cannot be reached, and the question of
 * whether the key survives. job_session used to call secret_forget() on ANY
 * failure -- including "could not connect" -- so a network blip at launch
 * destroyed a working sign-in and demanded a fresh Discord login. Port 59999
 * on loopback refuses immediately, so this stays hermetic and fast. */
static int run_unreachable(void) {
    char tmpl[] = "/tmp/rnet_unreach_XXXXXX";
    const char *dir = mkdtemp(tmpl);
    char keep[512];
    FILE *f;
    int i, st = 0;
    if (!dir) return 1;
    snprintf(keep, sizeof(keep), "%s/netplay_secret", dir);
    f = fopen(keep, "wb");
    if (!f) return 1;
    fprintf(f, "player-keep secret-keep\n");
    fclose(f);

    rnet_account_set_secret_path(keep);
    rnet_account_init("ws://127.0.0.1:59999");
    for (i = 0; i < 150; i++) {
        rnet_account_pump();
        st = rnet_account_state();
        if (st == 2 || st == 3) break;
        usleep(20000);
    }
    f = fopen(keep, "rb");
    if (!f) { printf("    child: KEY WAS DELETED\n"); return 1; }
    fclose(f);
    if (st == 0) { printf("    child: fell back to GUEST silently\n"); return 1; }
    printf("    child: key kept, state=%d, err=%s\n", st, rnet_account_error());
    return 0;
}

static const char *self_path;

int main(int argc, char **argv) {
    char tmpl[] = "/tmp/rnet_secret_test_XXXXXX";
    const char *dir;
    char newpath[512];
    self_path = argv[0];
    if (argc > 1 && strcmp(argv[1], "--unreachable") == 0) return run_unreachable();
    dir = mkdtemp(tmpl);
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

    /* Scenario 2 runs as a SEPARATE PROCESS (below): the account client is a
     * process-lifetime singleton -- rnet_account_init only loads the secret
     * once, and tried_stored_key never resets -- so a second redemption in the
     * same process would not exercise the real path. */
    {
        char cmd[1024];
        int rc;
        snprintf(cmd, sizeof(cmd), "\"%s\" --unreachable", self_path);
        rc = system(cmd);
        check(rc == 0, "an unreachable sign-in server leaves the stored key "
                       "alone and reports the failure (child process)");
    }

    printf(failures ? "FAILED\n" : "PASSED\n");
    return failures;
}

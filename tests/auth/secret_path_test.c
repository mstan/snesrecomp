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
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

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

    {
        struct timespec t0, t1;
        long ms;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        rnet_account_set_secret_path(keep);
        rnet_account_init("ws://127.0.0.1:59999");
        for (i = 0; i < 400; i++) {
            rnet_account_pump();
            st = rnet_account_state();
            if (st == 2 || st == 3) break;
            usleep(20000);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ms = (t1.tv_sec - t0.tv_sec) * 1000 +
             (t1.tv_nsec - t0.tv_nsec) / 1000000;

        f = fopen(keep, "rb");
        if (!f) { printf("    child: KEY WAS DELETED\n"); return 1; }
        fclose(f);
        if (st == 0) { printf("    child: fell back to GUEST silently\n"); return 1; }
        /* The retries are the point. A refused connect returns instantly, so
         * without them this settles in ~0ms; the 250+750ms backoffs put a
         * floor under it. One lost connect used to end the attempt and send a
         * player with a good key back to the Discord prompt. */
        if (ms < 900) {
            printf("    child: settled in %ldms -- retries did not run\n", ms);
            return 1;
        }
        printf("    child: key kept, state=%d after %ldms of retries\n", st, ms);
    }
    return 0;
}

/* Child mode: prove what secret_load() actually parsed, without adding a
 * test-only accessor. Put the credential at the LEGACY cwd-relative name and
 * configure a different path: the migration then writes back exactly the
 * player_id and secret it parsed, so the output file is the parse, on disk.
 * A truncated read shows up as a short secret in that file. */
static int run_roundtrip(const char *dir, const char *want_id, const char *want_secret) {
    char out[512], line[512];
    FILE *f;
    size_t n;
    if (chdir(dir) != 0) return 1;
    f = fopen("netplay_secret", "wb");
    if (!f) return 1;
    fprintf(f, "%s %s\n", want_id, want_secret);
    fclose(f);

    snprintf(out, sizeof(out), "%s/migrated_secret", dir);
    rnet_account_set_secret_path(out);
    rnet_account_init("ws://127.0.0.1:59999");

    f = fopen(out, "rb");
    if (!f) { printf("    child: nothing migrated\n"); return 1; }
    n = fread(line, 1, sizeof(line) - 1, f);
    line[n] = '\0';
    fclose(f);
    {
        char want[512];
        snprintf(want, sizeof(want), "%s %s\n", want_id, want_secret);
        if (strcmp(line, want) != 0) {
            printf("    child: parsed %d bytes, wanted %d -- TRUNCATED\n",
                   (int)strlen(line), (int)strlen(want));
            return 1;
        }
    }
    printf("    child: %d-char id + %d-char secret round-tripped intact\n",
           (int)strlen(want_id), (int)strlen(want_secret));
    return 0;
}

static const char *self_path;

int main(int argc, char **argv) {
    char tmpl[] = "/tmp/rnet_secret_test_XXXXXX";
    const char *dir;
    char newpath[512];
    self_path = argv[0];
    if (argc > 1 && strcmp(argv[1], "--unreachable") == 0) return run_unreachable();
    if (argc > 4 && strcmp(argv[1], "--roundtrip") == 0)
        return run_roundtrip(argv[2], argv[3], argv[4]);
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

    /* ---- a real-length credential must round-trip intact ---------------
     *
     * The line is "<player_id> <secret>", and it used to be read into
     * g.secret (SECRET_MAX = 96). A UUID id plus a 68-char key is 105
     * characters, so fread's 95-byte cap silently truncated the secret to 58
     * before it was split out. The proof was then wrong, the server said 401,
     * and the key was thrown away as invalid -- which is why signing in
     * worked and the NEXT launch always failed. Sizes here match a real
     * credential exactly. */
    {
        const char *real_id = "1ade336d-7caa-4638-b7e2-089c30830e9f";  /* 36 */
        char long_secret[69];
        int i;
        for (i = 0; i < 68; i++) long_secret[i] = (char)('a' + (i % 26));
        long_secret[68] = '\0';

        char rt_dir[512];
        snprintf(rt_dir, sizeof(rt_dir), "%s/rt", dir);
        if (mkdir(rt_dir, 0700) != 0) { check(0, "could not make rt dir"); }

        /* Reload in a child: the module loads its secret once per process. */
        {
            char cmd[1024];
            snprintf(cmd, sizeof(cmd), "\"%s\" --roundtrip \"%s\" \"%s\" \"%s\"",
                     self_path, rt_dir, real_id, long_secret);
            check(system(cmd) == 0,
                  "a 36-char id and a 68-char secret survive the load intact");
        }
    }

    /* Scenario 2 runs as a SEPARATE PROCESS (below): the account client is a
     * process-lifetime singleton -- rnet_account_init only loads the secret
     * once, and tried_stored_key never resets -- so a second redemption in the
     * same process would not exercise the real path. */
    {
        char cmd[1024];
        int rc;
        snprintf(cmd, sizeof(cmd), "\"%s\" --unreachable", self_path);
        rc = system(cmd);
        check(rc == 0, "an unreachable server is retried, keeps the key, and "
                       "reports the failure (child process)");
    }

    printf(failures ? "FAILED\n" : "PASSED\n");
    return failures;
}

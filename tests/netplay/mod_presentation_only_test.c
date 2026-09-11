/*
 * presentation_only: a mod that changes what a machine DRAWS, not what it
 * simulates, may be invisible to the set two netplay peers compare -- but ONLY
 * where the match's authority has granted it.
 *
 *   cc tests/netplay/mod_presentation_only_test.c runner/src/mod_runtime.cpp \
 *      runner/src/crc32.c runner/src/sha256.c -Irunner/src -o /tmp/t && /tmp/t
 *
 * Writes its own catalog and its own selection file, so it needs neither a
 * ROM nor recomp-ui: everything it drives goes through the runtime's own
 * extern "C" surface. Enabling a feature through state.toml rather than
 * through the launcher provider is deliberate twice over -- it keeps this
 * test buildable in a standalone snesrecomp checkout, where recomp-ui's
 * headers are not present, and it exercises the selection loader that a real
 * launch uses rather than a path only the GUI takes.
 *
 * WHY THIS EXISTS
 * ---------------
 * snes_mod_runtime_effective_set_c refuses a match whose mod set is not
 * byte-identical, because a mod that patches guest memory means the two sides
 * are running different games. A presentation filter is not that, and the
 * first one to need saying so is an accessibility feature -- a
 * photosensitivity flash guard. Requiring both peers to agree on it would
 * make it unavailable in precisely the matches it exists for, since a player
 * who needs it does not choose their opponent.
 *
 * THE FLAG ALONE GRANTS NOTHING. `presentation_only = true` is written by
 * whoever wrote the mod, so a cheat wanting out of the comparison writes it
 * too, and nothing in the client can tell the two apart. The exemption needs a
 * second key: the session's cosmetic allowlist, which comes from the automatch
 * ruleset the SERVER published or from the lobby host -- never from the
 * machine that wants the exemption. An ungranted claim is an ordinary
 * simulation-affecting mod, which is the behaviour that predates the flag.
 *
 * So the first case below is the security one: flag set, nothing granted,
 * treated as an ordinary mod. The rest check that a grant works, that it can
 * be pinned to package bytes, and that the exemption reaches all three places
 * netplay looks.
 *
 * Three things have to hold once granted, and the last two are the ones that
 * would fail quietly:
 *
 *   1. the feature never appears in the effective set, so a peer without it
 *      and a peer with it compare equal;
 *   2. nor in the PLAN ROWS, which are the other grain -- one row per
 *      package, and what the lobby server matches a joiner's offer against.
 *      A row there tells a joiner to go and install something, so publishing
 *      one for a local-only filter would rebuild the same barrier one level
 *      up. GWED's automatch assertion also reads the effective set
 *      (GwedSimAffectingModSet in src/main.c), and defaults to treating an
 *      UNKNOWN package as simulation-affecting -- which would refuse the
 *      queue outright -- so being absent upstream is what keeps automatch
 *      working rather than any allowlist entry;
 *   3. ADOPTING a host's set does not switch it off. adopt_set disables every
 *      feature before re-enabling what the host names, and a feature the host
 *      can never name would be swept off and never restored -- turning a
 *      player's accessibility filter off at the moment they join a lobby,
 *      with nothing on screen to say so.
 *
 * An ordinary feature must keep behaving exactly as before, which is what the
 * `sim` control is for in every case below. A test that only proved the new
 * flag suppresses things would pass just as well if it suppressed everything.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "mod_runtime.h"

#define PKG "test.presentation"
#define SHA "0000000000000000000000000000000000000000000000000000000000000000"

/* Bounded well under the buffers below so the fixture paths cannot truncate. */
#define ROOT_MAX 256

static int fails;

static void check(const char *what, int ok)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

static void make_dirs(const char *path)
{
    char acc[1024];
    size_t i;
    const size_t n = strlen(path);
    if (n >= sizeof(acc)) return;
    for (i = 0; i <= n; ++i) {
        if (path[i] == '/' || path[i] == '\0') {
            memcpy(acc, path, i);
            acc[i] = '\0';
            if (acc[0]) {
#ifdef _WIN32
                _mkdir(acc);
#else
                mkdir(acc, 0777);
#endif
            }
        }
    }
}

/* The fixture is written rather than committed so the test carries it: a
 * checked-in manifest that drifts from the parser it exercises is worse than
 * no fixture at all. */
static int write_fixture(const char *root)
{
    char dir[ROOT_MAX + 64];
    char path[ROOT_MAX + 96];
    FILE *f;

    snprintf(dir, sizeof(dir), "%s/packages/" PKG "/1.0.0", root);
    make_dirs(dir);
    snprintf(path, sizeof(path), "%s/manifest.toml", dir);
    if (!(f = fopen(path, "wb"))) return 0;
    fprintf(f,
        "format_version = 1\n"
        "id = \"" PKG "\"\n"
        "version = \"1.0.0\"\n"
        "name = \"presentation_only fixture\"\n"
        "resolver = \"declarative\"\n"
        "\n"
        "[[target]]\n"
        "game_id = \"test\"\n"
        "rom_sha256 = \"" SHA "\"\n"
        "\n"
        /* The control: an ordinary feature, which must keep appearing in the
         * set and must keep being swept off by an adopt. */
        "[[feature]]\n"
        "id = \"sim\"\n"
        "name = \"Simulation feature\"\n"
        "default_enabled = false\n"
        "\n"
        "[[feature]]\n"
        "id = \"draw\"\n"
        "name = \"Presentation feature\"\n"
        "default_enabled = false\n"
        "presentation_only = true\n"
        "\n"
        "[[plugin]]\n"
        "feature = \"draw\"\n"
        "id = \"test.draw.plugin\"\n"
        "\n"
        /* A second presentation feature whose plugin this "build" registers
         * the ORDINARY way -- the smuggling case. Its manifest claims exactly
         * what the real one claims; only the executable's classification
         * differs, which is the whole point of the test. */
        "[[feature]]\n"
        "id = \"sneaky\"\n"
        "name = \"Claims to be presentation\"\n"
        "default_enabled = false\n"
        "presentation_only = true\n"
        "\n"
        "[[plugin]]\n"
        "feature = \"sneaky\"\n"
        "id = \"test.sneaky.plugin\"\n");
    fclose(f);

    return 1;
}

/* The selection a launch starts from. `draw` on and `sim` off is a
 * photosensitive player who has switched their filter on and nothing else --
 * and it is the state that makes the adopt cases take the sweeping path
 * rather than returning OK unchanged. */
static int write_state(const char *root, int draw_on, int sneaky_on)
{
    char path[ROOT_MAX + 96];
    FILE *f;
    snprintf(path, sizeof(path), "%s/state.toml", root);
    if (!(f = fopen(path, "wb"))) return 0;
    fprintf(f,
        "format_version = 1\n"
        "\n"
        "[[package]]\n"
        "id = \"" PKG "\"\n"
        "version = \"1.0.0\"\n"
        "\n"
        "[[feature]]\n"
        "package_id = \"" PKG "\"\n"
        "id = \"draw\"\n"
        "enabled = %s\n"
        "\n"
        "[[feature]]\n"
        "package_id = \"" PKG "\"\n"
        "id = \"sneaky\"\n"
        "enabled = %s\n"
        "\n"
        "[[feature]]\n"
        "package_id = \"" PKG "\"\n"
        "id = \"sim\"\n"
        "enabled = false\n",
        draw_on ? "true" : "false", sneaky_on ? "true" : "false");
    fclose(f);
    return 1;
}

/* Grant the fixture package, pinned to whatever bytes it currently has. */
static void grant_this_package(const char *root)
{
    char digest[72];
    char entry[256];
    (void)root;
    if (!snes_mod_runtime_package_digest_c(PKG, "1.0.0", digest,
                                           sizeof(digest))) {
        snes_mod_runtime_set_cosmetic_allow_c("");
        return;
    }
    snprintf(entry, sizeof(entry), PKG "@1.0.0#%s", digest);
    snes_mod_runtime_set_cosmetic_allow_c(entry);
}

static void noop_plugin(void) {}

int main(int argc, char **argv)
{
    const char *host = PKG "@1.0.0/sim\n";
    char root[ROOT_MAX];
    char set[4096];
    char why[256];

    snprintf(root, sizeof(root), "%s",
             argc > 1 ? argv[1] : "./mod_presentation_only_fixture");
    if (!write_fixture(root) || !write_state(root, 1, 0)) {
        printf("could not write the fixture under %s\n", root);
        return 1;
    }
    /* The executable's classifications, which is the only place they can be
     * made. One plugin vouched for; one registered the ordinary way. */
    snes_mod_register_presentation_plugin("test.draw.plugin", noop_plugin);
    snes_mod_register_activation_plugin("test.sneaky.plugin", noop_plugin);

    if (!snes_mod_runtime_initialize_c(root, "test", SHA)) {
        printf("initialize failed: %s\n", snes_mod_runtime_last_error_c());
        return 1;
    }

    printf("\n0. WITHOUT A GRANT the claim counts for nothing\n");
    {
        /* The security case. No allowlist has been set, which is the state of
         * an older host, a ruleset with no such key, and a fresh process --
         * and the state a cheat would like to be exempt in. */
        char bad[512];
        check("the runtime reports the feature enabled locally",
              snes_mod_runtime_feature_enabled_c(PKG, "draw") == 1);
        check("the ordinary feature is off, as the fixture asked",
              snes_mod_runtime_feature_enabled_c(PKG, "sim") == 0);
        snes_mod_runtime_effective_set_c(set, sizeof(set));
        check("IT APPEARS IN THE COMPARED SET, like any other mod",
              strstr(set, "/draw") != NULL);
        check("so a peer running nothing REFUSES us",
              snes_mod_runtime_check_set_c("(none)\n", why, sizeof(why))
                  != SNES_MODSET_OK);
        check("and it is reported as an unapproved cosmetic claim, by name",
              snes_mod_runtime_unapproved_cosmetics_c(bad, sizeof(bad)) > 0 &&
              strstr(bad, "/draw") != NULL);
    }

    printf("\n0b. a grant for SOMETHING ELSE does not help it\n");
    {
        snes_mod_runtime_set_cosmetic_allow_c("other.package@1.0.0");
        snes_mod_runtime_effective_set_c(set, sizeof(set));
        check("still in the compared set", strstr(set, "/draw") != NULL);
    }

    printf("\n0c. a grant with the WRONG DIGEST does not help it either\n");
    {
        /* The case that makes the list a whitelist rather than a naming
         * convention: right id, right version, different bytes. */
        snes_mod_runtime_set_cosmetic_allow_c(
            PKG "@1.0.0#"
            "1111111111111111111111111111111111111111111111111111111111111111");
        snes_mod_runtime_effective_set_c(set, sizeof(set));
        check("still in the compared set", strstr(set, "/draw") != NULL);
    }

    printf("\n0d. the right digest DOES grant it\n");
    {
        char digest[72];
        char entry[256];
        check("the runtime can digest the installed package",
              snes_mod_runtime_package_digest_c(PKG, "1.0.0", digest,
                                                sizeof(digest)) == 1 &&
              strlen(digest) == 64);
        snprintf(entry, sizeof(entry), PKG "@1.0.0#%s", digest);
        snes_mod_runtime_set_cosmetic_allow_c(entry);
        snes_mod_runtime_effective_set_c(set, sizeof(set));
        check("NOW it is exempt from the compared set",
              strstr(set, "/draw") == NULL);
        check("and nothing is reported as unapproved",
              snes_mod_runtime_unapproved_cosmetics_c(set, sizeof(set)) == 0);
    }

    printf("\n1. granted, it is absent from the compared set\n");
    snes_mod_runtime_effective_set_c(set, sizeof(set));
    check("effective set is \"(none)\": equal to a peer without it",
          strcmp(set, "(none)\n") == 0);
    check("so a peer running nothing accepts us",
          snes_mod_runtime_check_set_c("(none)\n", why, sizeof(why))
              == SNES_MODSET_OK);

    printf("\n2. adopting a host's set leaves it alone\n");
    check("precondition: check_set says OPTION, so the adopt will sweep",
          snes_mod_runtime_check_set_c(host, why, sizeof(why))
              == SNES_MODSET_OPTION);
    check("adopt succeeded",
          snes_mod_runtime_adopt_set_c(host, why, sizeof(why))
              == SNES_MODSET_OK);
    check("the ordinary feature was turned ON by the adopt",
          snes_mod_runtime_feature_enabled_c(PKG, "sim") == 1);
    check("THE PRESENTATION FEATURE SURVIVED THE ADOPT",
          snes_mod_runtime_feature_enabled_c(PKG, "draw") == 1);

    printf("\n3. nor in the plan rows the lobby publishes\n");
    {
        /* At this point the adopt above has turned the ordinary feature on,
         * so a row for the package SHOULD exist -- and must list only the
         * ordinary feature. Asserting the row is present, not merely that the
         * feature is missing from it, is what stops this passing if plan_rows
         * ever started returning nothing at all. */
        SnesModPkgRow rows[8];
        int n = snes_mod_runtime_plan_rows_c(rows, 8);
        int i, row = -1;
        for (i = 0; i < n; ++i)
            if (strcmp(rows[i].id, PKG) == 0) row = i;
        check("the package has a row, because its ordinary feature is on",
              row >= 0);
        check("the row lists the ordinary feature",
              row >= 0 && strstr(rows[row].features, "sim") != NULL);
        check("THE ROW DOES NOT LIST THE PRESENTATION FEATURE",
              row >= 0 && strstr(rows[row].features, "draw") == NULL);
    }

    printf("\n4. a package contributing ONLY a presentation feature has no row\n");
    {
        /* The case that matters: a player whose entire mod selection is an
         * accessibility filter must look, to a joiner, exactly like a player
         * running nothing at all. */
        SnesModPkgRow rows[8];
        int n, i, found = 0;
        check("adopt to vanilla succeeded",
              snes_mod_runtime_adopt_set_c("(none)\n", why, sizeof(why))
                  == SNES_MODSET_OK);
        check("precondition: only the presentation feature is left on",
              snes_mod_runtime_feature_enabled_c(PKG, "draw") == 1 &&
              snes_mod_runtime_feature_enabled_c(PKG, "sim") == 0);
        n = snes_mod_runtime_plan_rows_c(rows, 8);
        for (i = 0; i < n; ++i)
            if (strcmp(rows[i].id, PKG) == 0) found = 1;
        check("no row is published for it at all", !found);
    }

    printf("\n5. and an ordinary feature is still compared as it always was\n");
    check("re-adopting the host's set succeeded",
          snes_mod_runtime_adopt_set_c(host, why, sizeof(why))
              == SNES_MODSET_OK);
    snes_mod_runtime_effective_set_c(set, sizeof(set));
    check("the ordinary feature IS in the set", strstr(set, "/sim") != NULL);
    check("the presentation feature still is not",
          strstr(set, "/draw") == NULL);
    check("we match the host byte for byte", strcmp(set, host) == 0);

    printf("\n6. revoking the grant puts it back under the comparison\n");
    {
        /* Leaving a lobby lapses the grant, and the exemption must lapse with
         * it rather than persisting into the next match. */
        snes_mod_runtime_set_cosmetic_allow_c(NULL);
        snes_mod_runtime_effective_set_c(set, sizeof(set));
        check("it is in the compared set again", strstr(set, "/draw") != NULL);
        check("and unapproved again",
              snes_mod_runtime_unapproved_cosmetics_c(set, sizeof(set)) > 0);
    }

    printf("\n7. a manifest cannot classify ITSELF as presentation\n");
    {
        /* `sneaky` claims presentation_only in exactly the words `draw` uses,
         * and is granted by the same allowlist entry -- the package is the
         * same package. The ONLY difference is that this build vouched for
         * one plugin and not the other, and that has to be enough, because
         * the manifest is the half an attacker writes.
         *
         * Its own launch state, because the adopts above legitimately swept
         * it off and a precondition that quietly depends on a previous case
         * is how a security test starts passing for the wrong reason. */
        check("re-armed both claiming features", write_state(root, 1, 1) == 1);
        check("re-initialize",
              snes_mod_runtime_initialize_c(root, "test", SHA) == 1);
        grant_this_package(root);
        check("precondition: sneaky is enabled and claims presentation_only",
              snes_mod_runtime_feature_enabled_c(PKG, "sneaky") == 1);
        check("precondition: so is the vouched-for one",
              snes_mod_runtime_feature_enabled_c(PKG, "draw") == 1);
        snes_mod_runtime_effective_set_c(set, sizeof(set));
        check("IT IS STILL COMPARED, despite the claim and the grant",
              strstr(set, "/sneaky") != NULL);
        check("while the vouched-for feature beside it IS exempt",
              strstr(set, "/draw") == NULL);
    }

    printf("\n8. a package that ships files cannot be cosmetic at all\n");
    {
        /* Drop one extra file into the package and the claim lapses --
         * including for the plugin this build DID vouch for. A payload is a
         * lever over the guest whatever the manifest says, because a built-in
         * feature may consume it (localization patches ROM text from exactly
         * such a file). */
        char extra[ROOT_MAX + 128];
        FILE *f;
        write_state(root, 1, 0);
        snprintf(extra, sizeof(extra),
                 "%s/packages/" PKG "/1.0.0/payload.bin", root);
        f = fopen(extra, "wb");
        check("wrote a payload into the package", f != NULL);
        if (f) { fputs("x", f); fclose(f); }

        /* Re-initialize: the manifest-only answer is memoised per run, as it
         * is in a real process, so this models the next LAUNCH rather than a
         * file appearing mid-session. The grant is recomputed from the NEW
         * bytes, so this is not the digest test over again -- the allowlist
         * matches perfectly and the claim still fails. */
        snes_mod_runtime_initialize_c(root, "test", SHA);
        grant_this_package(root);
        snes_mod_runtime_effective_set_c(set, sizeof(set));
        check("THE VOUCHED-FOR FEATURE IS NO LONGER EXEMPT",
              strstr(set, "/draw") != NULL);
        remove(extra);
    }

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

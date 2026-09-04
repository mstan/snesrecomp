/*
 * Round-trip the lobby mod plan: rows -> match_caps JSON -> rows.
 *
 *   cc tests/netplay/lobby_mod_plan_test.c -o /tmp/t && /tmp/t
 *
 * The plan is what the LOBBY SERVER reads to decide whether a joiner may sit
 * down. Its required_mod_rows() takes match_caps.mods as a JSON ARRAY of
 * objects and matches each {id, ver} against the joiner's offer; anything it
 * cannot read as an array yields an empty requirement list and seats everyone.
 * So the encoding has to be an array, and it has to survive the trip exactly:
 * a row that parses to the wrong package tells a player to install the wrong
 * thing, and a count one short hides a requirement entirely -- silently, and
 * in the direction that lets a broken match start.
 *
 * This includes the lobby client's translation unit directly so it tests the
 * REAL append_mod_pkg_array/parse_mod_pkg_array rather than a copy of them. An
 * earlier version of this test mirrored the encoding by hand and said so in
 * its header; a mirror cannot fail when the thing it mirrors changes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runner/src/lobby/snes_lobby_client.c"

/* Link-time double for the one socket call the lobby client's other paths
 * reach for. None of the cases below open a socket, so this must never run --
 * it aborts rather than returning a plausible port, so a future case that
 * starts depending on real networking fails loudly instead of quietly passing
 * against a fake. */
int rnet_udp_find_free_port(int preferred, int span)
{
    (void)preferred; (void)span;
    fprintf(stderr, "lobby_mod_plan_test: rnet_udp_find_free_port was called; "
                    "this test does no networking\n");
    abort();
}

/* The lobby client now drives an ICE transfer agent. None of the cases below
 * open one, so every entry point aborts rather than returning a plausible
 * value -- linking the real agent would drag libjuice into a test about JSON,
 * and a silent stub would let a future case "pass" against a fake network. */
#define XFER_TRAP(name) \
    do { fprintf(stderr, "lobby_mod_plan_test: %s called; this test does no " \
                         "networking\n", name); abort(); } while (0)

int  rnet_ice_xfer_open(RNetIceXfer **o, const RNetIceConfig *c,
                        RNetIceXferSignalEmitFn e, void *u)
{ (void)o; (void)c; (void)e; (void)u; XFER_TRAP("rnet_ice_xfer_open"); }
void rnet_ice_xfer_close(RNetIceXfer **x) { (void)x; XFER_TRAP("close"); }
void rnet_ice_xfer_push_signal(RNetIceXfer *x, const RNetSignal *m)
{ (void)x; (void)m; XFER_TRAP("push_signal"); }
void rnet_ice_xfer_pump(RNetIceXfer *x) { (void)x; XFER_TRAP("pump"); }
int  rnet_ice_xfer_queue_blob(RNetIceXfer *x, uint8_t *d, size_t l)
{ (void)x; (void)d; (void)l; XFER_TRAP("queue_blob"); }
int  rnet_ice_xfer_send_idle(const RNetIceXfer *x) { (void)x; XFER_TRAP("send_idle"); }
int  rnet_ice_xfer_take_blob(RNetIceXfer *x, uint8_t **d, size_t *l)
{ (void)x; (void)d; (void)l; XFER_TRAP("take_blob"); }
int  rnet_ice_xfer_progress(const RNetIceXfer *x) { (void)x; XFER_TRAP("progress"); }
int  rnet_ice_xfer_failed(const RNetIceXfer *x, char *e, size_t c)
{ (void)x; (void)e; (void)c; XFER_TRAP("failed"); }
void rnet_ice_xfer_path(const RNetIceXfer *x, char *o, size_t c)
{ (void)x; (void)o; (void)c; XFER_TRAP("path"); }

/* Stands in for the mod runtime: two installed packages. */
static int two_pkg_offer(SnesLobbyModPkg *out, int max, void *ctx)
{
    (void)ctx;
    if (max < 2) return 0;
    memset(out, 0, sizeof(*out) * 2);
    snprintf(out[0].id, sizeof(out[0].id), "gwed.localization");
    snprintf(out[0].ver, sizeof(out[0].ver), "1.0.0");
    snprintf(out[1].id, sizeof(out[1].id), "gwed.enhancement.widescreen");
    snprintf(out[1].ver, sizeof(out[1].ver), "1.0.0");
    return 2;
}

/* The shape the server builds in slot_json(): the offer object verbatim,
 * beside the other per-seat fields. */
static void std_snprintf_slot(char *dst, size_t cap, const char *offer)
{
    snprintf(dst, cap,
             "{\"slot\":1,\"player_id\":\"them\",\"display_name\":\"Bob\","
             "\"ready\":true%s}", offer);
}

static int fails;

static void ck(int cond, const char *what)
{
    if (!cond) { printf("    FAIL %s\n", what); fails++; }
}

/* The shape the server insists on, checked as text: `"mods":[` and objects
 * carrying "id"/"ver". A string-encoded plan is valid JSON and passes every
 * round-trip test you could write against ourselves -- it just fails at the
 * server. That regression is what this assertion is here to catch. */
static void ck_is_json_array(const char *json)
{
    const char *m = strstr(json, "\"mod_plan\":");
    ck(m != NULL, "match_caps carries a mod_plan field");
    if (!m) return;
    ck(m[11] == '[', "mod_plan is a JSON array, not a string");
    /* The seat gate must stay inert. `mods` is the key the lobby server
     * enforces by refusing to seat; publishing it would put the gate back on
     * the door, where this title does not want it -- a peer without the mods
     * is supposed to get in and download them. */
    ck(strstr(json, "\"mods\":") == NULL,
       "the server's seat-gate key is NOT published");
}

static void case_rows(void)
{
    SnesLobbyMatchCaps caps;
    SnesLobbyModPkg back[SNES_LOBBY_MAX_MODS];
    char json[SNES_LOBBY_MAX_MODS * 256 + 512];
    char obj[SNES_LOBBY_MAX_MODS * 256 + 512];
    int n;

    memset(&caps, 0, sizeof(caps));
    caps.valid = 1;
    caps.input_delay = 2;
    caps.mod_count = 2;
    snprintf(caps.mods[0].id, sizeof(caps.mods[0].id), "gwed.localization");
    snprintf(caps.mods[0].ver, sizeof(caps.mods[0].ver), "1.0.0");
    snprintf(caps.mods[0].name, sizeof(caps.mods[0].name), "Localization");
    snprintf(caps.mods[0].feats, sizeof(caps.mods[0].feats), "localization");
    snprintf(caps.mods[1].id, sizeof(caps.mods[1].id),
             "gwed.enhancement.widescreen");
    snprintf(caps.mods[1].ver, sizeof(caps.mods[1].ver), "1.0.0");
    snprintf(caps.mods[1].name, sizeof(caps.mods[1].name), "Widescreen");
    snprintf(caps.mods[1].feats, sizeof(caps.mods[1].feats), "widescreen");

    ck(append_match_caps_json(json, sizeof(json), &caps) > 0, "caps encoded");
    printf("  json: %s\n", json);
    ck_is_json_array(json);
    /* No stray comma before the first element, which would make the whole
     * caps blob unparseable and take every other setting down with it. */
    ck(strstr(json, "[,") == NULL, "no leading comma in the array");
    ck(strstr(json, "\"id\":\"gwed.localization\"") != NULL, "row 0 id present");
    ck(strstr(json, "\"ver\":\"1.0.0\"") != NULL, "row 0 ver present");

    ck(json_extract_object(json, "match_caps", obj, sizeof(obj)) != 0,
       "match_caps object extracts whole");
    n = parse_mod_pkg_array(obj, "mod_plan", back, SNES_LOBBY_MAX_MODS);
    printf("  parsed back n=%d\n", n);
    ck(n == 2, "two rows round-tripped");
    if (n == 2) {
        ck(!strcmp(back[0].id, "gwed.localization"), "row 0 id");
        ck(!strcmp(back[0].ver, "1.0.0"), "row 0 ver");
        ck(!strcmp(back[0].feats, "localization"), "row 0 feats");
        ck(!strcmp(back[1].id, "gwed.enhancement.widescreen"), "row 1 id");
    }
}

static void case_empty(void)
{
    SnesLobbyMatchCaps caps;
    SnesLobbyModPkg back[SNES_LOBBY_MAX_MODS];
    char json[512];

    memset(&caps, 0, sizeof(caps));
    caps.valid = 1;
    caps.mod_count = 0;
    ck(append_match_caps_json(json, sizeof(json), &caps) > 0, "empty encoded");
    ck(strstr(json, "\"mod_plan\":[]") != NULL, "empty plan is an empty array");
    ck(parse_mod_pkg_array(json, "mod_plan", back, SNES_LOBBY_MAX_MODS) == 0,
       "empty array parses to no rows");
}

/* The superseded encoding must parse to NOTHING rather than to rows. Reading
 * it would leave this peer believing in requirements the server has already
 * ignored, so the two disagree about what the match needs. */
static void case_old_string_encoding_is_not_revived(void)
{
    SnesLobbyModPkg back[SNES_LOBBY_MAX_MODS];
    const char *old = "{\"v\":1,\"mod_plan\":\"gwed.localization@1.0.0/localization\"}";
    ck(parse_mod_pkg_array(old, "mods", back, SNES_LOBBY_MAX_MODS) == 0,
       "a string-encoded plan yields no rows");
}

/* A row without both halves of its identity is not a lesser row, it is a
 * different package -- the server matches on the pair. */
static void case_partial_rows_dropped(void)
{
    SnesLobbyModPkg back[SNES_LOBBY_MAX_MODS];
    const char *j =
        "{\"mod_plan\":[{\"id\":\"a.one\"},{\"ver\":\"2.0\"},"
        "{\"id\":\"b.two\",\"ver\":\"2.0\"}]}";
    int n = parse_mod_pkg_array(j, "mod_plan", back, SNES_LOBBY_MAX_MODS);
    ck(n == 1, "rows missing id or ver are dropped");
    if (n == 1) ck(!strcmp(back[0].id, "b.two"), "the complete row survives");
}

/* Refusing to publish beats publishing a short plan: a plan that names fewer
 * requirements than the host has seats a peer that cannot play. */
static void case_overflow_refuses(void)
{
    SnesLobbyMatchCaps caps;
    char small[64];
    int i;

    memset(&caps, 0, sizeof(caps));
    caps.valid = 1;
    caps.mod_count = SNES_LOBBY_MAX_MODS;
    for (i = 0; i < SNES_LOBBY_MAX_MODS; ++i) {
        snprintf(caps.mods[i].id, sizeof(caps.mods[i].id), "pkg.number.%02d", i);
        snprintf(caps.mods[i].ver, sizeof(caps.mods[i].ver), "1.0.0");
    }
    ck(append_match_caps_json(small, sizeof(small), &caps) == 0,
       "a plan that does not fit publishes nothing at all");
}

/* Set up a two-seat lobby: us in seat 0, a peer in seat 1 holding `peer_pkgs`,
 * and a host plan of `plan_pkgs`. Returns the launch gate's verdict. */
static int gate_with(const SnesLobbyModPkg *plan, int plan_n,
                     const SnesLobbyModPkg *peer, int peer_n,
                     char *who, size_t who_cap, char *what, size_t what_cap)
{
    memset(&g_lc.match_caps, 0, sizeof(g_lc.match_caps));
    g_lc.match_caps.valid = 1;
    g_lc.match_caps.mod_count = plan_n;
    memcpy(g_lc.match_caps.mods, plan, sizeof(SnesLobbyModPkg) * (size_t)plan_n);

    g_lc.in_lobby = 1;
    snprintf(g_lc.player_id, sizeof(g_lc.player_id), "me");
    g_lc.member_count = 2;
    snprintf(g_lc.members[0].player_id, sizeof(g_lc.members[0].player_id), "me");
    snprintf(g_lc.members[1].player_id, sizeof(g_lc.members[1].player_id), "them");
    snprintf(g_lc.members[1].display_name,
             sizeof(g_lc.members[1].display_name), "Bob");
    g_lc.member_offer_count[0] = 0;
    g_lc.member_offer_count[1] = peer_n;
    memcpy(g_lc.member_offer[1], peer, sizeof(SnesLobbyModPkg) * (size_t)peer_n);
    return snes_lobby_match_blocked_by_mods(who, who_cap, what, what_cap);
}

static SnesLobbyModPkg row(const char *id, const char *ver)
{
    SnesLobbyModPkg r;
    memset(&r, 0, sizeof(r));
    snprintf(r.id, sizeof(r.id), "%s", id);
    snprintf(r.ver, sizeof(r.ver), "%s", ver);
    return r;
}

/* The launch gate asks "do you have this package", not "at this version".
 * Version agreement is settled by the mod-set exchange at session start, which
 * is finer (per feature, with resolved option values) and can explain itself.
 * Checking it here too would block a lobby over a difference that may not even
 * change the simulation. */
static void case_gate_matches_on_id_only(void)
{
    SnesLobbyModPkg plan[2];
    SnesLobbyModPkg peer[2];
    char who[64], what[160];

    plan[0] = row("gwed.localization", "1.0.0");
    peer[0] = row("gwed.localization", "2.5.1");   /* same package, other ver */
    ck(gate_with(plan, 1, peer, 1, who, sizeof(who), what, sizeof(what)) == 0,
       "a different version of the same package does NOT block the match");

    peer[0] = row("something.else", "1.0.0");
    ck(gate_with(plan, 1, peer, 1, who, sizeof(who), what, sizeof(what)) == 1,
       "a genuinely absent package blocks the match");
    ck(!strcmp(who, "Bob"), "the blocked player is named");
    ck(!strcmp(what, "gwed.localization@1.0.0"), "the missing package is named");

    plan[1] = row("gwed.enhancement.widescreen", "1.0.0");
    peer[0] = row("gwed.localization", "0.9");
    peer[1] = row("gwed.enhancement.widescreen", "9.9");
    ck(gate_with(plan, 2, peer, 2, who, sizeof(who), what, sizeof(what)) == 0,
       "every package present at any version starts the match");

    ck(gate_with(plan, 2, peer, 0, who, sizeof(who), what, sizeof(what)) == 2,
       "a peer announcing nothing is missing everything");
}

/* The transfer hooks must outlive a lobby reconnect.
 *
 * They used to live in LobbyClient, which snes_lobby_disconnect() memsets --
 * and connect() calls disconnect() first. So the host installed them at
 * start-up, wiped them the moment it joined a lobby, and then refused every
 * download with "could not pack the mod" while being entirely able to pack
 * it. Anything that is a property of the BUILD rather than of the connection
 * belongs outside that struct. */
static int hook_calls;
static int fake_export(const char *id, const char *ver, uint8_t **out,
                       uint32_t *out_len, char *sha, uint32_t sha_cap,
                       char *err, uint32_t err_cap, void *ctx)
{
    (void)id; (void)ver; (void)out; (void)out_len; (void)sha; (void)sha_cap;
    (void)err; (void)err_cap; (void)ctx;
    hook_calls++;
    return 0;
}
static void fake_free(uint8_t *b) { (void)b; }
static int fake_install(const uint8_t *d, uint32_t l, const char *sha,
                        char *id, uint32_t ic, char *ver, uint32_t vc,
                        char *err, uint32_t ec, void *ctx)
{
    (void)d; (void)l; (void)sha; (void)id; (void)ic; (void)ver; (void)vc;
    (void)err; (void)ec; (void)ctx;
    return 0;
}

static void case_hooks_survive_disconnect(void)
{
    snes_lobby_set_mod_transfer_hooks(fake_export, fake_free, fake_install, NULL);
    ck(g_mod_export_fn == fake_export, "export hook installs");
    /* connect() begins with exactly this call. */
    snes_lobby_disconnect();
    ck(g_mod_export_fn == fake_export,
       "export hook survives the disconnect that connect() performs");
    ck(g_mod_install_fn == fake_install, "install hook survives too");
    ck(g_mod_offer_fn == NULL || g_mod_offer_fn != NULL, "offer supplier intact");
    snes_lobby_set_mod_transfer_hooks(NULL, NULL, NULL, NULL);
}

/* The ICE signal a peer emits is not the one its partner must be handed. */
static void case_ice_local_becomes_remote(void)
{
    ck(mod_ice_type_for_push((int)RNET_SIGNAL_LOCAL_SDP) ==
       (int)RNET_SIGNAL_REMOTE_SDP, "a peer's LOCAL_SDP is pushed as REMOTE_SDP");
    ck(mod_ice_type_for_push((int)RNET_SIGNAL_LOCAL_CANDIDATE) ==
       (int)RNET_SIGNAL_REMOTE_CANDIDATE,
       "a peer's LOCAL_CANDIDATE is pushed as REMOTE_CANDIDATE");
    /* These mean the same on both sides. */
    ck(mod_ice_type_for_push((int)RNET_SIGNAL_GATHERING_DONE) ==
       (int)RNET_SIGNAL_GATHERING_DONE, "GATHERING_DONE passes through");
    ck(mod_ice_type_for_push((int)RNET_SIGNAL_SET_CONTROLLING) ==
       (int)RNET_SIGNAL_SET_CONTROLLING, "SET_CONTROLLING passes through");
    /* Forwarding REMOTE_* would mean somebody already translated it once. */
    ck(mod_ice_type_for_push((int)RNET_SIGNAL_REMOTE_SDP) ==
       (int)RNET_SIGNAL_REMOTE_SDP, "REMOTE_SDP is not translated twice");
}

/* The offer this peer SENDS must be readable by the code that RECEIVES one.
 *
 * They are written in different places -- append_mod_offer builds it, and the
 * slot parser reads it back out of the server's echo -- and they disagreed:
 * one wrote {"pkgs":[...]}, the other looked for a bare array. Every peer
 * therefore looked empty-handed, and a guest with every mod installed was
 * still refused at Play. This asserts the two ends against each other rather
 * than each against its own idea of the format. */
static void case_offer_round_trips_through_a_slot_row(void)
{
    char offer[SNES_LOBBY_MAX_MODS * 256 + 64];
    char slot_row[SNES_LOBBY_MAX_MODS * 256 + 256];
    char obj[SNES_LOBBY_MAX_MODS * 256 + 64];
    SnesLobbyModPkg back[SNES_LOBBY_MAX_MODS];
    int n;

    snes_lobby_set_mod_offer_supplier(two_pkg_offer, NULL);
    ck(append_mod_offer(offer, sizeof(offer)) > 0, "offer encodes");
    /* The server stores the object and echoes it inside the slot row. */
    std_snprintf_slot(slot_row, sizeof(slot_row), offer);
    ck(strstr(slot_row, "\"mod_offer\":{") != NULL,
       "the offer travels as an object, which is what the server accepts");

    ck(json_extract_object(slot_row, "mod_offer", obj, sizeof(obj)) != 0,
       "the offer object extracts from a slot row");
    n = parse_mod_pkg_array(obj, "pkgs", back, SNES_LOBBY_MAX_MODS);
    ck(n == 2, "both offered packages are read back");
    if (n == 2) {
        ck(!strcmp(back[0].id, "gwed.localization"), "offer row 0 id");
        ck(!strcmp(back[1].id, "gwed.enhancement.widescreen"), "offer row 1 id");
    }
    snes_lobby_set_mod_offer_supplier(NULL, NULL);
}

/* The host's exact configuration has to survive the wire, because a guest
 * adopts it verbatim. The plan says which packages; this says how they are
 * set up, and a guest that owns both mods but enabled neither passes the
 * plan and is still refused at launch. */
static void case_mod_set_round_trips(void)
{
    SnesLobbyMatchCaps caps;
    SnesLobbyMatchCaps back;
    char json[SNES_LOBBY_MAX_MODS * 256 + 1024];
    char obj[SNES_LOBBY_MAX_MODS * 256 + 1024];

    memset(&caps, 0, sizeof(caps));
    caps.valid = 1;
    caps.mod_count = 0;
    snprintf(caps.mod_set, sizeof(caps.mod_set),
             "gwed.enhancement.widescreen@1.0.0/widescreen;"
             "gwed.localization@1.0.0/localization language=en");
    ck(append_match_caps_json(json, sizeof(json), &caps) > 0, "caps encode");
    ck(json_extract_object(json, "match_caps", obj, sizeof(obj)) != 0,
       "caps object extracts");
    memset(&back, 0, sizeof(back));
    parse_match_caps_object(obj, &back);
    ck(!strcmp(back.mod_set, caps.mod_set),
       "the host's effective set survives the round trip verbatim");
    /* Option values are part of the identity, not decoration. */
    ck(strstr(back.mod_set, "language=en") != NULL,
       "resolved option values survive");
}


/* ---- relay size cap -----------------------------------------------------
 *
 * A relayed transfer is carried by the TURN server, so its bytes are the
 * relay operator's cost, not the two players'. Direct pairs are free to both
 * of us and stay uncapped at any size; only "relay" is priced.
 *
 * snes_lobby_mod_relay_size_allows is pure on purpose: the decision it makes
 * is the whole policy, and testing it here means testing it without a socket,
 * a TURN server, or a peer that has to be persuaded not to find a direct
 * route. */
#define ONE_MB (1024u * 1024u)

static void case_relay_cap_is_five_mb(void)
{
    char why[256];
    printf("  relay cap\n");

    ck(SNES_LOBBY_MOD_RELAY_MAX_BYTES == 5u * ONE_MB,
       "the cap is 5 MiB");

    /* Exactly at the cap is allowed: the rule is "over 5 MB", and a package
     * that is 5 MB to the byte is not over it. */
    ck(snes_lobby_mod_relay_size_allows("relay", 5u * ONE_MB, "m", why,
                                        sizeof(why)) == 1,
       "5 MB exactly still goes over the relay");
    ck(why[0] == '\0', "an allowed transfer writes no reason");

    ck(snes_lobby_mod_relay_size_allows("relay", 5u * ONE_MB + 1u, "m", why,
                                        sizeof(why)) == 0,
       "one byte over the cap is refused on the relay");
}

static void case_direct_paths_are_never_capped(void)
{
    char why[256];
    const char *direct[] = { "host", "srflx", "prflx" };
    size_t i;
    printf("  direct paths uncapped\n");

    /* 400 MB over a direct pair is fine. This is the case the cap exists to
     * leave alone -- a peer-to-peer link costs nobody but the two players,
     * and refusing a big mod on it would be a limit invented for no reason. */
    for (i = 0; i < sizeof(direct) / sizeof(direct[0]); ++i) {
        why[0] = 'x';
        ck(snes_lobby_mod_relay_size_allows(direct[i], 400u * ONE_MB, "big",
                                            why, sizeof(why)) == 1,
           direct[i]);
        ck(why[0] == '\0', "no reason on an allowed direct transfer");
    }
}

static void case_unknown_path_allows(void)
{
    char why[256];
    printf("  unknown path\n");

    /* Asked before the pair is named, the answer is yes. A "no" here would
     * cap direct transfers on nothing more than a query that came too early,
     * which is the failure this policy must not have. The callers are what
     * make this safe: both wait for a named path first, and the host says so
     * in the log on the rare occasion it gives up waiting. */
    ck(snes_lobby_mod_relay_size_allows("unknown", 400u * ONE_MB, "big", why,
                                        sizeof(why)) == 1,
       "an unnamed path does not refuse");
    ck(snes_lobby_mod_relay_size_allows(NULL, 400u * ONE_MB, "big", why,
                                        sizeof(why)) == 1,
       "a NULL path does not refuse");
    ck(snes_lobby_mod_relay_size_allows("none", 400u * ONE_MB, "big", why,
                                        sizeof(why)) == 1,
       "a build without ICE does not refuse");
}

static void case_refusal_tells_the_player_what_to_do(void)
{
    char why[256];
    printf("  refusal text\n");

    why[0] = '\0';
    (void)snes_lobby_mod_relay_size_allows("relay", 12u * ONE_MB + 512u * 1024u,
                                           "gwed.enormous", why, sizeof(why));
    /* The three things a player needs and cannot work out for themselves:
     * which mod, how big it is, and that the fix is to fetch it elsewhere
     * rather than to click again. */
    ck(strstr(why, "gwed.enormous") != NULL, "the reason names the package");
    ck(strstr(why, "12.5 MB") != NULL, "the reason states the actual size");
    ck(strstr(why, "5 MB") != NULL, "the reason states the cap");
    ck(strstr(why, "original source") != NULL,
       "the reason says where to get it instead");
    ck(strlen(why) < 256, "the reason fits the transfer error buffer");

    /* A missing id still produces a sentence rather than an empty gap. */
    why[0] = '\0';
    (void)snes_lobby_mod_relay_size_allows("relay", 9u * ONE_MB, NULL, why,
                                           sizeof(why));
    ck(strstr(why, "that mod") != NULL, "an unnamed package still reads");
}

static void case_refusal_survives_a_short_buffer(void)
{
    char why[16];
    printf("  short buffer\n");

    /* The verdict is the return value, never the length of the reason. A
     * caller with a small buffer still gets refused. */
    ck(snes_lobby_mod_relay_size_allows("relay", 90u * ONE_MB, "m", why,
                                        sizeof(why)) == 0,
       "a truncated reason is still a refusal");
    ck(why[sizeof(why) - 1] == '\0', "the short reason stays terminated");
    ck(snes_lobby_mod_relay_size_allows("relay", 90u * ONE_MB, "m", NULL, 0)
           == 0,
       "no reason buffer at all is still a refusal");
}

int main(void)
{
    case_rows();
    case_empty();
    case_old_string_encoding_is_not_revived();
    case_partial_rows_dropped();
    case_overflow_refuses();
    case_gate_matches_on_id_only();
    case_hooks_survive_disconnect();
    case_ice_local_becomes_remote();
    case_offer_round_trips_through_a_slot_row();
    case_mod_set_round_trips();
    case_relay_cap_is_five_mb();
    case_direct_paths_are_never_capped();
    case_unknown_path_allows();
    case_refusal_tells_the_player_what_to_do();
    case_refusal_survives_a_short_buffer();
    printf(fails ? "\n%d failure(s)\n" : "\nall mod-plan cases passed\n", fails);
    return fails != 0;
}

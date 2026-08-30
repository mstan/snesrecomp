#include "snes_host_lobby.h"
#include "snes_netplay_identity.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "recomp_net/lan_lobby.h"
#include "recomp_net/lan_direct.h"
#include "recomp_net/address.h"

#if !defined(RECOMP_LAUNCHER) && !defined(SNES_HOST_HAS_RECOMP_UI)

int snes_host_lobby_init(const SnesHostLobbyIdentity *id,
                         const SnesHostLobbyOpts *opts)
{
  (void)id;
  (void)opts;
  return -1;
}
void snes_host_lobby_shutdown(void) {}
void snes_host_lobby_prepare_rematch(void) {}
int snes_host_lobby_leave(void) { return -1; }
void snes_host_lobby_disconnect(void) {}
const char *snes_host_lobby_resume_endpoint(void) { return ""; }
int snes_host_lobby_in_lan(void) { return 0; }
void snes_host_lobby_set_runtime_error(const char *error_code)
{
  (void)error_code;
}

#else

enum { kMaxLocalAddresses = 16 };

static SnesHostLobbyIdentity g_id;
static SnesHostLobbyOpts g_opts;
static int g_inited;
static int g_hosting_lan;
static int g_joined_lan;
static int g_joined_direct; /* UDP Direct IP (remote); not file-registry */
static RecompLauncherCNetplayLaunch g_lan_launch;
static RNetLanLobby g_lan_room; /* host + direct-guest in-memory seat state */
static RNetLanDirectHost *g_direct_host;
static RNetLanDirectGuest *g_direct_guest;
static char g_direct_peer_endpoint[64]; /* guest launch peer = typed IP:port */
static char g_lobby_url[256];
static char g_resume_endpoint[64];
static char g_runtime_error[64];
static RNetIpv4Address g_local_addresses[kMaxLocalAddresses];
static int g_local_address_count;
static char g_external_ip[RNET_IPV4_ADDRESS_TEXT_MAX];
static int g_lobby_input_delay = 2; /* waiting-room setting; clamped 2..20 */
static int g_lobby_force_turn = 0;  /* host: ICE relay-only for server lobbies */
static int g_lobby_force_input_relay = 0; /* host: server UDP input relay */
static int g_lobby_max_slots = 2;   /* seat ceiling for current/created room */
static int g_lan_guest_rtt_ms = -1;

static int clamp_lobby_max_slots(int slots)
{
  if (slots < 2)
    return 2;
  if (slots > RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS)
    return RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS;
  return slots;
}

static int clamp_input_delay(int delay)
{
  if (delay < 2)
    return 2;
  if (delay > 20)
    return 20;
  return delay;
}

static const char *lan_path(void)
{
  return g_id.lan_registry_path && g_id.lan_registry_path[0]
             ? g_id.lan_registry_path
             : "netplay_lan_lobby.txt";
}

static void close_direct_sockets(void)
{
  rnet_lan_direct_host_close(&g_direct_host);
  rnet_lan_direct_guest_close(&g_direct_guest);
}

static int publish_lan_room(void)
{
  return rnet_lan_lobby_publish(lan_path(), &g_lan_room) == RNET_LAN_LOBBY_OK;
}

static const char *game_name(void)
{
  return g_id.game_name && g_id.game_name[0] ? g_id.game_name : "SNES Game";
}

static const char *game_version(void)
{
  return g_id.game_version && g_id.game_version[0] ? g_id.game_version
                                                    : "0.0.0";
}

static SnesLobbyMatchCaps default_caps(const RecompLauncherCSettings *settings)
{
  SnesLobbyMatchCaps caps;
  memset(&caps, 0, sizeof(caps));
  caps.valid = 1;
  caps.rollback = 1;   /* framework default; fill_match_caps may override */
  caps.input_delay = clamp_input_delay(g_lobby_input_delay);
  if (settings) {
    caps.widescreen = settings->widescreen != 0;
    caps.widescreen_hud = settings->widescreen_hud != 0;
    caps.ignore_aspect = settings->ignore_aspect != 0;
  }
  if (g_opts.fill_match_caps)
    g_opts.fill_match_caps(g_opts.caps_ctx, settings, &caps);
  caps.input_delay = clamp_input_delay(caps.input_delay);
  caps.force_turn = g_lobby_force_turn ? 1 : 0;
  caps.force_input_relay = g_lobby_force_input_relay ? 1 : 0;
  return caps;
}

static int read_lan(RNetLanLobby *state)
{
  return rnet_lan_lobby_read(lan_path(), game_name(), game_version(), state) ==
         RNET_LAN_LOBBY_OK;
}

static int create_lan(const char *name, const char *endpoint,
                      const char *password)
{
  char advertised[RNET_LAN_LOBBY_ENDPOINT_MAX];
  const char *stored_endpoint = endpoint;
  const char *colon;
  const char *port;
  char bind_hp[64];
  memset(&g_lan_room, 0, sizeof(g_lan_room));
  close_direct_sockets();
  snprintf(g_lan_room.name, sizeof(g_lan_room.name), "%s",
           name && name[0]
               ? name
               : (g_id.default_lobby_name ? g_id.default_lobby_name
                                          : "LAN Lobby"));
  snprintf(g_lan_room.game, sizeof(g_lan_room.game), "%s", game_name());
  snprintf(g_lan_room.game_version, sizeof(g_lan_room.game_version), "%s",
           game_version());
  /* Online hosts bind 0.0.0.0, but that wildcard is not a routable guest
   * destination. Keep the bind; advertise a concrete local IPv4 in the LAN
   * registry row when present. */
  if (endpoint && strncmp(endpoint, "0.0.0.0:", 8) == 0) {
    RNetIpv4Address address;
    if (rnet_ipv4_enumerate(&address, 1) > 0 && address.address[0]) {
      snprintf(advertised, sizeof(advertised), "%s:%s", address.address,
               endpoint + 8);
      stored_endpoint = advertised;
    }
  }
  snprintf(g_lan_room.endpoint, sizeof(g_lan_room.endpoint), "%s",
           stored_endpoint && stored_endpoint[0] ? stored_endpoint
                                                 : "127.0.0.1:7777");
  snprintf(g_lan_room.host_name, sizeof(g_lan_room.host_name), "%s",
           snes_lobby_display_name()[0] ? snes_lobby_display_name() : "Host");
  snprintf(g_lan_room.password, sizeof(g_lan_room.password), "%s",
           password ? password : "");
  g_lan_room.host_slot = 0;
  g_lan_room.input_delay = clamp_input_delay(g_lobby_input_delay);
  if (!publish_lan_room())
    return 0;
  /* UDP waiting-room listen on the game port for remote Join Direct. */
  colon = strrchr(g_lan_room.endpoint, ':');
  port = colon ? colon + 1 : "7777";
  snprintf(bind_hp, sizeof(bind_hp), "0.0.0.0:%s", port);
  if (rnet_lan_direct_host_open(&g_direct_host, bind_hp, &g_lan_room) !=
      RNET_LAN_DIRECT_OK) {
    fprintf(stderr,
            "snes_host_lobby: Direct IP listen failed on %s — same-machine "
            "file join still works; remote Join Direct will time out\n",
            bind_hp);
  }
  g_hosting_lan = 1;
  g_joined_lan = 0;
  g_joined_direct = 0;
  g_direct_peer_endpoint[0] = '\0';
  g_lan_guest_rtt_ms = -1;
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  snprintf(g_resume_endpoint, sizeof(g_resume_endpoint), "%s",
           g_lan_room.endpoint);
  return 1;
}

static int fill_lan_row(RecompLauncherCNetplayLobby *out)
{
  RNetLanLobby state;
  if (!out)
    return 0;
  if (g_hosting_lan)
    state = g_lan_room;
  else if (!read_lan(&state))
    return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->lobby_id, sizeof(out->lobby_id), "lan:%s", state.endpoint);
  snprintf(out->name, sizeof(out->name), "LAN - %s",
           state.name[0] ? state.name : "Lobby");
  snprintf(out->game_name, sizeof(out->game_name), "%s", state.game);
  snprintf(out->game_version, sizeof(out->game_version), "%s",
           state.game_version);
  out->player_count = state.joiner_name[0] ? 2 : 1;
  out->max_slots = 2;
  out->has_password = state.password[0] != '\0';
  return 1;
}

static void clear_lan_joiner(void)
{
  if (g_joined_direct && g_direct_guest)
    (void)rnet_lan_direct_guest_leave(g_direct_guest);
  close_direct_sockets();
  g_joined_lan = 0;
  g_joined_direct = 0;
  g_direct_peer_endpoint[0] = '\0';
  g_lan_guest_rtt_ms = -1;
  memset(&g_lan_room, 0, sizeof(g_lan_room));
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
}

static void sync_lan_joiner(void)
{
  RNetLanLobby state;
  const char *name;
  int ev;
  if (!g_joined_lan)
    return;
  if (g_joined_direct) {
    int rtt = -1;
    ev = rnet_lan_direct_guest_pump(g_direct_guest, &g_lan_room, &rtt);
    if (ev == 2) /* KICK / CLOSE */
      clear_lan_joiner();
    else if (ev == 3 && rtt >= 0)
      g_lan_guest_rtt_ms = rtt;
    return;
  }
  if (!read_lan(&state)) {
    clear_lan_joiner();
    return;
  }
  name = snes_lobby_display_name();
  if (!state.joiner_name[0] ||
      (name && name[0] && strcmp(state.joiner_name, name) != 0))
    clear_lan_joiner();
}

static int use_lan_members(RNetLanLobby *state)
{
  RNetLanLobby local;
  if (!state)
    state = &local;
  sync_lan_joiner();
  if (g_hosting_lan || g_joined_direct) {
    *state = g_lan_room;
    return g_hosting_lan || g_joined_lan;
  }
  if (!read_lan(state))
    return 0;
  if (g_joined_lan)
    return 1;
  if (!g_hosting_lan)
    return 0;
  return state->joiner_name[0] || snes_lobby_member_count() < 2;
}

static void arm_lan_launch(const RNetLanLobby *state)
{
  const char *colon;
  const char *port;
  const char *peer;
  if (!state)
    return;
  /* Hand the UDP port to the game session socket. */
  if (g_hosting_lan)
    (void)rnet_lan_direct_host_notify_start(g_direct_host, state);
  close_direct_sockets();
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  g_lan_launch.enabled = 1;
  g_lan_launch.local_slot =
      g_hosting_lan ? state->host_slot : 1 - state->host_slot;
  g_lan_launch.input_player = 0;
  g_lan_launch.session_id = 1;
  g_lan_launch.input_delay =
      clamp_input_delay(state->input_delay >= 2 ? state->input_delay
                                                : g_lobby_input_delay);
  if (g_hosting_lan) {
    colon = strrchr(state->endpoint, ':');
    port = colon ? colon + 1 : "7777";
    snprintf(g_lan_launch.bind_hostport, sizeof(g_lan_launch.bind_hostport),
             "0.0.0.0:%s", port);
  } else {
    peer = g_direct_peer_endpoint[0] ? g_direct_peer_endpoint : state->endpoint;
    snprintf(g_lan_launch.bind_hostport, sizeof(g_lan_launch.bind_hostport),
             "0.0.0.0:0");
    snprintf(g_lan_launch.peer_hostport, sizeof(g_lan_launch.peer_hostport),
             "%s", peer);
  }
}

int snes_host_lobby_init(const SnesHostLobbyIdentity *id,
                         const SnesHostLobbyOpts *opts)
{
  if (!id || !id->game_name || !id->game_name[0])
    return -1;
  memset(&g_id, 0, sizeof(g_id));
  g_id = *id;
  memset(&g_opts, 0, sizeof(g_opts));
  if (opts)
    g_opts = *opts;
  g_hosting_lan = 0;
  g_joined_lan = 0;
  g_joined_direct = 0;
  close_direct_sockets();
  memset(&g_lan_room, 0, sizeof(g_lan_room));
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  g_direct_peer_endpoint[0] = '\0';
  g_lobby_url[0] = '\0';
  g_resume_endpoint[0] = '\0';
  g_runtime_error[0] = '\0';
  g_external_ip[0] = '\0';
  g_local_address_count = 0;
  g_inited = 1;
  return 0;
}

void snes_host_lobby_shutdown(void)
{
  snes_host_lobby_disconnect();
  g_inited = 0;
}

void snes_host_lobby_prepare_rematch(void)
{
  const char *colon;
  const char *port;
  char bind_hp[64];
  if (g_hosting_lan || g_joined_lan) {
    g_lan_room.started = 0;
    (void)rnet_lan_lobby_set_started(lan_path(), 0);
  }
  /* Re-bind Direct IP listen after the game session released the UDP port. */
  if (g_hosting_lan && !g_direct_host && g_lan_room.endpoint[0]) {
    colon = strrchr(g_lan_room.endpoint, ':');
    port = colon ? colon + 1 : "7777";
    snprintf(bind_hp, sizeof(bind_hp), "0.0.0.0:%s", port);
    (void)rnet_lan_direct_host_open(&g_direct_host, bind_hp, &g_lan_room);
  }
  if (g_opts.rematch_set_ready)
    snes_lobby_set_ready(1);
  else
    snes_lobby_set_ready(0);
  snes_lobby_clear_launch_pending();
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
}

int snes_host_lobby_leave(void)
{
  int rc;
  if (g_hosting_lan) {
    (void)rnet_lan_direct_host_notify_close(g_direct_host);
    close_direct_sockets();
    (void)rnet_lan_lobby_leave(lan_path(), 1);
  } else if (g_joined_direct) {
    if (g_direct_guest)
      (void)rnet_lan_direct_guest_leave(g_direct_guest);
    close_direct_sockets();
  } else if (g_joined_lan) {
    (void)rnet_lan_lobby_leave(lan_path(), 0);
  }
  g_hosting_lan = 0;
  g_joined_lan = 0;
  g_joined_direct = 0;
  g_direct_peer_endpoint[0] = '\0';
  memset(&g_lan_room, 0, sizeof(g_lan_room));
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  rc = snes_lobby_leave();
  return rc;
}

void snes_host_lobby_disconnect(void)
{
  (void)snes_host_lobby_leave();
  snes_lobby_disconnect();
}

const char *snes_host_lobby_resume_endpoint(void)
{
  if (g_resume_endpoint[0])
    return g_resume_endpoint;
  return "";
}

int snes_host_lobby_in_lan(void)
{
  return g_hosting_lan || g_joined_lan;
}

void snes_host_lobby_set_runtime_error(const char *error_code)
{
  snprintf(g_runtime_error, sizeof(g_runtime_error), "%s",
           error_code ? error_code : "");
}

static const char *cb_default_url(void *ctx)
{
  (void)ctx;
  return g_lobby_url[0] ? g_lobby_url : snes_lobby_default_url();
}

static void cb_set_url(void *ctx, const char *url)
{
  (void)ctx;
  snprintf(g_lobby_url, sizeof(g_lobby_url), "%s",
           url && url[0] ? url : snes_lobby_default_url());
}

static int cb_connect(void *ctx)
{
  (void)ctx;
  snes_lobby_set_game_identity(game_name(), game_version());
  return snes_lobby_connect(cb_default_url(NULL));
}

static int cb_connected(void *ctx)
{
  (void)ctx;
  return snes_lobby_connected();
}

static void cb_pump(void *ctx)
{
  (void)ctx;
  snes_lobby_pump();
  if (g_hosting_lan && g_direct_host) {
    int rtt = -1;
    if (rnet_lan_direct_host_pump(g_direct_host, &g_lan_room, &rtt))
      (void)publish_lan_room();
    if (rtt >= 0)
      g_lan_guest_rtt_ms = rtt;
    /* Host probes guest RTT about once per second. */
    {
      /* rnet_os_monotonic_ms is in the static lib; use a coarse clock here. */
      static unsigned s_tick;
      s_tick++;
      if ((s_tick % 60) == 0) /* ~1s at 60fps pump */
        (void)rnet_lan_direct_host_ping(g_direct_host);
    }
  }
  if (g_joined_direct && g_direct_guest) {
    static unsigned s_gtick;
    s_gtick++;
    if ((s_gtick % 60) == 0)
      (void)rnet_lan_direct_guest_ping(g_direct_guest);
  }
  sync_lan_joiner();
  if (g_opts.auto_ready_guests && snes_lobby_in_lobby() &&
      !snes_lobby_is_host() && !snes_lobby_local_ready())
    (void)snes_lobby_set_ready(1);
}

static void cb_set_player_name(void *ctx, const char *name)
{
  (void)ctx;
  snes_lobby_set_display_name(name && name[0] ? name : "Player");
  /* Persist immediately: the launcher may never reach PLAY (the player can
   * set a name, browse the lobby and quit), and a name that only survives a
   * successful launch still prompts on the next run. */
  if (name && name[0])
    (void)snes_netplay_identity_store(name);
}

static const char *cb_player_name(void *ctx)
{
  static char persisted[64];
  (void)ctx;
  {
    const char *live = snes_lobby_display_name();
    if (live && live[0])
      return live;
  }
  /* Nothing set this session: hand back what the last run stored, so the
   * launcher opens with the name already filled in. */
  if (snes_netplay_identity_load(persisted, sizeof(persisted)))
    return persisted;
  return snes_lobby_display_name();
}

static void cb_request_list(void *ctx)
{
  (void)ctx;
  snes_lobby_request_list();
}

static int cb_list_count(void *ctx)
{
  RecompLauncherCNetplayLobby lan;
  (void)ctx;
  return snes_lobby_list_count() + (fill_lan_row(&lan) ? 1 : 0);
}

static int cb_list_get(void *ctx, int index, RecompLauncherCNetplayLobby *out)
{
  SnesLobbyRow row;
  int remote_count;
  (void)ctx;
  if (!out || index < 0)
    return 0;
  remote_count = snes_lobby_list_count();
  if (index >= remote_count)
    return index == remote_count ? fill_lan_row(out) : 0;
  if (!snes_lobby_list_get(index, &row))
    return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->lobby_id, sizeof(out->lobby_id), "%s", row.lobby_id);
  snprintf(out->name, sizeof(out->name), "%s", row.name);
  snprintf(out->game_name, sizeof(out->game_name), "%s", row.game_name);
  snprintf(out->game_version, sizeof(out->game_version), "%s",
           row.game_version);
  out->player_count = row.player_count;
  out->max_slots = row.max_slots;
  out->has_password = row.has_password;
  return 1;
}

static int refresh_local_addresses(void)
{
  int count = rnet_ipv4_enumerate(g_local_addresses, kMaxLocalAddresses);
  if (count < 0)
    count = 0;
  if (count > kMaxLocalAddresses)
    count = kMaxLocalAddresses;
  g_local_address_count = count;
  return count;
}

static int cb_local_address_get(void *ctx, int index,
                                RecompLauncherCNetplayLocalAddress *out)
{
  (void)ctx;
  if (!out || index < 0)
    return 0;
  if (index == 0)
    refresh_local_addresses();
  if (index >= g_local_address_count)
    return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->address, sizeof(out->address), "%s",
           g_local_addresses[index].address);
  snprintf(out->label, sizeof(out->label), "%s",
           g_local_addresses[index].interface_label);
  return 1;
}

static int cb_local_ip(void *ctx, char *out, size_t out_len)
{
  RecompLauncherCNetplayLocalAddress address;
  if (!out || !out_len || !cb_local_address_get(ctx, 0, &address))
    return 0;
  snprintf(out, out_len, "%s", address.address);
  return out[0] != '\0';
}

static int cb_external_ip(void *ctx, char *out, size_t out_len)
{
  RNetExternalIpv4Config config;
  int rc;
  (void)ctx;
  if (!out || !out_len)
    return 0;
  if (!g_external_ip[0]) {
    rnet_external_ipv4_config_init(&config);
    config.timeout_ms = 900;
    rc = rnet_external_ipv4_discover(&config, g_external_ip,
                                     sizeof(g_external_ip));
    if (rc != RNET_EXTERNAL_IPV4_OK) {
      snprintf(out, out_len, "Unavailable");
      return 0;
    }
  }
  snprintf(out, out_len, "%s", g_external_ip);
  return out[0] != '\0';
}

static int cb_create(void *ctx, const char *lobby_name, char *host_endpoint,
                     const char *password,
                     const RecompLauncherCSettings *settings, int lan_only,
                     int max_slots)
{
  SnesLobbyMatchCaps caps = default_caps(settings);
  (void)ctx;
  g_lobby_max_slots = clamp_lobby_max_slots(max_slots);
  if (!host_endpoint)
    return -1;
  if (!host_endpoint[0])
    snprintf(host_endpoint, 64, lan_only ? "127.0.0.1:7777" : "0.0.0.0:7777");
  if (lan_only) {
    if (!create_lan(lobby_name, host_endpoint, password))
      return -1;
    return 0;
  }
  close_direct_sockets();
  (void)rnet_lan_lobby_leave(lan_path(), 1);
  g_hosting_lan = 0;
  g_joined_lan = 0;
  g_joined_direct = 0;
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  return snes_lobby_create(
      lobby_name && lobby_name[0]
          ? lobby_name
          : (g_id.default_lobby_name ? g_id.default_lobby_name : "Netplay Lobby"),
      game_name(), game_version(), password ? password : "", host_endpoint,
      &caps, g_lobby_max_slots);
}

static int map_direct_join_rc(int rc)
{
  if (rc == RNET_LAN_DIRECT_OK)
    return 0;
  if (rc == RNET_LAN_DIRECT_ERR_PASSWORD || rc == RNET_LAN_LOBBY_ERR_PASSWORD)
    return -2;
  if (rc == RNET_LAN_DIRECT_ERR_TIMEOUT || rc == RNET_LAN_DIRECT_ERR_IO ||
      rc == RNET_LAN_LOBBY_ERR_IO)
    return -3;
  /* full / identity / started / other */
  return -1;
}

static int cb_join(void *ctx, const char *lobby_id, const char *password,
                   char *guest_bind)
{
  RNetLanLobby state;
  const char *name;
  const char *endpoint;
  int rc;
  (void)ctx;
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  if (lobby_id && strncmp(lobby_id, "lan:", 4) == 0) {
    name = snes_lobby_display_name();
    endpoint = lobby_id + 4;
    close_direct_sockets();
    g_hosting_lan = 0;
    g_joined_lan = 0;
    g_joined_direct = 0;
    g_direct_peer_endpoint[0] = '\0';

    /* Same-machine fast path: shared file registry. */
    rc = rnet_lan_lobby_join(lan_path(), game_name(), game_version(),
                             password ? password : "",
                             name && name[0] ? name : "Player", &state);
    if (rc == RNET_LAN_LOBBY_OK) {
      g_lan_room = state;
      g_joined_lan = 1;
      snprintf(g_resume_endpoint, sizeof(g_resume_endpoint), "%s",
               state.endpoint);
      snprintf(g_direct_peer_endpoint, sizeof(g_direct_peer_endpoint), "%s",
               endpoint[0] ? endpoint : state.endpoint);
      return 0;
    }

    /* Remote / cross-subnet: UDP JOIN_REQ to the typed IP:port. */
    rc = rnet_lan_direct_guest_join(
        endpoint, game_name(), game_version(), password ? password : "",
        name && name[0] ? name : "Player", guest_bind, 2500, &state,
        &g_direct_guest);
    if (rc != RNET_LAN_DIRECT_OK)
      return map_direct_join_rc(rc);
    g_lan_room = state;
    g_joined_lan = 1;
    g_joined_direct = 1;
    snprintf(g_direct_peer_endpoint, sizeof(g_direct_peer_endpoint), "%s",
             endpoint);
    snprintf(g_resume_endpoint, sizeof(g_resume_endpoint), "%s", endpoint);
    return 0;
  }
  g_hosting_lan = 0;
  g_joined_lan = 0;
  g_joined_direct = 0;
  return snes_lobby_join(lobby_id, password ? password : "", guest_bind);
}

static int cb_leave(void *ctx)
{
  (void)ctx;
  return snes_host_lobby_leave();
}

static int cb_in_lobby(void *ctx)
{
  (void)ctx;
  sync_lan_joiner();
  return g_hosting_lan || g_joined_lan || snes_lobby_in_lobby();
}

static int cb_is_host(void *ctx)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return g_hosting_lan ? 1 : 0;
  return snes_lobby_is_host();
}

static int cb_member_count(void *ctx)
{
  RNetLanLobby state;
  (void)ctx;
  return use_lan_members(&state) ? 2 : snes_lobby_member_count();
}

static int cb_member_get(void *ctx, int index,
                         RecompLauncherCNetplayMember *out)
{
  SnesLobbyMember member;
  RNetLanLobby state;
  (void)ctx;
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  out->latency_ms = -1;
  if (use_lan_members(&state)) {
    if (index < 0 || index > 1)
      return 0;
    out->slot = index == 0 ? state.host_slot : 1 - state.host_slot;
    out->ready = index == 0 || state.joiner_name[0] != '\0';
    out->is_host = index == 0;
    snprintf(out->display_name, sizeof(out->display_name), "%s",
             index == 0 ? state.host_name : state.joiner_name);
    /* Host row: N/A. Guest row: Direct-IP / LAN UDP RTT when known. */
    if (!out->is_host && out->display_name[0] && g_lan_guest_rtt_ms >= 0)
      out->latency_ms = g_lan_guest_rtt_ms;
    return 1;
  }
  if (!snes_lobby_member_get(index, &member))
    return 0;
  out->slot = member.slot;
  out->ready = member.ready;
  out->is_host = snes_lobby_member_is_host(&member);
  snprintf(out->display_name, sizeof(out->display_name), "%s",
           member.display_name);
  out->latency_ms = snes_lobby_member_latency_ms(member.slot);
  return 1;
}

static int cb_move_member(void *ctx, int from_slot, int to_slot)
{
  (void)ctx;
  if (g_hosting_lan && from_slot >= 0 && from_slot <= 1 && to_slot >= 0 &&
      to_slot <= 1 && from_slot != to_slot) {
    g_lan_room.host_slot = 1 - g_lan_room.host_slot;
    g_lan_room.started = 0;
    (void)publish_lan_room();
    return 0;
  }
  if (g_joined_lan)
    return -1;
  return snes_lobby_move(from_slot, to_slot);
}

static int cb_kick_member(void *ctx, int slot)
{
  int guest_slot;
  (void)ctx;
  if (g_hosting_lan) {
    guest_slot = 1 - g_lan_room.host_slot;
    if (slot < 0 || slot > 1 || slot != guest_slot ||
        !g_lan_room.joiner_name[0])
      return -1;
    (void)rnet_lan_direct_host_notify_kick(g_direct_host);
    g_lan_room.joiner_name[0] = '\0';
    g_lan_room.started = 0;
    (void)publish_lan_room();
    return 0;
  }
  if (g_joined_lan)
    return -1;
  return snes_lobby_kick(slot);
}

static int cb_local_ready(void *ctx)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 1;
  return snes_lobby_local_ready();
}

static int cb_all_ready(void *ctx)
{
  RNetLanLobby state;
  (void)ctx;
  if (use_lan_members(&state))
    return state.joiner_name[0] != '\0';
  return snes_lobby_all_ready();
}

static int cb_set_ready(void *ctx, int ready)
{
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0;
  return snes_lobby_set_ready(ready);
}

static int cb_request_start(void *ctx, const RecompLauncherCSettings *settings)
{
  SnesLobbyMatchCaps caps = default_caps(settings);
  (void)ctx;
  if (g_hosting_lan) {
    if (!g_lan_room.joiner_name[0])
      return -1;
    g_lan_room.started = 1;
    g_lan_room.input_delay = clamp_input_delay(g_lobby_input_delay);
    (void)publish_lan_room();
    arm_lan_launch(&g_lan_room);
    return 0;
  }
  return snes_lobby_request_start(&caps);
}

static int cb_launch_pending(void *ctx)
{
  RNetLanLobby state;
  int ev;
  (void)ctx;
  if (g_joined_direct && !g_lan_launch.enabled) {
    int rtt = -1;
    ev = rnet_lan_direct_guest_pump(g_direct_guest, &g_lan_room, &rtt);
    if (ev == 3 && rtt >= 0)
      g_lan_guest_rtt_ms = rtt;
    if (ev == 1 || g_lan_room.started)
      arm_lan_launch(&g_lan_room);
  } else if (g_joined_lan && !g_joined_direct && !g_lan_launch.enabled &&
             read_lan(&state) && state.started) {
    g_lan_room = state;
    arm_lan_launch(&g_lan_room);
  } else if (g_hosting_lan && !g_lan_launch.enabled && g_lan_room.started) {
    arm_lan_launch(&g_lan_room);
  }
  return g_lan_launch.enabled || snes_lobby_launch_pending();
}

static void cb_clear_launch_pending(void *ctx)
{
  (void)ctx;
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  snes_lobby_clear_launch_pending();
}

static const char *cb_last_error(void *ctx)
{
  const SnesLobbyJoinInfo *join;
  (void)ctx;
  if (g_runtime_error[0])
    return g_runtime_error;
  join = snes_lobby_join_info();
  return (join && join->last_error[0]) ? join->last_error : "";
}

static void cb_clear_last_error(void *ctx)
{
  (void)ctx;
  g_runtime_error[0] = '\0';
  snes_lobby_clear_last_error();
}

static int cb_input_delay_get(void *ctx)
{
  const SnesLobbyMatchCaps *caps;
  RNetLanLobby state;
  (void)ctx;
  if (g_hosting_lan || g_joined_lan) {
    if (use_lan_members(&state) && state.input_delay >= 2)
      return clamp_input_delay(state.input_delay);
    return clamp_input_delay(g_lobby_input_delay);
  }
  caps = snes_lobby_match_caps();
  if (caps && caps->valid)
    return clamp_input_delay(caps->input_delay);
  return clamp_input_delay(g_lobby_input_delay);
}

static int cb_input_delay_set(void *ctx, int delay_frames)
{
  SnesLobbyMatchCaps caps;
  (void)ctx;
  g_lobby_input_delay = clamp_input_delay(delay_frames);
  if (g_hosting_lan) {
    g_lan_room.input_delay = g_lobby_input_delay;
    (void)publish_lan_room();
    if (g_direct_host && g_lan_room.joiner_name[0])
      /* notify_caps takes the room, not the delay. The delay it should carry
       * was already written into g_lan_room two lines up, and passing the int
       * here made the guest read an integer as a room pointer. Only a build
       * with recomp-ui compiles this branch, which is why it survived: GCC 14
       * turned int-conversion into an error and surfaced it. */
      (void)rnet_lan_direct_host_notify_caps(g_direct_host, &g_lan_room);
    return 0;
  }
  if (g_joined_lan)
    return 0;
  if (!snes_lobby_is_host())
    return -1;
  caps = default_caps(NULL);
  caps.input_delay = g_lobby_input_delay;
  return snes_lobby_set_match_caps(&caps);
}

static int cb_force_input_relay_get(void *ctx)
{
  const SnesLobbyMatchCaps *caps;
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0;
  caps = snes_lobby_match_caps();
  if (caps && caps->valid)
    return caps->force_input_relay ? 1 : 0;
  return g_lobby_force_input_relay ? 1 : 0;
}

static int cb_force_input_relay_set(void *ctx, int force)
{
  SnesLobbyMatchCaps caps;
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0; /* LAN/Direct IP does not use server input relay */
  if (!snes_lobby_is_host())
    return -1;
  g_lobby_force_input_relay = force ? 1 : 0;
  caps = default_caps(NULL);
  caps.force_input_relay = g_lobby_force_input_relay ? 1 : 0;
  return snes_lobby_set_match_caps(&caps);
}

static int cb_force_turn_get(void *ctx)
{
  const SnesLobbyMatchCaps *caps;
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0;
  caps = snes_lobby_match_caps();
  if (caps && caps->valid)
    return caps->force_turn ? 1 : 0;
  return g_lobby_force_turn ? 1 : 0;
}

static int cb_force_turn_set(void *ctx, int force)
{
  SnesLobbyMatchCaps caps;
  (void)ctx;
  if (g_hosting_lan || g_joined_lan)
    return 0; /* LAN/Direct IP does not use ICE TURN */
  if (!snes_lobby_is_host())
    return -1;
  g_lobby_force_turn = force ? 1 : 0;
  caps = default_caps(NULL);
  caps.force_turn = g_lobby_force_turn ? 1 : 0;
  return snes_lobby_set_match_caps(&caps);
}

static int cb_lobby_max_slots(void *ctx)
{
  const SnesLobbyJoinInfo *join;
  RNetLanLobby state;
  (void)ctx;
  /* SNES LAN / Direct IP rooms are always 2-player. */
  if (use_lan_members(&state) || g_hosting_lan || g_joined_lan || g_joined_direct)
    return 2;
  if (!snes_lobby_in_lobby())
    return 0;
  join = snes_lobby_join_info();
  if (join && join->ok && join->max_slots >= 2)
    return clamp_lobby_max_slots(join->max_slots);
  return clamp_lobby_max_slots(g_lobby_max_slots);
}

static int cb_fill_launch(void *ctx, RecompLauncherCNetplayLaunch *out)
{
  SnesLobbyJoinInfo join;
  const SnesLobbyMatchCaps *caps;
  (void)ctx;
  if (!out)
    return 0;
  if (g_lan_launch.enabled) {
    *out = g_lan_launch;
    out->force_input_relay = 0;
    out->max_slots = 2;
    out->player_count = 2;
    return 1;
  }
  if (!snes_lobby_try_fill_launch(&join))
    return 0;
  caps = snes_lobby_match_caps();
  if (!caps || !caps->valid)
    return 0;
  memset(out, 0, sizeof(*out));
  out->enabled = 1;
  out->local_slot = join.local_slot;
  out->input_player = 0;
  out->session_id = join.session_id;
  out->input_delay = clamp_input_delay(caps->input_delay);
  out->force_input_relay = caps->force_input_relay ? 1 : 0;
  out->max_slots = join.max_slots >= 2 ? clamp_lobby_max_slots(join.max_slots)
                                       : clamp_lobby_max_slots(g_lobby_max_slots);
  out->player_count = join.player_count > 0 ? join.player_count : out->max_slots;
  snprintf(out->bind_hostport, sizeof(out->bind_hostport), "%s",
           join.bind_hostport);
  snprintf(out->peer_hostport, sizeof(out->peer_hostport), "%s",
           join.peer_hostport);
  return 1;
}

static RecompLauncherCNetplayCallbacks g_callbacks = {
    NULL,
    cb_default_url,
    cb_set_url,
    cb_connect,
    cb_connected,
    cb_pump,
    cb_set_player_name,
    cb_player_name,
    cb_request_list,
    cb_list_count,
    cb_list_get,
    cb_local_ip,
    cb_external_ip,
    cb_create,
    cb_join,
    cb_leave,
    cb_in_lobby,
    cb_is_host,
    cb_member_count,
    cb_member_get,
    cb_move_member,
    cb_local_ready,
    cb_all_ready,
    cb_set_ready,
    cb_request_start,
    cb_launch_pending,
    cb_clear_launch_pending,
    cb_fill_launch,
    cb_local_address_get,
    cb_kick_member,
    cb_last_error,
    cb_clear_last_error,
    cb_input_delay_get,
    cb_input_delay_set,
    cb_force_input_relay_get,
    cb_force_input_relay_set,
    cb_lobby_max_slots,
    cb_force_turn_get,
    cb_force_turn_set,
};

const RecompLauncherCNetplayCallbacks *snes_host_lobby_callbacks(void)
{
  return g_inited ? &g_callbacks : NULL;
}

static uint64_t auto_now_ms(void)
{
#ifdef _WIN32
  return (uint64_t)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static void auto_sleep_ms(unsigned ms)
{
#ifdef _WIN32
  Sleep(ms);
#else
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000u);
  ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
  nanosleep(&ts, NULL);
#endif
}

static void auto_log(const char *role, const char *stage)
{
  const RecompLauncherCNetplayCallbacks *cb = snes_host_lobby_callbacks();
  const char *err = (cb && cb->last_error) ? cb->last_error(NULL) : "";
  fprintf(stderr,
          "[netplay selftest] role=%s stage=%s members=%d ready=%d "
          "in_lobby=%d error=%s\n",
          role, stage, snes_lobby_member_count(), snes_lobby_all_ready(),
          snes_lobby_in_lobby(), err ? err : "");
}

int snes_host_lobby_auto_launch(const char *role, const char *player_name,
                                const char *lobby_name, unsigned timeout_ms,
                                RecompLauncherCNetplayLaunch *out)
{
  const RecompLauncherCNetplayCallbacks *cb;
  RecompLauncherCSettings settings;
  RecompLauncherCNetplayLobby row;
  uint64_t deadline;
  uint64_t next_list = 0;
  int is_host;
  int joined = 0;
  int host_ready_sent = 0;
  int start_sent = 0;
  int i;

  cb = snes_host_lobby_callbacks();
  if (!cb || !g_inited)
    return -1;
  if (!role || !lobby_name || !lobby_name[0] || !out)
    return -1;
  is_host = strcmp(role, "host") == 0;
  if (!is_host && strcmp(role, "guest") != 0)
    return -1;
  if (timeout_ms < 1000u)
    timeout_ms = 60000u;
  deadline = auto_now_ms() + timeout_ms;
  memset(&settings, 0, sizeof(settings));
  memset(out, 0, sizeof(*out));
  cb->set_player_name(NULL, player_name && player_name[0]
                                ? player_name
                                : (is_host ? "HostTest" : "GuestTest"));
  if (cb->connect(NULL) != 0) {
    auto_log(role, "connect_failed");
    return -2;
  }

  while (!snes_lobby_player_id()[0]) {
    cb->pump(NULL);
    if (!cb->connected(NULL)) {
      auto_log(role, "handshake_disconnected");
      return -3;
    }
    if (auto_now_ms() >= deadline) {
      auto_log(role, "welcome_timeout");
      return -4;
    }
    auto_sleep_ms(10);
  }
  auto_log(role, "connected");

  if (is_host) {
    char endpoint[64] = "0.0.0.0:7777";
    if (cb->create(NULL, lobby_name, endpoint, "", &settings, 0, 2) != 0) {
      auto_log(role, "create_failed");
      return -5;
    }
  }

  while (!cb->launch_pending(NULL)) {
    cb->pump(NULL);
    if (!cb->connected(NULL)) {
      auto_log(role, "lobby_disconnected");
      return -6;
    }

    if (!is_host && !joined && auto_now_ms() >= next_list) {
      cb->request_list(NULL);
      next_list = auto_now_ms() + 500u;
    }
    if (!is_host && !joined) {
      for (i = 0; i < cb->list_count(NULL); ++i) {
        if (cb->list_get(NULL, i, &row) && strcmp(row.name, lobby_name) == 0 &&
            strcmp(row.game_name, game_name()) == 0) {
          char guest_bind[64];
          guest_bind[0] = '\0';
          if (cb->join(NULL, row.lobby_id, "", guest_bind) != 0) {
            auto_log(role, "join_failed");
            return -7;
          }
          joined = 1;
          auto_log(role, "join_sent");
          break;
        }
      }
    }

    if (is_host && cb->in_lobby(NULL) && cb->member_count(NULL) >= 2 &&
        !host_ready_sent) {
      if (cb->set_ready(NULL, 1) != 0) {
        auto_log(role, "ready_failed");
        return -8;
      }
      host_ready_sent = 1;
      auto_log(role, "ready_sent");
    }
    if (is_host && host_ready_sent && cb->all_ready(NULL) && !start_sent) {
      if (cb->request_start(NULL, &settings) != 0) {
        auto_log(role, "start_failed");
        return -9;
      }
      start_sent = 1;
      auto_log(role, "start_sent");
    }

    if (auto_now_ms() >= deadline) {
      auto_log(role, "launch_timeout");
      return -10;
    }
    auto_sleep_ms(10);
  }
  if (!cb->fill_launch(NULL, out)) {
    auto_log(role, "invalid_launch");
    return -11;
  }
  fprintf(stderr,
          "[netplay selftest] role=%s stage=launch slot=%d session=%u "
          "bind=%s peer=%s\n",
          role, out->local_slot, (unsigned)out->session_id, out->bind_hostport,
          out->peer_hostport);
  return 0;
}

#endif /* RECOMP_LAUNCHER || SNES_HOST_HAS_RECOMP_UI */

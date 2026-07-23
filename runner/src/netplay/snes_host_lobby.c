#include "snes_host_lobby.h"

#include <stdio.h>
#include <string.h>

#include "recomp_net/lan_lobby.h"
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

#else

enum { kMaxLocalAddresses = 16 };

static SnesHostLobbyIdentity g_id;
static SnesHostLobbyOpts g_opts;
static int g_inited;
static int g_hosting_lan;
static int g_joined_lan;
static RecompLauncherCNetplayLaunch g_lan_launch;
static char g_lobby_url[256];
static char g_resume_endpoint[64];
static RNetIpv4Address g_local_addresses[kMaxLocalAddresses];
static int g_local_address_count;
static char g_external_ip[RNET_IPV4_ADDRESS_TEXT_MAX];

static const char *lan_path(void)
{
  return g_id.lan_registry_path && g_id.lan_registry_path[0]
             ? g_id.lan_registry_path
             : "netplay_lan_lobby.txt";
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
  caps.input_delay = 2;
  if (settings) {
    caps.widescreen = settings->widescreen != 0;
    caps.widescreen_hud = settings->widescreen_hud != 0;
    caps.ignore_aspect = settings->ignore_aspect != 0;
  }
  if (g_opts.fill_match_caps)
    g_opts.fill_match_caps(g_opts.caps_ctx, settings, &caps);
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
  RNetLanLobby state;
  memset(&state, 0, sizeof(state));
  snprintf(state.name, sizeof(state.name), "%s",
           name && name[0]
               ? name
               : (g_id.default_lobby_name ? g_id.default_lobby_name
                                          : "LAN Lobby"));
  snprintf(state.game, sizeof(state.game), "%s", game_name());
  snprintf(state.game_version, sizeof(state.game_version), "%s", game_version());
  snprintf(state.endpoint, sizeof(state.endpoint), "%s",
           endpoint && endpoint[0] ? endpoint : "127.0.0.1:7777");
  snprintf(state.host_name, sizeof(state.host_name), "%s",
           snes_lobby_display_name()[0] ? snes_lobby_display_name() : "Host");
  snprintf(state.password, sizeof(state.password), "%s",
           password ? password : "");
  state.host_slot = 0;
  if (rnet_lan_lobby_publish(lan_path(), &state) != RNET_LAN_LOBBY_OK)
    return 0;
  g_hosting_lan = 1;
  g_joined_lan = 0;
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  snprintf(g_resume_endpoint, sizeof(g_resume_endpoint), "%s", state.endpoint);
  return 1;
}

static int fill_lan_row(RecompLauncherCNetplayLobby *out)
{
  RNetLanLobby state;
  if (!out || !read_lan(&state))
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
  g_joined_lan = 0;
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
}

static void sync_lan_joiner(void)
{
  RNetLanLobby state;
  const char *name;
  if (!g_joined_lan)
    return;
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
  if (!state)
    return;
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  g_lan_launch.enabled = 1;
  g_lan_launch.local_slot =
      g_hosting_lan ? state->host_slot : 1 - state->host_slot;
  g_lan_launch.input_player = 0;
  g_lan_launch.session_id = 1;
  g_lan_launch.input_delay = 2;
  if (g_hosting_lan) {
    colon = strrchr(state->endpoint, ':');
    port = colon ? colon + 1 : "7777";
    snprintf(g_lan_launch.bind_hostport, sizeof(g_lan_launch.bind_hostport),
             "0.0.0.0:%s", port);
  } else {
    snprintf(g_lan_launch.bind_hostport, sizeof(g_lan_launch.bind_hostport),
             "0.0.0.0:0");
    snprintf(g_lan_launch.peer_hostport, sizeof(g_lan_launch.peer_hostport),
             "%s", state->endpoint);
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
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  g_lobby_url[0] = '\0';
  g_resume_endpoint[0] = '\0';
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
  if (g_hosting_lan || g_joined_lan)
    (void)rnet_lan_lobby_set_started(lan_path(), 0);
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
  if (g_hosting_lan)
    (void)rnet_lan_lobby_leave(lan_path(), 1);
  else if (g_joined_lan)
    (void)rnet_lan_lobby_leave(lan_path(), 0);
  g_hosting_lan = 0;
  g_joined_lan = 0;
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
  sync_lan_joiner();
  if (g_opts.auto_ready_guests && snes_lobby_in_lobby() &&
      !snes_lobby_is_host() && !snes_lobby_local_ready())
    (void)snes_lobby_set_ready(1);
}

static void cb_set_player_name(void *ctx, const char *name)
{
  (void)ctx;
  snes_lobby_set_display_name(name && name[0] ? name : "Player");
}

static const char *cb_player_name(void *ctx)
{
  (void)ctx;
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
                     const RecompLauncherCSettings *settings, int lan_only)
{
  SnesLobbyMatchCaps caps = default_caps(settings);
  (void)ctx;
  if (!host_endpoint)
    return -1;
  if (!host_endpoint[0])
    snprintf(host_endpoint, 64, lan_only ? "127.0.0.1:7777" : "0.0.0.0:7777");
  if (lan_only) {
    if (!create_lan(lobby_name, host_endpoint, password))
      return -1;
    return 0;
  }
  (void)rnet_lan_lobby_leave(lan_path(), 1);
  g_hosting_lan = 0;
  g_joined_lan = 0;
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  return snes_lobby_create(
      lobby_name && lobby_name[0]
          ? lobby_name
          : (g_id.default_lobby_name ? g_id.default_lobby_name : "Netplay Lobby"),
      game_name(), game_version(), password ? password : "", host_endpoint,
      &caps);
}

static int cb_join(void *ctx, const char *lobby_id, const char *password,
                   char *guest_bind)
{
  RNetLanLobby state;
  const char *name;
  (void)ctx;
  memset(&g_lan_launch, 0, sizeof(g_lan_launch));
  if (lobby_id && strncmp(lobby_id, "lan:", 4) == 0) {
    (void)guest_bind;
    name = snes_lobby_display_name();
    if (rnet_lan_lobby_join(lan_path(), game_name(), game_version(),
                            password ? password : "",
                            name && name[0] ? name : "Player",
                            &state) != RNET_LAN_LOBBY_OK)
      return -1;
    g_hosting_lan = 0;
    g_joined_lan = 1;
    snprintf(g_resume_endpoint, sizeof(g_resume_endpoint), "%s",
             state.endpoint);
    return 0;
  }
  g_hosting_lan = 0;
  g_joined_lan = 0;
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
  if (use_lan_members(&state)) {
    if (index < 0 || index > 1)
      return 0;
    out->slot = index == 0 ? state.host_slot : 1 - state.host_slot;
    out->ready = index == 0 || state.joiner_name[0] != '\0';
    out->is_host = index == 0;
    snprintf(out->display_name, sizeof(out->display_name), "%s",
             index == 0 ? state.host_name : state.joiner_name);
    return 1;
  }
  if (!snes_lobby_member_get(index, &member))
    return 0;
  out->slot = member.slot;
  out->ready = member.ready;
  out->is_host = snes_lobby_member_is_host(&member);
  snprintf(out->display_name, sizeof(out->display_name), "%s",
           member.display_name);
  return 1;
}

static int cb_move_member(void *ctx, int from_slot, int to_slot)
{
  RNetLanLobby state;
  (void)ctx;
  if (g_hosting_lan && from_slot >= 0 && from_slot <= 1 && to_slot >= 0 &&
      to_slot <= 1 && from_slot != to_slot && read_lan(&state))
    return rnet_lan_lobby_set_host_slot(lan_path(), 1 - state.host_slot);
  if (g_joined_lan)
    return -1;
  return snes_lobby_move(from_slot, to_slot);
}

static int cb_kick_member(void *ctx, int slot)
{
  RNetLanLobby state;
  int guest_slot;
  (void)ctx;
  if (g_hosting_lan) {
    if (slot < 0 || slot > 1 || !read_lan(&state))
      return -1;
    guest_slot = 1 - state.host_slot;
    if (slot != guest_slot || !state.joiner_name[0])
      return -1;
    return rnet_lan_lobby_kick(lan_path()) == RNET_LAN_LOBBY_OK ? 0 : -1;
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
  RNetLanLobby state;
  (void)ctx;
  if (g_hosting_lan) {
    if (!read_lan(&state) || !state.joiner_name[0])
      return -1;
    if (rnet_lan_lobby_set_started(lan_path(), 1) != RNET_LAN_LOBBY_OK)
      return -1;
    state.started = 1;
    arm_lan_launch(&state);
    return 0;
  }
  return snes_lobby_request_start(&caps);
}

static int cb_launch_pending(void *ctx)
{
  RNetLanLobby state;
  (void)ctx;
  if ((g_hosting_lan || g_joined_lan) && !g_lan_launch.enabled &&
      read_lan(&state) && state.started)
    arm_lan_launch(&state);
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
  join = snes_lobby_join_info();
  return (join && join->last_error[0]) ? join->last_error : "";
}

static void cb_clear_last_error(void *ctx)
{
  (void)ctx;
  snes_lobby_clear_last_error();
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
    return 1;
  }
  if (!snes_lobby_try_fill_launch(&join))
    return 0;
  caps = snes_lobby_match_caps();
  memset(out, 0, sizeof(*out));
  out->enabled = 1;
  out->local_slot = join.local_slot;
  out->input_player = 0;
  out->session_id = join.session_id;
  out->input_delay = caps && caps->valid ? caps->input_delay : 2;
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
};

const RecompLauncherCNetplayCallbacks *snes_host_lobby_callbacks(void)
{
  return g_inited ? &g_callbacks : NULL;
}

#endif /* RECOMP_LAUNCHER || SNES_HOST_HAS_RECOMP_UI */

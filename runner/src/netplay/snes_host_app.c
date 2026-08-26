#include "snes_host_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desktop/sdl_compat.h"

#if defined(RECOMP_LAUNCHER) || defined(SNES_HOST_HAS_RECOMP_UI)

void snes_host_app_apply_launch(const RecompLauncherCNetplayLaunch *net,
                                SnesHostLaunchResult *out)
{
  if (!out)
    return;
  memset(out, 0, sizeof(*out));
  out->caps_ws_extra = -1;
  if (!net || !net->enabled)
    return;
  out->netplay_enabled = 1;
  out->from_lobby = 1;
  snes_netplay_config_defaults(&out->net_cfg);
  out->net_cfg.enabled = 1;
  out->net_cfg.local_slot = net->local_slot;
  out->net_cfg.input_player =
      (net->input_player == 0 || net->input_player == 1) ? net->input_player
                                                         : -1;
  out->net_cfg.session_id = net->session_id ? net->session_id : 1u;
  out->net_cfg.transport = 0;
  snprintf(out->net_cfg.bind_hostport, sizeof(out->net_cfg.bind_hostport), "%s",
           net->bind_hostport);
  snprintf(out->net_cfg.peer_hostport, sizeof(out->net_cfg.peer_hostport), "%s",
           net->peer_hostport);
  snes_netplay_apply_env(&out->net_cfg);
  if (net->input_delay >= 0 && net->input_delay <= 20)
    out->net_cfg.input_delay = net->input_delay;
  out->net_cfg.force_turn = 0;
  out->net_cfg.force_input_relay = net->force_input_relay ? 1 : 0;
  {
    const SnesLobbyMatchCaps *caps = snes_lobby_match_caps();
    if (caps && caps->valid) {
      out->caps_ws_extra = caps->ws_extra;
      if (caps->force_turn)
        out->net_cfg.force_turn = 1;
    }
  }
}

void snes_host_app_begin_soft_return(RecompLauncherCGameInfo *gi,
                                     int set_resume_room)
{
  snes_host_lobby_prepare_rematch();
  snes_netplay_clear_return_to_lobby();
  if (!gi || !set_resume_room)
    return;
  gi->resume_netplay_room = 1;
  {
    const char *ep = snes_host_lobby_resume_endpoint();
    if (ep && ep[0])
      gi->resume_netplay_endpoint = ep;
  }
}

#endif /* RECOMP_LAUNCHER */

const char *snes_host_connect_timeout_error_code(int is_ice)
{
  return is_ice ? "connect_timeout_ice" : "connect_timeout_lan";
}

const char *snes_host_connect_timeout_message(int is_ice)
{
  return is_ice
             ? "Could not establish an online connection to the other "
               "player within 30 seconds.\n\nAllow the game through the "
               "Windows firewall, make sure both players are still in the "
               "lobby, then rejoin and retry."
             : "Could not establish a direct connection to the other "
               "player within 30 seconds.\n\nCheck the lobby address, "
               "firewall, and that both players are still connected, then "
               "rejoin and retry.";
}

static void barrier_soft_exit(int from_lobby, int *running, const char *origin,
                              int *desync_logged, int *wait_logged);

#define SNES_STARVATION_ENTER_DEFAULT 4
#define SNES_STARVATION_EXIT_DEFAULT 3
#define SNES_STARVATION_EXIT_HR_LEAD_DEFAULT 0
#define SNES_STARVATION_GRACE_TICKS 60
/* Default 0: after starvation clears, resume ~1 sim/wall frame and let
 * remote_lead rebuild toward D instead of a turbo recovery burst. */
#define SNES_STARVATION_RECOVERY_BURST_DEFAULT 0
#define SNES_CATCHUP_CAP_DEFAULT 0

static struct {
  int latched;
  int enter_run;
  int exit_run;
  int recovery_amount;
  int pending_consume;
  int latch_logged;
  int just_cleared;
} g_starv;

static int starv_env_int(const char *name, int def)
{
  const char *v = getenv(name);
  long n;
  char *end;

  if (!v || !v[0])
    return def;
  n = strtol(v, &end, 10);
  if (end == v || *end != '\0' || n < 0 || n > 64)
    return def;
  return (int)n;
}

static void starv_reset(void)
{
  memset(&g_starv, 0, sizeof(g_starv));
}

static int starv_runway_ok(void)
{
  int lead = snes_netplay_remote_lead();
  int delay = snes_netplay_input_delay();
  int hr_lead = starv_env_int("SNES_NET_STARVATION_EXIT_HR_LEAD",
                              SNES_STARVATION_EXIT_HR_LEAD_DEFAULT);

  if (delay < 0)
    delay = 0;
  return lead >= delay + hr_lead;
}

static void barrier_soft_exit(int from_lobby, int *running, const char *origin,
                              int *desync_logged, int *wait_logged)
{
  starv_reset();
  snes_netplay_soft_exit_to_lobby(origin, from_lobby);
  snes_netplay_connect_wait_reset();
  if (desync_logged)
    *desync_logged = 0;
  if (wait_logged)
    *wait_logged = 0;
  if (running)
    *running = 0;
}

static int barrier_poll_admit(int enter_need)
{
  if (snes_netplay_poll_admit()) {
    g_starv.enter_run = 0;
    if (g_starv.just_cleared) {
      int burst = starv_env_int("SNES_NET_STARVATION_RECOVERY_BURST",
                                SNES_STARVATION_RECOVERY_BURST_DEFAULT);
      g_starv.just_cleared = 0;
      g_starv.recovery_amount = burst;
      g_starv.pending_consume = burst > 0 ? 1 : 0;
      if (burst > 0) {
        fprintf(stderr,
                "snes_netplay: delay_sync_starvation cleared sim=%u lead=%d "
                "D=%d — recovery burst %d\n",
                (unsigned)snes_netplay_sim_tick(), snes_netplay_remote_lead(),
                snes_netplay_input_delay(), burst);
      } else {
        fprintf(stderr,
                "snes_netplay: delay_sync_starvation cleared sim=%u lead=%d "
                "D=%d — resume 1:1 (rebuild input buffer)\n",
                (unsigned)snes_netplay_sim_tick(), snes_netplay_remote_lead(),
                snes_netplay_input_delay());
      }
    }
    return 1;
  }

  g_starv.just_cleared = 0;
  g_starv.enter_run++;
  if (g_starv.enter_run >= enter_need) {
    g_starv.latched = 1;
    g_starv.enter_run = 0;
    if (!g_starv.latch_logged) {
      fprintf(stderr,
              "snes_netplay: delay_sync_starvation latched sim=%u lead=%d D=%d "
              "(enter=%d)\n",
              (unsigned)snes_netplay_sim_tick(), snes_netplay_remote_lead(),
              snes_netplay_input_delay(), enter_need);
      g_starv.latch_logged = 1;
    }
  }
  return 0;
}

int snes_host_barrier_admit(int from_lobby, int *running,
                            const SnesHostBarrierHooks *hooks)
{
  static int desync_logged;
  static int wait_logged;
  static int ice_fail_logged;
  uint32_t peer_ms;
  uint32_t connect_ms;
  uint32_t dt = 0, lh = 0, rh = 0;
  int want_soft = 0;
  uint16_t pad;
  const char *soft_origin;

  if (!snes_netplay_active()) {
    starv_reset();
    return 0;
  }
  if (!hooks || !hooks->capture_local_pad)
    return 0;

  peer_ms = hooks->peer_timeout_ms ? hooks->peer_timeout_ms : 1500u;
  connect_ms = hooks->connect_timeout_ms;

  if (snes_netplay_peer_disconnected(peer_ms)) {
    barrier_soft_exit(from_lobby, running, "peer_disconnect", &desync_logged,
                      &wait_logged);
    ice_fail_logged = 0;
    return 0;
  }

  if (connect_ms && !snes_netplay_is_running()) {
    if (!wait_logged) {
      fprintf(stderr,
              "snes_netplay: waiting for peer transport=%s timeout=%ums\n",
              snes_netplay_transport_name(), (unsigned)connect_ms);
      wait_logged = 1;
    }
    if (snes_netplay_ice_failed() && !ice_fail_logged) {
      fprintf(stderr,
              "snes_netplay: ICE FAILED while waiting for peer — "
              "STUN/TURN path unusable (check Coturn / firewall)\n");
      ice_fail_logged = 1;
    }
    if (snes_netplay_connect_timed_out(connect_ms)) {
      if (hooks->on_connect_timeout)
        hooks->on_connect_timeout(hooks->ctx);
      barrier_soft_exit(from_lobby, running, "connect_timeout",
                        &desync_logged, &wait_logged);
      ice_fail_logged = 0;
      return 0;
    }
  } else {
    wait_logged = 0;
    ice_fail_logged = 0;
  }

  if (snes_netplay_input_desync(&dt, &lh, &rh)) {
    if (!desync_logged) {
      fprintf(stderr,
              "snes_netplay: INPUT desync tick=%u local=%08x remote=%08x — "
              "stalled\n",
              (unsigned)dt, (unsigned)lh, (unsigned)rh);
      desync_logged = 1;
    }
    if (hooks->poll_events)
      hooks->poll_events(hooks->ctx, &want_soft);
    if (want_soft) {
      soft_origin = (want_soft == 2) ? "sdl_quit" : "escape";
      barrier_soft_exit(from_lobby, running, soft_origin, &desync_logged,
                        &wait_logged);
      return 0;
    }
    return 0; /* skip sim; host presents held frame */
  }
  desync_logged = 0;

  pad = hooks->capture_local_pad(hooks->ctx);
  snes_netplay_stage_local(pad);

  if (hooks->poll_events)
    hooks->poll_events(hooks->ctx, &want_soft);
  if (want_soft) {
    soft_origin = (want_soft == 2) ? "sdl_quit" : "escape";
    barrier_soft_exit(from_lobby, running, soft_origin, &desync_logged,
                      &wait_logged);
    return 0;
  }

  if (g_starv.pending_consume) {
    g_starv.recovery_amount = 0;
    g_starv.pending_consume = 0;
  }

  /*
   * Rollback owns its own pacing. The starvation latch below is delay-sync
   * policy: it stalls when remote_lead drops under D, which under rollback is
   * the ordinary running state rather than a fault — inventing past missing
   * remote input is the mechanism, and retcomm-rbengine's scheduler already
   * decides when to wait instead. Leaving the latch armed would throttle
   * rollback straight back into lockstep.
   */
  if (snes_netplay_rollback_active())
    return snes_netplay_poll_admit();

  {
    uint32_t sim = snes_netplay_sim_tick();
    int enter_need = starv_env_int("SNES_NET_STARVATION_ENTER_FRAMES",
                                   SNES_STARVATION_ENTER_DEFAULT);
    int exit_need = starv_env_int("SNES_NET_STARVATION_EXIT_FRAMES",
                                  SNES_STARVATION_EXIT_DEFAULT);

    if (sim < SNES_STARVATION_GRACE_TICKS) {
      if (snes_netplay_poll_admit())
        return 1;
      return 0;
    }

    if (g_starv.latched) {
      snes_netplay_pump();
      if (starv_runway_ok()) {
        g_starv.exit_run++;
        if (g_starv.exit_run >= exit_need) {
          g_starv.latched = 0;
          g_starv.exit_run = 0;
          g_starv.latch_logged = 0;
          g_starv.just_cleared = 1;
        } else {
          return 0;
        }
      } else {
        g_starv.exit_run = 0;
        return 0;
      }
    }

    if (barrier_poll_admit(enter_need))
      return 1;
  }
  return 0; /* input starvation / confirm stall — present held, retry next wall tick */
}

int snes_host_catchup_budget(void)
{
  int lead;
  int delay;
  int extra;
  int budget;
  int cap;

  if (!snes_netplay_active())
    return 0;
  /* No delay-sync catch-up burst under rollback: the scheduler paces admit,
   * and a burst here would race it. */
  if (snes_netplay_rollback_active())
    return 0;
  cap = starv_env_int("SNES_NET_CATCHUP_CAP", SNES_CATCHUP_CAP_DEFAULT);
  if (cap <= 0 && g_starv.recovery_amount <= 0)
    return 0;
  lead = snes_netplay_remote_lead();
  delay = snes_netplay_input_delay();
  if (delay < 0)
    delay = 0;
  /* Only spend surplus above D; keep the delay runway intact. */
  extra = lead - delay;
  if (extra < 0)
    extra = 0;
  budget = extra;
  if (g_starv.recovery_amount > budget)
    budget = g_starv.recovery_amount;
  if (budget > cap)
    budget = cap;
  return budget;
}

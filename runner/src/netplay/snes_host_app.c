#include "snes_host_app.h"

#include <stdio.h>
#include <string.h>

#include <SDL.h>

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
  if (net->input_delay >= 0 && net->input_delay <= 16)
    out->net_cfg.input_delay = net->input_delay;
  {
    const SnesLobbyMatchCaps *caps = snes_lobby_match_caps();
    if (caps && caps->valid)
      out->caps_ws_extra = caps->ws_extra;
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

int snes_host_barrier_admit(int from_lobby, int *running,
                            const SnesHostBarrierHooks *hooks)
{
  static int desync_logged;
  static uint32_t connect_wait_started_ms;
  uint32_t peer_ms;
  uint32_t connect_ms;

  if (!snes_netplay_active())
    return 0;
  if (!hooks || !hooks->capture_local_pad)
    return 0;

  peer_ms = hooks->peer_timeout_ms ? hooks->peer_timeout_ms : 1500u;
  connect_ms = hooks->connect_timeout_ms;

  for (;;) {
    uint32_t dt = 0, lh = 0, rh = 0;
    int want_soft = 0;
    uint16_t pad;

    if (snes_netplay_peer_disconnected(peer_ms)) {
      snes_netplay_soft_exit_to_lobby("peer_disconnect", from_lobby);
      desync_logged = 0;
      connect_wait_started_ms = 0;
      if (running && snes_netplay_return_to_lobby_requested())
        *running = 0;
      return 0;
    }

    if (connect_ms && !snes_netplay_is_running()) {
      uint32_t now = SDL_GetTicks();
      if (!connect_wait_started_ms)
        connect_wait_started_ms = now ? now : 1u;
      if ((uint32_t)(now - connect_wait_started_ms) >= connect_ms) {
        if (hooks->on_connect_timeout)
          hooks->on_connect_timeout(hooks->ctx);
        snes_netplay_soft_exit_to_lobby("connect_timeout", from_lobby);
        connect_wait_started_ms = 0;
        if (running)
          *running = 0;
        return 0;
      }
    } else {
      connect_wait_started_ms = 0;
    }

    if (snes_netplay_input_desync(&dt, &lh, &rh)) {
      if (!desync_logged) {
        fprintf(stderr,
                "snes_netplay: INPUT desync tick=%u local=%08x remote=%08x — "
                "stalled\n",
                (unsigned)dt, (unsigned)lh, (unsigned)rh);
        desync_logged = 1;
      }
      SDL_Delay(16);
      if (hooks->poll_events)
        hooks->poll_events(hooks->ctx, &want_soft);
      if (want_soft) {
        snes_netplay_soft_exit_to_lobby("escape", from_lobby);
        if (running)
          *running = 0;
        return 0;
      }
      continue;
    }
    desync_logged = 0;

    pad = hooks->capture_local_pad(hooks->ctx);
    snes_netplay_stage_local(pad);

    if (hooks->poll_events)
      hooks->poll_events(hooks->ctx, &want_soft);
    if (want_soft) {
      snes_netplay_soft_exit_to_lobby("escape", from_lobby);
      if (running)
        *running = 0;
      return 0;
    }

    if (snes_netplay_poll_admit())
      return 1;
    SDL_Delay(1);
  }
}

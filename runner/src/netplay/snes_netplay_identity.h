/*
 * snes_netplay_identity.h — persisted netplay display name.
 *
 * The lobby needs a display name every launch. Without persistence the
 * launcher prompts every single time, which is the complaint this solves.
 * Framework-owned on purpose: the alternative is a copy of the same field in
 * every game's own config (MetalWarriorsSNESRecomp carries one), and a
 * per-title copy cannot inherit fixes.
 *
 * Storage is config.ini beside the executable, [Netplay] PlayerName — the
 * same file and section the shared config parser reads into
 * g_config.netplay_player_name, so a host that uses the framework Config gets
 * it for free and a host that parses config.ini itself can call these.
 *
 * Both calls are safe before any lobby exists and on a read-only install
 * (load yields "", store reports failure and changes nothing).
 */
#ifndef SNES_NETPLAY_IDENTITY_H
#define SNES_NETPLAY_IDENTITY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copy the persisted name into `out`. Empty string when none is stored.
 * Returns 1 when a non-empty name was found. */
int snes_netplay_identity_load(char *out, size_t cap);

/* Persist `name` (NULL/empty clears the key). Returns 1 on success.
 * Comment-preserving: rewrites only the [Netplay] PlayerName line. */
int snes_netplay_identity_store(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* SNES_NETPLAY_IDENTITY_H */

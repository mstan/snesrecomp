#pragma once

#include <stdint.h>

#ifdef __cplusplus
#include <filesystem>
#include <string>

#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#endif

namespace SNESRecomp {

/*
 * Initialize the package catalog rooted at <exe>/mods for one verified game.
 * The ROM digest is the canonical lowercase SHA-256 used by package targets.
 */
bool mod_runtime_initialize(const std::filesystem::path& root,
                            const std::string& game_id,
                            const std::string& rom_sha256,
                            std::string* error = nullptr);

/*
 * Resolve the staged feature selections, validate the selected ROM, persist
 * state, and prepare the trusted-plugin activation plan.
 */
bool mod_runtime_commit(const std::filesystem::path& rom_path = {},
                        std::string* error = nullptr);

/* Invoke the trusted, statically linked plugins selected by the committed plan. */
void mod_runtime_activate_plugins();

#if defined(RECOMP_LAUNCHER)
const ::RecompLauncherCModProvider* mod_runtime_launcher_provider();
#endif

}  // namespace SNESRecomp
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*SNESModActivationCallback)(void);
typedef void (*SNESModFrameCallback)(void);
typedef int (*SNESModApuWriteCallback)(uint16_t reg, uint8_t value);

struct RecompLauncherCModProvider;

int snes_mod_runtime_initialize_c(const char* root,
                                  const char* game_id,
                                  const char* rom_sha256);
int snes_mod_runtime_commit_c(const char* rom_path);
void snes_mod_runtime_activate_plugins_c(void);
const struct RecompLauncherCModProvider*
snes_mod_runtime_launcher_provider_c(void);
const char* snes_mod_runtime_last_error_c(void);
int snes_mod_runtime_feature_enabled_c(const char* package_id,
                                       const char* feature_id);
/* Verdicts for snes_mod_runtime_check_set_c. Values match the wire codes in
 * the netplay protocol, but are declared here so the mod runtime does not have
 * to know what a packet is -- the netplay layer owns that mapping. */
#define SNES_MODSET_OK      0
#define SNES_MODSET_MISSING 1  /* package absent entirely */
#define SNES_MODSET_VERSION 2  /* present at a different version */
#define SNES_MODSET_OPTION  3  /* option, value, or selection we cannot meet */
#define SNES_MODSET_TOO_BIG 4  /* set too large to compare honestly */

/* Write the host's set into this build's own selection, so a player whose
 * mods differ can join by starting again rather than by reproducing someone
 * else's configuration by hand. Disables everything the host does not run: a
 * set is the whole selection, and an extra mod differs as surely as a missing
 * one. All-or-nothing -- a partially adopted set matches neither side.
 *
 * Cannot take effect in the current launch: mods activate before the netplay
 * session exists, so the contract is "your settings now match, start again". */
int snes_mod_runtime_adopt_set_c(const char* want, char* reason, uint32_t cap);

/* Is `package_id` installed at `version`? 1 yes, 0 present at another version,
 * -1 absent. Writes the package's display name (falling back to its id) so a
 * lobby row can name what a player needs to install. */
int snes_mod_runtime_have_package_c(const char* package_id, const char* version,
                                    char* name_out, uint32_t name_cap);

/* One package row on the lobby wire.
 *
 * The effective-set text below is keyed by FEATURE, which is the right grain
 * for "are we simulating the same thing". The lobby server asks a coarser
 * question -- does this peer have this package at this version, at all -- so
 * these rows are per PACKAGE, and carry the enabled feature ids alongside for
 * the player to read. Both grains describe the same selection; neither
 * replaces the other. */
#define SNES_MOD_ROW_ID_LEN    96
#define SNES_MOD_ROW_VER_LEN   32
#define SNES_MOD_ROW_NAME_LEN  64
#define SNES_MOD_ROW_FEATS_LEN 192

typedef struct SnesModPkgRow {
    char id[SNES_MOD_ROW_ID_LEN];
    char version[SNES_MOD_ROW_VER_LEN];
    char name[SNES_MOD_ROW_NAME_LEN];
    /* Plan rows only: comma-separated ids of the features the host turned on.
     * Empty on an offer row, which says only "I have this package". */
    char features[SNES_MOD_ROW_FEATS_LEN];
} SnesModPkgRow;

/* The host's required plan: one row per package with at least one ENABLED
 * feature, at the version actually selected. Returns the number of rows
 * written (capped at `max`), or 0 when nothing is enabled.
 *
 * A row whose id or version does not fit is DROPPED rather than truncated: a
 * truncated id names a different package, and the server would compare it to
 * a peer's offer and reach a confident wrong answer. */
int snes_mod_runtime_plan_rows_c(SnesModPkgRow* out, int max);

/* Every (package, version) installed on this machine -- what a peer offers so
 * the server can tell what it is missing. Enabled or not: the question is
 * possession, and the host's plan decides what runs. Same drop-don't-truncate
 * rule as above. */
int snes_mod_runtime_installed_rows_c(SnesModPkgRow* out, int max);

/* ---- peer-to-peer package transfer -------------------------------------
 *
 * The host packs a package it has; the guest verifies and installs it. The
 * bytes travel over a direct ICE connection between the two players, never
 * through the lobby server -- see runner/src/lobby.
 */

/* Pack `package_id`@`version` into a .snesmod archive in memory.
 *
 * Writes the archive's SHA-256 as lowercase hex into `sha256_hex` (needs 65
 * bytes). The digest is computed here, at the source, so the receiver checks
 * the bytes it actually got against a value that never shared a path with
 * them. Returns 1 on success; *out must then be released with
 * snes_mod_runtime_free_blob_c. */
int snes_mod_runtime_export_package_c(const char* package_id,
                                      const char* version,
                                      uint8_t** out, uint32_t* out_len,
                                      char* sha256_hex, uint32_t sha_cap,
                                      char* err, uint32_t err_cap);
void snes_mod_runtime_free_blob_c(uint8_t* blob);

/* Install a received archive.
 *
 * `expect_sha256` is REQUIRED and checked before a single byte is unpacked:
 * this is code from another machine, and the digest is the only thing tying
 * what arrived to what the host said it was sending. A mismatch is refused
 * without touching the mod directory. Returns 1 on success. */
int snes_mod_runtime_install_blob_c(const uint8_t* data, uint32_t len,
                                    const char* expect_sha256,
                                    char* installed_id, uint32_t id_cap,
                                    char* installed_ver, uint32_t ver_cap,
                                    char* err, uint32_t err_cap);

/* Can this build honour the host's mod set? Returns one of the above and
 * writes a player-actionable reason ("missing mod: x", "y needs version z").
 * Empty reason on OK. */
int snes_mod_runtime_check_set_c(const char* want, char* reason, uint32_t cap);

/* The effective mod set as canonical text, one line per ENABLED feature:
 *   "<package>@<version>/<feature> <option>=<value> ...\n"
 * Sorted and using resolved option values, so equal selections produce
 * byte-identical output on any machine. "(none)\n" when nothing is enabled.
 * Returns the length that WOULD be written, so truncation is detectable.
 *
 * This is what netplay peers must agree on: a mod that patches guest memory is
 * simulation state, and two peers running different sets cannot stay in sync. */
int snes_mod_runtime_effective_set_c(char* out, uint32_t cap);

int snes_mod_runtime_feature_option_value_c(const char* package_id,
                                            const char* feature_id,
                                            const char* option_id,
                                            char* out,
                                            uint32_t cap);

/*
 * Register a trusted implementation. A .snesmod archive may select only this
 * stable id; archives never provide native code, symbols, or library paths.
 */
int snes_mod_register_activation_plugin(const char* id,
                                        SNESModActivationCallback callback);

/*
 * Register game-owned startup policy used to make a mod authoritative over a
 * legacy config flag. The reset callback runs before active plugins, so a
 * disabled feature reliably restores the stock behavior on every launch.
 */
int snes_mod_register_reset_callback(SNESModActivationCallback callback);

/*
 * Register callbacks from an active trusted plugin. APU write callbacks may
 * return nonzero to consume the write before it reaches the stock SPC ports.
 */
int snes_mod_register_frame_callback(SNESModFrameCallback callback);
int snes_mod_register_apu_write_callback(SNESModApuWriteCallback callback);
void snes_mod_runtime_frame_tick_c(void);
int snes_mod_runtime_filter_apu_write_c(uint16_t reg, uint8_t value);

/*
 * Request battery-backed SRAM for a stock cart that declares none. This is
 * for enhancement mods that add guest-visible saves to password-only games.
 * Existing cartridge SRAM is never resized by a mod.
 */
int snes_mod_request_synthetic_sram_c(uint32_t bytes);
uint32_t snes_mod_runtime_synthetic_sram_size_c(void);

#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
#define SNES_MOD_CONSTRUCTOR(name)                                          \
    static void __cdecl name(void);                                         \
    __declspec(allocate(".CRT$XCU"))                                        \
    static void (__cdecl* name##_constructor)(void) = name;                 \
    static void __cdecl name(void)
#elif defined(__GNUC__) || defined(__clang__)
#define SNES_MOD_CONSTRUCTOR(name)                                          \
    static void name(void) __attribute__((constructor));                    \
    static void name(void)
#else
#error "SNES mod plugin registration needs a supported constructor mechanism"
#endif

#ifdef __cplusplus
}
#endif

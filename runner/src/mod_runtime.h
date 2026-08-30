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

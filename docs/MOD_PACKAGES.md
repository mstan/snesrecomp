# SNESRecomp mod packages and trusted plugins

SNES mod support is an explicit per-game build feature:

```cmake
set(SNESRECOMP_ENABLE_MODS ON CACHE BOOL "" FORCE)
include(${SNESRECOMP_ROOT}/runner/runner.cmake)
```

The framework default is `OFF`; every project the scaffolder produces sets it
`ON` (`tools/new_project`), ships an empty `mods/preloaded` catalog, and wires
the runtime in `src/main.c` -- a title cannot exchange mods with a netplay peer
without it, so it is not left to be remembered. An enabled build also enables
recomp-ui's Mods surface, but that surface remains hidden until the game
initializes the runtime and supplies its provider.

## Product and trust model

The design follows psxrecomp's package/feature split:

- A **package** is an installation, update, provenance, and trust boundary.
- A **feature** is independently enabled and may have boolean, choice, or
  bounded-integer options.
- A **trusted plugin** is game-owned native behavior already statically linked
  into the executable and registered under a stable ID.

`.snesmod` archives are ZIP files with a root `manifest.toml`. Archives contain
data only. They cannot provide native code, symbols, DLLs, or library paths.
The loader accepts stored and DEFLATE entries, verifies CRCs, rejects encrypted
or unsafe paths, caps archives at 4096 files and 256 MiB expanded, stages
extraction, validates the manifest, and publishes a version atomically.

## Package layout

Installed packages live beside the executable:

```text
mods/
  state.toml
  packages/
    example.display/
      1.0.0/
        manifest.toml
```

A game may preload built-in packages with a post-build copy into this same
layout. Built-in features should default to disabled.

## Manifest format 1

```toml
format_version = 1
id = "example.display"
version = "1.0.0"
name = "Example Display Enhancements"
author = "Example Author"
description = "Game-specific presentation features."
license = "MIT"
resolver = "declarative"
save_compatibility = "shared"

[[target]]
game_id = "example-game-us"
rom_sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

[[feature]]
id = "widescreen"
name = "Widescreen (16:9)"
description = "Enables the game's surveyed widescreen implementation."
group = "Display"
default_enabled = false

[[plugin]]
feature = "widescreen"
id = "example.widescreen"
```

Options use the same feature-oriented concepts exposed by recomp-ui:

```toml
[[option]]
feature = "example-feature"
id = "mode"
label = "Mode"
type = "choice"
default = "stock"

[[option.choice]]
value = "stock"
label = "Stock"

[[option.choice]]
value = "enhanced"
label = "Enhanced"
```

Supported option types are `boolean`, `choice`, and bounded `integer`. The
initial SNES runtime exposes options and persists them for plugins that add
typed query services later; the first MMX integrations need only activation.

## Plugin registration

Game code registers trusted behavior before `main()`:

```c
#include "mod_runtime.h"

static void enable_widescreen(void) {
  GameDisplay_SetWidescreenEnabled(true);
}

static void reset_display_mods(void) {
  GameDisplay_SetWidescreenEnabled(false);
}

SNES_MOD_CONSTRUCTOR(register_display_mods) {
  snes_mod_register_reset_callback(reset_display_mods);
  snes_mod_register_activation_plugin(
      "example.widescreen", enable_widescreen);
}
```

On Play, the runtime verifies the selected stock ROM, resolves all enabled
features, rejects missing or multiply claimed plugin IDs, persists state, runs
each registered reset callback, then activates the resolved plugins. Reset
callbacks make the package state authoritative over old config files: disabling
widescreen restores native 4:3 on the next launch even if a legacy config once
stored `Widescreen = 1`.

The initial operation vocabulary is intentionally narrow: trusted activation
plugins only. Future guarded ROM writes, asset overlays, or interpreter hooks
must retain the same pre-boot validation and no-arbitrary-code model rather than
turning package order into an implicit patch priority.

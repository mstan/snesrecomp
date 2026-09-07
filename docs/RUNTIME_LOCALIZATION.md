# Runtime Localization

SNESrecomp supports a small runtime localization layer for game projects that
need translation patches without modifying the user's ROM file. The first
reference title is Gundam Wing Endless Duel; its game-specific workflow lives in
`GundamWingEndlessDuelSNESRecomp/docs/TRANSLATION_TILEMAP_REFERENCE.md`.

## Model

A game-owned mod registers a localization feature and calls:

```c
snes_text_xlate_init_c("translations/endless_duel.toml", "en");
snes_mod_register_frame_callback(snes_text_xlate_on_frame_c);
```

The table is TOML-like by design: it is simple enough to parse in the runner and
easy for game-side generators to rewrite. A patch has a verification source and
one or more language payloads:

```toml
schema = 1
default_lang = "en"
fallback_fr = "en"

[[rom_patch]]
address = 0x017000
source_hex = "..."
fr_hex = "..."
```

The selected language is tried first. If that language has no payload for a
patch, the runtime follows `fallback_<lang>` until it finds a payload or reaches
the default language. This allows partial languages to override known surfaces
while falling back cleanly for unmapped ones.

## Patch Kinds

- `[[rom_patch]]` patches the in-memory cartridge image after boot. This is the
  normal path for IPS-derived script, pointer, tilemap, or graphics bytes.
- `[[ram_patch]]` patches WRAM after verifying the existing bytes.
- `[[glyph_label]]` is an alias for RAM-backed text labels.
- `[[vram_patch]]` patches VRAM after the game uploads graphics or tilemaps.
  Use this when the asset source is compressed, shared, or easier to replace as
  runtime-injected tile data.

Each hex payload for a language must have the same byte width as its
`source_hex`. The runtime verifies source bytes before replacing them; failed
verification leaves the original bytes in place and increments the skipped
counter exposed through debug diagnostics.

## Text Entries

For simple byte-encoded text, a table can define glyph mappings and text
entries:

```toml
[[glyph]]
utf8 = "A"
hex = "80"

[[entry]]
address = 0x123456
source = "OPTION"
fr = "OPTION"
```

The runner maps UTF-8 units to byte sequences using `[[glyph]]` entries. This
works only for surfaces whose backing storage is actual text data. For tilemap
or tile-art text, game-side tools should generate explicit hex patches instead.

## Per-Title Workflow

Keep localization source data in the game repository, not in the framework:

1. Identify the surface through reference patches, debugger captures, or both.
2. Decide whether it is byte text, tilemap data, tile graphics, or a mix.
3. Store human-authored strings or asset descriptions in source files.
4. Generate language-specific `*_hex` payloads into the runtime table.
5. Run width/source/charmap validation before building.
6. Capture screenshots through the debug TCP server and inspect them visually.

Endless Duel currently demonstrates all three practical layers:

- byte text entries for option/key-config labels,
- generated tilemap rows for battle/ending dialogue,
- VRAM/PPU/OAM probes for title-menu tile-art investigation.

Do not expose a language if every user-visible surface falls back to another
language. For scripts such as Chinese, Japanese, or Korean, add real glyph/tile
assets or runtime-injected replacements before enabling the language.

## Debugging

Trace builds expose `xlate_stats` over the debug TCP server. The response
reports selected language, patch counts, apply counts, skip counts, and the last
localization error. Game projects should include a repeatable screenshot harness
for each translated surface and keep generated captures out of Git because they
are derived from ROM data.

# New project toolkit

Scaffold a new SNES recomp title end to end: probe the ROM, lay out the repo,
wire the submodules, seed the analysis config, generate C, build, and publish.

```sh
sh tools/new_project/setup_project.sh --rom ~/roms/game.sfc --dir ~/src
```

On a terminal that is the only argument you need: everything else is asked,
with the probed ROM identity supplying the defaults. Flags are for scripting —
anything passed explicitly skips its question, and `--yes` (or a non-TTY run)
takes every default without asking.

`sh setup_project.sh --help` lists every flag.

## What it asks

| Question | Default |
|---|---|
| Display name | the dump filename, cleaned of `(Region)` tags, or the cartridge header when that reads better |
| Max players (1-8) | 2 |
| Multitap port | derived from players; only asked above 2 seats |
| Release zip / CI artifact prefix | slug of the name |
| Description, publisher, year | blank (README metadata) |
| Region | from the cartridge header |
| Include the recomp-ui launcher submodule? | no |
| Enable netplay? | no — skipped entirely for a 1-player title |
| Also build rollback? | yes, when netplay is on |
| Add the GitHub Actions workflow? | yes |
| Generate C from the ROM now? | yes |
| Configure and build after generating? | yes, when generating |
| Create a GitHub repo with `gh`? | **no** |
| Owner / repo / visibility | asked only if you said yes |

Nothing assumes a GitHub repository exists. Declining the question skips
creation, the remote, and the push; the project is still a complete local git
repository. The SNES counterpart to
psxrecomp's `tools/new_project_layout/setup_project.sh`, and it follows the
same publish order for the same reason: scaffold + CI → commit → `gh repo
create` (no push) → generate/build → one push. Pushing earlier leaves a second
"initial" commit that collides when the script is re-run.

## What it produces

```text
<Title>SNESRecomp/
├── CMakeLists.txt          runner + generated C + host, netplay/multitap wired
├── VERSION                 release pin used for lobby version matching
├── framework_pins.txt      exact framework SHAs this project was cut against
├── recomp/                 bank*.cfg, symbols.toml — analysis input you own
├── src/                    main.c, game_rtl.c, host_contract.c, codegen_setup.c
│   └── gen/                generated C (gitignored — never committed)
├── tools/regen.sh          ROM → C, with digest verification
├── scripts/package_release.sh
├── .github/workflows/release.yml
└── snesrecomp/             framework submodule (owns lib/recomp-net, lib/retcomm-rbengine)
```

## Pieces

| File | Role |
|------|------|
| `setup_project.sh` | The end-to-end driver |
| `probe_rom.py` | Cartridge-header identity: mapping, title, region, coprocessor, vectors, digests |
| `fill_tokens.py` | `@TOKEN@` substitution; unknown tokens are an error, not a blank |
| `templates/` | Everything written into the new repo |

`probe_rom.py` is usable on its own — `python3 probe_rom.py game.sfc` prints
what the scaffold would bake in, which is the fastest way to check whether a
title uses a coprocessor this runner supports before investing in a port.

## Options worth knowing

- `--players N` (1-8). Above two configures a Super Multitap: port 2 for 3-5
  seats (the layout commercial titles use), both ports for 6-8. Override with
  `--multitap`.
- `--rollback` builds `retcomm-rbengine` in and implies `--netplay`.
  Delay-sync stays the runtime default; `SNES_NET_MODE=rollback` opts in.
- `--recomp-net-ref` / `--rbengine-ref` follow a branch for the modules nested
  inside snesrecomp instead of the SHA the framework pins. CI always builds
  the committed gitlink SHAs.
- `--create-github` needs `gh`. Repo creation and push are separate steps by
  design (see above): the repo is created after the scaffold is committed and
  pushed once at the very end, so a re-run never produces a competing
  "initial" commit.
- `--no-submodules` scaffolds offline. The project will not build until you
  run `git submodule update --init --recursive`.
- `SNESRECOMP_ROOT=/path/to/checkout` makes the generate step use a working
  tree instead of the pinned submodule — for framework development.

## The ROM never enters the repository

The ROM is probed where it lies and is never copied in. `tools/regen.sh` takes
`--rom` (or `SNESRECOMP_ROM`) so it can stay on your own drive, and the
generated `.gitignore` blocks `*.sfc` / `*.smc` / `src/gen/` regardless.
`scripts/package_release.sh` refuses to build a zip that contains ROM data.

## Failure handling

The framework remotes are checked **before** anything is created, and a
failure part way through removes the directory the script made rather than
leaving a partial project the next run would refuse to overwrite.

The framework URL is read from the checkout this script runs out of
(`git remote get-url origin`) instead of being hard-coded, so it cannot drift
from where snesrecomp actually lives.

## Tests

`tests/test_new_project.py` (in the framework suite, `python3
tests/run_tests.py`) runs the scaffolder against a synthetic, redistributable
image with `--no-submodules`, and checks the layout, that no `@TOKEN@`
survives, that the ROM digests reach `regen.sh` / `codegen_setup.c` / the
README identically, that multitap and rollback flags reach CMake, and that no
ROM is ever staged.

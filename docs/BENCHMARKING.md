# SNES benchmark output

Finite benchmark hosts emit one machine-readable line on stdout:

```text
SNESRECOMP_BENCHMARK {...}
```

The JSON always includes `frames`, `seconds`, and `fps`, matching
`tools/run_benchmark_pairs.py`. Title hosts may add correctness and activity
counters such as framebuffer hashes, logic hashes, audio counters, dispatch
hit/miss totals, interpreter instruction/cycle totals, and coprocessor
counters.

The shared A/B runner invokes title binaries as:

```text
<exe> --benchmark <frames> <rom>
```

For serialized title A/B on the shared Windows host, use the helper from a
normal PowerShell prompt with Windows Python:

```powershell
& py -3 "$fw/tools/run_benchmark_pairs.py" `
  --frames 3000 --pairs 5 --warmups 1 --timeout 180 `
  --a-name baseline --a-exe "<baseline.exe>" --a-rom "<rom.sfc>" `
  --b-name candidate --b-exe "<candidate.exe>" --b-rom "<rom.sfc>" `
  --json-out "<artifact.json>" > "<artifact.stdout.json>"
```

Use distinct labels even for A/A controls. Do not use the MSYS `python` shim
for Windows `F:\...` paths; it can reinterpret them as POSIX paths.

Desktop hosts may also expose `--benchmark-audio <frames> <rom>` for uncapped
audio-enabled smoke tests and `--benchmark-audio-paced <frames> <rom>` for
untimed audio correctness captures. The paced mode keeps audio enabled and
leaves normal frame-delay pacing on while still skipping launcher/autosave and
using the finite benchmark renderer setup. Do not use paced audio runs as
throughput data; they exist so `SNESRECOMP_DSPOUT` captures the native DSP
stream at normal presentation pace.

Headless title hosts may also keep older positional forms for local smoke
tests, but new benchmark-capable hosts should accept the shared form above.
If a title checkout is still pinned to an older vendored framework without
`runner/src/benchmark.c` and `runner/src/benchmark.h`, it should compile
without the helper and simply omit the `SNESRECOMP_BENCHMARK` line until the
framework pin is updated or `SNESRECOMP_ROOT` is pointed at a newer checkout.

Optional phase timing is controlled by the compile-time
`SNESRECOMP_BENCHMARK_PHASES` define and is off by default. When off, the
benchmark line still includes the same phase fields with zero values and
`"phase_timing":false`.

Compile every translation unit that includes `benchmark.h` with the same
`SNESRECOMP_BENCHMARK_PHASES` value used for `benchmark.c`. In title builds,
scope benchmark definitions to the consumer host source and optional
`benchmark.c`; do not put them on generated ROM-derived objects, because that
causes irrelevant rebuilds and can perturb code layout.

Phase buckets are inclusive caller brackets, not exclusive subsystem timings.
For SMW/MMX, `guest_frame` wraps `RtlRunFrame()`: guest execution plus any
work reached by that frame step, including frame-boundary APU/SPC
synchronization, DMA/coprocessor side effects, and enabled diagnostics. The
post-frame `ppu_draw` bracket wraps the host call to `RtlDrawPpuFrame()` or
`draw_ppu_frame()`, which includes line rendering, HDMA/raster-IRQ simulation,
widescreen preparation/present staging, and final framebuffer copy work reached
by that path. `host_present` brackets the renderer begin/end presentation
calls around drawing. `audio_render` brackets `RtlRenderAudio()` inside the SDL
audio callback or stream fill path; it measures host audio consumption/mixing
of produced samples, not the full authoritative SPC advancement, which is
driven from the guest-frame boundary. Do not sum phase buckets or subtract them
from one another; use them only to compare the same bucket across A/B builds or
to choose the next attribution pass.

The benchmark helper does not serialize phase accumulation internally. Callers
that update a shared `SnesRecompBenchmark` from more than one thread must hold
their own lock; the desktop hosts use `g_benchmark_phase_mutex` before the
audio callback updates shared phase counters. The paced-audio output health
statistics are separate and use the existing audio mutex.

## Final validation sequence

Do not use stale aggregate-regression artifacts for final validation. In the
September 2026 optimization lane, `build-codex-perf-current-full` executables
were known bad after address-log initialization was identified as the aggregate
regression source. Use a coherently rebuilt stabilized candidate only:
provisional `*-cachedbridge*` artifacts first, then the forthcoming DSP-gated
candidate. Rebuild any original-PPU substitution from that same stabilized base.

The WLOG regression itself is not a retained optimization and must not be
counted as a performance gain. It was an introduced bug where an unset
diagnostic env gate remained in the "unknown" state and called `getenv` from
the CPU write path. Also exclude the earlier stale-object cached-bridge relink
(`smw_fixedfull_vs_cachedbridge_5pairs.json`) because it linked against the
wrong object set. The accepted cached-bridge rerun is the `*_wlogrsp.json`
matrix.

Current accepted/provisional evidence roots:

- cached bridge:
  `build/perf/cachedbridge/smw_fixedfull_vs_cachedbridge_5pairs_wlogrsp.json`,
  `build/perf/cachedbridge/mmx_fixedfull_vs_cachedbridge_5pairs_wlogrsp.json`,
  `build/perf/cachedbridge/smw_original94_vs_cachedbridge_5pairs.json`, and
  `build/perf/cachedbridge/mmx_original94_vs_cachedbridge_5pairs.json`.
- DSP gate:
  `build/perf/dspgate/20260905-032354/smw_cachedbridge_vs_dspgate_5pairs.json`
  and
  `build/perf/dspgate/20260905-032354/mmx_cachedbridge_vs_dspgate_5pairs.json`.
- audio history variants:
  `build/perf/audio_variants/20260905-dspgate/measurement_summary.json` and
  `build/perf/audio_variants/20260905-dspgate/audio_variant_sizes_hashes.json`.

Those accepted/provisional matrices were run in each title's default
`SNESRECOMP_EXECUTION_MODE=lle` mode. For SMW/MMX that is a faithful
interpreter-driven scheduler/main-loop with generated AOT call-bounce when a
`g_dispatch_table` body exists, plus interpreter fallback on misses. Do not
describe these numbers as pure AOT-only or pure interpreter-only performance.

The audio-history COUNTERS variant is a memory-budget candidate only. It
removes the 16 MiB PCM ring, 8 MiB event ring, and 160 KiB snapshot ring from
core forensic BSS, but the first five-pair title timings were throughput-
neutral inside A/A noise and the normal benchmark path had audio disabled. Keep
COUNTERS opt-in unless a later source change earns a separate paced/audio
health gate. Mega Man X's desktop `s_icons` allocation is a separate UI buffer
and is not SNES core state.

The later SMALL memory-policy probe is the current candidate for reducing
audio history footprint. It used fresh coherent 2026-09-05b title builds with
the accepted diagnostic/DSP fixes, restored PPU source, and dropped CPU bus
guard. SMALL keeps retained history but shrinks the rings; `audio_trace.c.obj`
kept the same `.text 4672` as FULL while reducing core forensic BSS from
25,330,080 to 545,184 bytes, saving about 23.64 MiB. Five balanced native LLE
pairs, audio disabled, measured:

- SMW 15,000 frames: SMALL median **+1.294%** vs FULL, range
  `+0.079%..+1.641%`.
- MMX 24,000 frames: SMALL median **+0.141%** vs FULL, range
  `-0.999%..+1.615%`.

All records had fresh reports, Direct3D11 vsync off, and matching frame/main-
cycle status. The paced native and widescreen audio-health validation later
passed, the runtime default-promotion build/test step passed, and CMake now
defaults normal production builds to SMALL. Final aggregate/original94 timing
should use the restored original PPU source and avoid re-crediting any rejected
PPU or CPU-guard candidate.

`SNESRECOMP_AUDIO_TRACE_HISTORY` is a compile-time CMake/source selection, not
a runtime environment knob. Runtime audio stats and other cached runtime env
knobs are resolved once per process, not polled dynamically; in
COUNTERS/RESERVED builds, `SNESRECOMP_AUDIO_STATS` is resolved on the first
audio sample, so set it before launch when collecting stats.

SMW paced-audio native checkpoint (2026-09-05): coherent phase-off desktop
builds used GCC 15.2.0 from `C:\msys64\mingw64\bin`, Ninja from the same root,
Release CMake, `SNESRECOMP_ENABLE_TRACE=OFF`,
`SNESRECOMP_ENABLE_FRAME_FINGERPRINTS=OFF`,
`SNESRECOMP_ENABLE_PPU_DMA_HISTORY=OFF`,
`SNESRECOMP_ENABLE_DISPATCH_HISTORY=OFF`, and
`SNESRECOMP_ENABLE_BENCHMARK_PHASES=OFF`. Current framework
`SNESRECOMP_ROOT=F:/Projects/snesrecomp/snesrecomp` produced
`build-codex-perf-20260905b-current-phaseoff/SuperMarioWorldSNESRecomp.exe`
SHA-256 `26CD41D6685F85A8E74D5B7F588C35C82329E7C85E61CA493317E97D65C4B885`.
Baseline framework `SNESRECOMP_ROOT=F:/Projects/snesrecomp/_wt-snesrecomp-94c1197`
produced
`build-codex-perf-20260905b-baseline94-phaseoff/SuperMarioWorldSNESRecomp.exe`
SHA-256 `1A8FB06A30893F3C5D873B57B00BB8D31EA0B35357069949D2BAEF9F07230597`.
Compile-command audit found zero generated objects with benchmark macros;
current compiled `src/main.c` with `SNESRECOMP_HAS_BENCHMARK_HELPER=1` and
`runner/src/benchmark.c` with `SNESRECOMP_BENCHMARK_PHASES=0`, while the
baseline compiled `src/main.c` with `SNESRECOMP_HAS_BENCHMARK_HELPER=0`.

SMW native paced correctness with BMP/WRAM/APURAM tracing used frames
300..900 step 60 for BMP and 3,000 total frames. The captured BMP set matched
by filename/hash, WRAM matched exactly
(`9973689` bytes, SHA-256
`40E580B34EFC0E04FE3A596812430768D109E75BD89E7D2E6D47DC8523C40A8E`), and
APURAM matched exactly (`8053534` bytes, SHA-256
`47B94EFCE71AD05C27AC4F94B5499C0AAEB4BB82AF77C42E33AF659FF89D6FD3`). That
run showed diagnostic I/O can perturb audio scheduling: current reported
`dropped_audible=6212`, and the baseline output was incomplete before the
timeout. The follow-up audio-only paced run removed BMP/WRAM/APURAM tracing.
Current exited 0 with `audio_health.ok=true`, `dropped=0`,
`dropped_audible=0`, `host_output_peak=18531`, and `enqueue_failures=0`.
Baseline exited 3 with a preexisting strict-health failure:
`dropped=963`, `dropped_audible=689`, `host_output_peak=18526`, and
`enqueue_failures=0`. Both DSP captures were long and non-silent; signal
comparison found the same first nonzero native stereo frame (`881`), equal
sample peak (`18534`), and similar RMS (`1567.62` current vs `1567.90`
baseline). Artifact root:
`build/perf/smw_paced_audioonly_20260905d/parsed_signal_summary.json`.

The tentative PPU native/inactive-policy composition refactor was rejected and
removed on 2026-09-05. Same-base title probes were mixed/noisy rather than a
clear win, with MMX negative enough to fail the acceptance bar, and the helper
extraction added code size. Keep only the composition regression fixture for
future candidate-vs-HEAD output checks; do not run final widescreen/native PPU
performance acceptance for that rejected candidate.

Historical `20260905-dspgate` audio variant artifacts remain evidence for the
earlier memory and noisy throughput study, but are not final correctness
routes. They predate the latest audio stats clock gate and final CPU bus guard
revert, and their manifests derive from the old
`build-codex-perf-current-full` base RSP lineage:

- SMW FULL:
  `build/perf/audio_variants/20260905-dspgate/smw/full/SuperMarioWorldSNESRecomp-full.exe`,
  SHA-256 `0B0D93D9F58C3B708234D99D1ACB3306668F1A62178FA60000C8A7E4B3F33396`.
- SMW COUNTERS:
  `build/perf/audio_variants/20260905-dspgate/smw/counters/SuperMarioWorldSNESRecomp-counters.exe`,
  SHA-256 `644F8033F58A8DB5587731EC60C6F89D10075D212D3A258ADCFFEBF61FF56F62`.
- MMX FULL:
  `build/perf/audio_variants/20260905-dspgate/mmx/full/MegaManXSNESRecomp-full.exe`,
  SHA-256 `0F3C4E83AD24F54C281F9758FD6C5BA553E91ECFDD6C4B87941DE5A9C6019652`.
- MMX COUNTERS:
  `build/perf/audio_variants/20260905-dspgate/mmx/counters/MegaManXSNESRecomp-counters.exe`,
  SHA-256 `2EC2229E9789EE4FD348E87EFEC9717682C011914204F5D83DAD9149F60F2BFA`.

Fresh SMALL probe artifacts are under
`build/perf/20260905b-smallprobe/{smw,mmx}/{full,small}/manifest.json`, with
raw timing records under `build/perf/20260905b-smallprobe/timings/`. Prepared
final audio-history correctness commands must use those or newer stabilized
artifact directories, not the historical executable paths above. Use Python
`communicate` with captured pipes and write stdout/stderr after exit;
PowerShell native redirection previously added large startup/console overhead
to paced capture logs.

Final SMALL correctness milestone, 2026-09-05: the retained artifact root was
`build/perf/final_correctness_20260905c`. `summary.json` SHA-256 was
`A04AFF2B64C54E2C7140F9E3585638447AB43BDB7F29EDC031C4060D6C655954`; the
manifest SHA-256 was
`5F9AD9FDD64075958496AD353FB4B007DC0A04061186167F248165D2E7A48D1E`. The run
used the fresh `20260905b-smallprobe` FULL/SMALL artifacts with restored PPU
source and no CPU bus-guard candidate. All 8 paced audio-only cases
(SMW/MMX x FULL/SMALL x native/wide) reached an accepted attempt with
`audio_health.ok:true`, `dropped_audible:0`, `enqueue_failures:0`, and nonzero
host/DSP peak. Two first attempts failed honestly and are preserved in the
artifact tree: SMW FULL wide failed once with `dropped_audible=382`, then
passed retry; MMX SMALL wide failed once with `dropped_audible=291`, then
passed retry.

Accepted-attempt private bytes were SMW FULL native/wide `89.6/87.9 MB`
versus SMALL `64.0/62.8 MB`, and MMX FULL native/wide `107.2/108.3 MB`
versus SMALL `81.2/83.5 MB`. Working-set samples were SMW FULL native/wide
`74.6/75.8 MB` versus SMALL `66.5/57.8 MB`, and MMX FULL native/wide
`73.9/81.0 MB` versus SMALL `68.4/69.9 MB`. Treat these as host residency
observations, not the static BSS delta.

Separate BMP/WRAM/APURAM captures matched FULL vs SMALL for SMW native,
SMW wide, MMX native, and MMX wide, with 11 BMPs per case. SMW WRAM/APURAM
hashes were `40e580b34efc0e04fe3a596812430768d109e75bd89e7d2e6d47dc8523c40a8e`
and `47b94efce71ad05c27ac4f94b5499c0aaeb4bb82af77c42e33af659ff89d6fd3`.
MMX WRAM/APURAM hashes were
`1cb2ffeadda8f60bf71992961e78c04a556021ed761f4ac800a7ff6b5cde7064` and
`29ac403e02d0fd2fb02e193e243956d9346acc1f3776fb353ff8e304c2b31acc`.

After the audio-history default promotion, the final coherent aggregate
compared framework baseline94 against the retained source tree. Coverage was
five balanced native pairs for SMW and MMX, then a two-pair widescreen
confirmation for both titles so enhancement-path performance was represented
without re-crediting the rejected PPU candidate. The PPU source in that final
tree was the restored original policy body; render correctness gates cover
hardware output separately.

Final integration window, 2026-09-05:

- Original94-vs-final retained throughput used phase-off Release builds from
  the coherent 20260905b directories. Current builds cleared any cached
  `SNESRECOMP_AUDIO_TRACE_HISTORY` override and compiled `audio_trace.c` with
  `SNESRECOMP_AUDIO_TRACE_HISTORY=1`, exercising the SMALL default. Native
  SMW improved from 626.296 to 1,180.543 FPS across five pairs
  (**+88.496%** median); native MMX improved from 968.194 to 1,633.738 FPS
  (**+69.411%** median). Widescreen confirmations improved SMW from 635.969
  to 1,195.050 FPS (**+87.910%**, two pairs) and MMX from 870.840 to
  1,306.958 FPS (**+50.081%**, two pairs). All timing records had fresh
  reports and Direct3D11 with vsync off; widescreen windows were `1194x672`
  for SMW and `1284x720` for MMX. Artifacts:
  `build/perf/final-20260905-retained/artifact_manifest.json`,
  `build/perf/final-20260905-retained/measurement_summary.json`, and raw
  timing JSON under `build/perf/final-20260905-retained/timings/`.
  These percentages are measured from framework baseline `94c1197` to the
  retained campaign source, not from current `origin/main`; main already has
  independent newer runtime/title changes.
- Static section deltas, retained current minus baseline94: SMW executable
  bytes `-1,387`, `.text -2,112`, `.data +32`, `.bss -27,476,000`; MMX
  executable bytes `-1,899`, `.text -2,080`, `.data +32`, `.bss -27,475,968`.
  The BSS reduction combines SMALL audio history with the accepted production
  diagnostic-history removals; MMX's desktop `s_icons` UI buffer remains
  separate from SNES core state.
- PhaseON SMW/MMX desktop smoke passed with `phase_timing:true`,
  `phase_semantics:"inclusive"`, and positive `guest_frame`, `ppu_draw`, and
  `host_present` calls. Compile-command audit found no benchmark/helper macros
  on generated objects; host `src/main.c` had helper and phase macros,
  `benchmark.c` had the phase macro, and `audio_trace.c` used
  `SNESRECOMP_AUDIO_TRACE_HISTORY=1` from the SMALL default. Artifact:
  `build/perf/final_phaseon_20260905/summary.json`, SHA-256
  `3B10B742845C0110EF09AF7253EDF7F5F2647FE7E3340E656427566F9C253C85`.
- Original94-vs-final retained state/video passed for MMX and SMW, native and
  enabled-widescreen. For each title/mode, the 11 captured BMP hashes, WRAM
  JSONL hash, and APURAM JSONL hash matched. Artifact:
  `build/perf/final_original94_state_20260905d/summary.json`, SHA-256
  `FB0E46BDE0A4A41207910672AFEC3F3CA08A70E66DDB690B84CB71AB96355F1B`.
- Fresh SMALL-default SMK headless 3,000-frame scripted-input route passed
  strict qualification. Fresh SMALL-default SMRPG headless 3,000-frame
  save/load route emitted valid benchmark JSON but exited 8 with 598 audio
  underruns. Controls showed the same save/load failure on origin-main and
  current FULL builds; final SMALL without save/load still has the older
  one-underrun qualification failure. Track this separately in `beads-23p`.
  Artifact: `build/perf/final_headless_20260905/final_summary.json`, SHA-256
  `3A45E2CAED781F82C335DA4F1EC888ED9223AB68C00589FEF151D839C56B2B36`.

The script below stages native and enabled-widescreen copies, writes the
effective `config.local.ini` and `mods/state.toml` for each mode, runs paced
audio-only captures with `SNESRECOMP_DSPOUT` as the only direct diagnostic
file, samples only the owned process working set/private bytes, then runs
video/WRAM/APURAM captures separately so trace I/O cannot perturb audio
callback scheduling. Report host RSS separately from static/BSS evidence: a
50-second DSP capture touches only about 6 MiB of the 16 MiB PCM ring, so
observed RSS savings need not equal the 24.16 MiB reserved core forensic BSS
reduction. MMX's 16 MiB `s_icons` desktop buffer is separate UI state, not SNES
core state.

```powershell
@'
import array, ctypes, ctypes.wintypes as wt
import hashlib, json, math, os, pathlib, re, shutil, subprocess, threading, time

OUT = pathlib.Path(r"F:/Projects/snesrecomp/snesrecomp/build/perf/final_correctness_20260905")
FRAMES = 3000
TIMEOUT_SECONDS = 150

ARTIFACTS = [
    {
        "title": "smw",
        "variant": "full",
        "source_dir": r"<latest-stabilized-smw-full-dir>",
        "exe_name": r"<latest-stabilized-smw-full-exe-leaf>",
        "rom": r"F:/Projects/snesrecomp/SuperMarioWorldRecomp/smw.sfc",
    },
    {
        "title": "smw",
        "variant": "retained",
        "source_dir": r"<latest-stabilized-smw-retained-dir>",
        "exe_name": r"<latest-stabilized-smw-retained-exe-leaf>",
        "rom": r"F:/Projects/snesrecomp/SuperMarioWorldRecomp/smw.sfc",
    },
    {
        "title": "mmx",
        "variant": "full",
        "source_dir": r"<latest-stabilized-mmx-full-dir>",
        "exe_name": r"<latest-stabilized-mmx-full-exe-leaf>",
        "rom": r"F:/Projects/snesrecomp/MegamanXRecomp/mmx.sfc",
    },
    {
        "title": "mmx",
        "variant": "retained",
        "source_dir": r"<latest-stabilized-mmx-retained-dir>",
        "exe_name": r"<latest-stabilized-mmx-retained-exe-leaf>",
        "rom": r"F:/Projects/snesrecomp/MegamanXRecomp/mmx.sfc",
    },
]

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010

class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [
        ("cb", wt.DWORD),
        ("PageFaultCount", wt.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
    ]

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
OpenProcess = kernel32.OpenProcess
OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
OpenProcess.restype = wt.HANDLE
CloseHandle = kernel32.CloseHandle
CloseHandle.argtypes = [wt.HANDLE]
GetProcessMemoryInfo = psapi.GetProcessMemoryInfo
GetProcessMemoryInfo.argtypes = [
    wt.HANDLE, ctypes.POINTER(PROCESS_MEMORY_COUNTERS_EX), wt.DWORD]
GetProcessMemoryInfo.restype = wt.BOOL

def sample_process_memory(pid):
    handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not handle:
        return None
    try:
        counters = PROCESS_MEMORY_COUNTERS_EX()
        counters.cb = ctypes.sizeof(counters)
        if not GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            return None
        return counters.WorkingSetSize, counters.PrivateUsage
    finally:
        CloseHandle(handle)

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

def stage_case(artifact, mode):
    src = pathlib.Path(artifact["source_dir"])
    if not src.is_dir():
        raise SystemExit(f"missing artifact dir: {src}")
    dst = OUT / "stage" / f"{artifact['title']}-{artifact['variant']}-{mode}"
    stage_root = (OUT / "stage").resolve()
    if dst.exists():
        resolved = dst.resolve()
        if stage_root not in resolved.parents:
            raise SystemExit(f"refusing to remove outside stage root: {resolved}")
        shutil.rmtree(dst)
    shutil.copytree(src, dst)
    exe = dst / artifact["exe_name"]
    if not exe.is_file():
        raise SystemExit(f"missing staged exe: {exe}")
    write_config(dst, artifact["title"])
    write_state(dst, artifact["title"], mode == "wide")
    return dst, exe

def write_config(dst, title):
    if title == "mmx":
        widescreen_lines = "DisplayAspect = 4:3\nWidescreen = 0"
    else:
        widescreen_lines = "Widescreen = Standard\nWidescreenHud = 0"
    (dst / "config.local.ini").write_text(
        "[General]\n"
        "SkipLauncher = 1\n"
        "DisableFrameDelay = 0\n\n"
        "[Graphics]\n"
        "Fullscreen = 0\n"
        "WindowScale = 3\n"
        "OutputMethod = SDL\n"
        "NewRenderer = 1\n"
        "NoSpriteLimits = 1\n"
        f"{widescreen_lines}\n\n"
        "[Sound]\n"
        "EnableAudio = 1\n"
        "AudioFreq = 32040\n"
        "AudioChannels = 2\n"
        "AudioSamples = 512\n",
        encoding="ascii")

def write_state(dst, title, wide):
    mods = dst / "mods"
    mods.mkdir(exist_ok=True)
    enabled = "true" if wide else "false"
    if title == "smw":
        text = (
            "format_version = 1\n\n"
            "[[package]]\n"
            "id = \"super-mario-world.enhancement.msu1\"\n"
            "version = \"1.0.0\"\n\n"
            "[[package]]\n"
            "id = \"super-mario-world.enhancement.widescreen\"\n"
            "version = \"1.0.0\"\n\n"
            "[[feature]]\n"
            "package_id = \"super-mario-world.enhancement.msu1\"\n"
            "id = \"msu1\"\n"
            "enabled = false\n\n"
            "[[feature]]\n"
            "package_id = \"super-mario-world.enhancement.widescreen\"\n"
            "id = \"widescreen\"\n"
            f"enabled = {enabled}\n"
            "[feature.values]\n"
            "mode = \"16_9\"\n"
            "hud = \"split\"\n")
    else:
        text = (
            "format_version = 1\n\n"
            "[[package]]\n"
            "id = \"megaman-x.enhancement.widescreen\"\n"
            "version = \"1.0.0\"\n\n"
            "[[feature]]\n"
            "package_id = \"megaman-x.enhancement.widescreen\"\n"
            "id = \"widescreen\"\n"
            f"enabled = {enabled}\n")
    (mods / "state.toml").write_text(text, encoding="ascii")

def parse_benchmark(stdout_bytes):
    text = stdout_bytes.decode("utf-8", errors="replace")
    matches = re.findall(r"SNESRECOMP_BENCHMARK (\{.*\})", text)
    if not matches:
        raise SystemExit("missing SNESRECOMP_BENCHMARK JSON")
    return json.loads(matches[-1])

def pcm_signal(path):
    data = pathlib.Path(path).read_bytes()
    if len(data) < FRAMES * 500 * 4:
        raise SystemExit(f"DSP capture too short: {path} {len(data)} bytes")
    samples = array.array("h")
    samples.frombytes(data)
    if os.name == "nt" and samples.itemsize != 2:
        raise SystemExit("unexpected sample size")
    peak = max((abs(x) for x in samples), default=0)
    if peak == 0:
        raise SystemExit(f"DSP capture is silent: {path}")
    sq = sum(float(x) * float(x) for x in samples)
    first = next((i for i, x in enumerate(samples) if x), None)
    return {
        "bytes": len(data),
        "sha256": sha256_file(path),
        "peak": peak,
        "rms": math.sqrt(sq / len(samples)),
        "first_nonzero_sample": first,
    }

def run_capture(name, exe, rom, env_extra, args, timeout=TIMEOUT_SECONDS):
    case_dir = OUT / name
    case_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(env_extra)
    proc = subprocess.Popen(
        [str(exe)] + args + [rom], cwd=str(exe.parent), env=env,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    peaks = {"working_set": 0, "private": 0}
    done = False
    def sample_memory():
        while not done:
            sample = sample_process_memory(proc.pid)
            if sample:
                peaks["working_set"] = max(peaks["working_set"], sample[0])
                peaks["private"] = max(peaks["private"], sample[1])
            time.sleep(0.2)
    sampler = threading.Thread(target=sample_memory, daemon=True)
    sampler.start()
    try:
        try:
            stdout, stderr = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()
            raise SystemExit(f"{name}: timeout after {timeout}s")
    finally:
        done = True
        sampler.join(timeout=1)
    (case_dir / "stdout.txt").write_bytes(stdout)
    (case_dir / "stderr.txt").write_bytes(stderr)
    if proc.returncode != 0:
        raise SystemExit(f"{name}: exit {proc.returncode}")
    return case_dir, stdout, stderr, peaks

summary = {"audio": {}, "state": {}}
staged = []
for artifact in ARTIFACTS:
    for mode in ("native", "wide"):
        stage_dir, exe = stage_case(artifact, mode)
        staged.append((artifact, mode, stage_dir, exe))

for artifact, mode, stage_dir, exe in staged:
    name = f"audio/{artifact['title']}-{artifact['variant']}-{mode}"
    dsp = OUT / name / "dspout.s16le"
    env = {
        "SDL_AUDIODRIVER": "dummy",
        "SNESRECOMP_NO_LAUNCHER": "1",
        "SNESRECOMP_DSPOUT": str(dsp),
    }
    case_dir, stdout, stderr, peaks = run_capture(
        name, exe, artifact["rom"], env,
        ["--benchmark-audio-paced", str(FRAMES)])
    bench = parse_benchmark(stdout)
    health = bench.get("audio_health", {})
    if not health.get("ok"):
        raise SystemExit(f"{name}: audio_health not ok: {health}")
    if health.get("host_output_peak", 0) == 0:
        raise SystemExit(f"{name}: zero host_output_peak")
    if health.get("enqueue_failures", 0) != 0:
        raise SystemExit(f"{name}: enqueue failures")
    if health.get("dropped_audible", 0) != 0:
        raise SystemExit(f"{name}: audible drops")
    summary["audio"][name] = {
        "benchmark": bench,
        "signal": pcm_signal(dsp),
        "memory": peaks,
        "stage": str(stage_dir),
    }
    (case_dir / "summary.json").write_text(json.dumps(summary["audio"][name], indent=2))

for artifact, mode, stage_dir, exe in staged:
    name = f"state/{artifact['title']}-{artifact['variant']}-{mode}"
    case_root = OUT / name
    env = {
        "SNESRECOMP_NO_LAUNCHER": "1",
        "SNESRECOMP_FRAME_BMP_DIR": str(case_root / "bmp"),
        "SNESRECOMP_FRAME_BMP_START": "300",
        "SNESRECOMP_FRAME_BMP_END": "900",
        "SNESRECOMP_FRAME_BMP_STEP": "60",
        "SNESRECOMP_WRAM_TRACE_FILE": str(case_root / "wram.jsonl"),
        "SNESRECOMP_APURAM_TRACE_FILE": str(case_root / "apuram.jsonl"),
    }
    case_dir, stdout, stderr, peaks = run_capture(
        name, exe, artifact["rom"], env, ["--benchmark", str(FRAMES)])
    bmp_hashes = sorted(
        (p.name, sha256_file(p)) for p in (case_root / "bmp").glob("*.bmp"))
    summary["state"][name] = {
        "benchmark": parse_benchmark(stdout),
        "bmp_hashes": bmp_hashes,
        "wram": sha256_file(case_root / "wram.jsonl"),
        "apuram": sha256_file(case_root / "apuram.jsonl"),
        "stage": str(stage_dir),
    }
    (case_dir / "summary.json").write_text(json.dumps(summary["state"][name], indent=2))

for title in ("smw", "mmx"):
    for mode in ("native", "wide"):
        full = summary["state"][f"state/{title}-full-{mode}"]
        retained = summary["state"][f"state/{title}-retained-{mode}"]
        if full["bmp_hashes"] != retained["bmp_hashes"]:
            raise SystemExit(f"{title} {mode}: BMP mismatch")
        if full["wram"] != retained["wram"]:
            raise SystemExit(f"{title} {mode}: WRAM mismatch")
        if full["apuram"] != retained["apuram"]:
            raise SystemExit(f"{title} {mode}: APURAM mismatch")

(OUT / "summary.json").write_text(json.dumps(summary, indent=2))
'@ | py -3 -
```

## Remaining Final Window

Do not run these while another lane owns the execution token. After runtime
promotes SMALL as the default and produces fresh final artifacts, the remaining
short correctness window is:

1. PhaseON integration smoke for SMW and MMX.
2. Fresh-source SMK and SMRPG 3,000-frame headless routes.
3. Original94-vs-final state/video comparison for MMX, and SMW too if the
   quiet window is still available.

Use Python `subprocess.run`/`communicate` for host stdout/stderr capture.
PowerShell can orchestrate CMake, but avoid PowerShell native redirection for
paced or long title captures.

```powershell
$fw = "F:/Projects/snesrecomp/snesrecomp"
$oldfw = "F:/Projects/snesrecomp/_wt-snesrecomp-94c1197"
$smw = "F:/Projects/snesrecomp/SuperMarioWorldRecomp"
$mmx = "F:/Projects/snesrecomp/MegamanXRecomp"
$smk = "F:/Projects/snesrecomp/SuperMarioKartRecomp"
$smrpg = "F:/Projects/snesrecomp/SuperMarioRPGRecomp"
$cmake = "C:/msys64/mingw64/bin/cmake.exe"
$ninja = "C:/msys64/mingw64/bin/ninja.exe"
$cc = "C:/msys64/mingw64/bin/gcc.exe"
$cxx = "C:/msys64/mingw64/bin/g++.exe"
```

PhaseON smoke:

```powershell
& $cmake -S $smw -B "$smw/build-codex-final-phaseon" -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
  -DSNESRECOMP_ROOT=$fw -DSNESRECOMP_ENABLE_BENCHMARK_PHASES=ON `
  -DCMAKE_MAKE_PROGRAM=$ninja -DCMAKE_C_COMPILER=$cc -DCMAKE_CXX_COMPILER=$cxx
& $cmake --build "$smw/build-codex-final-phaseon" --config Release --parallel 1 --target SuperMarioWorldSNESRecomp

& $cmake -S $mmx -B "$mmx/build-codex-final-phaseon" -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
  -DSNESRECOMP_ROOT=$fw -DSNESRECOMP_ENABLE_BENCHMARK_PHASES=ON `
  -DCMAKE_MAKE_PROGRAM=$ninja -DCMAKE_C_COMPILER=$cc -DCMAKE_CXX_COMPILER=$cxx
& $cmake --build "$mmx/build-codex-final-phaseon" --config Release --parallel 1 --target MegaManXSNESRecomp

@'
import json, pathlib, re, subprocess
cases = [
    ("smw", r"F:/Projects/snesrecomp/SuperMarioWorldRecomp/build-codex-final-phaseon/SuperMarioWorldSNESRecomp.exe", r"F:/Projects/snesrecomp/SuperMarioWorldRecomp/smw.sfc"),
    ("mmx", r"F:/Projects/snesrecomp/MegamanXRecomp/build-codex-final-phaseon/MegaManXSNESRecomp.exe", r"F:/Projects/snesrecomp/MegamanXRecomp/mmx.sfc"),
]
out = pathlib.Path(r"F:/Projects/snesrecomp/snesrecomp/build/perf/final_phaseon_20260905")
out.mkdir(parents=True, exist_ok=True)
for name, exe, rom in cases:
    exe_path = pathlib.Path(exe)
    result = subprocess.run([str(exe_path), "--benchmark", "300", rom],
                            cwd=str(exe_path.parent), stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=90)
    case_dir = out / name
    case_dir.mkdir(exist_ok=True)
    (case_dir / "stdout.txt").write_bytes(result.stdout)
    (case_dir / "stderr.txt").write_bytes(result.stderr)
    if result.returncode:
        raise SystemExit(f"{name}: exit {result.returncode}")
    m = re.findall(rb"SNESRECOMP_BENCHMARK (\{.*\})", result.stdout)
    if not m:
        raise SystemExit(f"{name}: missing benchmark JSON")
    record = json.loads(m[-1].decode("utf-8", errors="replace"))
    if record.get("phase_timing") is not True:
        raise SystemExit(f"{name}: phase_timing not true")
    calls = record.get("phase_calls", {})
    for key in ("guest_frame", "ppu_draw", "host_present"):
        if calls.get(key, 0) <= 0:
            raise SystemExit(f"{name}: missing {key} phase calls")
'@ | py -3 -
```

Fresh SMK/SMRPG headless:

```powershell
& $cmake -S $smk -B "$smk/build-codex-final-headless" -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
  -DSNESRECOMP_ROOT=$fw -DSNESRECOMP_ENABLE_BENCHMARK_PHASES=ON `
  -DCMAKE_MAKE_PROGRAM=$ninja -DCMAKE_C_COMPILER=$cc -DCMAKE_CXX_COMPILER=$cxx
& $cmake --build "$smk/build-codex-final-headless" --config Release --parallel 1 --target SuperMarioKartSNESRecompHeadless

& $cmake -S $smrpg -B "$smrpg/build-codex-final-headless" -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
  -DSNESRECOMP_ROOT=$fw -DSNESRECOMP_ENABLE_BENCHMARK_PHASES=ON `
  -DCMAKE_MAKE_PROGRAM=$ninja -DCMAKE_C_COMPILER=$cc -DCMAKE_CXX_COMPILER=$cxx
& $cmake --build "$smrpg/build-codex-final-headless" --config Release --parallel 1 --target SuperMarioRPGSNESRecompHeadless

@'
import json, os, pathlib, re, subprocess
out = pathlib.Path(r"F:/Projects/snesrecomp/snesrecomp/build/perf/final_headless_20260905")
out.mkdir(parents=True, exist_ok=True)
cases = [
    ("smk", [r"F:/Projects/snesrecomp/SuperMarioKartRecomp/build-codex-final-headless/SuperMarioKartSNESRecompHeadless.exe",
             "--benchmark", "3000", r"F:/Projects/snesrecomp/SuperMarioKartRecomp/smk.sfc",
             "--input-file", r"F:/Projects/snesrecomp/SuperMarioKartRecomp/tools/smk-one-player-race.txt"], {}),
    ("smrpg", [r"F:/Projects/snesrecomp/SuperMarioRPGRecomp/build-codex-final-headless/SuperMarioRPGSNESRecompHeadless.exe",
               "--benchmark", "3000", r"F:/Projects/snesrecomp/SuperMarioRPGRecomp/smrpg.sfc"],
              {"SNESRECOMP_SAVE_ROOT": str(out / "smrpg_save_root"),
               "SNESRECOMP_SAVE_STATE_FRAME": "1200",
               "SNESRECOMP_LOAD_STATE_FRAME": "1800"}),
]
for name, args, extra_env in cases:
    exe = pathlib.Path(args[0])
    env = os.environ.copy()
    env.update(extra_env)
    result = subprocess.run(args, cwd=str(exe.parent), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=150)
    case_dir = out / name
    case_dir.mkdir(exist_ok=True)
    (case_dir / "stdout.txt").write_bytes(result.stdout)
    (case_dir / "stderr.txt").write_bytes(result.stderr)
    if result.returncode:
        raise SystemExit(f"{name}: exit {result.returncode}")
    m = re.findall(rb"SNESRECOMP_BENCHMARK (\{.*\})", result.stdout)
    if not m:
        raise SystemExit(f"{name}: missing benchmark JSON")
    record = json.loads(m[-1].decode("utf-8", errors="replace"))
    if record.get("frames") != 3000:
        raise SystemExit(f"{name}: wrong frame count {record.get('frames')}")
'@ | py -3 -
```

Original94-vs-final state/video comparison:

```powershell
@'
import hashlib, json, os, pathlib, re, subprocess

OUT = pathlib.Path(r"F:/Projects/snesrecomp/snesrecomp/build/perf/final_original94_state_20260905")
FRAMES = "3000"
CASES = [
    ("mmx-original94", r"<baseline94-mmx-exe>", r"F:/Projects/snesrecomp/MegamanXRecomp/mmx.sfc"),
    ("mmx-final", r"<final-small-default-mmx-exe>", r"F:/Projects/snesrecomp/MegamanXRecomp/mmx.sfc"),
    ("smw-original94", r"<baseline94-smw-exe>", r"F:/Projects/snesrecomp/SuperMarioWorldRecomp/smw.sfc"),
    ("smw-final", r"<final-small-default-smw-exe>", r"F:/Projects/snesrecomp/SuperMarioWorldRecomp/smw.sfc"),
]

def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

summary = {}
for name, exe, rom in CASES:
    if exe.startswith("<"):
        continue
    exe_path = pathlib.Path(exe)
    case_dir = OUT / name
    bmp_dir = case_dir / "bmp"
    bmp_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({
        "SNESRECOMP_NO_LAUNCHER": "1",
        "SNESRECOMP_FRAME_BMP_DIR": str(bmp_dir),
        "SNESRECOMP_FRAME_BMP_START": "300",
        "SNESRECOMP_FRAME_BMP_END": "900",
        "SNESRECOMP_FRAME_BMP_STEP": "60",
        "SNESRECOMP_WRAM_TRACE_FILE": str(case_dir / "wram.jsonl"),
        "SNESRECOMP_APURAM_TRACE_FILE": str(case_dir / "apuram.jsonl"),
    })
    result = subprocess.run([str(exe_path), "--benchmark", FRAMES, rom],
                            cwd=str(exe_path.parent), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=120)
    (case_dir / "stdout.txt").write_bytes(result.stdout)
    (case_dir / "stderr.txt").write_bytes(result.stderr)
    if result.returncode:
        raise SystemExit(f"{name}: exit {result.returncode}")
    summary[name] = {
        "bmp_hashes": sorted((p.name, sha(p)) for p in bmp_dir.glob("*.bmp")),
        "wram": sha(case_dir / "wram.jsonl"),
        "apuram": sha(case_dir / "apuram.jsonl"),
    }

for title in ("mmx", "smw"):
    a, b = f"{title}-original94", f"{title}-final"
    if a in summary and b in summary:
        for key in ("bmp_hashes", "wram", "apuram"):
            if summary[a][key] != summary[b][key]:
                raise SystemExit(f"{title}: {key} mismatch")
(OUT / "summary.json").write_text(json.dumps(summary, indent=2))
'@ | py -3 -
```

The broad title sweep is still a burn-down item. A read-only inventory on
2026-09-05 found these top-level title checkouts with configured upstream
remotes under `F:\Projects\snesrecomp`; no network fetch was performed, so
public accessibility still needs validation:

- Root-level ROM present and build directories present: Gundam Wing Endless
  Duel, Zelda ALttP, Mega Man X, Mega Man X2, Mega Man X3, SimCity, Star Fox,
  Super Mario RPG, Super Mario World, and Super Metroid.
- Root-level ROM present but no top-level build directory at inventory time:
  Super Mario Kart.
- Configured upstream remote present but root-level ROM missing: DKC2, F-Zero,
  and Metal Warriors. DKC2 and Metal Warriors had build directories; F-Zero
  did not.

Inventory artifact:
`build/perf/title_inventory_top_level_20260905.json`.

All builds on the shared Windows host use Release, BelowNormal priority, and
one job. Export `compile_commands.json` for definition audits.

```powershell
$fw = "<snesrecomp checkout>"
$oldfw = "<clean pre-helper framework checkout, e.g. baseline94>"
$smw = "<SuperMarioWorldRecomp checkout>"
$mmx = "<MegamanXRecomp checkout>"
$smk = "<SuperMarioKartRecomp checkout>"
$smrpg = "<SuperMarioRPGRecomp checkout>"

function Build-BelowNormal($dir, $target = $null) {
  $args = @("--build", $dir, "--config", "Release", "--parallel", "1")
  if ($target) { $args += @("--target", $target) }
  $p = Start-Process -FilePath "cmake" -ArgumentList $args `
      -NoNewWindow -PassThru
  $p.PriorityClass = "BelowNormal"
  $p.WaitForExit()
  if ($p.ExitCode -ne 0) { throw "build failed: $dir $target" }
}
```

1. Build SMW/MMX phase-off and phase-on desktop controls from the stabilized
   source.

```powershell
cmake -S $smw -B "$smw/build-perf-phaseoff" -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DSNESRECOMP_ROOT=$fw `
  -DSNESRECOMP_ENABLE_BENCHMARK_PHASES=OFF
Build-BelowNormal "$smw/build-perf-phaseoff" "SuperMarioWorldSNESRecomp"

cmake -S $smw -B "$smw/build-perf-phaseon" -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DSNESRECOMP_ROOT=$fw `
  -DSNESRECOMP_ENABLE_BENCHMARK_PHASES=ON
Build-BelowNormal "$smw/build-perf-phaseon" "SuperMarioWorldSNESRecomp"

cmake -S $mmx -B "$mmx/build-perf-phaseoff" -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DSNESRECOMP_ROOT=$fw `
  -DSNESRECOMP_ENABLE_BENCHMARK_PHASES=OFF
Build-BelowNormal "$mmx/build-perf-phaseoff" "MegaManXSNESRecomp"

cmake -S $mmx -B "$mmx/build-perf-phaseon" -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DSNESRECOMP_ROOT=$fw `
  -DSNESRECOMP_ENABLE_BENCHMARK_PHASES=ON
Build-BelowNormal "$mmx/build-perf-phaseon" "MegaManXSNESRecomp"
```

2. Build helper-absent controls by pointing each title at the clean older
   framework checkout. This verifies optional integration remains compatible
   with older vendored roots.

```powershell
cmake -S $smw -B "$smw/build-perf-oldhelper" -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DSNESRECOMP_ROOT=$oldfw `
  -DSNESRECOMP_ENABLE_BENCHMARK_PHASES=ON
Build-BelowNormal "$smw/build-perf-oldhelper" "SuperMarioWorldSNESRecomp"

cmake -S $mmx -B "$mmx/build-perf-oldhelper" -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DSNESRECOMP_ROOT=$oldfw `
  -DSNESRECOMP_ENABLE_BENCHMARK_PHASES=ON
Build-BelowNormal "$mmx/build-perf-oldhelper" "MegaManXSNESRecomp"
```

3. Audit compile definitions. Generated objects must not receive benchmark
   macros. The host consumer source must receive helper/phase macros, and
   optional `benchmark.c` must receive the same phase value.

```powershell
$builds = @(
  "$smw/build-perf-phaseoff", "$smw/build-perf-phaseon",
  "$mmx/build-perf-phaseoff", "$mmx/build-perf-phaseon")
@'
import json, pathlib, sys
for build in map(pathlib.Path, sys.argv[1:]):
    db = json.loads((build / "compile_commands.json").read_text())
    bad_gen = [e["file"] for e in db
               if ("/src/gen/" in e["file"].replace("\\", "/") and
                   "SNESRECOMP_BENCHMARK" in e["command"])]
    consumer = [e for e in db
                if e["file"].replace("\\", "/").endswith(("/src/main.c", "/src/headless_main.c"))]
    bench = [e for e in db if e["file"].replace("\\", "/").endswith("/runner/src/benchmark.c")]
    if bad_gen:
        raise SystemExit(f"{build}: generated benchmark defs: {bad_gen[:3]}")
    if bench and not all("SNESRECOMP_BENCHMARK_PHASES=" in e["command"] for e in bench):
        raise SystemExit(f"{build}: benchmark.c missing phase define")
    if consumer and not all("SNESRECOMP_HAS_BENCHMARK_HELPER=" in e["command"] for e in consumer):
        raise SystemExit(f"{build}: benchmark consumer missing helper define")
    print(f"{build}: compile defs OK")
'@ | py -3 - @builds
```

4. In phase-off builds, inspect the host object or disassembly to confirm the
   finite-loop hot path has no calls to `SnesRecompBenchmarkPhaseBegin` or
   `SnesRecompBenchmarkPhaseEnd`. The helper may still contain JSON printing
   functions when linked; the host loop must not call phase clocks when phases
   are off.

```powershell
ninja -C "$smw/build-perf-phaseoff" -t commands | Select-String "src/main.c"
ninja -C "$mmx/build-perf-phaseoff" -t commands | Select-String "src/main.c"
# Use the compiler toolchain's objdump/nm on the resulting main.c object:
# no relocation/call from main.c.obj to SnesRecompBenchmarkPhaseBegin/End.
```

5. Run short finite phase JSON smoke only after the build/timing token is
   granted. Phase-off records must be valid JSON with `"phase_timing":false`
   and zero phase seconds/calls. Phase-on records must be valid JSON with
   `"phase_timing":true`; `guest_frame`, `ppu_draw`, and `host_present` calls
   must be positive, and `--benchmark-audio` must report positive
   `audio_render` calls. A phase-on audio smoke also verifies
   `g_benchmark_phase_mutex` was initialized before the audio callback can
   update shared phase counters. For
   DSP correctness captures, use `--benchmark-audio-paced`; it reports
   `benchmark_mode:"audio_paced"` and an `audio_health` object. Treat
   `audio_health.ok:false`, zero `host_output_peak`, native
   `produced`/`consumed` below `min_native_samples`, or nonzero
   `dropped_audible` as a failed capture.

   Alternate CPU-orchestration smokes are still pending until actually run:

```powershell
$env:SNESRECOMP_EXECUTION_MODE = "hle"; & "<exe>" --benchmark 3000 "<rom.sfc>"
$env:SNESRECOMP_EXECUTION_MODE = "lle"; $env:SNESRECOMP_LLE_BOUNCE = "0"; & "<exe>" --benchmark 300 "<rom.sfc>"
```

Concrete phase-on smoke command shape for the final window:

```powershell
@'
import json, pathlib, re, subprocess

cases = [
    ("smw-phaseon", r"F:/Projects/snesrecomp/SuperMarioWorldRecomp/build-perf-phaseon/SuperMarioWorldSNESRecomp.exe", r"F:/Projects/snesrecomp/SuperMarioWorldRecomp/smw.sfc"),
    ("mmx-phaseon", r"F:/Projects/snesrecomp/MegamanXRecomp/build-perf-phaseon/MegaManXSNESRecomp.exe", r"F:/Projects/snesrecomp/MegamanXRecomp/mmx.sfc"),
]

out = pathlib.Path(r"F:/Projects/snesrecomp/snesrecomp/build/perf/phase_smoke_20260905")
out.mkdir(parents=True, exist_ok=True)
for name, exe, rom in cases:
    exe_path = pathlib.Path(exe)
    result = subprocess.run(
        [str(exe_path), "--benchmark", "300", rom],
        cwd=str(exe_path.parent), stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=90)
    case_dir = out / name
    case_dir.mkdir(exist_ok=True)
    (case_dir / "stdout.txt").write_bytes(result.stdout)
    (case_dir / "stderr.txt").write_bytes(result.stderr)
    if result.returncode != 0:
        raise SystemExit(f"{name}: exit {result.returncode}")
    text = result.stdout.decode("utf-8", errors="replace")
    matches = re.findall(r"SNESRECOMP_BENCHMARK (\{.*\})", text)
    if not matches:
        raise SystemExit(f"{name}: missing benchmark JSON")
    record = json.loads(matches[-1])
    if record.get("phase_timing") is not True:
        raise SystemExit(f"{name}: phase_timing not true")
    calls = record.get("phase_calls", {})
    for key in ("guest_frame", "ppu_draw", "host_present"):
        if calls.get(key, 0) <= 0:
            raise SystemExit(f"{name}: no {key} phase calls")
'@ | py -3 -
```

6. Build SMK and SMRPG headless long-correctness routes after SMW/MMX build
   checks. Both should run at least 3,000 frames and emit
   `SNESRECOMP_BENCHMARK` JSON with valid phase fields.

```powershell
cmake -S $smk -B "$smk/build-perf-headless" -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DSNESRECOMP_ROOT=$fw `
  -DSNESRECOMP_ENABLE_BENCHMARK_PHASES=ON
Build-BelowNormal "$smk/build-perf-headless" "SuperMarioKartSNESRecompHeadless"

cmake -S $smrpg -B "$smrpg/build-perf-headless" -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DSNESRECOMP_ROOT=$fw `
  -DSNESRECOMP_ENABLE_BENCHMARK_PHASES=ON
Build-BelowNormal "$smrpg/build-perf-headless" "SuperMarioRPGSNESRecompHeadless"

& "$smk/build-perf-headless/SuperMarioKartSNESRecompHeadless.exe" `
  --benchmark 3000 "<smk.sfc>" --input-file "$smk/tools/smk-one-player-race.txt"

$env:SNESRECOMP_SAVE_ROOT = "$smrpg/build-perf-headless/save-validation"
$env:SNESRECOMP_SAVE_STATE_FRAME = "1200"
$env:SNESRECOMP_LOAD_STATE_FRAME = "1800"
& "$smrpg/build-perf-headless/SuperMarioRPGSNESRecompHeadless.exe" `
  --benchmark 3000 "<smrpg.sfc>"
Remove-Item Env:\SNESRECOMP_SAVE_ROOT,Env:\SNESRECOMP_SAVE_STATE_FRAME,Env:\SNESRECOMP_LOAD_STATE_FRAME

$builds = @("$smk/build-perf-headless", "$smrpg/build-perf-headless")
# Re-run the compile-definition audit from step 3 on these headless builds.
```

Existing early-lane headless artifacts to rebuild or supersede in the final
window:

- SMK:
  `F:/Projects/snesrecomp/SuperMarioKartRecomp/build-codex-bench-p0/SuperMarioKartSNESRecompHeadless.exe`,
  SHA-256 `BE80AF62B9C57A8D133C66FDCDF72DF683E9149AF4348070286A454560178633`,
  bytes `6425077`.
- SMRPG:
  `F:/Projects/snesrecomp/SuperMarioRPGRecomp/build-codex-bench-p0/SuperMarioRPGSNESRecompHeadless.exe`,
  SHA-256 `5240962BED1BD947125BB0240E4CCA6EAEC0A0E9F8A34DF356A909E83D8D3F04`,
  bytes `957870`.

Both `build-codex-bench-p0` caches are Release, `SNESRECOMP_ROOT` pointing at
`F:/Projects/snesrecomp/snesrecomp`, `SNESRECOMP_ENABLE_TRACE=OFF`, and
`SNESRECOMP_ENABLE_BENCHMARK_PHASES=ON`. They do not contain
`compile_commands.json`, so reconfigure with `CMAKE_EXPORT_COMPILE_COMMANDS=ON`
before treating them as final audit artifacts. Concrete 3,000-frame commands
for these exact paths are:

```powershell
$smkExe = "F:/Projects/snesrecomp/SuperMarioKartRecomp/build-codex-bench-p0/SuperMarioKartSNESRecompHeadless.exe"
$smrpgExe = "F:/Projects/snesrecomp/SuperMarioRPGRecomp/build-codex-bench-p0/SuperMarioRPGSNESRecompHeadless.exe"
$smkRom = "F:/Projects/snesrecomp/SuperMarioKartRecomp/smk.sfc"
$smrpgRom = "F:/Projects/snesrecomp/SuperMarioRPGRecomp/smrpg.sfc"
$smkInput = "F:/Projects/snesrecomp/SuperMarioKartRecomp/tools/smk-one-player-race.txt"
$headlessOut = "F:/Projects/snesrecomp/snesrecomp/build/perf/headless_correctness_20260905"
New-Item -ItemType Directory -Force -Path $headlessOut | Out-Null

& $smkExe --benchmark 3000 $smkRom --input-file $smkInput `
  > "$headlessOut/smk_3000_stdout.txt" 2> "$headlessOut/smk_3000_stderr.txt"
if ($LASTEXITCODE -ne 0) { throw "SMK headless failed: $LASTEXITCODE" }

$env:SNESRECOMP_SAVE_ROOT = "$headlessOut/smrpg_save_root"
$env:SNESRECOMP_SAVE_STATE_FRAME = "1200"
$env:SNESRECOMP_LOAD_STATE_FRAME = "1800"
try {
  & $smrpgExe --benchmark 3000 $smrpgRom `
    > "$headlessOut/smrpg_3000_saveload_stdout.txt" `
    2> "$headlessOut/smrpg_3000_saveload_stderr.txt"
  if ($LASTEXITCODE -ne 0) { throw "SMRPG headless failed: $LASTEXITCODE" }
} finally {
  Remove-Item Env:\SNESRECOMP_SAVE_ROOT,Env:\SNESRECOMP_SAVE_STATE_FRAME,Env:\SNESRECOMP_LOAD_STATE_FRAME -ErrorAction SilentlyContinue
}
```

7. Stage native and enabled-widescreen PPU acceptance workloads only for a new
   measured PPU candidate. The 2026-09-05 composition helper refactor was
   rejected and removed, so final paced validation for the current optimization
   set should use the original PPU source from the stabilized framework build.
   The staging tool intentionally refuses implicit current artifact paths.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  "$fw/tools/prepare_ppu_workload_staging.ps1" -Force `
  -SmwCurrentDir "<stabilized-smw-candidate-dir>" `
  -SmwOriginalPpuDir "<same-base-smw-original-ppu-dir>" `
  -MmxCurrentDir "<stabilized-mmx-candidate-dir>" `
  -MmxOriginalPpuDir "<same-base-mmx-original-ppu-dir>" `
  -SmwCurrentExeName "<candidate-smw-exe-leaf>" `
  -MmxCurrentExeName "<candidate-mmx-exe-leaf>"
```

Run the generated `commands.ps1` only during an assigned quiet timing window.
Use the native and widescreen BMP/DSP/WRAM/APURAM captures as untimed
correctness checks for the final candidate after the audio-history/default
choice is settled.

## Source-only validation commands

These commands do not launch title binaries and can be run outside the quiet
timing window:

```powershell
git diff --check -- runner/src/audio_trace.c runner/src/audio_trace.h `
  runner/src/common_rtl.c runner/src/snes/interp_bridge.c `
  tests/audio/audio_trace_history_test.c `
  tests/runtime_dispatch/apu_port_diag_getenv_test.c `
  tests/runtime_dispatch/run_diagnostic_gates_test.ps1 `
  tools/prepare_audio_trace_variants.py tools/run_benchmark_pairs.py

py -3 -m py_compile tools/prepare_audio_trace_variants.py `
  tools/run_benchmark_pairs.py

py -3 tools/prepare_audio_trace_variants.py --help
py -3 tools/run_benchmark_pairs.py --help
```

The focused C/runtime checks and title timings below require the build/test
token because they compile or execute generated binaries:

```powershell
tests/runtime_dispatch/run_diagnostic_gates_test.ps1

# Audio trace history modes, using the MinGW compiler configured for the title
# builds. Compile and run SNESRECOMP_AUDIO_TRACE_HISTORY=0,1,2,3 against:
# tests/audio/audio_trace_history_test.c runner/src/audio_trace.c

& py -3 tools/run_benchmark_pairs.py --frames 3000 --pairs 5 ...
& py -3 tools/run_benchmark_pairs.py --frames 6000 --pairs 5 ...
```

Campaign validation already run by the optimization lanes should be carried
forward in the final handoff rather than rediscovered from scratch:

- Framework Python suite: grew from 84 to 93 checks during this campaign.
- v2 generator suite: 375 checks.
- interpreter/runtime-dispatch: interpreter 35/35, bridge 86/86,
  tier-2 capture PASS, non-local return/yield/resume/deadline PASS.
- CPU differential: 533 variants, 1,599,000 checks, 0 divergences.
- Coprocessor/peripheral/state gates: DSP-1 HLE save/restore PASS, plus
  DSP-1, SA-1, joypad, Super FX, and PPU focused gates.
- Benchmark/helper render lane: phase-off/on helper checks PASS, SMK 60-frame
  JSON/input smoke PASS, SMRPG 80-frame save/load JSON smoke PASS.
- Audio lane: diagnostic gates PASS and audio history modes 0/1/2/3 PASS.

Generated timing artifacts under `build/perf/**` and scratch relink outputs
are evidence, not source. Keep the JSON referenced above, but do not commit
large generated executables, copied DLL payloads, or one-off experimental
staging directories unless they become part of the maintained tooling surface.

# ma5play

**Play Yamaha SMAF (.mmf) ringtones in your browser — bit-exact audio via t386 WASM emulation of the original MA-5 synth DLLs.**

ma5play runs the real Yamaha `M5_EmuSmw5` / `M5_EmuHw` DLLs — the same code
that shipped in phone handsets — inside a 386 emulator compiled to
WebAssembly. Nothing is re-implemented or approximated: the closed-source
DLLs are executed instruction-by-instruction, so what you hear is
sample-identical to the original hardware.

Try it live: **<https://denjhang.github.io/ma5play_js/>** — the site ships
with a built-in library of 1,484 real ringtones (34 MB, the full test
corpus) and runs entirely static. No backend, no install.

---

## Feature tour

### Audio engine — bit-exact by construction

- **t386 emulation of the proprietary DLLs** (i386 + x87 FPU interpreter,
  PE loader, Win32 thunk layer) compiled to WASM with Emscripten.
- **960-frame (20 ms) pump chunks.** The DLL's effect chain keeps internal
  state across pump calls; we measured that larger chunks audibly corrupt
  time-varying effects (a wah-guitar patch turns into a broken filter).
  20 ms is exactly what the original GUI player used, and a full-track MD5
  of the browser-rendered PCM is **byte-identical to the native reference
  renderer** (`core/md5check.mjs`).
- **Compact preload boot** — the emulated machine boots from a
  zlib-compressed memory image (`ma5_ds.bin.z`, 4.3 MB → 18.9 MB),
  decompressed in the Worker via `DecompressionStream('deflate')`.
- **Worker rendering + message-passing flow control** — PCM crosses threads
  as transferable buffers with ~4 s of outstanding budget and per-block
  acks, so the UI thread never blocks on synthesis.
- **Clean track switching** — stop pump → 120 ms fade for in-flight blocks →
  suspend → flush queues → generation-tagged load. Zero residue from the
  previous track, no clicks.
- **Preroll gating** — playback starts only after a 2 s buffer is built;
  on underrun the player re-primes instead of stuttering.

### Visualization — synced to the real sound, not the scheduler

The piano keyboard and channel table are anchored to
`AudioContext.getOutputTimestamp()` — the estimated *actual sound output
moment* — interpolated against a calibrated anchor set in the audio
callback. This is why the keys light when you *hear* the note (tens of ms
of observed latency on MA-3 content), not when it is scheduled.

- **Piano keyboard** in the style of Modizer/ma2play: channel-colored keys,
  velocity-scaled brightness, multi-channel unisons split into horizontal
  bands, two-row layout on phones.
- **Channel table** mirroring the reference synth's internal note routing
  (this required porting the sequencer's decision tree — see
  [development history](#development-history)):
  - `Ch0-15` rows — FM-synthesized notes only
  - `P0-7` — PCM ROM drums (both MA-3 **and** MA-5)
  - `e<waveID>` — ext waveforms (user PCM voices), each row with its own
    live pitch
  - `s<idx>` — MA-5 Mwa audio streams
  - `ATR0/1` — MA-2 ADPCM tracks
  - rows are grouped with section headers, colored per ma2play's palette,
    and **sticky within a track** (values never reset to placeholders;
    the table is rebuilt only on track switch)

### Full SMAF parser — A/B-verified against the C++ reference

`web/mmf.js` is a line-by-line JS port of the reference C++ parser, and it
is continuously **diffed against the real thing**: the C++ sources are kept
under `ref/` and compiled into ground-truth tools (`count_events`,
`dump_events`, `test_vis_slots`). On the 1,484-file corpus, every file
matches the C++ implementation exactly — note/CC/PC/pitch-bend totals and
per-channel note counts.

Coverage: MA-1/2/3/5/7 version detection, titles/artists, MobileStandard,
**Huffman-compressed** sequences, HPS (4-channel), SEQU phrase format,
MMMG containers, ADPCM tracks, SysEx voice/waveform registration
(including 7-bit packed voice parameters and MA-5 raw 16-byte packets).

### Player & library UX

- File browser with breadcrumbs (right-priority with `...` overflow),
  folder history, last-dir memory, path entry mode
- **Built-in static library** — browse and play the full corpus on
  GitHub Pages with zero backend (`assets/tracks.json` index, automatic
  fallback from the dev `/api` server)
- **Staged loading progress** — five phases (download core / compile /
  download preload / decompress / init), each with live percentages; the
  compile phase (inherently atomic) is animated on a time curve capped at
  92% so the bar never freezes; downloads show MB/s
- Responsive layout for phone portrait and landscape; local file drag &
  drop; loop mode; auto-advance to next track

---

## Architecture

```
 Yamaha M5_EmuSmw5/M5_EmuHw DLLs (closed source)
        │  executed by t386 interpreter (i386 + FPU + PE loader)
        ▼
 ma5t C core ──emcc──► WASM ── boots from preload image (zlib, 4.3 MB)
        │
        ▼
 Web Worker ── 20 ms pump chunks (960 frames — effect-chain fidelity)
        │   postMessage transferable PCM + ack flow control
        ▼
 ScriptProcessorNode (48 kHz stereo)
        │   getOutputTimestamp() anchor
        ▼
 UI: piano + channel table + browser  ◄── mmf.js parser (routing model)
```

The visualization is parser-driven (the "VIS_PARSER" approach): the JS
parser replays the sequence once at load, classifies every note through
the ported `noteOn` routing tree, and the UI walks that timeline against
the audio clock.

## Development history

The project was built in verification-first increments; each layer was
pinned against ground truth before the next was added.

**Round 1 — make the core run at all.** Emscripten toolchain recovery
(a partial pacman upgrade had broken every binary in it), first WASM
build of the t386 core, node smoke test rendering a full track at 2.9×
realtime. Learned the hard way that the core must be the *compact-preload*
build (the GUI player's exact configuration), not the experimental flat
build — the wrong source produced tracks that stopped after 3.4 s.

**Round 2 — fidelity.** A wah-guitar patch sounded "broken, not clipped".
Channel-level traces (the methodology this project now mandates: compare
call chains, never audio) isolated the cause to pump chunk size: 50 ms
chunks corrupt the DLL's effect-chain state; 20 ms matches the GUI. MD5
of the rendered PCM became byte-identical with the native reference —
the project's central equivalence proof.

**Round 3 — a player people can use.** Worker rendering, flow control,
clean switching (the stop→fade→flush→load sequence with generation IDs),
preroll gating, the ma2play-style UI, mobile layouts, staged progress.

**Round 4 — visualization that tells the truth.** The piano was rebuilt
on the real audio clock after discovering that subtracting
`AudioContext.outputLatency` (1–2 s on some webviews) produced a
*lagging* visualization; anchoring on `getOutputTimestamp()` fixed it.

**Round 5 — the parser is rewritten by evidence.** Users reported missing
notes. We copied the three version-specific reference sequencer sources
(`libymf825_ma2/3/5`) into `ref/`, compiled the real C++ tools, and
diffed the JS parser against them file-by-file across all 1,484 corpus
files. Five decoder bugs fell out — most dramatically: Note-Off events
(`0x8X`) in Mobile format are *note events* with inherited velocity;
treating them as no-ops had silently dropped **80% of all notes**
(507 → 2,582 notes on one test track). Result: 1484/1484 files now match
the C++ parser exactly.

**Round 6 — route every note like the synth does.** The channel table was
rewired through a port of the sequencer's `noteOn` decision tree: FM vs
ROM drum vs ext waveform vs Mwa stream, with voice→waveform links decoded
from the SysEx packets exactly as the C++ does (7-bit packed parameters
for MA-3, raw 16-byte payloads for MA-5). Drums now light the PCM rows —
including MA-5, which the reference GUI had left unfinished.

**Round 7 — ship it.** Corpus embedded (34 MB, 1,484 tracks), static
GitHub Pages mode with automatic `/api` fallback, MIT license, release
packaging with checksums. Live at
<https://denjhang.github.io/ma5play_js/>.

### Known limitations

- **MA-2 tracks render below realtime on slow devices** (the t386 core's
  per-pump overhead; the MA-5/MA-3 path is comfortably realtime). Under
  investigation in the companion native project; the web player re-primes
  instead of stuttering in the meantime.
- MA-3 Mwa drum-RAM rows use a conservative fallback (lit while playing)
  until the live core-state snapshot lands.
- Roadmap: seek bar (core-side `ma5w_seek_play` is ready), AudioWorklet
  shell + 48-channel live snapshots, PWA/offline.

## Verification

- `core/md5check.mjs` — full-track PCM MD5 vs the native reference
  renderer (byte-identical at the 960-frame pump)
- Parser A/B — `ref/libymf825_ma3/count_events.exe` over the corpus,
  diffed against `mmf.js` (procedure in [docs/PROGRESS.md](docs/PROGRESS.md))
- `core/switch_test.mjs` — four consecutive track switches, checked for
  residue

## Repository layout

```
core/    emcc build + C shell (ma5w_* exports) + equivalence scripts
web/     player: app.js / mmf.js / render-worker.js / server.mjs /
         mktracks.mjs (static library index) / assets/ (wasm + corpus)
ref/     copies of the three libymf825 reference sources + voice banks
         + compiled C++ ground-truth tools (read-only reference material)
docs/    PROGRESS.md — detailed per-round engineering log
```

## Running locally

```sh
# 1. Build the WASM core (git bash; -O1 — see note)
/d/msys64/usr/bin/bash -lc "cd core && MA5PLAY_OPT=-O1 sh build.sh web"

# 2. Assemble assets + regenerate the library index
sh web/prepare.sh && node web/mktracks.mjs

# 3. Serve — either works
node web/server.mjs           # dev server with /api (browses any disk dir)
python -m http.server -d web  # plain static, exactly like Pages
```

Toolchain note: builds use msys2's Emscripten package; `-O2` is avoided
because `wasm-opt` hangs on this particular toolchain build — the build
script takes the optimization level via `MA5PLAY_OPT`.

## GitHub Pages deployment

`.github/workflows/pages.yml` deploys `web/` on every push to `main`.
The WASM core and the track library are committed, so there is no CI
build step. The Pages site must use **Source: GitHub Actions** (not
"deploy from branch", which would run Jekyll over the README).

## License

Original code in this repository (web player, C shell, JS parser, build
scripts) is released under the MIT License — see [LICENSE](LICENSE).
The WASM core embeds emulations of proprietary Yamaha DLLs; the preload
image and voice-bank assets originate from Yamaha SDKs and follow their
original terms. `ref/` contains unmodified third-party reference sources.

## Acknowledgements

- **dmplayer / ma2play** — the companion native project: the compact
  libma5t core, the reference GUI whose visualization this project
  mirrors, and the libymf825 parser/sequencer we port and continuously
  cross-check against
- **chip-player-js** (mmontag) — WASM shell conventions
- **Modizer** — keyboard visualization interaction concepts

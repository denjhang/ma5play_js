# ma5play

**Play Yamaha SMAF (.mmf) ringtones in your browser — bit-exact, via t386 WASM emulation of the original MA-5 synth DLL.**

ma5play runs the real Yamaha `M5_EmuSmw5/M5_EmuHw` DLLs (the same code phone
handsets shipped with) inside a 386 emulator compiled to WebAssembly. What you
hear is sample-identical to the original hardware: the render loop uses the
exact 20 ms pump chunk the original GUI player used (verified byte-identical
MD5 against the native reference renderer).

![layout](docs/screenshot-placeholder.png)

## Features

- **Bit-exact audio** — t386 emulation of the closed-source Yamaha MA-5 DLLs,
  48 kHz stereo, rendered in a Web Worker with message-passing flow control
- **Full SMAF parser** (JS port of the reference C++ parser, A/B-verified on a
  1,484-file corpus: every note/CC/PC/pitch-bend count and per-channel note
  count matches the C++ implementation exactly):
  - MA-1 / MA-2 / MA-3 / MA-5 / MA-7 detection, titles, Huffman-compressed and
    SEQU phrase formats, MMMG containers, ADPCM tracks (ATR)
- **ma2play-style visualization**, synced to the real audio clock
  (`getOutputTimestamp`, no lag):
  - piano keyboard with per-channel colors (same-note polyphony shown as
    horizontal splits)
  - channel table mirroring the reference synth's note routing: FM rows for
    MIDI channels, `P0-7` PCM ROM drum rows (MA-3 *and* MA-5), `e<waveID>`
    ext-waveform rows, `s<idx>` MA-5 stream rows, ATR rows for MA-2
  - values are sticky within a track; rows are rebuilt only on track switch
- **File browser** with breadcrumbs, folder history, last-dir memory, and a
  local-file picker (drag & drop)
- **Staged loading progress** — download / compile / decompress each with live
  percentages, speed readout, and smooth bar motion
- **Mobile-ready responsive layout** (portrait + landscape)

## Quick start

**Just browse the deployed site** — the repository ships a built-in library of
1,484 MMF ringtones (34 MB, the full test corpus) and runs entirely static:
no backend required. The file browser falls back to a pre-generated
`assets/tracks.json` index when the optional `/api` server is absent.

To run locally:

```sh
# 1. Build the WASM core (git bash; -O1 — see Toolchain notes)
/d/msys64/usr/bin/bash -lc "cd core && MA5PLAY_OPT=-O1 sh build.sh web"

# 2. Assemble web assets (wasm, preload image, icon) + regenerate tracks.json
sh web/prepare.sh
node web/mktracks.mjs

# 3. Serve
node web/server.mjs          # → http://127.0.0.1:8095  (adds /api file browser)
python -m http.server -d web # or any static server — works too
```

The dev server exposes `/api/list` + `/api/file` over any directory on disk;
in pure-static mode the built-in library is served from `web/assets/tracks/`.

## GitHub Pages deployment

`.github/workflows/pages.yml` deploys `web/` on every push to `main` — no CI
build step needed because the WASM core and the track library are committed.
Enable it once: **repo Settings → Pages → Source: GitHub Actions**.

If you rebuild the core or change the library, refresh the committed assets:

```sh
sh web/prepare.sh && node web/mktracks.mjs && git add web/assets
```

## How it works

```
ma5t C core (t386 + PE loader + preload image)
        │ emcc → WASM
        ▼
Web Worker ──20 ms pump chunks (960 frames — effect-chain fidelity)──►
        │  postMessage (transferable PCM, ack flow control)
        ▼
ScriptProcessorNode (48 kHz) ──audio-clock anchor──► UI visualization
```

- **Why 960-frame chunks?** The DLL's effect chain keeps internal state per
  pump call; larger chunks audibly corrupt effects (e.g. wah-guitar patches).
  960 frames is what the original GUI used.
- **Preload image** — the emulated DLL boots from a zlib-compressed memory
  image (`ma5_ds.bin.z`, 4.3 MB → 18.9 MB), decompressed in the worker via
  `DecompressionStream`.
- **Visualization data** — the JS parser ports the reference sequencer's
  `noteOn` routing tree (FM / ROM drums / ext waveforms / Mwa streams), with
  voice→waveID links decoded from the SysEx voice packets exactly as the
  C++ implementation does.

## Repository layout

```
core/    emcc build + C shell (ma5w_* exports) + equivalence test scripts
web/     player: app.js / mmf.js / render-worker.js / server.mjs / style.css
ref/     local copies of the three libymf825 reference sources + voice banks
         + compiled C++ ground-truth tools used to A/B-test the JS parser
docs/    progress log
```

`ref/` exists so the JS parser can be continuously diffed against the
reference implementation (count_events / dump_events / test_vis_slots). Treat
it as read-only reference material.

## Verification

- `core/md5check.mjs` — full-track MD5 of the rendered PCM against the native
  reference (byte-identical at the 960-frame pump)
- parser A/B — run `ref/libymf825_ma3/count_events.exe` over a corpus and
  diff against the JS parser (see docs/PROGRESS.md for the exact procedure)

## Roadmap

- [ ] Seek bar (core-side `ma5w_seek_play` is ready)
- [ ] AudioWorklet shell + 48-channel live state snapshots (replaces the
      parser-based table with real-time core state)
- [ ] PWA manifest + offline caching
- [ ] MA-2 render-efficiency improvements (reverse-engineering tracked in the
      companion native project)

## License

The original code in this repository (web player, C shell, build scripts,
JS parser) is released under the MIT License — see [LICENSE](LICENSE).
The WASM core embeds emulations of proprietary Yamaha DLLs and the preload
image / voice-bank assets originate from Yamaha SDKs; redistribution rights
for those follow their original terms.

## Acknowledgements

- The **dmplayer / ma2play** project — reference GUI player, the compact
  libma5t core, and the libymf825 reference parser & sequencer this project
  ports and continuously cross-checks against
- **chip-player-js** (mmontag) — WASM shell conventions
- **Modizer** — keyboard visualization interaction concepts

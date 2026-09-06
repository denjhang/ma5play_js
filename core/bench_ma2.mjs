import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { inflateSync } from 'node:zlib';
const req = createRequire(import.meta.url);
const COMPACT = 'D:/working/vscode-projects/YM2163-Midi/Denjhang_Music_Player_v16/ma2play/libma5t_compact';
const BASE = 'D:/working/vscode-projects/YM2163-Midi/Denjhang_Music_Player_v16/ma2play/bin/mmf/';
const FILES = [
  'Panasonic/G60/Dot Beat.mmf',
  'Panasonic/G50/Melody A.mmf',
  'Panasonic/X300/Sound 1.mmf',
  'LGRingTonesMMF/LG KG800 Chocolate ringtones/Sound_14.mmf',
  'Sharp/J-SH010/20 Disney - Wow an E-mail!.mmf',
  'DefleMask YMU759/Delek - Red Leaf.mmf',
  "MMF's for Samsung phones (from the Samsung PC Studio)/64poly/Melody03.mmf",
];
const m = await req('./build/ma5play_node.js')();
const img = inflateSync(readFileSync(COMPACT + '/ma5_ds.bin.z').subarray(4));
const pp = m._malloc(img.length); m.HEAPU8.set(img, pp); m._ma5w_set_preload(pp, img.length);
const dp = m._malloc(COMPACT.length + 1); m.HEAPU8.set(Buffer.from(COMPACT + '\0'), dp);
m._ma5w_init(dp);
const CHUNK = Number(process.argv[2] ?? 960);
const buf = m._malloc(CHUNK * 4);
for (const f of FILES) {
  let d;
  try { d = readFileSync(BASE + f); } catch { console.log(f.split('/').pop().padEnd(24), 'SKIP(no file)'); continue; }
  const p = m._malloc(d.length); m.HEAPU8.set(d, p);
  if (m._ma5w_load(p, d.length) !== 0) { console.log(f.split('/').pop().padEnd(24), 'LOAD FAIL'); continue; }
  let frames = 0, idle = 0;
  const t0 = performance.now();
  while (!m._ma5w_ended() && idle < 200 && frames < 48000 * 30) {
    const n = m._ma5w_pump(buf, CHUNK);
    if (n > 0) { frames += n; idle = 0; } else idle++;
  }
  const wall = (performance.now() - t0) / 1000;
  console.log(f.split('/').pop().padEnd(24), (frames / 48000).toFixed(1) + 's audio', wall.toFixed(1) + 's wall', (frames / 48000 / wall).toFixed(2) + 'x rt');
}

import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
const zlib = await import('node:zlib');
const req = createRequire(import.meta.url);
const COMPACT = 'D:/working/vscode-projects/YM2163-Midi/Denjhang_Music_Player_v16/ma2play/libma5t_compact';
const MMF = 'D:/working/vscode-projects/YM2163-Midi/Denjhang_Music_Player_v16/ma2play/bin/mmf/Samsung/Samsung SGH-D500 ringtones/Pre-downloaded/Melody05.mmf';

async function render(js) {
  const m = await req(js)();
  const img = zlib.inflateSync(readFileSync(COMPACT + '/ma5_ds.bin.z').subarray(4));
  const pp = m._malloc(img.length); m.HEAPU8.set(img, pp); m._ma5w_set_preload(pp, img.length);
  const dp = m._malloc(COMPACT.length + 1); m.HEAPU8.set(Buffer.from(COMPACT + '\0'), dp);
  m._ma5w_init(dp);
  const d = readFileSync(MMF);
  const p = m._malloc(d.length); m.HEAPU8.set(d, p);
  m._ma5w_load(p, d.length);
  const buf = m._malloc(2400 * 4);
  let frames = 0, idle = 0, hash = 0, peak = 0;
  while (!m._ma5w_ended() && idle < 300 && frames < 48000 * 60) {
    const n = m._ma5w_pump(buf, 2400);
    if (n > 0) {
      const b = m.HEAPU8.subarray(buf, buf + n * 4);
      for (let i = 0; i < b.length; i += 11) hash = (hash * 31 + b[i]) | 0;
      const s = new Int16Array(m.HEAPU8.buffer, m.HEAPU8.byteOffset + buf, n * 2);
      for (let i = 0; i < s.length; i++) { const a = Math.abs(s[i]); if (a > peak) peak = a; }
      frames += n; idle = 0;
    } else idle++;
  }
  return { frames, hash, peak };
}
const a = await render('./build/ma5play_node.js');       // -O1（现役 web 同款）
const b = await render('./build/O0dir/ma5play_node.js');    // -O0
console.log('O1 =', JSON.stringify(a));
console.log('O0 =', JSON.stringify(b));
console.log(a.hash === b.hash ? 'SAME: 与优化级别无关' : 'DIFFER: 编译器优化改变了输出（效果链被误编译）');

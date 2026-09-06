import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { inflateSync } from 'node:zlib';
import { createHash } from 'node:crypto';
const req = createRequire(import.meta.url);
const COMPACT = 'D:/working/vscode-projects/YM2163-Midi/Denjhang_Music_Player_v16/ma2play/libma5t_compact';
const MMF = 'D:/working/vscode-projects/YM2163-Midi/Denjhang_Music_Player_v16/ma2play/bin/mmf/Samsung/Samsung SGH-D500 ringtones/Pre-downloaded/Melody05.mmf';

const m = await req('./build/ma5play_node.js')();
const img = inflateSync(readFileSync(COMPACT + '/ma5_ds.bin.z').subarray(4));
console.log('映像解压 md5 =', createHash('md5').update(img).digest('hex').slice(0, 8), img.length + 'B');
const pp = m._malloc(img.length); m.HEAPU8.set(img, pp); m._ma5w_set_preload(pp, img.length);
const dp = m._malloc(COMPACT.length + 1); m.HEAPU8.set(Buffer.from(COMPACT + '\0'), dp);
m._ma5w_init(dp);
const d = readFileSync(MMF);
const p = m._malloc(d.length); m.HEAPU8.set(d, p);
m._ma5w_load(p, d.length);
const buf = m._malloc(960 * 4);
const chunks = [];
let frames = 0, idle = 0;
while (!m._ma5w_ended() && idle < 300) {
  const n = m._ma5w_pump(buf, 960);
  if (n > 0) { chunks.push(Buffer.from(m.HEAPU8.subarray(buf, buf + n * 4))); frames += n; idle = 0; }
  else idle++;
}
const pcm = Buffer.concat(chunks);
console.log('全曲', (frames / 48000).toFixed(1) + 's', 'md5 =', createHash('md5').update(pcm).digest('hex').slice(0, 8));
console.log('前30s md5 =', createHash('md5').update(pcm.subarray(0, 192000 * 30)).digest('hex').slice(0, 8));

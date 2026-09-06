// smoke.mjs — node 渲染冒烟测试
// 要求：加载 wasm → 渲染一段 → 出 PCM（非全零）。不做 wav/md5 对拍。
// 用法: node smoke.mjs <mmf路径> [秒数=5]   (cwd 需含 m5_snapshot.bin，flat 模式 fopen 读它)
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const createModule = require('./build/ma5play_node.js');

const mmfPath = process.argv[2];
const maxSec = Number(process.argv[3] ?? 5);
if (!mmfPath) { console.error('usage: node smoke.mjs <mmf> [sec=5]'); process.exit(1); }

const mmf = readFileSync(mmfPath);
const m = await createModule();

// compact 预载模式（文档验证路径：MA5T_PRELOAD + MA5T_EMBED 的 wasm 等价）
const preloadPath = process.env.MA5PLAY_PRELOAD ||
  'D:\\working\\vscode-projects\\YM2163-Midi\\Denjhang_Music_Player_v16\\ma2play\\libma5t_exp\\compact_preload.bin';
const preload = readFileSync(preloadPath);
const prePtr = m._malloc(preload.length);
m.HEAPU8.set(preload, prePtr);
m._ma5w_set_preload(prePtr, preload.length);
console.log('[smoke] preload image:', preloadPath, preload.length, 'bytes');

if (m._ma5w_init() !== 0) {
  console.error('init fail:', m._ma5w_last_error());
  process.exit(1);
}
console.log('[smoke] init ok, compact_mode =', m._ma5w_compact_mode());

const mmfPtr = m._malloc(mmf.length);
m.HEAPU8.set(mmf, mmfPtr);
if (m._ma5w_load(mmfPtr, mmf.length) !== 0) {
  console.error('load fail:', m._ma5w_last_error());
  process.exit(1);
}
if (m._ma5w_open_standby_start() !== 0) {
  console.error('open_standby_start fail:', m._ma5w_last_error());
  process.exit(1);
}
console.log('[smoke] loaded', mmf.length, 'bytes, standby started');

// 渲染循环（语义同 test_render_t.c：48kHz s16 stereo = 192000 B/s）
const pcmBuf = m._malloc(19200 * 16);
const limit = Math.floor(maxSec * 192000);
let got = 0, guard = 0, nonzero = 0, peak = 0, musicSeen = false;
const t0 = Date.now();
while (got < limit && guard++ < 50000) {
  m._ma5w_pump_seq();
  m._ma5w_pump_audio();
  const n = m._ma5w_take_pcm(pcmBuf, 19200 * 16);
  if (n) {
    const v = new DataView(m.HEAPU8.buffer, m.HEAPU8.byteOffset + pcmBuf, n);
    for (let i = 0; i + 1 < n; i += 2) {
      const s = v.getInt16(i, true);
      if (s) { nonzero++; const a = Math.abs(s); if (a > peak) peak = a; }
    }
    if (nonzero) musicSeen = true;
    got += n;
  } else if (musicSeen && ++guard % 1 === 0 && guard > 49000) break;
}
const wall = (Date.now() - t0) / 1000;
const audioSec = got / 192000;

// 冒烟判定：取到 PCM 且有非零样本（即“出声”）
if (got === 0 || nonzero === 0) {
  console.error(`[smoke] FAIL: got=${got} bytes, nonzero samples=${nonzero}`);
  process.exit(1);
}
console.log(`[smoke] PASS: ${got} bytes (${audioSec.toFixed(2)}s audio), ` +
  `nonzero=${nonzero}, peak=${peak}, wall=${wall.toFixed(2)}s, ` +
  `speed=${(audioSec / Math.max(wall, 0.001)).toFixed(2)}x realtime`);

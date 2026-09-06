// switch_test.mjs — 验证 wasm 切曲：同一实例连播三首
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const m = await require('./build/ma5play_node.js')();

const pre = readFileSync('D:/working/vscode-projects/YM2163-Midi/Denjhang_Music_Player_v16/ma2play/libma5t_exp/compact_preload.bin');
const pp = m._malloc(pre.length); m.HEAPU8.set(pre, pp); m._ma5w_set_preload(pp, pre.length);
if (m._ma5w_init() !== 0) throw new Error('init');
const buf = m._malloc(19200 * 16);

const play = f => {
  const d = readFileSync(f);
  const p = m._malloc(d.length); m.HEAPU8.set(d, p);
  if (m._ma5w_load(p, d.length) !== 0) throw new Error('load ' + f + ': ' + m._ma5w_last_error());
  if (m._ma5w_open_standby_start() !== 0) throw new Error('oss');
  let got = 0, nz = 0;
  for (let i = 0; i < 600 && got < 192000; i++) {   // 1s 音频
    m._ma5w_pump_seq(); m._ma5w_pump_audio();
    const n = m._ma5w_take_pcm(buf, 19200 * 16);
    for (let j = 0; j < n; j += 2) if (m.HEAPU8[buf + j]) { nz++; break; }
    got += n;
  }
  return { got, nzBlocks: nz };
};

const dir = "D:/working/vscode-projects/YM2163-Midi/Denjhang_Music_Player_v16/ma2play/bin/mmf/MMF's for Samsung phones (from the Samsung PC Studio)/64poly/";
for (const f of ['Melody01.mmf', 'Melody02.mmf', 'Melody03.mmf', 'Melody01.mmf']) {
  console.log(f, JSON.stringify(play(dir + f)));
}
console.log('SWITCH PASS');

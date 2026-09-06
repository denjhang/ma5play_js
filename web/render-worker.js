// render-worker.js — ma5t 渲染 Worker（消息传递版，无需 SharedArrayBuffer/COOP-COEP）
// Worker 独立线程 pump，按块 postMessage（transferable ArrayBuffer）给主线程消费；
// 主线程消费后回 ack，Worker 以未确认帧数做流控（保持 ~4s 在途预算）。
importScripts('assets/ma5play.js');           // 全局 ma5play 工厂
let M = null, pcmPtr = 0;
const FRAMES_PER_SEC = 48000;
const CHUNK_FRAMES = 2400;                    // 50ms/块
const ONSIDE_MAX = FRAMES_PER_SEC * 4;        // 在途上限

let outstanding = 0;                          // 未 ack 帧
let musicSeen = false, silentFrames = 0, noPcm = 0, ended = false, trackLoaded = false;

onmessage = async e => {
  const msg = e.data;
  if (msg.cmd === 'init') {
    try {
      M = await ma5play({ locateFile: p => 'assets/' + p });
    const img = await (await fetch('assets/preload.bin')).arrayBuffer();
    const ip = M._malloc(img.byteLength);
    new Uint8Array(M.HEAPU8.buffer, M.HEAPU8.byteOffset + ip, img.byteLength).set(new Uint8Array(img));
    M._ma5w_set_preload(ip, img.byteLength);
    if (M._ma5w_init() !== 0) { postMessage({ type: 'error', msg: 'init fail' }); return; }
    pcmPtr = M._malloc(4 * CHUNK_FRAMES);
    postMessage({ type: 'ready', compact: !!M._ma5w_compact_mode() });
    setInterval(pump, 50);
    } catch (e) { postMessage({ type: 'error', msg: 'init: ' + e.message }); }
  }
  if (msg.cmd === 'load' && M) {
    musicSeen = false; silentFrames = 0; noPcm = 0; ended = false;
    trackLoaded = true; outstanding = 0;
    const p = M._malloc(msg.buf.byteLength);
    new Uint8Array(M.HEAPU8.buffer, M.HEAPU8.byteOffset + p, msg.buf.byteLength).set(new Uint8Array(msg.buf));
    const ok = M._ma5w_load(p, msg.buf.byteLength) === 0 && M._ma5w_open_standby_start() === 0;
    postMessage({ type: 'loaded', ok, err: ok ? null : '' + M._ma5w_last_error() });
    if (ok) pump();
  }
  if (msg.cmd === 'ack') { outstanding -= msg.frames; if (outstanding < 0) outstanding = 0; }
};

let dbgN = 0;
function pump() {
  if (!M || !trackLoaded || ended) return;
  try {
  const t0 = performance.now();
  let audioMs = 0;
  while (outstanding < ONSIDE_MAX) {
    M._ma5w_pump_seq();
    M._ma5w_pump_audio();
    const bytes = M._ma5w_take_pcm(pcmPtr, 4 * CHUNK_FRAMES);
    if (bytes === 0) {
      if (musicSeen && ++noPcm > 5) { ended = true; postMessage({ type: 'end' }); return; }
      break;
    }
    noPcm = 0;
    const frames = bytes / 4;
    // 拷出堆（下一轮 take_pcm 会覆盖）
    const out = new Uint8Array(bytes);
    out.set(new Uint8Array(M.HEAPU8.buffer, M.HEAPU8.byteOffset + pcmPtr, bytes));
    let nz = false;
    for (let i = 0; i + 3 < bytes; i += 4) if (out[i] | out[i + 1] | out[i + 2] | out[i + 3]) { nz = true; break; }
    if (nz) { musicSeen = true; silentFrames = 0; }
    else if (musicSeen && (silentFrames += frames) > FRAMES_PER_SEC * 2) { ended = true; postMessage({ type: 'end' }); return; }
    outstanding += frames;
    audioMs += frames / 48;
    postMessage({ type: 'pcm', buf: out.buffer, frames }, [out.buffer]);
  }
  const wall = performance.now() - t0;
  if (wall > 5 && audioMs > 0) postMessage({ type: 'speed', v: Math.round((audioMs / wall * 1000) * 100) });
  if (++dbgN % 40 === 1) postMessage({ type: 'log', msg: `pump#${dbgN} out=${outstanding} audioMs=${audioMs.toFixed(0)}` });
  } catch (e) { postMessage({ type: 'error', msg: 'pump: ' + e.message }); }
}

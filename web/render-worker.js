// render-worker.js — ma5t 渲染 Worker（libma5t_compact 版，GUI ma5t 后端同源）
// 消息传递：按块 postMessage（transferable）给主线程消费，ack 流控（~4s 在途预算）。
// 曲终判定在 wasm shell 内部（no_pcm>100 + 出声后 2s 全零，同 ma5p_pump）。
importScripts('assets/ma5play.js');           // 全局 ma5play 工厂

let M = null, pcmPtr = 0;
const FRAMES_PER_SEC = 48000;
const CHUNK_FRAMES = 960;                     // 20ms/块——必须与 GUI/tp5 一致！
                                              // 50ms 块会破坏 DLL 效果链状态
                                              // （melody05 哇音损坏，md5 验证 960=原生）
const ONSIDE_MAX = FRAMES_PER_SEC * 4;
let outstanding = 0, trackId = 0, trackLoaded = false;

// 流式 fetch（带百分比进度回调）
async function fetchProgress(url, onPct) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  const total = Number(r.headers.get('Content-Length')) || 0;
  if (!r.body || !total) return new Uint8Array(await r.arrayBuffer());
  const reader = r.body.getReader();
  const chunks = [];
  let got = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value); got += value.length;
    onPct(Math.min(100, Math.round(got / total * 100)));
  }
  const out = new Uint8Array(got);
  let off = 0;
  for (const c of chunks) { out.set(c, off); off += c.length; }
  return out;
}

// ma5_ds.bin.z = 4 字节小端原始长度 + zlib 流（DecompressionStream 'deflate' = zlib 包装）
async function loadPreload(onPct) {
  const z = await fetchProgress('assets/ma5_ds.bin.z', p => onPct(50 + p * 0.2));  // 50~70%
  const rawLen = z[0] | (z[1] << 8) | (z[2] << 16) | (z[3] << 24);
  const ds = new DecompressionStream('deflate');
  const stream = new Blob([z.subarray(4)]).stream().pipeThrough(ds);
  const reader = stream.getReader();
  const out = new Uint8Array(rawLen);
  let got = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    out.set(value, got); got += value.length;
    onPct(70 + Math.min(100, got / rawLen * 100) * 0.25);                        // 70~95%
  }
  if (got !== rawLen) throw new Error(`preload len ${got} != ${rawLen}`);
  return out;
}

onmessage = async e => {
  const msg = e.data;
  if (msg.cmd === 'init') {
    try {
      const prog = (pct, label) => postMessage({ type: 'progress', pct: Math.round(pct), label });
      prog(2, '下载核心');
      const wasmBin = await fetchProgress('assets/ma5play.wasm', p => prog(2 + p * 0.43, '下载核心'));   // 2~45%
      prog(46, '编译核心');
      M = await ma5play({ wasmBinary: wasmBin.buffer, locateFile: p => 'assets/' + p });
      prog(50, '下载预载映像');
      const img = await loadPreload((p, l) => prog(p, l));
      prog(96, '初始化引擎');
      const ip = M._malloc(img.byteLength);
      new Uint8Array(M.HEAPU8.buffer, M.HEAPU8.byteOffset + ip, img.byteLength).set(img);
      M._ma5w_set_preload(ip, img.byteLength);
      if (M._ma5w_init(0) !== 0) { postMessage({ type: 'error', msg: 'init: ' + M._ma5w_last_error() }); return; }
      pcmPtr = M._malloc(4 * CHUNK_FRAMES);
      prog(100, '就绪');
      postMessage({ type: 'ready', compact: !!M._ma5w_compact_mode() });
      setInterval(pump, 50);
    } catch (e) { postMessage({ type: 'error', msg: 'init: ' + e.message }); }
  }
  if (msg.cmd === 'stop') { trackLoaded = false; }   // 立即停旧泵：切曲前先叫停，杜绝在途旧块
  if (msg.cmd === 'load' && M) {
    trackLoaded = true; outstanding = 0; trackId = msg.id;
    const p = M._malloc(msg.buf.byteLength);
    new Uint8Array(M.HEAPU8.buffer, M.HEAPU8.byteOffset + p, msg.buf.byteLength).set(new Uint8Array(msg.buf));
    const ok = M._ma5w_load(p, msg.buf.byteLength) === 0;
    postMessage({ type: 'loaded', id: trackId, ok, err: ok ? null : '' + M._ma5w_last_error() });
    if (ok) pump();
  }
  if (msg.cmd === 'pause' && M) M._ma5w_pause();
  if (msg.cmd === 'resume' && M) M._ma5w_resume();
  if (msg.cmd === 'seek' && M) M._ma5w_seek_play(msg.ms);
  if (msg.cmd === 'ack') { outstanding -= msg.frames; if (outstanding < 0) outstanding = 0; }
};

let dbgN = 0;
function pump() {
  if (!M || !trackLoaded) return;
  try {
    const t0 = performance.now();
    let audioMs = 0, wasEmpty = outstanding < CHUNK_FRAMES;
    while (outstanding < ONSIDE_MAX) {
      const n = M._ma5w_pump(pcmPtr, CHUNK_FRAMES);
      if (n > 0) {
        const out = new Uint8Array(n * 4);
        out.set(new Uint8Array(M.HEAPU8.buffer, M.HEAPU8.byteOffset + pcmPtr, n * 4));
        outstanding += n; audioMs += n / 48;
        postMessage({ type: 'pcm', id: trackId, buf: out.buffer, frames: n }, [out.buffer]);
        if (M._ma5w_ended()) break;
      } else {
        if (M._ma5w_ended()) break;
        break;                                  // 暂无数据（曲首忙等），下轮再来
      }
    }
    if (M._ma5w_ended()) postMessage({ type: 'end', id: trackId });
    // 速度只在"从空填满整段缓冲"的轮次测（稳态小口补充测不出真实吞吐）
    const wall = performance.now() - t0;
    if (wasEmpty && outstanding >= ONSIDE_MAX - CHUNK_FRAMES && wall > 5)
      postMessage({ type: 'speed', v: Math.round((audioMs / wall * 1000) * 100) });
    if (++dbgN % 80 === 1) postMessage({ type: 'log', msg: `pump#${dbgN} out=${outstanding}` });
  } catch (e) { postMessage({ type: 'error', msg: 'pump: ' + e.message }); }
}

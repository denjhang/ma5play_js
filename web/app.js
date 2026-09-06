// app.js — ma5play 测试播放器 v3
// 布局同 ma2play：左 File Info | 右上 钢琴+通道状态 | 右下 文件浏览器(含传输条)+Log
// 音频：Worker 渲染（独立线程，不受 UI 卡顿影响）→ 消息传递 PCM 块（transferable）
//   → 主线程帧队列 → ScriptProcessor(48kHz) 消费后 ack 流控。
// 钢琴 = parser 可视化（ma2play VIS_PARSER 同源思路）：MMF 乐谱音符时间轴按
// 实际播放时钟（playedFrames）点亮。48ch 通道快照待 AudioWorklet 阶段。
import { parseMMF } from './mmf.js';

const $ = id => document.getElementById(id);
const FRAMES_PER_SEC = 48000;
const PREROLL_FRAMES = FRAMES_PER_SEC * 2;
const fmt = s => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;
const status = (t, ok) => { $('status').textContent = t; $('status').classList.toggle('ok', !!ok); };

/* ---------------- Log ---------------- */
const logEl = $('log');
function log(msg) {
  const t = new Date();
  const ts = `${String(t.getHours()).padStart(2, '0')}:${String(t.getMinutes()).padStart(2, '0')}:${String(t.getSeconds()).padStart(2, '0')}`;
  const div = document.createElement('div');
  div.innerHTML = `<span class="t">${ts}</span> ${msg.replace(/</g, '&lt;')}`;
  logEl.appendChild(div);
  while (logEl.children.length > 200) logEl.firstChild.remove();
  logEl.scrollTop = logEl.scrollHeight;
}

/* ---------------- 状态 ---------------- */
const S = {
  workerReady: false, loaded: false, playing: false, loop: false, ended: false,
  priming: false, switching: false,
  name: null, curPath: null,
  meta: null,
  playedFrames: 0, speed: 0,
  queue: [],                    // 待播 PCM 块 [{i16: Int16Array, frames}]
  qFrames: 0,                   // 队列总帧数
};
let ctx = null, procNode = null, gainNode = null, muted = false;
let worker = null;

/* ---------------- 载入进度：分阶段 + 平滑推进（下载/解压=真实百分比，
 * 编译/初始化=原子阶段无子进度，用时间曲线推演到 92% 封顶，完成即贴合真值） ---------------- */
const LOAD_STAGES = [
  ['download-core',    'Downloading core',    30, '📥'],
  ['compile',          'Compiling core',      22, '⚙️'],
  ['download-preload', 'Downloading preload', 13, '📦'],
  ['decompress',       'Decompressing image', 25, '🔓'],
  ['init',             'Initializing engine', 10, '🚀'],
];
const loadUi = { idx: 0, pct: 0, atomic: false, stageT0: 0, extra: '', shown: -1, timer: null };
function stageGlobal(idx, pct) {
  let g = 0;
  for (let i = 0; i < idx; i++) g += LOAD_STAGES[i][2];
  return g + pct * LOAD_STAGES[idx][2] / 100;
}
function loadTick() {
  let real = stageGlobal(loadUi.idx, loadUi.pct);
  if (loadUi.atomic)                                  // 原子阶段：~8s 缓动到 92%
    real = Math.max(real, stageGlobal(loadUi.idx, Math.min(92, (Date.now() - loadUi.stageT0) / 80)));
  loadUi.shown = loadUi.shown < 0 ? real * 0.4
    : loadUi.shown + Math.max(0.12, (real - loadUi.shown) * 0.10);   // 永远追不上真值，平滑推进
  if (loadUi.shown > real) loadUi.shown = real;
  const [, label, w, ico] = LOAD_STAGES[loadUi.idx];
  const stagePct = Math.max(0, Math.min(100, Math.round((loadUi.shown - stageGlobal(loadUi.idx, 0)) / w * 100)));
  $('loadfill').style.width = loadUi.shown.toFixed(1) + '%';
  const extra = loadUi.extra ? ' · ' + loadUi.extra : '';
  status(`${ico} ${label} ${stagePct}%${extra}`, false);
  if (loadUi.shown >= 99.9) { clearInterval(loadUi.timer); loadUi.timer = null; }
}
function onLoadProgress(m) {
  const idx = LOAD_STAGES.findIndex(s => s[0] === m.stage);
  if (idx < 0) return;
  loadUi.idx = idx; loadUi.pct = m.pct; loadUi.atomic = !!m.atomic;
  loadUi.stageT0 = Date.now(); loadUi.extra = m.extra || '';
  $('loadbar').hidden = false;
  if (!loadUi.timer) loadTick(), loadUi.timer = setInterval(loadTick, 60);
}

/* ---------------- 渲染 Worker ---------------- */
function bootWorker() {
  worker = new Worker('render-worker.js');
  worker.onmessage = e => {
    const m = e.data;
    if (m.id !== undefined && m.id !== S.loadId) return;   // 旧代消息：切曲后迟到，丢弃
    if (m.type === 'progress') { onLoadProgress(m); return; }
    if (m.type === 'ready') {
      if (loadUi.timer) { clearInterval(loadUi.timer); loadUi.timer = null; }
      $('loadfill').style.width = '100%';
      setTimeout(() => { $('loadbar').hidden = true; }, 350);
      S.workerReady = true;
      $('chipMode').textContent = m.compact ? 'compact 预载' : 'flat';
      status('Core ready (worker)', true);
      log(`[core] ma5t worker ready (${m.compact ? 'compact preload' : 'flat'})`);
    } else if (m.type === 'pcm') {
      S.pcmRecv = (S.pcmRecv || 0) + 1;
      try {
        S.queue.push({ i16: new Int16Array(m.buf), frames: m.frames });
        S.qFrames += m.frames;
        if (!S.firstPcm) S.firstPcm = { frames: m.frames, bytes: m.buf.byteLength, qLen: S.queue.length };
      } catch (e) { if (S.pcmRecv < 3) log('[pcm] ' + e.message + ' buf=' + (m.buf ? m.buf.byteLength : 'null')); }
    } else if (m.type === 'speed') {
      S.speed = m.v / 100;
    } else if (m.type === 'loaded') {
      if (m.ok) {
        S.loaded = true; S.ended = false; S.playing = true; S.priming = true;
        S.playedFrames = 0; S.clkAt = undefined;
        $('trackName').textContent = S.name.replace(/\.mmf$/i, '');
        $('trackSize').textContent = (S.size / 1024).toFixed(1) + ' KB';
        showMeta();
        log(`[play] ${S.curPath || S.name}`);
        ensureAudio();
        if (ctx.state === 'running') ctx.suspend();   // 预滚：先攒缓冲再开声
        $('chipState').textContent = 'buffering';
        syncPlayBtn();
      } else {
        status('MaSound_Load failed', false);
        log(`[load] ${S.name}: ${m.err ?? 'MaSound_Load failed'}`);
      }
      S.switching = false;
    } else if (m.type === 'log') { log('[w] ' + m.msg); }
    else if (m.type === 'end') { S.coreEnded = true; }   // 核心渲染完毕：缓冲播完后才曲终
    else if (m.type === 'error') { status('核心错误: ' + m.msg, false); log('[core] ' + m.msg); }
  };
  worker.onerror = e => log('[worker] ' + e.message);
  worker.postMessage({ cmd: 'init' });
}
function showMeta() {
  const ma = S.meta?.version ? `MA-${S.meta.version}` : 'unknown';
  $('trackVer').textContent = ma;
  $('trackVer').style.color = ma === 'MA-5' ? 'var(--accent2)' : ma === '?' ? 'var(--dim)' : 'var(--accent)';
  $('trackTitle').textContent = S.meta?.title || '—';
  $('trackLen').textContent = S.meta?.durationMs ? fmt(S.meta.durationMs / 1000) : '—';
  $('timeEnd').textContent = '/ ' + (S.meta?.durationMs ? fmt(S.meta.durationMs / 1000) : '--:--');
  $('trackNotes').textContent = S.meta?.notes?.length ?? 0;
}

/* ---------------- 音频消费 ---------------- */
function ensureAudio() {
  if (ctx) return;
  ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'playback' });
  gainNode = ctx.createGain();
  applyVolume();
  gainNode.connect(ctx.destination);
  procNode = ctx.createScriptProcessor(4096, 0, 2);
  procNode.onaudioprocess = ev => {
    const L = ev.outputBuffer.getChannelData(0), R = ev.outputBuffer.getChannelData(1);
    S.spCalls = (S.spCalls || 0) + 1;
    let fed = 0, i = 0;
    let blk = S.queue[0];
    let bi = S.blkOff || 0;                    // 跨回调持久：半块不重播
    for (; i < L.length; i++) {
      if (!blk) break;
      const l = blk.i16[bi * 2], r = blk.i16[bi * 2 + 1];
      L[i] = l / 32768; R[i] = r / 32768;
      bi++; fed++;
      if (bi >= blk.frames) {
        S.queue.shift(); S.qFrames -= blk.frames;
        worker.postMessage({ cmd: 'ack', frames: blk.frames });
        blk = S.queue[0]; bi = 0;
      }
    }
    for (; i < L.length; i++) L[i] = R[i] = 0;  // 欠载补静音
    S.blkOff = blk ? bi : 0;
    S.spFed = (S.spFed || 0) + fed;
    if (fed) S.playedFrames += fed;
    // 可视化时钟锚点：音频回调（~85ms 一次）只做校准，帧间用 ctx.currentTime
    // 高频插值（ma2play 同步法），否则钢琴/通道表会按 85ms 台阶跳。
    S.clkSong = S.playedFrames / FRAMES_PER_SEC;
    S.clkAt = ctx.currentTime;
  };
  procNode.connect(gainNode);
  setInterval(tick, 100);                       // 定时器而非 rAF：后台/隐藏面板也要推进状态机
}
function applyVolume() { if (gainNode) gainNode.gain.value = muted ? 0 : $('vol').value / 100; }
/* 可视化时钟：以 getOutputTimestamp() 的 contextTime 为基准——那是浏览器
 * 报告的"此刻正在发声"的真实音频时刻，无需猜测/扣减任何延迟值。
 * 锚点（clkSong/clkAt）由音频回调校准，映射到发声时刻即完成解析器对齐。 */
function visClock() {
  if (!ctx || S.clkAt === undefined) return Math.max(0, (S.playedFrames || 0) / FRAMES_PER_SEC);
  let heardCtx = ctx.currentTime;                      // 回退：调度时刻
  try { const ts = ctx.getOutputTimestamp?.(); if (ts && ts.contextTime > 0) heardCtx = ts.contextTime; } catch { }
  return Math.max(0, S.clkSong + (heardCtx - S.clkAt));
}
function fadeIn() {
  const t = ctx.currentTime;
  gainNode.gain.cancelScheduledValues(t);
  gainNode.gain.setValueAtTime(Math.max(gainNode.gain.value, 0.0001), t);
  gainNode.gain.linearRampToValueAtTime(muted ? 0 : $('vol').value / 100, t + 0.04);
}

function tick() {
  if (!S.loaded) return;
  if (S.priming && S.playing && (S.qFrames >= PREROLL_FRAMES || (S.coreEnded && S.qFrames > 2400))) {
    // 预滚完成 → 开声（核心已渲染完时不足 2s 也开播——超短曲）
    S.priming = false;
    fadeIn();
    ctx.resume();
    $('chipState').textContent = 'playing';
  } else if (!S.priming && S.playing && ctx.state === 'running' && S.qFrames === 0) {
    if (S.coreEnded) songEnd();                 // 缓冲播完 + 核心完毕 = 曲终
    else if (!S.ended) {                        // 欠载：回预滚攒满再续
      S.priming = true;
      ctx.suspend();
      $('chipState').textContent = 'buffering';
    }
  } else if (S.coreEnded && S.qFrames === 0 && !S.ended && !S.priming) {
    songEnd();
  }
  updateUI();
}
async function loadTrack(name, buf, path) {
  if (!S.workerReady || S.switching) return false;
  S.switching = true;
  $('chipState').textContent = 'switch';
  try {
    worker.postMessage({ cmd: 'stop' });       // 先停旧泵：之后不再有新 PCM 产生
    if (ctx && gainNode && S.playing) {        // 淡出已排队的旧曲尾
      const t = ctx.currentTime;
      gainNode.gain.cancelScheduledValues(t);
      gainNode.gain.setValueAtTime(gainNode.gain.value, t);
      gainNode.gain.linearRampToValueAtTime(0, t + 0.05);
      await new Promise(r => setTimeout(r, 120));   // 淡出播完 + 在途消息全部送达
      ctx.suspend();
    } else {
      await new Promise(r => setTimeout(r, 30));    // 无需淡出也要等在途消息送达
    }
    S.playing = false; S.loaded = false; S.ended = false; S.coreEnded = false;
    S.name = name; S.curPath = path ?? null; S.size = buf.byteLength;
    S.playedFrames = 0; S.clkAt = undefined;
    S.queue = []; S.qFrames = 0; S.blkOff = 0;  // 此后不会再有旧代 PCM 进队
    S.meta = parseMMF(buf);
    piano.onTrack(S.meta);
    chOnTrack(S.meta);
    ensureAudio();
    S.loadId = (S.loadId || 0) + 1;
    worker.postMessage({ cmd: 'load', id: S.loadId, buf }, [buf]);
    return true;
  } catch (e) {
    log('[load] ' + e.message);
    S.switching = false;
    return false;
  }
}
function play() {
  if (!S.loaded) return;
  S.playing = true;
  if (S.priming || S.qFrames < PREROLL_FRAMES) {
    S.priming = true;
    $('chipState').textContent = 'buffering';
  } else {
    ctx.resume(); fadeIn();
    $('chipState').textContent = 'playing';
  }
  syncPlayBtn();
}
function pause() {
  S.playing = false;
  if (ctx && S.loaded) { $('chipState').textContent = 'paused'; ctx.suspend(); }
  syncPlayBtn();
}
function stop() {
  pause(); S.playedFrames = 0; S.clkAt = undefined; piano.onTrack(S.meta); chOnTrack(S.meta);
  $('chipState').textContent = 'stop'; updateUI();
}
function syncPlayBtn() { $('btnPlay').textContent = S.playing ? '⏸' : '▶'; }
function songEnd() {
  if (S.ended) return;
  S.ended = true; S.playing = false;
  $('chipState').textContent = 'end';
  syncPlayBtn();
  log(`[play] end of track @ ${fmt(S.playedFrames / FRAMES_PER_SEC)}`);
  if (S.loop) setTimeout(() => replayCurrent(), 500);
  else shiftTrack(1, true);
}
async function replayCurrent() {
  if (!S.curPath) return;
  const r = await fetch('/api/file?path=' + encodeURIComponent(S.curPath));
  if (r.ok) loadTrack(S.name, await r.arrayBuffer(), S.curPath);
}

/* ---------------- 钢琴（照抄 ma2play RenderPianoArea） ---------------- */
const CH_COLORS = [                            // kChColors[16]
  [80, 220, 80], [80, 140, 255], [255, 80, 80], [255, 180, 60],
  [180, 80, 255], [80, 220, 220], [255, 220, 80], [220, 80, 180],
  [140, 200, 80], [80, 180, 180], [200, 140, 80], [180, 100, 140],
  [100, 160, 220], [220, 160, 160], [160, 220, 160], [200, 200, 100],
];
const CH_NAMES = Array.from({ length: 16 }, (_, i) => 'Ch' + i);
const piano = (() => {
  const c = $('piano'), g = c.getContext('2d');
  const MIN_NOTE = 12, MAX_NOTE = 107;            // C0..B8（同 ma2play）
  const isBlack = n => [1, 3, 6, 8, 10].includes(n % 12);
  const KEY_RATIO = 6.2;                          // 白键长 = 6.2×白键宽（真钢琴 ~150/23.5mm）
  let notes = [], cursor = 0, layout = null;
  const blendKey = (col, lv, black) => {
    const b = black ? 20 : 255, bl = 0.55 + lv * 0.45;
    return `rgb(${b + ((col[0] - b) * bl) | 0},${b + ((col[1] - b) * bl) | 0},${b + ((col[2] - b) * bl) | 0})`;
  };
  // 布局：窄屏（<700px）白键对半拆两行；行高 = 键宽×真比例（上限防过长）
  function relayout(cssW) {
    const shortVp = window.innerHeight < 520;               // 手机横屏等矮视口
    const rows = (cssW < 700 || shortVp) ? 2 : 1;
    let numWhite = 0;
    for (let n = MIN_NOTE; n <= MAX_NOTE; n++) if (!isBlack(n)) numWhite++;
    const wkW = cssW / (rows === 1 ? numWhite : Math.ceil(numWhite / 2));
    const narrow = cssW < 700;
    const wkH = Math.min(Math.round(wkW * KEY_RATIO), (shortVp || narrow) ? 60 : 86);
    const rowRanges = [];
    if (rows === 1) rowRanges.push([MIN_NOTE, MAX_NOTE]);
    else {
      const half = Math.ceil(numWhite / 2);
      let seen = 0, split = MAX_NOTE;
      for (let n = MIN_NOTE; n <= MAX_NOTE; n++) {
        if (!isBlack(n)) { if (seen === half) { split = n - 1; break; } seen++; }
      }
      rowRanges.push([MIN_NOTE, split], [split + 1, MAX_NOTE]);
    }
    return { wkW, wkH, rowRanges, cssH: rows * wkH + (rows - 1) * 4 };
  }
  function draw() {
    requestAnimationFrame(draw);
    const cssW = c.clientWidth || 600;
    if (!layout || layout.forW !== cssW || layout.forH !== window.innerHeight) {
      layout = relayout(cssW); layout.forW = cssW; layout.forH = window.innerHeight;
      const dpr = window.devicePixelRatio || 1;
      c.width = cssW * dpr; c.height = layout.cssH * dpr;
      c.style.height = layout.cssH + 'px';
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
    }
    // 同步时钟：音频回调锚点 + currentTime 插值（visClock），减输出延迟
    const t = visClock();
    const active = new Map();
    const FADE = 0.12;                             // 键释放渐隐（秒）
    while (cursor < notes.length && notes[cursor].end < t - 0.5) cursor++;
    for (let i = cursor; i < notes.length; i++) {
      const nt = notes[i];
      if (nt.t > t) break;
      if (nt.note < 0 || nt.note > 127) continue;
      let lv;
      if (nt.end >= t) lv = Math.min(1, Math.max(0.15, nt.vel / 127));          // 按住
      else if (nt.end > t - FADE) lv = Math.max(0, nt.vel / 127) * (1 - (t - nt.end) / FADE);  // 释放渐隐
      else continue;
      if (lv <= 0.02) continue;
      const list = active.get(nt.note) ?? [];
      list.push({ lv, ch: nt.ch });
      active.set(nt.note, list);
      // 首个点亮日志：与静态解析摘要对比（t 应吻合 notes[0].t ± 延迟）
      if (!draw.litLogged && nt.end >= t) {
        draw.litLogged = true;
        log(`[vis] piano first key lit @ t=${t.toFixed(2)}s note=${noteName(nt.note)} ch=${nt.ch}`);
      }
    }
    const { wkW, wkH, rowRanges } = layout;
    const bkW = wkW * 0.65, bkH = wkH * 0.62;
    g.clearRect(0, 0, cssW, layout.cssH);
    rowRanges.forEach(([lo, hi], r) => {
      const y = r * (wkH + 4);
      let wkIdx = 0;
      for (let n = lo; n <= hi; n++) {                        // Pass 1: 白键
        if (isBlack(n)) continue;
        const x = wkIdx * wkW;
        const a = active.get(n);
        if (a?.length) {                            // 多通道同音：水平切分（上下堆叠），各通道一色
          const sh = wkH / a.length;
          a.forEach((seg, si) => {
            g.fillStyle = blendKey(CH_COLORS[seg.ch], seg.lv, false);
            g.fillRect(x, y + si * sh, wkW - 1, sh);
          });
        } else {
          g.fillStyle = 'rgb(255,255,255)';
          g.fillRect(x, y, wkW - 1, wkH);
        }
        g.strokeStyle = 'rgb(80,80,80)';
        g.strokeRect(x + 0.5, y + 0.5, wkW - 1, wkH - 1);
        if (n % 12 === 0) {                          // C 音名：始终显示（手机两行也标）
          g.fillStyle = 'rgba(0,0,0,0.7)';
          g.font = wkW > 14 ? '9px system-ui' : '8px system-ui';
          g.fillText(`C${(n / 12) - 1}`, x + 1, y + wkH - 3);
        }
        if (a?.length && wkW > 12) {
          g.fillStyle = 'rgba(0,0,0,0.78)';
          g.font = '8px system-ui';
          g.fillText(CH_NAMES[a[0].ch], x + 1, y + 10);
        }
        wkIdx++;
      }
      wkIdx = 0;
      for (let n = lo; n <= hi; n++) {                        // Pass 2: 黑键
        if (!isBlack(n)) { wkIdx++; continue; }
        const x = (wkIdx - 1) * wkW + wkW - bkW * 0.5;
        const a = active.get(n);
        if (a?.length) {
          const sh = bkH / a.length;
          a.forEach((seg, si) => {
            g.fillStyle = blendKey(CH_COLORS[seg.ch], seg.lv, true);
            g.fillRect(x, y + si * sh, bkW, sh);
          });
        } else {
          g.fillStyle = 'rgb(20,20,20)';
          g.fillRect(x, y, bkW, bkH);
        }
        g.strokeStyle = 'rgb(0,0,0)';
        g.strokeRect(x + 0.5, y + 0.5, bkW - 1, bkH - 1);
        if (a?.length && bkW > 8) {
          g.fillStyle = 'rgba(255,255,255,0.78)';
          g.font = '8px system-ui';
          g.fillText(CH_NAMES[a[0].ch], x + 1, y + 10);
        }
      }
    });
    // 通道表挂在钢琴 rAF 里（~50ms 节流），与时钟同帧更新，不另走定时器
    if (S.loaded && (draw.chT = (draw.chT || 0) + 1) % 3 === 0) updateChTable(t);
  }
  requestAnimationFrame(draw);
  return { onTrack(meta) { notes = (meta?.notes ?? []).slice().sort((a, b) => a.t - b.t); cursor = 0; draw.litLogged = false; } };
})();

/* ---------------- UI ---------------- */
function updateUI() {
  const heard = S.playedFrames / FRAMES_PER_SEC;
  $('timeCur').textContent = fmt(heard);
  const total = S.meta?.durationMs ? S.meta.durationMs / 1000 : 300;
  $('bar').style.width = Math.min(100, (heard / total) * 100) + '%';
  $('chipSpeed').textContent = S.speed ? S.speed.toFixed(2) + 'x rt' : '—';
}
/* ---------------- 通道表（ma2play 通道可视化同款：Ch/Prog/Note/Vol/Pan/Exp/Event） ---------------- */
const GM_NAMES = ['Acoustic Grand Piano','Bright Acoustic Piano','Electric Grand Piano','Honky-tonk Piano','Electric Piano 1','Electric Piano 2','Harpsichord','Clavinet','Celesta','Glockenspiel','Music Box','Vibraphone','Marimba','Xylophone','Tubular Bells','Dulcimer','Drawbar Organ','Percussive Organ','Rock Organ','Church Organ','Reed Organ','Accordion','Harmonica','Tango Accordion','Acoustic Guitar (nylon)','Acoustic Guitar (steel)','Electric Guitar (jazz)','Electric Guitar (clean)','Electric Guitar (muted)','Overdriven Guitar','Distortion Guitar','Guitar Harmonics','Acoustic Bass','Electric Bass (finger)','Electric Bass (pick)','Fretless Bass','Slap Bass 1','Slap Bass 2','Synth Bass 1','Synth Bass 2','Violin','Viola','Cello','Contrabass','Tremolo Strings','Pizzicato Strings','Orchestral Harp','Timpani','String Ensemble 1','String Ensemble 2','SynthStrings 1','SynthStrings 2','Choir Aahs','Voice Oohs','Synth Voice','Orchestra Hit','Trumpet','Trombone','Tuba','Muted Trumpet','French Horn','Brass Section','Synth Brass 1','Synth Brass 2','Soprano Sax','Alto Sax','Tenor Sax','Baritone Sax','Oboe','English Horn','Bassoon','Clarinet','Piccolo','Flute','Recorder','Pan Flute','Blown Bottle','Shakuhachi','Whistle','Ocarina','Lead 1 (square)','Lead 2 (sawtooth)','Lead 3 (calliope)','Lead 4 (chiff)','Lead 5 (charang)','Lead 6 (voice)','Lead 7 (fifths)','Lead 8 (bass + lead)','Pad 1 (new age)','Pad 2 (warm)','Pad 3 (polysynth)','Pad 4 (choir)','Pad 5 (bowed)','Pad 6 (metallic)','Pad 7 (halo)','Pad 8 (sweep)','FX 1 (rain)','FX 2 (soundtrack)','FX 3 (crystal)','FX 4 (atmosphere)','FX 5 (brightness)','FX 6 (goblins)','FX 7 (echoes)','FX 8 (sci-fi)','Sitar','Banjo','Shamisen','Koto','Kalimba','Bagpipe','Fiddle','Shanai','Tinkle Bell','Agogo','Steel Drums','Woodblock','Taiko Drum','Melodic Tom','Synth Drum','Reverse Cymbal','Guitar Fret Noise','Breath Noise','Seashore','Bird Tweet','Telephone Ring','Helicopter','Applause','Gunshot'];
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const GM_DRUMS = { 35:'Acoustic Bass Drum',36:'Bass Drum 1',37:'Side Stick',38:'Acoustic Snare',39:'Hand Clap',40:'Electric Snare',41:'Low Floor Tom',42:'Closed Hi-Hat',43:'High Floor Tom',44:'Pedal Hi-Hat',45:'Low Tom',46:'Open Hi-Hat',47:'Low-Mid Tom',48:'Hi-Mid Tom',49:'Crash Cymbal 1',50:'High Tom',51:'Ride Cymbal 1',52:'Chinese Cymbal',53:'Ride Bell',54:'Tambourine',55:'Splash Cymbal',56:'Cowbell',57:'Crash Cymbal 2',58:'Vibraslap',59:'Ride Cymbal 2',60:'Hi Bongo',61:'Low Bongo',62:'Mute Hi Conga',63:'Open Hi Conga',64:'Low Conga',65:'High Timbale',66:'Low Timbale',67:'High Agogo',68:'Low Agogo',69:'Cabasa',70:'Maracas',71:'Short Whistle',72:'Long Whistle',73:'Short Guiro',74:'Long Guiro',75:'Claves',76:'Hi Wood Block',77:'Low Wood Block',78:'Mute Cuica',79:'Open Cuica',80:'Mute Triangle',81:'Open Triangle' };
const noteName = n => n >= 0 && n <= 127 ? NOTE_NAMES[n % 12] + ((n / 12 | 0) - 1) : '--';
const chVis = { chans: [] };

function buildChGrid() {
  $('chTable').innerHTML = `<colgroup><col style="width:9%"><col style="width:34%"><col style="width:17%"><col style="width:8%"><col style="width:9%"><col style="width:8%"><col style="width:15%"></colgroup><thead><tr><th>Ch</th><th>Prog</th><th>Note</th><th>Vol</th><th>Pan</th><th>Exp</th><th>Event</th></tr></thead>`;
  chVis.chans = Array.from({ length: 16 }, () => ({ ev: [], notes: [], ei: 0, pc: -1, vol: 100, pan: 64, exp: 127, last: '--', act: false, used: false, progShown: '' }));
}
function chOnTrack(meta) {
  // ma2play 原则：切曲时一次性全扫定型——通道集合、各通道首个乐器/状态
  // 预解析；播放期间行数与乐器名（粘性）不再增删重置。
  for (const c of chVis.chans) { c.ev = []; c.notes = []; c.ei = 0; c.pc = -1; c.vol = 100; c.pan = 64; c.exp = 127; c.last = '--'; c.note = -1; c.lastNote = -1; c.act = false; c.used = false; c.progShown = ''; c.bankM = 0; c.bankL = 0; }
  for (const e of meta?.chEv ?? []) { const c = chVis.chans[e.ch]; if (c) { c.ev.push(e); c.used = true; } }
  for (const n of meta?.notes ?? []) { const c = chVis.chans[n.ch]; if (c) { c.notes.push(n); c.used = true; } }
  for (const c of chVis.chans) {
    c.ev.sort((a, b) => a.t - b.t); c.notes.sort((a, b) => a.t - b.t);
    // 全扫：取首个 PC/CC 作为初始显示（粘性起点，不等时间轴到达）
    for (const e of c.ev) { if (c.pc < 0 && e.k === 'PC') c.pc = e.pc; if (c.pc < 0) break; }
    for (const e of c.ev) { if (e.k === 'CC') { if (e.cc === 7) { c.vol = e.v; break; } } }
  }
  // 分组标题 + 只显示用到的通道（数量切曲时定型）+ 分版本的非 MIDI 通道行（ma2play 设计）
  const tb = $('chTable');
  tb.querySelectorAll('tbody').forEach(b => b.remove());
  const body = document.createElement('tbody');
  const addGroup = title => {
    const tr = document.createElement('tr');
    tr.className = 'chgroup';
    tr.innerHTML = `<td colspan="7">${title}</td>`;
    body.appendChild(tr);
  };
  let anyMidi = chVis.chans.some(c => c.used);
  if (anyMidi) addGroup('MIDI Channels');
  chVis.chans.forEach((c, i) => {
    if (!c.used) return;
    const tr = document.createElement('tr');
    tr.id = 'chrow' + i;
    tr.className = 'chrow';
    tr.style.borderLeft = `3px solid rgb(${CH_COLORS[i].join(',')})`;
    tr.innerHTML = `<td>Ch${i}</td><td class="prog">—</td><td class="note">--</td><td class="vol">--</td><td class="pan">--</td><td class="exp">--</td><td class="evt">--</td>`;
    body.appendChild(tr);
  });
  buildNonMidiRows(body, meta, addGroup);
  tb.appendChild(body);
  // 日志摘要
  const nNotes = meta?.notes?.length ?? 0;
  const usedCh = chVis.chans.map((c, i) => c.used ? i : -1).filter(i => i >= 0);
  const last = meta?.notes?.length ? meta.notes.reduce((m2, n) => n.end > m2 ? n.end : m2, 0) : 0;
  log(`[vis] parsed: ${nNotes} notes, ch=[${usedCh.join(',')}], span 0→${last.toFixed(2)}s (dur ${(meta?.durationMs / 1000 || 0).toFixed(1)}s)`);
  if (!nNotes) log('[vis] ⚠ no notes parsed (compressed/SEQU format not supported for piano)');
}
/* 非 MIDI 通道行（ma2play 同款设计，parser 静态数据源）：
 * MA-2 = ATR0/1（ADPCM 音轨）；MA-3 = P0-7（ROM 鼓）+ wave 行；
 * MA-5 = wave 行。wave 行 Ch 标签 e<id>/m<idx>（ext=m<waveID>, mwa=m<idx>），
 * 来源标签 ext/mwa，配色照抄（ext 青 / mwa 紫 / stream 橙 / ATR 琥珀）。 */
const ROW_COLORS = { atr: 'rgb(230,179,77)', ext: 'rgb(79,199,230)', mwa: 'rgb(217,140,230)', stream: 'rgb(230,171,79)' };
/* PCM 行 8 色板（ma2play kPcmRowCol，与音量条同色系） */
const PCM_ROW_COLORS = ['rgb(120,230,181)', 'rgb(181,140,240)', 'rgb(240,140,181)', 'rgb(140,219,240)', 'rgb(219,199,120)', 'rgb(181,240,140)', 'rgb(240,161,219)', 'rgb(161,199,219)'];
function buildNonMidiRows(body, meta, addGroup) {
  chVis.extra = [];
  // PCM 音色 drum 键表：(bankL:pc) -> Set(drum)——MA-5 鼓行与流区分的依据
  chVis.drumKeys = new Map();
  for (const v of meta?.voices ?? []) {
    if (v.vtype === 'FM' || v.vtype == null || v.pc === undefined) continue;
    const k = (v.bankL ?? 0) + ':' + v.pc;
    if (!chVis.drumKeys.has(k)) chVis.drumKeys.set(k, new Set());
    if (v.drum != null) chVis.drumKeys.get(k).add(v.drum);
  }
  // 通道最终 bank/pc 映射（MA-5 鼓行枚举 + mwa 兜底判定共用）
  chVis.chBankFinal = {};
  for (const e of meta?.chEv ?? []) {
    if (e.k === 'PC') (chVis.chBankFinal[e.ch] ??= {}).pc = e.pc;
    else if (e.k === 'CC' && e.cc === 0) (chVis.chBankFinal[e.ch] ??= {}).bankM = e.v;
    else if (e.k === 'CC' && e.cc === 32) (chVis.chBankFinal[e.ch] ??= {}).bankL = e.v;
  }
  const addRow = (id, label, color, cells, desc) => {
    const tr = document.createElement('tr');
    tr.id = 'xrow-' + id;
    tr.className = 'chrow extra';
    tr.style.borderLeft = `3px solid ${color}`;
    tr.innerHTML = `<td>${label}</td><td class="prog">${cells.prog ?? '--'}</td><td class="note">${cells.note ?? '--'}</td><td>${cells.vol ?? '--'}</td><td>${cells.pan ?? '--'}</td><td>${cells.exp ?? '--'}</td><td class="evt">${cells.evt ?? '--'}</td>`;
    body.appendChild(tr);
    tr.dataset.baseEvt = cells.evt ?? '--';
    tr.children[2].dataset.baseNote = cells.note ?? '--';
    chVis.extra.push({ id, ...desc, tr, noteShown: '' });
  };
  const ma = meta?.version ?? 0;
  // ATR 行：showAtrRows = !(MA>=3)——MA-2 恒显两条（ma2play 同款，不按 atrCount）
  if (ma <= 2) {
    addGroup('ADPCM Tracks (ATR)');
    for (let a = 0; a < 2; a++)
      addRow('atr' + a, 'ATR' + a, ROW_COLORS.atr,
        { prog: 'ADPCM Stream', note: '#' + a, evt: 'adpcm' },
        { kind: 'atr', idx: a, hasData: (meta?.atrCount ?? 0) > a });
  }
  // PCM 行 P0-7：MA-3 与 MA-5 都用 PCM ROM 鼓（ymf825 因实现不完全关了 MA-5 的，
  // 实际 MA-5 也走 ROM 鼓）。MA-3 = ch9 音符枚举；MA-5 = bankM=125 通道上
  // 命中 PCM 音色 drum 键的音符（LG KG920 语料静态验证：36/38/42/49/57/64/70 等 GM 鼓键）
  if (ma >= 3) {
    let drums;
    if (ma === 3) {
      drums = [...new Set((meta?.notes ?? []).filter(n => n.ch === 9).map(n => n.note))];
    } else {
      drums = (meta?.notes ?? []).filter(n => {
        const b = chVis.chBankFinal[n.ch];
        return b?.bankM === 125 && chVis.drumKeys?.get((b.bankL ?? 0) + ':' + b.pc)?.has(n.note);
      }).map(n => n.note);
      drums = [...new Set(drums)];
    }
    drums = drums.sort((a, b) => a - b).slice(0, 8);
    if (drums.length) addGroup('PCM ROM Drums');
    drums.forEach((dn, i) =>
      addRow('pcm' + i, 'P' + i, PCM_ROW_COLORS[i % 8],
        { prog: GM_DRUMS[dn] ?? 'ROM Drum', note: noteName(dn), evt: 'rom' },
        { kind: 'pcm', noteNum: dn }));
  }
  // WAVE 行（MA≥3，MwaSlot 定型）：ext=e<waveID> / mwa=m<idx>；Note=同类内 #seq
  if (ma >= 3 && (meta?.waves ?? []).length) {
    addGroup('WAVE Channels');
    const seq = { ext: 0, mwa: 0 };
    meta.waves.slice(0, 10).forEach((w, wi) => {
      const isMwa = w.src === 'Mwa';
      const kind = isMwa ? 'mwa' : 'ext';
      addRow('wave' + wi, isMwa ? 'm' + seq.mwa : 'e' + (w.id ?? 0),
        ROW_COLORS[kind],
        { prog: isMwa ? `Mwa ${w.hz / 1000 | 0}kHz${w.stereo ? ' st' : ''}` : 'Inline Wave', note: '#' + (seq[kind]++), evt: kind },
        { kind: 'wave', src: kind, extIdx: isMwa ? -1 : seq[kind] - 1 });
    });
  }
  // ext 波归因表（LG KG920 语料静态结论）：bankM=124 通道按 (bankL,pc) 命中
  // PCM 音色（vtype≠FM），PCM 音色注册序 ↔ ext 波 id 序一一对应；
  // bankM=125/bankL=0 = Mwa 流通道（不走音色匹配）。voice→wave 的精确链接
  // 在 DLL 内部，此处为注册序近似，AudioWorklet 快照后换核心实时数据。
  chVis.pcmVoices = new Map();
  (meta?.voices ?? []).filter(v => v.vtype && v.vtype !== 'FM' && v.pc !== undefined)
    .forEach((v, i) => { const k = (v.bankL ?? 0) + ':' + v.pc; if (!chVis.pcmVoices.has(k)) chVis.pcmVoices.set(k, i); });
  // mwa 兜底：全曲无 bankM=125 流音符（触发在 setup SysEx / DLL 内，纯音频/背景垫类，
  // 语料 79 个文件）→ 播放期常亮，保证不死行
  chVis.mwaFallback = (meta?.waves ?? []).some(w => w.src === 'Mwa') &&
    !(meta?.notes ?? []).some(n => chVis.chBankFinal[n.ch]?.bankM === 125);
}

function updateChTable(tSec) {
  const activeNotes = [];
  chVis.chans.forEach((c, i) => {
    if (!c.used) return;
    while (c.ei < c.ev.length && c.ev[c.ei].t <= tSec) {
      const e = c.ev[c.ei++];
      if (e.k === 'PC') c.pc = e.pc;
      else if (e.k === 'CC') { if (e.cc === 7) c.vol = e.v; else if (e.cc === 10) c.pan = e.v; else if (e.cc === 11) c.exp = e.v; else if (e.cc === 0) c.bankM = e.v; else if (e.cc === 32) c.bankL = e.v; }
      if (e.k !== 'Note') c.last = e.k;
    }
    let note = -1;
    for (let j = 0; j < c.notes.length; j++) {
      const n = c.notes[j];
      if (n.t > tSec) break;
      if (n.end > tSec) note = n.note;
    }
    c.act = note >= 0;
    if (note >= 0) c.lastNote = note;              // 粘滞：单曲目内保留最后发音，不复位 '--'
    if (c.act) activeNotes.push({ ch: i, note });
    const tr = $('chrow' + i);
    if (!tr) return;
    const tds = tr.children;
    tr.classList.toggle('on', c.act);
    // 乐器名粘性：一旦确定不再回退默认；仅在变化时写 DOM（布局稳定）
    const progTxt = i === 9 ? 'Drums' : c.pc >= 0 ? `${c.pc} ${GM_NAMES[c.pc] ?? '?'}` : '—';
    if (progTxt !== c.progShown) { c.progShown = progTxt; tds[1].textContent = progTxt; tds[1].title = progTxt; }
    const shownNote = c.lastNote ?? note;
    tds[2].textContent = shownNote >= 0 ? (i === 9 ? (GM_DRUMS[shownNote] ?? noteName(shownNote)) : noteName(shownNote)) : '--';
    tds[3].textContent = c.vol;
    tds[4].textContent = c.pan === 64 ? 'C' : (c.pan > 64 ? 'R' + (c.pan - 64) : 'L' + (64 - c.pan));
    tds[5].textContent = c.exp;
    tds[6].textContent = c.act ? 'Note' : c.last;
  });
  updateExtraRows(tSec, activeNotes);
}

/* 非 MIDI 行播放期更新（parser 静态数据 + bankM/bankL 音符归因，AudioWorklet 快照前的过渡方案）：
 * ATR = 音轨存在且正在播放即激活；PCM P 行 = 对应鼓音正在发声（ch9 非 bankM=125 流通道）；
 * ext wave 行 = bankM=124 通道 (bankL,pc) 命中 PCM 音色（注册序 ↔ ext id 序），各行独立音高；
 * mwa 行 = bankM=125 流通道正在发声（流不按音高触发，Note 显示 stream）。
 * 粘滞铁律：单曲目内所有条目不复位——失活只撤高亮，Note/Event 保留最后值，切曲才重建。 */
function updateExtraRows(tSec, activeNotes) {
  if (!chVis.extra?.length) return;
  const drumHits = new Set();     // MA-5：b125 通道命中 drum 键的音符
  const drumNotes = new Set();    // MA-3：ch9（非流通道）正在发声的音符
  // 每个 PCM 音色（voiceIdx）当前发声的最高音；mwa 流通道是否有发声
  const extAct = new Map();       // voiceIdx -> note
  let mwaAct = false;
  for (const a of activeNotes) {
    const c = chVis.chans[a.ch];
    if (!c) continue;
    if (c.bankM === 125) {
      const dk = chVis.drumKeys?.get((c.bankL ?? 0) + ':' + (c.pc ?? -1));
      if (dk?.has(a.note)) drumHits.add(a.note);
      else mwaAct = true;           // 非鼓键的 b125 活动 = Mwa 流
      continue;
    }
    if (a.ch === 9) drumNotes.add(a.note);
    if (c.bankM !== 124) continue;
    const vi = chVis.pcmVoices?.get((c.bankL ?? 0) + ':' + (c.pc ?? -1));
    if (vi === undefined) continue;
    const prev = extAct.get(vi);
    if (prev === undefined || a.note > prev) extAct.set(vi, a.note);
  }
  for (const x of chVis.extra) {
    let act = false, noteTxt = null, evt = null;
    if (x.kind === 'atr') { act = x.hasData && S.playing; if (act) evt = 'play'; }
    else if (x.kind === 'pcm') {
      act = drumHits.has(x.noteNum) || drumNotes.has(x.noteNum);
      if (act) { noteTxt = noteName(x.noteNum); evt = 'hit'; }
    } else if (x.kind === 'wave') {
      if (x.src === 'mwa' && (mwaAct || (chVis.mwaFallback && S.playing))) { act = true; noteTxt = 'stream'; evt = 'mwa'; }
      else if (x.src === 'ext') {
        const n = extAct.get(x.extIdx);
        if (n !== undefined) { act = true; noteTxt = noteName(n); evt = 'wave'; }
      }
    }
    x.tr.classList.toggle('on', act);
    const tds = x.tr.children;
    if (noteTxt !== null && noteTxt !== x.noteShown) { x.noteShown = noteTxt; tds[2].textContent = noteTxt; }
    if (evt !== null && evt !== x.evtShown) { x.evtShown = evt; tds[6].textContent = evt; }
  }
}

/* ---------------- 文件浏览器 ---------------- */
const browser = {
  path: null, parent: null,
  back: [], fwd: [],
  history: JSON.parse(localStorage.getItem('folderHist') || '[]'),
};
async function navigateTo(path, pushHist = true) {
  const r = await fetch('/api/list?path=' + encodeURIComponent(path));
  const d = await r.json();
  if (d.error) { log(`[browser] cannot open ${path}: ${d.error}`); return; }
  if (browser.path && pushHist) { browser.back.push(browser.path); browser.fwd = []; }
  browser.path = d.path; browser.parent = d.parent; browser.entries = d.entries;
  if (pushHist) addFolderHistory(d.path);
  localStorage.setItem('lastDir', d.path);   // 记住上次打开的目录
  renderBrowser();
}
function addFolderHistory(p) {
  browser.history = browser.history.filter(x => x !== p);
  browser.history.unshift(p);
  browser.history = browser.history.slice(0, 20);
  localStorage.setItem('folderHist', JSON.stringify(browser.history));
  renderHistSelect();
}
function renderHistSelect() {
  const sel = $('folderHist');
  sel.innerHTML = '';
  if (!browser.history.length) { sel.innerHTML = '<option value="">(no history)</option>'; return; }
  for (const p of browser.history) {
    const o = document.createElement('option');
    o.value = p; o.textContent = p.split(/[\\/]/).filter(Boolean).pop() || p; o.title = p;
    sel.appendChild(o);
  }
}
function renderBrowser() {
  $('navBack').disabled = !browser.back.length;
  $('navFwd').disabled = !browser.fwd.length;
  $('navUp').disabled = !browser.parent;
  renderCrumbs();
  const ul = $('fileList'); ul.innerHTML = '';
  for (const e of browser.entries) {
    const li = document.createElement('li');
    li.dataset.path = (browser.path + '\\' + e.name).replace(/\\\\/g, '\\');
    if (e.dir) {
      li.innerHTML = `<span class="diricon">📁</span><span class="nm">${e.name}</span>`;
      li.onclick = () => navigateTo(li.dataset.path);
    } else {
      li.innerHTML = `<span class="diricon">🎵</span><span class="nm">${e.name}</span><span class="sub">${(e.size / 1024).toFixed(0)}K</span>`;
      li.onclick = () => playByPath(li.dataset.path);
    }
    ul.appendChild(li);
  }
  markActiveFile();
}
async function playByPath(path) {
  const r = await fetch('/api/file?path=' + encodeURIComponent(path));
  if (!r.ok) { log(`[load] read failed ${path}`); return; }
  const buf = await r.arrayBuffer();
  if (await loadTrack(path.split(/[\\/]/).pop(), buf, path)) markActiveFile();
}
/* 面包屑（照抄 ma2play：优先显示最内层目录，左侧溢出折叠成 "..."，
 * 点 "..." 或空白处进入路径输入模式，Enter 导航 / Esc 取消） */
let crumbEditing = false;
function renderCrumbs() {
  const crumbs = $('crumbs');
  if (crumbEditing) return;
  crumbs.innerHTML = '';
  const parts = browser.path.split(/[\\/]/).filter(Boolean);
  const accPath = i => {
    let acc = '';
    for (let j = 0; j <= i; j++) {
      const seg = parts[j];
      acc = j === 0 ? (/[A-Za-z]:$/.test(seg) ? seg + '\\' : seg) : acc.replace(/[\\/]+$/, '') + '\\' + seg;
    }
    return acc;
  };
  // 先全量渲染（不可见），从右往左保留能放下的段，左边折叠成 "..."
  const frag = document.createDocumentFragment();
  const segEls = [];
  parts.forEach((seg, i) => {
    if (i) { const sp = document.createElement('span'); sp.className = 'sep'; sp.textContent = '›'; frag.appendChild(sp); }
    const s = document.createElement('span');
    s.className = 'seg'; s.textContent = seg; s.title = accPath(i);
    s.onclick = () => navigateTo(accPath(i));
    frag.appendChild(s); segEls.push(s);
  });
  crumbs.appendChild(frag);
  const ellipsis = document.createElement('span');
  ellipsis.className = 'seg ellipsis'; ellipsis.textContent = '...'; ellipsis.title = '输入完整路径';
  ellipsis.onclick = enterPathEdit;
  // 测宽：从右累计，超出可用宽度即截（ma2play firstVisibleSegment 同算法）
  const sepW = 16, avail = crumbs.clientWidth - 24;
  let used = 0, first = segEls.length - 1;
  for (let i = segEls.length - 1; i >= 0; i--) {
    const w = segEls[i].offsetWidth + (i < segEls.length - 1 ? sepW : 0);
    const needEll = i > 0 ? 34 : 0;
    if (used + w + needEll > avail) break;
    used += w; first = i;
  }
  if (first > 0) {
    // 隐藏 first 之前的段及其分隔符，左侧以 "..." 折叠
    segEls.forEach((el, i) => {
      if (i >= first) return;
      el.style.display = 'none';
      const sep = el.previousSibling;         // 段前的 ›
      if (sep?.classList?.contains('sep')) sep.style.display = 'none';
    });
    crumbs.insertBefore(ellipsis, crumbs.firstChild);
    const sp = document.createElement('span'); sp.className = 'sep'; sp.textContent = '›';
    crumbs.insertBefore(sp, crumbs.children[1] || null);
  }
  // 空白处点击 → 输入模式
  crumbs.onclick = e => { if (e.target === crumbs) enterPathEdit(); };
}
function enterPathEdit() {
  if (crumbEditing) return;
  crumbEditing = true;
  const crumbs = $('crumbs');
  crumbs.innerHTML = '';
  const inp = document.createElement('input');
  inp.className = 'pathinput';
  inp.value = browser.path;
  crumbs.appendChild(inp);
  inp.focus();
  inp.select();
  const done = ok => {
    crumbEditing = false;
    if (ok && inp.value.trim()) navigateTo(inp.value.trim());
    else renderCrumbs();
  };
  inp.onkeydown = e => {
    if (e.key === 'Enter') done(true);
    else if (e.key === 'Escape') done(false);
    e.stopPropagation();
  };
  inp.onblur = () => done(false);
  crumbs.onclick = null;
}

function markActiveFile() {
  document.querySelectorAll('#fileList li').forEach(li =>
    li.classList.toggle('active', li.dataset.path === S.curPath));
}
function shiftTrack(dir, auto = false) {
  const files = [...document.querySelectorAll('#fileList li')].filter(li => li.textContent.includes('🎵'));
  if (!files.length) return;
  const idx = files.findIndex(li => li.dataset.path === S.curPath);
  const next = idx < 0 ? files[0] : (dir > 0 ? files[idx + 1] : files[idx - 1]);
  if (!next) { if (!auto) log('[nav] ' + (dir > 0 ? 'end of list' : 'start of list')); return; }
  playByPath(next.dataset.path);
}

/* ---------------- 本机文件 ---------------- */
async function handleFiles(files) {
  for (const f of files) {
    if (!/\.mmf$/i.test(f.name)) continue;
    loadTrack(f.name, await f.arrayBuffer(), null);
  }
}

/* ---------------- 事件 ---------------- */
$('fileinput').onchange = e => handleFiles(e.target.files);
document.addEventListener('dragover', e => { e.preventDefault(); $('drop').classList.add('over'); });
document.addEventListener('dragleave', () => $('drop').classList.remove('over'));
document.addEventListener('drop', e => { e.preventDefault(); $('drop').classList.remove('over'); handleFiles(e.dataTransfer.files); });
$('btnPlay').onclick = () => S.playing ? pause() : play();
$('btnStop').onclick = stop;
$('btnPrev').onclick = () => shiftTrack(-1);
$('btnNext').onclick = () => shiftTrack(1);
$('btnLoop').onclick = () => { S.loop = !S.loop; $('btnLoop').classList.toggle('on', S.loop); log(`[loop] ${S.loop ? 'on' : 'off'}`); };
$('vol').oninput = applyVolume;
$('btnMute').onclick = () => { muted = !muted; $('btnMute').textContent = muted ? '🔇' : '🔊'; applyVolume(); };
$('navBack').onclick = () => { if (browser.back.length) { browser.fwd.push(browser.path); navigateTo(browser.back.pop(), false); } };
$('navFwd').onclick = () => { if (browser.fwd.length) { browser.back.push(browser.path); navigateTo(browser.fwd.pop(), false); } };
$('navUp').onclick = () => browser.parent && navigateTo(browser.parent);
$('folderHist').onchange = e => { if (e.target.value) { navigateTo(e.target.value); e.target.value = ''; } };
window.addEventListener('resize', () => { if (!crumbEditing) renderCrumbs(); });

/* ---------------- 启动 ---------------- */
window.loadTrack = loadTrack;
window.__updateUI = () => { try { updateUI(); return 'ok'; } catch (e) { return 'ERR ' + e.message; } };
window.__dbg = () => ({
  q: S.queue.length, qFrames: S.qFrames, loaded: S.loaded, playing: S.playing, priming: S.priming, ended: S.ended,
  played: S.playedFrames, ctx: ctx?.state,
  visT: visClock().toFixed(2), clkSong: S.clkSong?.toFixed(2), clkAt: S.clkAt?.toFixed(2),
  now: ctx?.currentTime.toFixed(2), latency: ctx ? (ctx.outputLatency || ctx.baseLatency || 0).toFixed(3) : '-',
  n0: S.meta?.notes?.[0], chEv0: S.meta?.chEv?.[0],
});
buildChGrid();
log('[ui] ma5play started');
bootWorker();
navigateTo(localStorage.getItem('lastDir') || '');   // 上次打开的目录（无记录用服务器默认）
renderHistSelect();

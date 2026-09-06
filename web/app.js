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

/* ---------------- 渲染 Worker ---------------- */
function bootWorker() {
  worker = new Worker('render-worker.js');
  worker.onmessage = e => {
    const m = e.data;
    if (m.id !== undefined && m.id !== S.loadId) return;   // 旧代消息：切曲后迟到，丢弃
    if (m.type === 'progress') {
      $('loadbar').hidden = false;
      $('loadfill').style.width = m.pct + '%';
      status(`${m.label} ${m.pct}%`, m.pct >= 100);
      return;
    }
    if (m.type === 'ready') {
      $('loadbar').hidden = true;
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
/* 可视化时钟（ma2play 同步法）：音频回调锚点 + ctx.currentTime 帧间插值，
 * 减输出延迟对齐"正在听到"的时刻。suspend 时 currentTime 冻结 → 画面自然停住。 */
function visClock() {
  if (!ctx || S.clkAt === undefined) return Math.max(0, (S.playedFrames || 0) / FRAMES_PER_SEC);
  const t = S.clkSong + Math.max(0, ctx.currentTime - S.clkAt);
  return Math.max(0, t - (ctx.outputLatency || ctx.baseLatency || 0));
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
    const rows = cssW < 700 ? 2 : 1;
    let numWhite = 0;
    for (let n = MIN_NOTE; n <= MAX_NOTE; n++) if (!isBlack(n)) numWhite++;
    const wkW = cssW / (rows === 1 ? numWhite : Math.ceil(numWhite / 2));
    const wkH = Math.min(Math.round(wkW * KEY_RATIO), 86);
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
    if (!layout || layout.forW !== cssW) {
      layout = relayout(cssW); layout.forW = cssW;
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
      const prev = active.get(nt.note);
      if (!prev || lv > prev.lv) active.set(nt.note, { lv, ch: nt.ch });
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
        g.fillStyle = a ? blendKey(CH_COLORS[a.ch], a.lv, false) : 'rgb(255,255,255)';
        g.fillRect(x, y, wkW - 1, wkH);
        g.strokeStyle = 'rgb(80,80,80)';
        g.strokeRect(x + 0.5, y + 0.5, wkW - 1, wkH - 1);
        if (n % 12 === 0) {                          // C 音名：始终显示（手机两行也标）
          g.fillStyle = 'rgba(0,0,0,0.7)';
          g.font = wkW > 14 ? '9px system-ui' : '8px system-ui';
          g.fillText(`C${(n / 12) - 1}`, x + 1, y + wkH - 3);
        }
        if (a && wkW > 12) {
          g.fillStyle = 'rgba(0,0,0,0.78)';
          g.font = '8px system-ui';
          g.fillText(CH_NAMES[a.ch], x + 1, y + 10);
        }
        wkIdx++;
      }
      wkIdx = 0;
      for (let n = lo; n <= hi; n++) {                        // Pass 2: 黑键
        if (!isBlack(n)) { wkIdx++; continue; }
        const x = (wkIdx - 1) * wkW + wkW - bkW * 0.5;
        const a = active.get(n);
        g.fillStyle = a ? blendKey(CH_COLORS[a.ch], a.lv, true) : 'rgb(20,20,20)';
        g.fillRect(x, y, bkW, bkH);
        g.strokeStyle = 'rgb(0,0,0)';
        g.strokeRect(x + 0.5, y + 0.5, bkW - 1, bkH - 1);
        if (a && bkW > 8) {
          g.fillStyle = 'rgba(255,255,255,0.78)';
          g.font = '8px system-ui';
          g.fillText(CH_NAMES[a.ch], x + 1, y + 10);
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
const GM_NAMES = ['AcGrandPiano','BrightPiano','ElectricGrand','HonkyTonk','ElectricPiano1','ElectricPiano2','Harpsichord','Clavinet','Celesta','Glockenspiel','MusicBox','Vibraphone','Marimba','Xylophone','TubularBells','Dulcimer','DrawbarOrgan','PercOrgan','RockOrgan','ChurchOrgan','ReedOrgan','Accordion','Harmonica','TangoAccord','AcGuitarNylon','AcGuitarSteel','JazzGuitar','CleanGuitar','MutedGuitar','OverdriveGuitar','DistortionGuitar','GuitarHarmonics','AcBass','FingerBass','PickBass','FretlessBass','SlapBass1','SlapBass2','SynthBass1','SynthBass2','Violin','Viola','Cello','Contrabass','TremoloStrings','Pizzicato','OrchestralHarp','Timpani','StringEns1','StringEns2','SynthStrings1','SynthStrings2','ChoirAahs','VoiceOohs','SynthVox','OrchestraHit','Trumpet','Trombone','Tuba','MutedTrumpet','FrenchHorn','BrassSection','SynthBrass1','SynthBrass2','SopranoSax','AltoSax','TenorSax','BaritoneSax','Oboe','EnglishHorn','Bassoon','Clarinet','Piccolo','Flute','Recorder','PanFlute','BlownBottle','Shakuhachi','Whistle','Ocarina','SquareLead','SawLead','CalliopeLead','ChiffLead','CharangLead','VoiceLead','FifthLead','BassLead','NewAgePad','WarmPad','PolySynthPad','ChoirPad','BowedPad','MetallicPad','HaloPad','SweepPad','Rain','Soundtrack','Crystal','Atmosphere','Brightness','Goblins','Echoes','SciFi','Sitar','Banjo','Shamisen','Koto','Kalimba','Bagpipe','Fiddle','Shanai','TinkleBell','Agogo','SteelDrums','Woodblock','TaikoDrum','MelodicTom','SynthDrum','ReverseCymbal','GuitarFretNoise','BreathNoise','Seashore','BirdTweet','Telephone','Helicopter','Applause','Gunshot'];
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const noteName = n => n >= 0 && n <= 127 ? NOTE_NAMES[n % 12] + ((n / 12 | 0) - 1) : '--';
const chVis = { chans: [] };

function buildChGrid() {
  const tb = $('chTable');
  tb.innerHTML = `<thead><tr><th>Ch</th><th>Prog</th><th>Note</th><th>Vol</th><th>Pan</th><th>Exp</th><th>Event</th></tr></thead>`;
  const body = document.createElement('tbody');
  for (let i = 0; i < 16; i++) {
    const tr = document.createElement('tr');
    tr.id = 'chrow' + i;
    tr.innerHTML = `<td>Ch${i}</td><td class="prog">—</td><td class="note">--</td><td class="vol">--</td><td class="pan">--</td><td class="exp">--</td><td class="evt">--</td>`;
    body.appendChild(tr);
  }
  tb.appendChild(body);
  chVis.chans = Array.from({ length: 16 }, () => ({ ev: [], ei: 0, notes: [], ni: 0, pc: -1, vol: 100, pan: 64, exp: 127, last: '--', note: -1, act: false }));
}
function chOnTrack(meta) {
  // 载入即出解析摘要日志（静态对照用，带曲目时间标记）
  const nNotes = meta?.notes?.length ?? 0;
  const usedCh = [...new Set((meta?.notes ?? []).map(n => n.ch))].sort((a, b) => a - b);
  const last = meta?.notes?.length ? meta.notes.reduce((m2, n) => n.end > m2 ? n.end : m2, 0) : 0;
  log(`[vis] parsed: ${nNotes} notes, ch=[${usedCh.join(',')}], span 0→${last.toFixed(2)}s (dur ${(meta?.durationMs / 1000 || 0).toFixed(1)}s)`);
  if (!nNotes) log('[vis] ⚠ no notes parsed (compressed/SEQU format not supported for piano)');
  for (const c of chVis.chans) { c.ev = []; c.notes = []; c.ei = 0; c.ni = 0; c.pc = -1; c.vol = 100; c.pan = 64; c.exp = 127; c.last = '--'; c.note = -1; c.act = false; }
  for (const e of meta?.chEv ?? []) chVis.chans[e.ch]?.ev.push(e);
  for (const n of meta?.notes ?? []) chVis.chans[n.ch]?.notes.push(n);
  for (const c of chVis.chans) { c.ev.sort((a, b) => a.t - b.t); c.notes.sort((a, b) => a.t - b.t); }
}
function updateChTable(tSec) {
  for (let i = 0; i < 16; i++) {
    const c = chVis.chans[i];
    while (c.ei < c.ev.length && c.ev[c.ei].t <= tSec) {
      const e = c.ev[c.ei++];
      if (e.k === 'PC') c.pc = e.pc;
      else if (e.k === 'CC') { if (e.cc === 7) c.vol = e.v; else if (e.cc === 10) c.pan = e.v; else if (e.cc === 11) c.exp = e.v; else if (e.cc === 0) c.last = 'Bank'; }
      if (e.k !== 'Note') c.last = e.k;
    }
    // 当前音符：最近的 note-on 且未结束
    let note = -1;
    for (let j = c.ni; j < c.notes.length; j++) {
      const n = c.notes[j];
      if (n.t > tSec) break;
      if (n.end > tSec) note = n.note;
    }
    c.act = note >= 0;
    const tr = $('chrow' + i);
    const tds = tr.children;
    tr.classList.toggle('on', c.act || c.pc >= 0);
    tds[1].textContent = i === 9 ? 'Drums' : c.pc >= 0 ? `${c.pc} ${GM_NAMES[c.pc] ?? '?'}` : '—';
    tds[2].textContent = note >= 0 ? noteName(note) : '--';
    tds[3].textContent = c.vol;
    tds[4].textContent = c.pan === 64 ? 'C' : (c.pan > 64 ? 'R' + (c.pan - 64) : 'L' + (64 - c.pan));
    tds[5].textContent = c.exp;
    tds[6].textContent = c.act ? 'Note' : c.last;
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

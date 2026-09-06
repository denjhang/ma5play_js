// app.js — ma5play 测试播放器
// 布局同 ma2play smaf_window：左 Controls | 右上 示波器+通道状态 | 右下 文件浏览器+Log
// 音频：主线程 wasm pump → Int32 环形帧缓冲(6s) → ScriptProcessorNode(48kHz)。
//   priming 预滚：攒满 2s 才开声；欠载自动回预滚（不连续小口供声）。
'use strict';

const $ = id => document.getElementById(id);
const FRAMES_PER_SEC = 48000;
const RING_SECS = 6;
const PREROLL_FRAMES = FRAMES_PER_SEC * 2;
const fmt = s => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;
const status = (t, ok) => { $('status').textContent = t; $('status').classList.toggle('ok', !!ok); };

/* ---------------- Log 面板 ---------------- */
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
  coreReady: false, loaded: false, playing: false, loop: false, ended: false,
  priming: false,
  name: null, curBuf: null,
  ring: new Int32Array(FRAMES_PER_SEC * RING_SECS),
  rd: 0, wr: 0, totalWr: 0, playedFrames: 0,
  musicSeen: false, silentFrames: 0, noPcm: 0,
  speed: 0,
};
let M = null, pcmPtr = 0, loadPtr = 0;
let ctx = null, procNode = null, gainNode = null, muted = false;

/* ---------------- wasm 核心 ---------------- */
async function bootCore() {
  status('加载核心…');
  M = await ma5play({ locateFile: p => 'assets/' + p });
  const img = await (await fetch('assets/preload.bin')).arrayBuffer();
  const p = M._malloc(img.byteLength);
  new Uint8Array(M.HEAPU8.buffer, M.HEAPU8.byteOffset + p, img.byteLength).set(new Uint8Array(img));
  M._ma5w_set_preload(p, img.byteLength);
  if (M._ma5w_init() !== 0) { status('核心初始化失败', false); log('[core] init 失败'); return; }
  pcmPtr = M._malloc(4 * 2400);
  S.coreReady = true;
  $('chipMode').textContent = M._ma5w_compact_mode() ? 'compact 预载' : 'flat';
  status('核心就绪', true);
  log(`[core] ma5t 就绪（${M._ma5w_compact_mode() ? 'compact 预载' : 'flat'}，映像 ${(img.byteLength / 1048576).toFixed(1)}MB）`);
}

/* ---------------- 音频 ---------------- */
function ensureAudio() {
  if (ctx) return;
  ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'playback' });
  gainNode = ctx.createGain();
  applyVolume();
  gainNode.connect(ctx.destination);
  procNode = ctx.createScriptProcessor(4096, 0, 2);
  procNode.onaudioprocess = ev => {
    const L = ev.outputBuffer.getChannelData(0), R = ev.outputBuffer.getChannelData(1);
    let fed = 0;
    for (let i = 0; i < L.length; i++) {
      if (S.rd !== S.wr) {
        const f = S.ring[S.rd];
        L[i] = ((f << 16) >> 16) / 32768;
        R[i] = (f >> 16) / 32768;
        S.rd = (S.rd + 1) % S.ring.length;
        fed++;
      } else { L[i] = R[i] = 0; }
    }
    if (fed) S.playedFrames += fed;
  };
  procNode.connect(gainNode);
  setInterval(fillLoop, 120);
}
function applyVolume() { if (gainNode) gainNode.gain.value = muted ? 0 : $('vol').value / 100; }
function fadeIn() {
  const t = ctx.currentTime;
  gainNode.gain.cancelScheduledValues(t);
  gainNode.gain.setValueAtTime(Math.max(gainNode.gain.value, 0.0001), t);
  gainNode.gain.linearRampToValueAtTime(muted ? 0 : $('vol').value / 100, t + 0.04);
}

function fillLoop() {
  if (!S.loaded || S.ended) return;
  const cap = S.ring.length;
  const t0 = performance.now();
  let audioMs = 0, wallBusyMs = 0;
  const buffered = () => (S.wr - S.rd + cap) % cap;
  if (S.playing && !S.priming && ctx && ctx.state === 'running' && buffered() === 0 && !S.ended) {
    S.priming = true; ctx.suspend(); $('chipState').textContent = 'buffering';
  }
  while (true) {
    const used = buffered();
    if (cap - used < FRAMES_PER_SEC * 3) break;
    M._ma5w_pump_seq();
    M._ma5w_pump_audio();
    const bytes = M._ma5w_take_pcm(pcmPtr, 4 * 2400);
    if (bytes === 0) {
      if (S.musicSeen && ++S.noPcm > 5) return songEnd();
      break;
    }
    S.noPcm = 0;
    const frames = bytes / 4;
    const src = new Int16Array(M.HEAPU8.buffer, M.HEAPU8.byteOffset + pcmPtr, frames * 2);
    let nz = false;
    for (let i = 0; i < frames; i++) {
      const l = src[i * 2], r = src[i * 2 + 1];
      if (l | r) nz = true;
      S.ring[S.wr] = (l & 0xffff) | ((r & 0xffff) << 16);
      S.wr = (S.wr + 1) % cap;
    }
    S.totalWr += frames; audioMs += frames / 48; wallBusyMs = performance.now() - t0;
    if (nz) { S.musicSeen = true; S.silentFrames = 0; }
    else if (S.musicSeen && (S.silentFrames += frames) > FRAMES_PER_SEC * 2) return songEnd();
  }
  if (wallBusyMs > 5 && audioMs > 0) S.speed = audioMs / wallBusyMs;
  if (S.priming && S.playing && buffered() >= PREROLL_FRAMES) {
    S.priming = false;
    fadeIn();
    ctx.resume();
    $('chipState').textContent = 'playing';
  }
  updateUI();
}

function songEnd() {
  S.ended = true; S.playing = false;
  $('chipState').textContent = 'end';
  syncPlayBtns();
  log(`[play] 曲终 ${fmt(S.playedFrames / FRAMES_PER_SEC)}`);
  if (S.loop) setTimeout(() => S.curBuf && loadTrack(S.name, S.curBuf, S.curPath), 500);
  else nextTrack(true);
}

/* ---------------- 曲目 ---------------- */
let switching = false;
async function loadTrack(name, buf, path) {
  if (!S.coreReady) await bootCore();
  if (!S.coreReady || switching) return false;
  switching = true;
  $('chipState').textContent = 'switch';
  try {
    if (ctx && gainNode && S.playing) {
      const t = ctx.currentTime;
      gainNode.gain.cancelScheduledValues(t);
      gainNode.gain.setValueAtTime(gainNode.gain.value, t);
      gainNode.gain.linearRampToValueAtTime(0, t + 0.06);
      await new Promise(r => setTimeout(r, 90));
      ctx.suspend();
    }
    S.playing = false; S.loaded = false;
    S.rd = S.wr = S.totalWr = 0; S.playedFrames = 0;
    S.musicSeen = false; S.silentFrames = 0; S.noPcm = 0;
    S.ended = false; S.name = name; S.curBuf = buf; S.curPath = path ?? S.curPath;
    if (loadPtr) M._free(loadPtr);
    loadPtr = M._malloc(buf.byteLength);
    new Uint8Array(M.HEAPU8.buffer, M.HEAPU8.byteOffset + loadPtr, buf.byteLength).set(new Uint8Array(buf));
    if (M._ma5w_load(loadPtr, buf.byteLength) !== 0) {
      status('MaSound_Load 失败', false);
      log(`[load] ${name}: MaSound_Load 失败`);
      return false;
    }
    if (M._ma5w_open_standby_start() !== 0) { status('standby 启动失败', false); return false; }
    S.loaded = true;
    $('trackName').textContent = name.replace(/\.mmf$/i, '');
    $('trackSize').textContent = (buf.byteLength / 1024).toFixed(1) + ' KB';
    log(`[play] ${path || name}`);
    play();
    return true;
  } finally { switching = false; }
}
function play() {
  if (!S.loaded) return;
  ensureAudio();
  const buffered = () => (S.wr - S.rd + S.ring.length) % S.ring.length;
  if (ctx.state === 'suspended' || buffered() < PREROLL_FRAMES) {
    S.priming = true; S.playing = true;
    $('chipState').textContent = 'buffering';
    syncPlayBtns();
    fillLoop();
    return;
  }
  ctx.resume(); fadeIn();
  S.playing = true;
  $('chipState').textContent = 'playing';
  syncPlayBtns();
}
function pause() {
  S.playing = false;
  if (ctx && S.loaded) { $('chipState').textContent = 'paused'; ctx.suspend(); }
  syncPlayBtns();
}
function stop() {
  pause(); S.rd = S.wr = 0; S.playedFrames = 0;
  $('chipState').textContent = 'stop'; updateUI();
}
function syncPlayBtns() { $('btnPlay').textContent = $('btnPlay2').textContent = S.playing ? '⏸' : '▶'; }

/* ---------------- 文件浏览器（同 ma2play 设计） ---------------- */
const browser = {
  path: null, parent: null, entries: [],
  back: [], fwd: [],          // 导航栈
  history: JSON.parse(localStorage.getItem('folderHist') || '[]'),
};
async function navigateTo(path, pushHist = true) {
  const r = await fetch('/api/list?path=' + encodeURIComponent(path));
  const d = await r.json();
  if (d.error) { log(`[browser] 无法打开 ${path}: ${d.error}`); return; }
  if (browser.path && pushHist) { browser.back.push(browser.path); browser.fwd = []; }
  browser.path = d.path; browser.parent = d.parent; browser.entries = d.entries;
  if (pushHist) addFolderHistory(d.path);
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
  if (!browser.history.length) { sel.innerHTML = '<option value="">(无历史)</option>'; return; }
  for (const p of browser.history) {
    const short = p.split(/[\\/]/).filter(Boolean).pop() || p;
    const o = document.createElement('option');
    o.value = p; o.textContent = short; o.title = p;
    sel.appendChild(o);
  }
}
function renderBrowser() {
  $('navBack').disabled = !browser.back.length;
  $('navFwd').disabled = !browser.fwd.length;
  $('navUp').disabled = !browser.parent;
  // 面包屑：D:\a\b → [D:] [a] [b]，逐段可点
  const crumbs = $('crumbs'); crumbs.innerHTML = '';
  const parts = browser.path.split(/[\\/]/).filter(Boolean);
  let acc = '';
  parts.forEach((seg, i) => {
    acc = i === 0 ? (/[A-Za-z]:$/.test(seg) ? seg + '\\' : seg) : acc.replace(/[\\/]+$/, '') + '\\' + seg;
    if (i) crumbs.insertAdjacentHTML('beforeend', '<span class="sep">›</span>');
    const s = document.createElement('span');
    s.className = 'seg'; s.textContent = seg; s.title = acc;
    s.onclick = () => navigateTo(acc);
    crumbs.appendChild(s);
  });
  // 文件列表
  const ul = $('fileList'); ul.innerHTML = '';
  for (const e of browser.entries) {
    const li = document.createElement('li');
    li.dataset.path = (browser.path + '\\' + e.name).replace(/\\\\/g, '\\');
    if (e.dir) {
      li.innerHTML = `<span class="diricon">📁</span><span class="nm">${e.name}</span>`;
      li.onclick = () => navigateTo(li.dataset.path);
    } else {
      li.innerHTML = `<span class="diricon">🎵</span><span class="nm">${e.name}</span><span class="sub">${(e.size / 1024).toFixed(0)}K</span>`;
      li.onclick = () => playByPath(li.dataset.path, e.name);
    }
    ul.appendChild(li);
  }
  markActiveFile();
}
async function playByPath(path, name) {
  const r = await fetch('/api/file?path=' + encodeURIComponent(path));
  if (!r.ok) { log(`[load] 读取失败 ${path}`); return; }
  const buf = await r.arrayBuffer();
  if (await loadTrack(name ?? path.split(/[\\/]/).pop(), buf, path)) markActiveFile();
}
function filesInView() { return [...document.querySelectorAll('#fileList li')].filter(li => !li.querySelector('.diricon') || li.textContent.includes('🎵')); }
function markActiveFile() {
  document.querySelectorAll('#fileList li').forEach(li =>
    li.classList.toggle('active', li.dataset.path === S.curPath));
}
function shiftTrack(dir, auto = false) {
  const files = [...document.querySelectorAll('#fileList li')].filter(li => li.textContent.includes('🎵'));
  if (!files.length) return;
  const idx = files.findIndex(li => li.dataset.path === S.curPath);
  let next;
  if (idx < 0) next = files[0];
  else next = dir > 0 ? files[idx + 1] : files[idx - 1];
  if (!next) { if (!auto) log('[nav] ' + (dir > 0 ? '已是最后一曲' : '已是第一曲')); return; }
  playByPath(next.dataset.path);
}
function nextTrack(auto) { shiftTrack(1, auto); }

/* ---------------- 通道状态占位（48ch，待 worklet 快照接入） ---------------- */
function buildChGrid() {
  const g = $('chGrid'); g.innerHTML = '';
  for (let i = 0; i < 48; i++) {
    const d = document.createElement('div');
    d.className = `chcell ${i < 16 ? 'fm' : 'pcm'}`;
    d.id = 'ch' + i;
    d.textContent = i < 16 ? `F${i}` : `P${i - 16}`;
    g.appendChild(d);
  }
}

/* ---------------- UI ---------------- */
function updateUI() {
  const heard = S.playedFrames / FRAMES_PER_SEC;
  $('timeCur').textContent = fmt(heard);
  $('bar').style.width = Math.min(100, (heard / 300) * 100) + '%';
  $('chipSpeed').textContent = S.speed ? S.speed.toFixed(2) + 'x rt' : '—';
}
function drawScope() {
  const c = $('scope'), g = c.getContext('2d');
  const W = c.width, H = c.height, mid = H / 2;
  g.clearRect(0, 0, W, H);
  g.strokeStyle = '#1d2740'; g.beginPath(); g.moveTo(0, mid); g.lineTo(W, mid); g.stroke();
  if (S.loaded) {
    g.strokeStyle = '#5b8cff'; g.lineWidth = 1.5; g.beginPath();
    const N = 300, span = Math.floor(S.ring.length / N);
    for (let x = 0; x < N; x++) {
      const f = S.ring[(S.wr - S.ring.length + x * span + S.ring.length * 2) % S.ring.length];
      g.lineTo((x / N) * W, mid + (((f << 16) >> 16) / 32768) * mid * 0.92);
    }
    g.stroke();
  }
  requestAnimationFrame(drawScope);
}

/* ---------------- 本机文件 ---------------- */
async function handleFiles(files) {
  for (const f of files) {
    if (!/\.mmf$/i.test(f.name)) continue;
    const buf = await f.arrayBuffer();
    loadTrack(f.name, buf, null);
  }
}

/* ---------------- 事件 ---------------- */
$('fileinput').onchange = e => handleFiles(e.target.files);
document.addEventListener('dragover', e => { e.preventDefault(); $('drop').classList.add('over'); });
document.addEventListener('dragleave', () => $('drop').classList.remove('over'));
document.addEventListener('drop', e => {
  e.preventDefault(); $('drop').classList.remove('over');
  handleFiles(e.dataTransfer.files);
});
$('btnPlay').onclick = $('btnPlay2').onclick = () => S.playing ? pause() : play();
$('btnStop').onclick = stop;
$('btnPrev').onclick = () => shiftTrack(-1);
$('btnNext').onclick = () => shiftTrack(1);
$('btnLoop').onclick = () => { S.loop = !S.loop; $('btnLoop').classList.toggle('on', S.loop); log(`[loop] ${S.loop ? '开' : '关'}`); };
$('vol').oninput = applyVolume;
$('btnMute').onclick = () => { muted = !muted; $('btnMute').textContent = muted ? '🔇' : '🔊'; applyVolume(); };
$('navBack').onclick = () => { if (browser.back.length) { browser.fwd.push(browser.path); navigateTo(browser.back.pop(), false); } };
$('navFwd').onclick = () => { if (browser.fwd.length) { browser.back.push(browser.path); navigateTo(browser.fwd.pop(), false); } };
$('navUp').onclick = () => browser.parent && navigateTo(browser.parent);
$('folderHist').onchange = e => { if (e.target.value) { navigateTo(e.target.value); e.target.value = ''; } };

/* ---------------- 启动 ---------------- */
window.loadTrack = loadTrack;
buildChGrid();
drawScope();
log('[ui] ma5play 启动');
bootCore();
navigateTo('');      // 默认目录（服务器端 DEFAULT_DIR）
renderHistSelect();

// app.js — ma5play 测试播放器
// 音频管线：主线程 wasm pump 填 Int16 环形缓冲 → ScriptProcessorNode(48kHz) 拉取播放。
// （AudioWorklet + 通道快照是下一阶段，这里先保证功能验证）
'use strict';

const $ = id => document.getElementById(id);
const BYTES_PER_SEC = 192000;                       // 48k · s16 · stereo
const FRAMES_PER_SEC = 48000;
const RING_SECS = 6;
const FRAME = 4;                                    // 每立体声帧 4 字节
const fmt = s => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;
const status = (t, ok) => { $('status').textContent = t; $('status').classList.toggle('ok', !!ok); };

/* ---------------- IndexedDB 历史（存 File blob，跨会话可重播） ---------------- */
const db = await new Promise((res, rej) => {
  const r = indexedDB.open('ma5play', 1);
  r.onupgradeneeded = () => r.result.createObjectStore('history', { keyPath: 'id', autoIncrement: true });
  r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
});
const store = mode => db.transaction('history', mode).objectStore('history');
const dbAll = () => new Promise(res => { const q = store('readonly').getAll(); q.onsuccess = () => res(q.result || []); });
async function pushHistory(name, size, blob) {
  (await dbAll()).filter(h => h.name === name && h.size === size)
    .forEach(h => store('readwrite').delete(h.id));
  await new Promise(res => { store('readwrite').add({ name, size, blob, last: Date.now() }).onsuccess = res; });
}

/* ---------------- 状态 ---------------- */
const PREROLL_FRAMES = FRAMES_PER_SEC * 2;          // 预滚：攒满 2s 才开声
const S = {
  coreReady: false, loaded: false, playing: false, loop: false, ended: false,
  priming: false,                          // 预滚中：只填缓冲不出声
  name: null, curBuf: null,               // 当前曲目原始 bytes（循环重播用）
  ring: new Int32Array(FRAMES_PER_SEC * RING_SECS),   // 每项一个立体声帧
  rd: 0, wr: 0, totalWr: 0,               // totalWr = 累计渲染帧数
  playedFrames: 0,
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
  if (M._ma5w_init() !== 0) { status('核心初始化失败', false); return; }
  pcmPtr = M._malloc(FRAME * 2400);       // 50ms 块
  S.coreReady = true;
  $('chipMode').textContent = M._ma5w_compact_mode() ? 'compact 预载' : 'flat';
  status('核心就绪', true);
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
        L[i] = ((f << 16) >> 16) / 32768;          // 低16=L（符号扩展）
        R[i] = (f >> 16) / 32768;                  // 高16=R
        S.rd = (S.rd + 1) % S.ring.length;
        fed++;
      } else { L[i] = R[i] = 0; }
    }
    if (fed) S.playedFrames = (S.playedFrames || 0) + fed;   // 真实听到的帧数
  };
  procNode.connect(gainNode);
  setInterval(fillLoop, 120);
}
function applyVolume() { if (gainNode) gainNode.gain.value = muted ? 0 : $('vol').value / 100; }

/* 渲染泵：保持缓冲 ≥3s；曲终判定同冒烟（出声后 2s 静音 / 连续取不到 PCM） */
function fillLoop() {
  if (!S.loaded || S.ended) return;
  const cap = S.ring.length;
  const t0 = performance.now();
  let audioMs = 0, wallBusyMs = 0;
  // 欠载保护：播放中缓冲耗尽 → 回到预滚，攒满再出声（不连续小口供声）
  const buffered = () => (S.wr - S.rd + cap) % cap;
  if (S.playing && !S.priming && ctx && ctx.state === 'running' &&
      buffered() === 0 && !S.ended) {
    S.priming = true;
    ctx.suspend();
    $('chipState').textContent = 'buffering';
  }
  while (true) {
    const used = buffered();
    if (cap - used < FRAMES_PER_SEC * 3) break;    // 缓冲已够
    M._ma5w_pump_seq();
    M._ma5w_pump_audio();
    const bytes = M._ma5w_take_pcm(pcmPtr, FRAME * 2400);
    if (bytes === 0) {
      if (S.musicSeen && ++S.noPcm > 5) return songEnd();
      break;                                       // 暂时无数据，下轮再补
    }
    S.noPcm = 0;
    const frames = bytes / FRAME;
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
  // 预滚完成 → 开声（暂停中则只攒着，等恢复）
  if (S.priming && S.playing && buffered() >= PREROLL_FRAMES) {
    S.priming = false;
    const t = ctx.currentTime;
    gainNode.gain.cancelScheduledValues(t);
    gainNode.gain.setValueAtTime(Math.max(gainNode.gain.value, 0.0001), t);
    gainNode.gain.linearRampToValueAtTime(muted ? 0 : $('vol').value / 100, t + 0.04);
    ctx.resume();
    $('chipState').textContent = 'playing';
  }
  updateUI();
}

function songEnd() {
  S.ended = true; S.playing = false;
  $('chipState').textContent = 'end';
  $('btnPlay').textContent = '▶';
  if (S.loop) setTimeout(() => S.curBuf && loadTrack(S.name, S.curBuf), 500);
}

/* ---------------- 曲目 ---------------- */
let switching = false;
async function loadTrack(name, buf) {
  if (!S.coreReady) await bootCore();
  if (!S.coreReady || switching) return false;
  switching = true;
  $('chipState').textContent = 'switch';
  try {
    // 短淡出（避免波形硬切爆音），等淡出播完再动核心
    if (ctx && gainNode && S.playing) {
      gainNode.gain.cancelScheduledValues(ctx.currentTime);
      gainNode.gain.setValueAtTime(gainNode.gain.value, ctx.currentTime);
      gainNode.gain.linearRampToValueAtTime(0, ctx.currentTime + 0.06);
      await new Promise(r => setTimeout(r, 90));
      ctx.suspend();
    }
    S.playing = false; S.loaded = false;   // 停泵：fillLoop 不再碰核心
    S.rd = S.wr = S.totalWr = 0; S.playedFrames = 0;
    S.musicSeen = false; S.silentFrames = 0; S.noPcm = 0;
    S.ended = false; S.name = name; S.curBuf = buf;
    if (loadPtr) M._free(loadPtr);
    loadPtr = M._malloc(buf.byteLength);
    new Uint8Array(M.HEAPU8.buffer, M.HEAPU8.byteOffset + loadPtr, buf.byteLength).set(new Uint8Array(buf));
    if (M._ma5w_load(loadPtr, buf.byteLength) !== 0) {
      status('MaSound_Load 失败（文件可能不是 MA-5 曲目）', false);
      return false;
    }
    if (M._ma5w_open_standby_start() !== 0) { status('standby 启动失败', false); return false; }
    S.loaded = true;
    $('trackName').textContent = name.replace(/\.mmf$/i, '');
    $('trackMeta').textContent = (buf.byteLength / 1024).toFixed(1) + ' KB · MA-5';
    status('正在渲染…', true);
    play();                                 // priming：缓冲满才真正出声
    markActive();
    return true;
  } finally {
    switching = false;
  }
}
function play() {
  if (!S.loaded) return;
  ensureAudio();
  const buffered = () => (S.wr - S.rd + S.ring.length) % S.ring.length;
  if (ctx.state === 'suspended' || buffered() < PREROLL_FRAMES) {
    // 预滚：先只填缓冲不出声，攒满再开声（消除开头的断续）
    S.priming = true;
    S.playing = true;
    $('btnPlay').textContent = '⏸';
    $('chipState').textContent = 'buffering';
    fillLoop();                              // 立刻开填，不等 120ms 定时
    return;
  }
  ctx.resume();
  // 淡入（切曲时 gain 被 ramp 到 0，这里恢复）
  const t = ctx.currentTime;
  gainNode.gain.cancelScheduledValues(t);
  gainNode.gain.setValueAtTime(Math.max(gainNode.gain.value, 0.0001), t);
  gainNode.gain.linearRampToValueAtTime(muted ? 0 : $('vol').value / 100, t + 0.04);
  S.playing = true; $('btnPlay').textContent = '⏸'; $('chipState').textContent = 'playing';
}
function pause() {
  S.playing = false; $('btnPlay').textContent = '▶';
  if (ctx && S.loaded) { $('chipState').textContent = 'paused'; ctx.suspend(); }
}
function stop() {
  pause(); S.rd = S.wr = 0; S.playedFrames = 0; $('chipState').textContent = 'stop';
}

/* ---------------- UI ---------------- */
function updateUI() {
  const heard = (S.playedFrames || 0) / FRAMES_PER_SEC;   // 实际已播放
  $('timeCur').textContent = fmt(heard);
  $('bar').style.width = Math.min(100, (heard / 300) * 100) + '%';  // 5 分钟参考刻度
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
      const v = ((f << 16) >> 16) / 32768;
      g.lineTo((x / N) * W, mid + v * mid * 0.92);
    }
    g.stroke();
  }
  requestAnimationFrame(drawScope);
}
function markActive() {
  document.querySelectorAll('.list li').forEach(li =>
    li.classList.toggle('active', li.dataset.key === S.name));
}

/* ---------------- 文件浏览器 + 历史 ---------------- */
async function handleFiles(files) {
  for (const f of files) {
    if (!/\.mmf$/i.test(f.name)) continue;
    const buf = await f.arrayBuffer();
    if (await loadTrack(f.name, buf)) {
      await pushHistory(f.name, f.size, f);
      renderHistory();
    }
  }
}
async function renderHistory() {
  const all = (await dbAll()).sort((a, b) => b.last - a.last).slice(0, 50);
  const ul = $('historyList'); ul.innerHTML = '';
  if (!all.length) { ul.innerHTML = '<li style="cursor:default;color:var(--dim)">暂无记录</li>'; return; }
  for (const h of all) {
    const li = document.createElement('li');
    li.dataset.key = h.name;
    const when = new Date(h.last);
    const nice = `${when.getMonth() + 1}/${when.getDate()} ${String(when.getHours()).padStart(2, '0')}:${String(when.getMinutes()).padStart(2, '0')}`;
    li.innerHTML = `<span class="nm">🕘 ${h.name.replace(/\.mmf$/i, '')}</span>
      <span class="sub">${(h.size / 1024).toFixed(0)}K · ${nice}</span>
      <button class="del" title="删除">✕</button>`;
    li.onclick = async e => {
      if (e.target.classList.contains('del')) { store('readwrite').delete(h.id); renderHistory(); return; }
      const src = h.blob;
      const buf = src instanceof ArrayBuffer ? src : await src.arrayBuffer();
      loadTrack(h.name, buf);
    };
    ul.appendChild(li);
  }
  markActive();
}
async function renderDemos() {
  const ul = $('demoList');
  try {
    const names = await (await fetch('assets/tracks/manifest.json')).json();
    for (const n of names) {
      const li = document.createElement('li');
      li.dataset.key = n;
      li.innerHTML = `<span class="nm">♪ ${n.replace(/\.mmf$/i, '')}</span><span class="sub">demo</span>`;
      li.onclick = async () => {
        const buf = await (await fetch('assets/tracks/' + encodeURIComponent(n))).arrayBuffer();
        loadTrack(n, buf);
        await pushHistory(n, buf.byteLength, new Blob([buf], { type: 'audio/mmf' }));
        renderHistory();
      };
      ul.appendChild(li);
    }
  } catch { ul.innerHTML = '<li style="cursor:default;color:var(--dim)">演示曲目未生成（跑 web/prepare.sh）</li>'; }
}

/* ---------------- 事件 ---------------- */
$('fileinput').onchange = e => handleFiles(e.target.files);
document.addEventListener('dragover', e => { e.preventDefault(); $('drop').classList.add('over'); });
document.addEventListener('dragleave', () => $('drop').classList.remove('over'));
document.addEventListener('drop', e => {
  e.preventDefault(); $('drop').classList.remove('over');
  handleFiles(e.dataTransfer.files);
});
$('btnPlay').onclick = () => S.playing ? pause() : play();
$('btnStop').onclick = stop;
$('btnLoop').onclick = () => { S.loop = !S.loop; $('btnLoop').classList.toggle('on', S.loop); };
$('vol').oninput = applyVolume;
$('btnMute').onclick = () => { muted = !muted; $('btnMute').textContent = muted ? '🔇' : '🔊'; applyVolume(); };
$('clearHistory').onclick = async () => {
  (await dbAll()).forEach(h => store('readwrite').delete(h.id));
  renderHistory();
};

/* ---------------- 启动 ---------------- */
window.loadTrack = loadTrack;        // 调试/自动化入口
renderDemos(); renderHistory(); drawScope();
bootCore();

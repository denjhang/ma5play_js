// mmf.js — 完整 MMF/SMAF 解析器（逐函数移植自 ymf825emu/src/mmf_parser.cpp）
// 支持：MobileStandard(2)/MobileCompressed(1, Huffman)/HPS(0)/SEQU(-1)，
//       MMMG 容器、独立 SEQU、多 MTR 轨、EXVO/Mtsu SysEx、CNTI 元信息。
// 输出：{ version, maName, title, artist, notes[{t,end,note,vel,ch}], durationMs, tracks[] }
// notes 的 note 已含 HPS/SEQU 八度移位；ch 为全局通道（HPS 多轨 = c + t*4）。

const TIMEBASE = { 0x00: 1, 0x01: 2, 0x02: 4, 0x03: 5, 0x10: 10, 0x11: 20, 0x12: 40, 0x13: 50 };
const SHORT_MOD = [0x00, 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x60, 0x70, 0x7F, 0x7F];
const SHORT_EXP = [0x00, 0x00, 0x1F, 0x27, 0x2F, 0x37, 0x3F, 0x47, 0x4F, 0x57, 0x5F, 0x67, 0x6F, 0x77, 0x7F, 0x7F];
const SIG = { MMMD: 0x4D4D4D44, CNTI: 0x434E5449, MTR: 0x4D545200, ATR: 0x41545200, SEQU: 0x53455155, MMMG: 0x4D4D4D47, MTSQ: 0x4D747371, MTSU: 0x4D747375, EXVO: 0x4558564F };

const u32 = (u8, p) => (u8[p] << 24 | u8[p + 1] << 16 | u8[p + 2] << 8 | u8[p + 3]) >>> 0;

function varint(u8, p, rest, allow3) {         // read_variable_int
  let result = 0, i = 0;
  while (rest > 0) {
    const b = u8[p]; p++; rest--;
    if (!allow3 && i === 1) return [(result + 0x80) | b, p, rest];
    result |= b & 0x7F;
    if (!(b & 0x80)) break;
    result <<= 7; i++;
  }
  return [result, p, rest];
}

/* -------- Huffman（MobileStandardCompressed）-------- */
class BitReader {
  constructor(u8, p, rest) { this.u8 = u8; this.p = p; this.rest = rest; this.buf = 0; this.bits = 0; }
  readBit() {
    if (this.bits === 0) {
      if (this.rest <= 0) return false;
      this.buf = this.u8[this.p++]; this.rest--; this.bits = 8;
    }
    const r = (this.buf & 0x80) !== 0;
    this.buf = (this.buf << 1) & 0xFF; this.bits--;
    return r;
  }
  readUint8() {
    if (this.rest <= 0) return 0;
    const b = this.u8[this.p++]; this.rest--;
    const r = this.buf | (b >> this.bits);
    this.buf = (b << (8 - this.bits)) & 0xFF;
    return r & 0xFF;
  }
}
function huffmanDecode(u8, p, rest) {          // 返回 [Uint8Array, 消费后p] 或 null
  if (rest < 4) return null;
  const outSize = u32(u8, p);
  p += 4; rest -= 4;
  const br = new BitReader(u8, p, rest);
  const left = new Int32Array(511), right = new Int32Array(511);
  let avail = 256;
  const rstack = [];
  let root = -1, rsp = 1;
  rstack[0] = { phase: 0, my: -1, parent: -1, slot: 0 };
  while (rsp > 0) {
    const e = rstack[rsp - 1];
    if (e.phase === 0) {
      if (!br.readBit()) {                     // 叶：8 位值
        const node = br.readUint8();
        if (e.parent < 0) root = node;
        else if (e.slot === 0) left[e.parent] = node; else right[e.parent] = node;
        rsp--; continue;
      }
      const inode = avail++;
      if (inode >= 511) { root = -1; break; }
      if (e.parent < 0) root = inode;
      else if (e.slot === 0) left[e.parent] = inode; else right[e.parent] = inode;
      e.my = inode; e.phase = 1;
      rstack[rsp++] = { phase: 0, my: -1, parent: inode, slot: 0 };
    } else if (e.phase === 1) {
      e.phase = 2;
      rstack[rsp++] = { phase: 0, my: -1, parent: e.my, slot: 1 };
    } else rsp--;
  }
  if (root < 0) return null;
  const dst = new Uint8Array(outSize);
  for (let k = 0; k < outSize; k++) {
    let j = root;
    let guard = 0;
    while (j >= 256 && guard++ < 4096) j = br.readBit() ? right[j] : left[j];
    dst[k] = j & 0xFF;
  }
  return [dst, br.p + (br.bits > 0 ? 1 : 0)];
}

/* -------- SysEx（read_exclusive，variable_length=true 路径）-------- */
function readExclusive(u8, p, rest) {
  let len;
  [len, p, rest] = varint(u8, p, rest, true);
  if (len <= 0) return [null, p, rest];
  len--;                                       // 不含 F7
  if (rest < len + 1) return [null, p, rest];
  const data = u8.slice(p, p + len);
  p += len; rest -= len;
  p++; rest--;                                 // 结束标记（应为 F7）
  return [data, p, rest];
}

/* -------- 事件解码：返回 {type,ch,note,vel,gate,...} 或 null -------- */
function evHps(u8, p, rest, octShift) {
  if (rest <= 0) return [null, p, rest];
  let sig = u8[p]; p++; rest--;
  if (sig === 0xFF) {
    if (rest < 1) return [null, p, rest];
    const s2 = u8[p]; p++; rest--;
    if (s2 === 0x00) return [{ type: 'nop' }, p, rest];
    if (s2 === 0xF0) { const [d, np, nr] = readExclusive(u8, p, rest); return [d ? { type: 'excl', data: d } : null, np, nr]; }
    return [null, p, rest];
  }
  if (sig !== 0) {                             // 音符
    const ch = sig >> 6, oct = (sig >> 4) & 3, nv = sig & 15;
    let gate; [gate, p, rest] = varint(u8, p, rest, false);
    return [{ type: 'note', ch, note: nv + (oct + 3) * 12, vel: 127, gate }, p, rest];
  }
  sig = u8[p]; p++; rest--;                    // 控制事件
  const ch = sig >> 6, type2 = (sig >> 4) & 3;
  if (type2 === 3) {
    if (rest < 1) return [null, p, rest];
    let v = u8[p]; p++; rest--;
    switch (sig & 15) {
      case 0: return [{ type: 'pc', ch, pc: v }, p, rest];
      case 2: if (v >= 0x80) v = 0x80 - v; return [{ type: 'oct', ch, v }, p, rest];
      case 4: return [{ type: 'bend', ch }, p, rest];
      case 7: return [{ type: 'cc', ch, cc: 7, v }, p, rest];
      case 10: return [{ type: 'cc', ch, cc: 10, v }, p, rest];
      case 11: return [{ type: 'cc', ch, cc: 11, v }, p, rest];
      case 1: return [{ type: 'cc', ch, cc: 32, v }, p, rest];    // bank LSB（C++ 同）
      case 3: return [{ type: 'cc', ch, cc: 1, v }, p, rest];     // modulation
      default: return [{ type: 'nop' }, p, rest];
    }
  }
  // type2 0/1/2 短格式（smaf825 表，C++ shortModTable/shortExpTable 同值）
  if (type2 === 2) return [{ type: 'cc', ch, cc: 1, v: SHORT_MOD[sig & 15] }, p, rest];
  if (type2 === 1) return [{ type: 'bend', ch }, p, rest];
  return [{ type: 'cc', ch, cc: 11, v: SHORT_EXP[sig & 15] }, p, rest];
}
/* Mobile 事件（fmt 2/1/3）。0x8X Note-Off 与 C++ 参考一致：也是 NOTE 事件
 * （velocity 取该通道上一个 0x9X 的 g_lastVelocity，note+gate 照读）——
 * sekai ni hana 一曲 ch9 鼓大量用 0x89 编码，按 nop 丢弃会丢 ~80% 音符。 */
const g_lastVel = new Array(32).fill(127);
function evMobile(u8, p, rest, fmt32) {
  if (rest <= 0) return [null, p, rest];
  let sig = u8[p]; p++; rest--;
  const st = fmt32 ? (sig | 0x80) : sig;
  const ch = st & 0x0F, status = st & 0xF0;
  if (status === 0x90) {
    if (rest < 2) return [null, p, rest];
    const note = u8[p]; p++; rest--;
    const vel = u8[p]; p++; rest--;
    g_lastVel[ch] = vel;
    let gate; [gate, p, rest] = varint(u8, p, rest, true);
    return [{ type: 'note', ch, note, vel, gate }, p, rest];
  }
  if (status === 0x80) {
    if (rest < 1) return [null, p, rest];
    const note = u8[p]; p++; rest--;
    let gate; [gate, p, rest] = varint(u8, p, rest, true);
    return [{ type: 'note', ch, note, vel: g_lastVel[ch], gate, off: true }, p, rest];
  }
  if (status === 0xB0) {
    if (rest < 2) return [null, p, rest];
    const cc = u8[p]; const v = u8[p + 1]; p += 2; rest -= 2;
    return [{ type: 'cc', ch, cc, v }, p, rest];
  }
  if (status === 0xC0) {
    if (rest < 1) return [null, p, rest];
    return [{ type: 'pc', ch, pc: u8[p++] }, p, --rest];
  }
  if (status === 0xE0) { p += 2; rest -= 2; return [{ type: 'bend', ch }, p, rest]; }
  if (st === 0xF0) { const [d, np, nr] = readExclusive(u8, p, rest); return [d ? { type: 'excl', data: d } : null, np, nr]; }
  if (st === 0xFF) { if (rest >= 1 && u8[p] === 0x00) { p++; rest--; } return [{ type: 'nop' }, p, rest]; }
  return [null, p, rest];
}
function evSequ(u8, p, rest, octShift) {
  if (rest <= 0) return [null, p, rest];
  let sig = u8[p]; p++; rest--;
  if (sig === 0x00) {
    if (rest < 1) return [null, p, rest];
    const s2 = u8[p]; p++; rest--;
    const ch = s2 >> 6, msg = s2 & 0x3F;
    const take1 = () => { if (rest < 1) return [null, p, rest]; const v = u8[p]; p++; rest--; return v; };
    if (msg === 0x00) return [take1() === null ? null : { type: 'nop' }, p, rest];          // fine tune（1 参）
    if (msg >= 0x01 && msg <= 0x0E) return [{ type: 'cc', ch, cc: 11, v: SHORT_EXP[msg] }, p, rest];
    if (msg >= 0x11 && msg <= 0x1E) return [{ type: 'bend', ch }, p, rest];
    if (msg >= 0x21 && msg <= 0x2E) return [{ type: 'cc', ch, cc: 1, v: SHORT_MOD[msg - 0x20] }, p, rest];
    if (msg === 0x30) { const v = take1(); return v === null ? [null, p, rest] : [{ type: 'pc', ch, pc: v }, p, rest]; }
    if (msg === 0x31) { const v = take1(); return v === null ? [null, p, rest] : [{ type: 'cc', ch, cc: 32, v }, p, rest]; }
    if (msg === 0x32) { const v = take1(); if (v === null) return [null, p, rest]; let o = v; if (o >= 0x80) o = 0x80 - o; return [{ type: 'oct', ch, v: o }, p, rest]; }
    if (msg === 0x33) { const v = take1(); return v === null ? [null, p, rest] : [{ type: 'cc', ch, cc: 1, v }, p, rest]; }
    if (msg === 0x34) { const v = take1(); return v === null ? [null, p, rest] : [{ type: 'bend', ch }, p, rest]; }
    if (msg === 0x36) { const v = take1(); return v === null ? [null, p, rest] : [{ type: 'cc', ch, cc: 11, v }, p, rest]; }
    if (msg === 0x37) { const v = take1(); return v === null ? [null, p, rest] : [{ type: 'cc', ch, cc: 7, v }, p, rest]; }
    if (msg === 0x3A) { const v = take1(); return v === null ? [null, p, rest] : [{ type: 'cc', ch, cc: 10, v }, p, rest]; }
    if (msg === 0x3B) { const v = take1(); return v === null ? [null, p, rest] : [{ type: 'cc', ch, cc: 11, v }, p, rest]; }
    return [{ type: 'nop' }, p, rest];
  }
  if (sig === 0xFF) {
    if (rest >= 1 && u8[p] === 0x00) { p++; rest--; return [{ type: 'nop' }, p, rest]; }
    if (rest >= 1 && u8[p] === 0xF0) { p++; rest--; const [d, np, nr] = readExclusive(u8, p, rest); return [d ? { type: 'excl', data: d } : null, np, nr]; }
    return [null, p, rest];
  }
  const ch = sig >> 6, oct = (sig >> 4) & 3, nv = sig & 15;       // 音符（同 HPS）
  let gate; [gate, p, rest] = varint(u8, p, rest, false);
  return [{ type: 'note', ch, note: nv + (oct + 3) * 12, vel: 127, gate }, p, rest];
}

/* -------- MTSQ 序列数据（parse_sequence_data）-------- */
function parseSequence(u8, off, size, fmt, chBase, out) {
  let p = off, rest = size, src = u8;
  if (fmt === 1) {                              // Huffman 解压
    const r = huffmanDecode(u8, p, rest);
    if (!r) return;
    src = r[0]; p = 0; rest = src.length;
  }
  const durTb = out.durTb, gateTb = out.gateTb;
  const octShift = new Int32Array(32);
  const allow3 = fmt === 2 || fmt === 1;
  let t = 0;
  while (rest > 0) {
    if (rest === 4 && u32eq0(src, p)) break;    // 4 字节全零 = EOS（非零则继续，同 C++）
    let d;
    [d, p, rest] = varint(src, p, rest, allow3);
    t += d * durTb;
    let ev;
    if (fmt === 0) [ev, p, rest] = evHps(src, p, rest, octShift);
    else if (fmt === 2 || fmt === 1) [ev, p, rest] = evMobile(src, p, rest, false);
    else if (fmt === 3) [ev, p, rest] = evMobile(src, p, rest, true);
    else if (fmt === -1) [ev, p, rest] = evSequ(src, p, rest, octShift);
    else break;
    if (!ev) continue;
    if (ev.type === 'oct') octShift[ev.ch] = ev.v;
    else if (ev.type === 'note') {
      const note = Math.max(0, Math.min(127, ev.note + octShift[ev.ch] * 12));
      out.notes.push({ t: t / 1000, end: (t + ev.gate * gateTb) / 1000, note, vel: ev.vel, ch: ev.ch + chBase });  // 单位: 秒
      out.chEv.push({ t: t / 1000, ch: ev.ch + chBase, k: 'Note', note, vel: ev.vel });
    } else if (ev.type === 'excl') out.excls.push(ev.data);
    else if (ev.type === 'cc') out.chEv.push({ t: t / 1000, ch: ev.ch + chBase, k: 'CC', cc: ev.cc, v: ev.v });
    else if (ev.type === 'pc') out.chEv.push({ t: t / 1000, ch: ev.ch + chBase, k: 'PC', pc: ev.pc });
    else if (ev.type === 'bend') out.chEv.push({ t: t / 1000, ch: ev.ch + chBase, k: 'Bend' });
  }
  out.durMs = Math.max(out.durMs, t);
}
const u32eq0 = (u8, p) => !u8[p] && !u8[p + 1] && !u8[p + 2] && !u8[p + 3];

/* -------- MTR 轨（parse_score_track）-------- */
function parseTrack(u8, off, size, chBase, out) {
  if (size < 4) return;
  const fmt = u8[off];                          // 0=HPS 1=压缩 2=普通 3=32ch(-1=SEQU 走上层)
  out.tracks.push({ format: fmt, seqType: u8[off + 1] });
  out.durTb = TIMEBASE[u8[off + 2]] ?? 2;
  out.gateTb = TIMEBASE[u8[off + 3]] ?? 2;
  let p = off + 4;
  const end = off + size;
  const stBytes = fmt === 0 ? 2 : (fmt === 3 ? 32 : 16);
  // 通道类型（NoCare/Melody/NoMel/Rhythm）——HPS 2 字节打包 / 其余每通道 1 字节
  if (fmt === 0) {
    const b = (u8[off + 4] << 8) | u8[off + 5];
    for (let c = 0; c < 4; c++) out.chTypes[chBase + c] = (b >> (12 - c * 4)) & 3;
  } else {
    for (let c = 0; c < stBytes && c < 32; c++) out.chTypes[chBase + c] = u8[off + 4 + c] & 3;
  }
  p += stBytes;
  while (p + 8 <= end) {
    const sig = u32(u8, p), csz = u32(u8, p + 4);
    p += 8;
    if (p + csz > end) break;
    if (sig === SIG.MTSQ) parseSequence(u8, p, csz, fmt, chBase, out);
    else if (sig === SIG.MTSU) {                // Setup：F0 SysEx 序列（FF 跳过）
      let q = p; const e = p + csz;
      while (q < e) {
        const s = u8[q];
        if (s === 0xFF) { q++; continue; }
        if (s === 0xF0) {
          let len; [len, q] = varint(u8, q + 1, e - q - 1, true);
          if (len > 0 && q + len - 1 <= e) {
            out.excls.push(u8.slice(q, q + len - 1));
            q += len - 1;
            if (u8[q] === 0xF7) q++;
          } else break;
        } else break;
      }
    } else if (sig === SIG.EXVO) {              // 整块 SysEx
      let len; [len] = varint(u8, p, csz, true);
      if (len > 0) out.excls.push(u8.slice(p + 1, p + 1 + len - 1 > end ? end : p + len - 1));
    } else if (sig === 0x4D747370) {            // "Mtsp"：PCM 波形块（Mwa 序列）
      let mp = p; const me = p + csz;
      while (me - mp >= 11 && u8[mp] === 0x4D && u8[mp + 1] === 0x77 && u8[mp + 2] === 0x61) {
        const type = u8[mp + 3];
        const len2 = u32(u8, mp + 4);
        if (len2 > 3 && 8 + len2 <= me - mp) {
          let hz = (u8[mp + 9] << 8) | u8[mp + 10];
          if (hz < 4000 || hz > 48000) hz = 16000;
          out.waves.push({ src: 'Mwa', type, stereo: (u8[mp + 8] & 0x80) !== 0, hz, size: len2 - 3 });
          mp += 8 + len2;
        } else break;
      }
    }
    p += csz;
  }
}
/* SysEx 全类型细分（ymf825emu parse_exclusive 同款语义）：
 * 声音注册（voices）+ 波形数据（waves） */
function scanExclusives(excls, out) {
  for (const d of excls) {
    const L = d.length;
    if (L >= 10 && d[0] === 0x43 && d[1] === 0x79 && d[2] === 0x07 && d[3] === 0x7F && d[4] === 0x01)
      out.voices.push({ kind: 'MA-5 Voice', bankM: d[5], bankL: d[6], pc: d[7], drum: d[8], vtype: d[9] === 0 ? 'FM' : 'PCM' });
    else if (L >= 10 && d[0] === 0x43 && d[1] === 0x79 && d[2] === 0x06 && d[3] === 0x7F && d[4] === 0x01)
      out.voices.push({ kind: 'MA-3 Voice', bankM: d[5], bankL: d[6], pc: d[7], drum: d[8], vtype: (d[9] & 1) ? 'WaveTable' : 'FM' });
    else if (L >= 6 && d[0] === 0x43 && d[1] === 0x79 && d[2] === 0x07 && d[3] === 0x7F && d[4] === 0x03)
      out.waves.push({ kind: 'MA-5 PCM waveform (ext)', form: '7F03', id: d[5], size: L - 6 });
    else if (L >= 6 && d[0] === 0x43 && d[1] === 0x79 && d[2] === 0x06 && d[3] === 0x7F && d[4] === 0x03)
      out.waves.push({ kind: 'MA-3 PCM waveform', id: d[5], size: L - 6 });
    else if (L >= 5 && d[0] === 0x43 && d[1] === 0x05 && d[2] === 0x01)
      out.voices.push({ kind: 'MA-5 FM Voice', bankL: d[3], pc: d[4], vtype: 'FM' });
    else if (L >= 5 && d[0] === 0x43 && d[1] === 0x05 && d[2] === 0x02)
      out.voices.push({ kind: 'MA-5 PCM Voice', bankL: d[3], pc: d[4], drum: (d[3] & 0x80) !== 0, vtype: 'PCM' });
    else if (L >= 4 && d[0] === 0x43 && d[1] === 0x05 && d[2] === 0x00)
      out.waves.push({ kind: 'MA-5 Wave data', form: '0500', id: d[3], size: L - 4 });
    else if (L >= 5 && d[0] === 0x43 && d[1] === 0x04 && d[2] === 0x01)
      out.voices.push({ kind: 'MA-5 4-op compact', vtype: 'FM' });
    else if (L >= 5 && d[0] === 0x43 && d[1] === 0x03)
      out.voices.push({ kind: 'MA-2 Voice (VMA)', bankL: d[3], pc: d[4], vtype: 'FM' });
    else if (L >= 5 && d[0] === 0x43 && d[1] === 0x02 && d[2] === 0x02)
      out.voices.push({ kind: 'SoftBank MA-5 legacy', vtype: 'PCM' });
  }
}
/* CNTI 标题：code_type!=0 时为逗号分隔 "ST:标题" 文本；=0 时为 tag+u16len 对 */
function parseCntiTitle(u8, off, size, out) {
  if (size < 5) return;
  const end = off + size;
  const ascii = (a, b) => { let r = ''; for (let i = a; i < b; i++) { const c = u8[i]; if (c >= 0x20 && c < 0x7F) r += String.fromCharCode(c); } return r; };
  if (u8[off + 2] !== 0x00) {
    const s = ascii(off + 5, end);
    const grab = tag => { const m = s.match(new RegExp(`(?:^|,)${tag}:([^,]*)`)); return m ? m[1].trim() : ''; };
    out.title = grab('ST'); out.artist = grab('AN');
    return;
  }
  let i = off + 5; const st = u8.slice(off + 5, end);
  let j = 0;
  while (j + 4 < st.length) {
    const tag = String.fromCharCode(st[j], st[j + 1]); j += 2;
    const sz = (st[j] << 8) | st[j + 1]; j += 2;
    if (j + sz > st.length) break;
    const val = ascii(j, j + sz);
    if (tag === 'ST') out.title = val;
    if (tag === 'AN') out.artist = val;
    j += sz;
  }
}
function versionFromCnti(stream) {              // mmf_detect_version_from_cnti
  for (let i = 0; i + 2 < stream.length; i++)
    if (stream[i] === 0x4D && stream[i + 1] >= 0x31 && stream[i + 1] <= 0x37 && stream[i + 2] === 0x3A)
      return stream[i + 1] - 0x30;
  return 0;
}
function versionFromExcls(excls) {              // mmf_detect_version_from_exclusives
  let v = 0;
  for (const d of excls) {
    const L = d.length;
    if (L >= 4 && d[0] === 0x43 && d[1] === 0x79 && d[2] === 0x08 && d[3] === 0x7F) { v = 7; continue; }
    if (L >= 5 && d[0] === 0x43 && d[1] === 0x79 && d[2] === 0x07 && d[3] === 0x7F) { if (v !== 7) v = 5; continue; }
    if (L >= 4 && d[0] === 0x43 && d[1] === 0x79 && d[2] === 0x06 && d[3] === 0x7F) { if (v !== 5 && v !== 7) v = 3; continue; }
    if (L >= 3 && d[0] === 0x43 && d[1] === 0x05 && d[2] === 0x01) { if (v !== 5 && v !== 3) v = 5; }
    else if (L >= 3 && d[0] === 0x43 && d[1] === 0x05 && d[2] === 0x02) { if (v !== 5 && v !== 3) v = 5; }
    else if (L >= 2 && d[0] === 0x43 && d[1] === 0x03) { if (!v) v = 2; }
    else if (L >= 2 && d[0] === 0x43 && d[1] === 0x02) { if (!v) v = 2; }
  }
  return v;
}

/* -------- 主入口（mmf_parse）-------- */
export function parseMMF(buf) {
  const u8 = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  const out = { version: 0, maName: 'Unknown', title: '', artist: '', notes: [], excls: [], tracks: [], durationMs: 0, error: '' };
  if (u8.length < 12 || u32(u8, 0) !== SIG.MMMD) { out.error = 'not MMF'; return out; }
  let p = 8;
  const end = u8.length - 2;                    // 尾部 CRC
  let trackIdx = 0;
  const out1 = { ...out, notes: [], excls: [], tracks: [], chEv: [], waves: [], voices: [], chTypes: new Array(32).fill(-1), atrCount: 0, durTb: 2, gateTb: 2, durMs: 0 };
  while (p + 8 <= end) {
    const sig = u32(u8, p), csz = u32(u8, p + 4);
    p += 8;
    if (p + csz > end) break;
    if (sig === SIG.CNTI) parseCntiTitle(u8, p, csz, out1);
    else if ((sig & 0xFFFFFF00) === 0x41545200) out1.atrCount++;   // "ATR*" ADPCM 音轨
    else if ((sig & 0xFFFFFF00) === SIG.MTR) {
      const fmt = u8[p];
      const chBase = (fmt === 0) ? trackIdx * 4 : 0;   // HPS 多轨：全局通道 = c + t*4
      parseTrack(u8, p, csz, chBase, out1);
      trackIdx++;
    } else if (sig === SIG.SEQU) {              // 独立 SEQU（无 MMMG 包装）
      out1.tracks.push({ format: -1, seqType: 0 });
      out1.durTb = out1.gateTb = 1;
      parseSequence(u8, p, csz, -1, 0, out1);
      trackIdx++;
    } else if (sig === SIG.MMMG) {              // MMMG：2 字节头（seqHeader, 字面 ms 时基）后为子块
      let q = p + 2;
      let mmmgTb = (u8[p + 1] > 0 && u8[p + 1] <= 250) ? u8[p + 1] : 20;  // ReMEXA 默认 20ms
      const me = p + csz;
      while (q + 8 <= me) {
        const msig = u32(u8, q), mcsz = u32(u8, q + 4);
        q += 8;
        if (q + mcsz > me) break;
        if ((msig & 0xFFFFFF00) === SIG.MTR) {
          const fmt = u8[q];
          parseTrack(u8, q, mcsz, fmt === 0 ? trackIdx * 4 : 0, out1);
          trackIdx++;
        } else if (msig === SIG.SEQU) {          // MMMG 内 SEQU（Beep 类系统音，字面 ms 时基）
          out1.tracks.push({ format: -1, seqType: 0 });
          out1.durTb = out1.gateTb = mmmgTb;
          parseSequence(u8, q, mcsz, -1, 0, out1);
          trackIdx++;
        } else if (msig === SIG.EXVO) {
          let len; [len] = varint(u8, q, mcsz, true);
          if (len > 0) out1.excls.push(u8.slice(q + 1, q + len - 1 > me ? me : q + len - 1));
        }
        q += mcsz;
      }
    }
    p += csz;
  }
  scanExclusives(out1.excls, out1);
  // 注册表去重（DefleMask 逐音符 SysEx 会产生上万条同键注册）
  { const seen = new Set(); out1.voices = out1.voices.filter(v => {
      const k = v.kind + '|' + (v.bankM ?? '') + '|' + (v.bankL ?? '') + '|' + (v.pc ?? '') + '|' + (v.vtype ?? '');
      if (seen.has(k)) return false; seen.add(k); return true;
    }); }
  { const seen = new Set(); out1.waves = out1.waves.filter(w => {
      const k = (w.kind ?? w.src) + '|' + (w.id ?? '') + (w.size ?? '');
      if (seen.has(k)) return false; seen.add(k); return true;
    }); }
  out1.version = versionFromCnti(out1.cntiStream ?? new Uint8Array(0)) || versionFromExcls(out1.excls);
  out1.maName = { 1: 'MA-1', 2: 'MA-2', 3: 'MA-3', 5: 'MA-5', 7: 'MA-7' }[out1.version] ?? 'Unknown';
  // 曲长 = 最后事件时间与最长音符结束的较大者
  out1.durationMs = Math.max(out1.durMs || 0, ...out1.notes.map(n => n.end), 0);
  return out1;
}

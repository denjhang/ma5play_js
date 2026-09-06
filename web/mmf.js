// mmf.js — MMF/SMAF 解析（移植自 dmplayer libymf825_ma5/src/mmf_parser.cpp，parser 可视化路线）
// 输出：{ version(2/3/5/7/0), title, notes:[{t,end,note,vel,ch}], durationMs }
// 支持 formatType 0(HPS/MA-1/2)、2(MobileNormal/16ch)、3(Mobile32ch/MA-7)；
// formatType 1(Huffman 压缩) 与 SEQU 暂不解析（notes 为空，不点亮钢琴）。
const TIMEBASE = { 0x00: 1, 0x01: 2, 0x02: 4, 0x03: 5, 0x10: 10, 0x11: 20, 0x12: 40, 0x13: 50 };

function varint(u8, p, allow3) {           // 返回 [value, 新指针]
  let result = 0, i = 0;
  while (p < u8.length) {
    const b = u8[p++];
    if (!allow3 && i === 1) return [(result + 0x80) | b, p];
    result |= b & 0x7F;
    if (!(b & 0x80)) break;
    result <<= 7; i++;
  }
  return [result, p];
}

// SysEx 独占数据扫描 → MA 版本（同 mmf_detect_version_from_exclusives）
function detectVersion(excls) {
  let v = 0;
  for (const d of excls) {
    if (d.length >= 4 && d[0] === 0x43 && d[1] === 0x79 && d[2] === 0x08 && d[3] === 0x7F) { v = 7; continue; }
    if (d.length >= 5 && d[0] === 0x43 && d[1] === 0x79 && d[2] === 0x07 && d[3] === 0x7F) { if (v !== 7) v = 5; continue; }
    if (d.length >= 4 && d[0] === 0x43 && d[1] === 0x79 && d[2] === 0x06 && d[3] === 0x7F) { if (v !== 5 && v !== 7) v = 3; continue; }
    if (d.length >= 3 && d[0] === 0x43 && d[1] === 0x05 && d[2] === 0x01) { if (v !== 5 && v !== 3) v = 5; }
    else if (d.length >= 3 && d[0] === 0x43 && d[1] === 0x05 && d[2] === 0x02) { if (v !== 5 && v !== 3) v = 5; }
    else if (d.length >= 2 && d[0] === 0x43 && d[1] === 0x03) { if (!v) v = 2; }
    else if (d.length >= 2 && d[0] === 0x43 && d[1] === 0x02) { if (!v) v = 2; }
  }
  return v;
}

// CNTI 标题：code_type!=0 时为逗号分隔 "ST:标题" 文本；=0 时为 tag+u16len 对
function parseCntiTitle(u8, off, size) {
  const end = off + size;
  if (size < 5) return '';
  const ascii = (a, b) => {
    let s = '';
    for (let i = a; i < b; i++) { const c = u8[i]; if (c >= 0x20 && c < 0x7F) s += String.fromCharCode(c); }
    return s;
  };
  if (u8[off + 2] !== 0x00) {               // 逗号分隔 "M2:x,L2:x,ST:title,..."
    const m = ascii(off + 5, end).match(/(?:^|,)ST:([^,]*)/);
    return m ? m[1].trim() : '';
  }
  let i = off + 5;                          // tag-value 对
  while (i + 4 < end) {
    const tag = String.fromCharCode(u8[i], u8[i + 1]); i += 2;
    const sz = (u8[i] << 8) | u8[i + 1]; i += 2;
    if (i + sz > end) break;
    if (tag === 'ST') return ascii(i, i + sz).trim();
    i += sz;
  }
  return '';
}

// SysEx 数据读：p 指向 F0 之后的长度 varint，格式 = F0 + len(varint) + 数据 + F7
function readExclusive(u8, p, end) {
  let len, q;
  [len, q] = varint(u8, p, false);
  const out = [];
  for (let i = 0; i < len && q < end; i++) out.push(u8[q++]);
  if (q < end && u8[q] === 0xF7) q++;         // 跳终结符
  return [out.slice(0, 64), q];
}

function parseTrackEvents(u8, off, size, notes) {
  const fmt = u8[off];
  const durTb = TIMEBASE[u8[off + 2]] ?? 2;
  const gateTb = TIMEBASE[u8[off + 3]] ?? 2;
  let p = off + 4;
  const end = off + size;
  // 通道状态区
  if (fmt === 0) p += 2;
  else if (fmt === 3) p += 32;
  else p += 16;
  // 子块
  let mtsq = null, mtsu = null;
  while (p + 8 <= end) {
    const sig = (u8[p] << 24) | (u8[p + 1] << 16) | (u8[p + 2] << 8) | u8[p + 3];
    const csz = (u8[p + 4] << 24) | (u8[p + 5] << 16) | (u8[p + 6] << 8) | u8[p + 7];
    p += 8;
    if (p + csz > end) break;
    if (sig === 0x4D747371) mtsq = [p, csz];
    else if (sig === 0x4D747375) mtsu = [p, csz];
    else if (sig === 0x4558564F) {          // "EXVO"：整块 SysEx
      const [d] = readExclusive(u8, p + 1, p + csz);
      excls.push(d);
    }
    p += csz;
  }
  const excls = [];
  if (mtsu) {                               // Mtsu：F0 SysEx 序列
    let q = mtsu[0]; const e = mtsu[0] + mtsu[1];
    while (q < e) {
      const s = u8[q];
      if (s === 0xFF) { q++; continue; }
      if (s === 0xF0) { const [d, nq] = readExclusive(u8, q, e); excls.push(d); q = nq; }
      else break;
    }
  }
  let durMs = 0;
  if (mtsq && (fmt === 0 || fmt === 2 || fmt === 3)) {
    let q = mtsq[0]; const e = mtsq[0] + mtsq[1];
    let t = 0;
    const allow3 = fmt !== 0;
    const lastVel = new Array(32).fill(0);
    const octShift = new Array(32).fill(0);
    while (q < e) {
      if (e - q === 4 && !u8[q] && !u8[q + 1] && !u8[q + 2] && !u8[q + 3]) break;  // EOS
      let d, gate;
      [d, q] = varint(u8, q, allow3);
      t += d * durTb;
      if (q >= e) break;
      const sig = u8[q++];
      if (fmt === 0) {
        // HPS：非零 sig = 音符（ch=sig>>6 oct=(sig>>4)&3 note=sig&15）
        if (sig !== 0xFF && sig !== 0) {
          [gate, q] = varint(u8, q, false);
          const ch = sig >> 6;
          const note = (sig & 15) + (((sig >> 4) & 3) + 3) * 12 + octShift[ch] * 12;
          notes.push({ t, end: t + gate * gateTb, note, vel: 127, ch });
        } else if (sig === 0xFF) {
          if (q < e && u8[q] === 0) { q++; }
          else if (q < e && u8[q] === 0xF0) { const [dd, nq] = readExclusive(u8, q + 1, e); excls.push(dd); q = nq; }
        } else {
          // sig==0：控制事件
          if (q >= e) break;
          const c = u8[q++];
          const ch = c >> 6, type2 = (c >> 4) & 3, sub = c & 15;
          if (type2 === 3) {
            if (q < e) { const v = u8[q++]; if (sub === 2) { let ov = v >= 0x80 ? 0x80 - v : v; octShift[ch] = ov; } }
          } else if (type2 === 0 && q < e) q++;      // 2 参数控制，跳过
        }
      } else {
        // Mobile normal / 32ch：MIDI 风格
        const st = fmt === 3 ? (sig | 0x80) : sig;
        const ch = st & 0x0F, status = st & 0xF0;
        if (status === 0x90) {
          if (q >= e) break;
          const note = u8[q++];
          if (q >= e) break;
          const vel = u8[q++]; lastVel[ch] = vel;
          [gate, q] = varint(u8, q, true);
          if (vel > 0) notes.push({ t, end: t + gate * gateTb, note, vel, ch });
        } else if (status === 0x80) {
          if (q >= e) break;
          q++;                                    // note
          [gate, q] = varint(u8, q, true);        // dur
        } else if (status === 0xB0) { q += 2; }
        else if (status === 0xC0) { q += 1; }
        else if (status === 0xE0) { q += 2; }
        else if (st === 0xF0) { const [dd, nq] = readExclusive(u8, q, e); excls.push(dd); q = nq; }
        else if (st === 0xFF) { if (q < e && u8[q] === 0) q++; }
      }
    }
    durMs = t;
  }
  return { excls, durMs };
}

export function parseMMF(buf) {
  const u8 = new Uint8Array(buf);
  const notes = [];
  const excls = [];
  let title = '', durMs = 0;
  if (u8.length < 16 || u8[0] !== 0x4D || u8[1] !== 0x4D && u8[1] !== 0x4D) {
    // 容器头 "MMMD"
  }
  if (!(u8[0] === 0x4D && u8[1] === 0x4D && u8[2] === 0x4D && u8[3] === 0x44)) return { version: 0, title, notes, durationMs: 0 };
  let p = 8;                                 // MMMD(4) + 总长 u32(4)，块从 8 起
  const end = u8.length;
  while (p + 8 <= end) {
    const sz = (u8[p + 4] << 24) | (u8[p + 5] << 16) | (u8[p + 6] << 8) | u8[p + 7];
    const sig = (u8[p] << 24) | (u8[p + 1] << 16) | (u8[p + 2] << 8) | u8[p + 3];
    const body = p + 8;
    if (body + sz > end) break;
    if (sig === 0x434E5449) title = parseCntiTitle(u8, body, sz);
    else if ((sig & 0xFFFFFF00) === 0x4D545200) {
      const r = parseTrackEvents(u8, body, sz, notes);
      excls.push(...r.excls);
      durMs = Math.max(durMs, r.durMs);
    }
    p = body + sz;
  }
  return { version: detectVersion(excls), title, notes, durationMs: durMs };
}

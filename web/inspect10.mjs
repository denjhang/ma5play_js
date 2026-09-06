import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { parseMMF } from './mmf.js';
const ROOT = 'D:/working/vscode-projects/YM2163-Midi/Denjhang_Music_Player_v16/ma2play/bin/mmf';
const files = [];
const walk = d => { for (const e of readdirSync(d, { withFileTypes: true })) { if (e.isDirectory()) walk(join(d, e.name)); else if (/\.mmf$/i.test(e.name)) files.push(join(d, e.name)); } };
walk(ROOT);
const byVer = { 2: [], 3: [], 5: [] };
for (const f of files) {
  let m; try { m = parseMMF(readFileSync(f)); } catch { continue; }
  if (byVer[m.version] && byVer[m.version].length < 10) byVer[m.version].push([f, m]);
}
const CH_T = ['NoCare', 'Melody', 'NoMel', 'Rhythm'];
for (const ver of [2, 3, 5]) {
  console.log(`\n========== MA-${ver} ×10 ==========`);
  for (const [f, m] of byVer[ver]) {
    const name = f.split(/[\/]/).pop();
    const usedCh = [...new Set(m.notes.map(n => n.ch))].sort((a, b) => a - b);
    const chT = m.chTypes.map((t, i) => t >= 0 ? `${i}:${CH_T[t]}` : null).filter(Boolean).slice(0, 8).join(' ');
    const waveKinds = [...new Set(m.waves.map(w => w.kind ?? w.src))].join('+') || '-';
    const voiceKinds = [...new Set(m.voices.map(v => v.kind))].join('+') || '-';
    console.log(`${name.slice(0, 26).padEnd(26)} notes=${String(m.notes.length).padStart(4)} ch=[${usedCh}] atr=${m.atrCount} waves=${m.waves.length}(${waveKinds}) voices=${m.voices.length}(${voiceKinds})`);
    console.log(`${' '.repeat(26)} chTypes: ${chT || '-'} | title=${m.title ? m.title.slice(0, 20) : '-'} | dur=${(m.durationMs / 1000).toFixed(1)}s`);
  }
}

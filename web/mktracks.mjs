// mktracks.mjs — 生成 web/assets/tracks.json（静态曲库索引，GitHub Pages 用）
// 用法: node web/mktracks.mjs
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const root = path.join(path.dirname(fileURLToPath(import.meta.url)), 'assets', 'tracks');
const index = {};
const walk = (dir, rel) => {
  const entries = [];
  for (const e of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }))) {
    if (e.name.startsWith('.')) continue;
    if (e.isDirectory()) {
      walk(path.join(dir, e.name), rel + e.name + '/');
    } else if (/\.mmf$/i.test(e.name)) {
      entries.push({ name: e.name, size: fs.statSync(path.join(dir, e.name)).size });
    }
  }
  if (entries.length) index[rel] = entries;
};
walk(root, '');
fs.writeFileSync(path.join(root, '..', 'tracks.json'), JSON.stringify(index));
console.log('tracks.json:', Object.keys(index).length, 'dirs,', Object.values(index).reduce((a, v) => a + v.length, 0), 'files');

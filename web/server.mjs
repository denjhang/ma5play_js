// server.mjs — 本地测试静态服务器 + 目录浏览 API（复刻 ma2play 文件浏览器的数据源）
// node web/server.mjs [port]  默认 http://127.0.0.1:8095
// 仅监听 127.0.0.1，本机测试工具，目录 API 有意允许浏览本机任意可读路径。
import { createServer } from 'node:http';
import { readFile, readdir } from 'node:fs/promises';
import { extname, join, normalize, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { statSync } from 'node:fs';

const ROOT = fileURLToPath(new URL('.', import.meta.url));
const PORT = Number(process.argv[2] ?? 8095);
const DEFAULT_DIR = 'D:\\working\\vscode-projects\\YM2163-Midi\\Denjhang_Music_Player_v16\\ma2play\\bin\\mmf';
const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.css': 'text/css',
  '.wasm': 'application/wasm', '.bin': 'application/octet-stream',
  '.z': 'application/octet-stream', '.mmf': 'application/octet-stream',
  '.json': 'application/json', '.png': 'image/png', '.ico': 'image/x-icon',
};

const H = { 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp', 'Cache-Control': 'no-store' };

createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  try {
    if (url.pathname === '/api/list') {
      let dir = url.searchParams.get('path') || DEFAULT_DIR;
      dir = resolve(dir);
      let entries;
      try { entries = await readdir(dir, { withFileTypes: true }); }
      catch (e) { res.writeHead(400, { 'Content-Type': 'application/json', ...H }); return res.end(JSON.stringify({ error: '' + e.code })); }
      const list = [];
      for (const e of entries) {
        try {
          const full = join(dir, e.name);
          if (e.isDirectory()) list.push({ name: e.name, dir: true });
          else if (/\.(mmf|mld|pmd|mid)$/i.test(e.name))
            list.push({ name: e.name, dir: false, size: statSync(full).size });
        } catch { /* 无权限项跳过 */ }
      }
      // 目录在前，各自按名字排（同 ma2play 浏览器习惯）
      list.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name, 'zh-CN', { numeric: true }));
      res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8', ...H });
      return res.end(JSON.stringify({ path: dir, parent: dirname(dir) === dir ? null : dirname(dir), entries: list }));
    }
    if (url.pathname === '/api/file') {
      const p = resolve(url.searchParams.get('path') ?? '');
      if (!/\.(mmf)$/i.test(p)) { res.writeHead(403); return res.end(); }
      const data = await readFile(p);
      res.writeHead(200, { 'Content-Type': 'application/octet-stream', ...H });
      return res.end(data);
    }
    let p = decodeURIComponent(url.pathname);
    if (p === '/') p = '/index.html';
    const file = normalize(join(ROOT, p));
    if (!file.startsWith(ROOT)) { res.writeHead(403); return res.end(); }
    const data = await readFile(file);
    res.writeHead(200, { 'Content-Type': MIME[extname(file)] ?? 'application/octet-stream', ...H });
    res.end(data);
  } catch {
    res.writeHead(404); res.end('not found');
  }
}).listen(PORT, '127.0.0.1', () => console.log(`ma5play test server: http://127.0.0.1:${PORT}`));

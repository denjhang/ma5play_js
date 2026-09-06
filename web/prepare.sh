#!/bin/sh
# prepare.sh — 拷贝运行期资源到 web/assets/（产物不入 git，本脚本可再生）
# 注意：含 node 图标提取，需在能找到 node 的环境跑（git bash 或 export PATH）
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${MA5T_SRC:-/d/working/vscode-projects/YM2163-Midi/Denjhang_Music_Player_v16/ma2play}"
A="$ROOT/web/assets"
mkdir -p "$A/tracks"

cp "$ROOT/core/build/ma5play.js"  "$A/"
cp "$ROOT/core/build/ma5play.wasm" "$A/"
[ -f "$ROOT/core/build/ma5play.data" ] && cp "$ROOT/core/build/ma5play.data" "$A/"

# 预载映像（worker 内 DecompressionStream 解压）
cp "$SRC/libma5t_compact/ma5_ds.bin.z" "$A/"
rm -f "$A/preload.bin"

# ma2play 应用图标（ICO 内嵌 PNG 项，取最大尺寸；node 是 Windows 程序，路径要 cygpath 转换）
WIN_SRC="$(cygpath -w "$SRC")"
WIN_A="$(cygpath -w "$A")"
node -e "
const fs = require('fs');
const ico = fs.readFileSync(process.argv[1] + '\\\\src\\\\app_icon.ico');
const n = ico.readUInt16LE(4);
let best = null;
for (let i = 0; i < n; i++) {
  const o = 6 + i * 16;
  const w = ico[o] || 256, size = ico.readUInt32LE(o + 8), off = ico.readUInt32LE(o + 12);
  if (ico[off] === 0x89 && ico[off+1] === 0x50 && (!best || w > best.w)) best = { w, off, size };
}
fs.writeFileSync(process.argv[2] + '\\\\icon.png', ico.slice(best.off, best.off + best.size));
console.log('icon.png ' + best.w + 'px');
" "$WIN_SRC" "$WIN_A"

# 演示曲目（MA-5 语料 64poly）
i=0
: > "$A/tracks/manifest.json"
echo "[" >> "$A/tracks/manifest.json"
for f in "$SRC/bin/mmf/MMF's for Samsung phones (from the Samsung PC Studio)/64poly/"Melody0*.mmf; do
  i=$((i+1)); cp "$f" "$A/tracks/"; echo "\"$(basename "$f")\"," >> "$A/tracks/manifest.json"
done
sed -i '$ s/,$//' "$A/tracks/manifest.json"
echo "]" >> "$A/tracks/manifest.json"
echo "assets ready: $i demo tracks, ma5_ds.bin.z $(du -h "$A/ma5_ds.bin.z" | cut -f1)"

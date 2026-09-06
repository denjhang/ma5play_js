#!/bin/sh
# package.sh — 打 web 发布包（dist/ma5play-<ver>-web.zip）
# 用法: sh package.sh [version]   （默认从 git describe 取，否则 v0.1.0）
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
VER="${1:-$(git -C "$ROOT" describe --tags 2>/dev/null || echo v0.1.0)}"
STAGE="$ROOT/dist/ma5play-$VER-web"
ZIP="$ROOT/dist/ma5play-$VER-web.tar.gz"

rm -rf "$STAGE" "$ZIP" "$ROOT/dist/ma5play-$VER-web.tar.gz"
mkdir -p "$STAGE"

# 播放器运行时（不含 tracks/、开发探针）
mkdir -p "$STAGE/web/assets"
cp "$ROOT"/web/index.html "$ROOT"/web/app.js "$ROOT"/web/mmf.js \
   "$ROOT"/web/render-worker.js "$ROOT"/web/style.css "$ROOT"/web/server.mjs "$STAGE/web/"
cp "$ROOT"/web/assets/ma5play.js "$ROOT"/web/assets/ma5play.wasm \
   "$ROOT"/web/assets/ma5_ds.bin.z "$ROOT"/web/assets/icon.png "$STAGE/web/assets/"
[ -f "$ROOT/web/assets/ma5play.data" ] && cp "$ROOT/web/assets/ma5play.data" "$STAGE/web/assets/"

# 文档
cp "$ROOT/README.md" "$ROOT/LICENSE" "$STAGE/"

# manifest：包内容清单 + 校验和
( cd "$STAGE" && find . -type f | sort | sed 's|^\./||' > MANIFEST.txt \
  && find . -type f | sort | xargs sha256sum > SHA256SUMS.txt )

mkdir -p "$ROOT/dist"
( cd "$ROOT/dist" && tar -czf "ma5play-$VER-web.tar.gz" "ma5play-$VER-web" )
echo "== $ZIP =="
tar -tzf "$ZIP" | wc -l

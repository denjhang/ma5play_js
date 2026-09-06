#!/bin/sh
# build.sh — emcc 编译 libma5t → ma5play wasm
# 用法: ./build.sh [node|web]   (默认 node；web 变体待 AudioWorklet 阶段定数据打包)
# 工具链: msys2 pacman mingw-w64-ucrt-x86_64-emscripten 6.0.9
#   PATH=/d/msys64/ucrt64/bin:$PATH; emcc 入口 /ucrt64/lib/emscripten/emcc
set -e
export PATH="/ucrt64/bin:$PATH"   # emcc 二进制工具（wasm-opt 等）在此
ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="${MA5T_SRC:-/d/working/vscode-projects/YM2163-Midi/Denjhang_Music_Player_v16/ma2play/libma5t_exp}"
OUT="$ROOT/build"
MODE="${1:-node}"

SRCS="$SRC/i386.c $SRC/fpu.c $SRC/pe_loader.c $SRC/win32_stubs.c $SRC/ma5t_host.c $ROOT/ma5play_shell.c"
# 与原生构建（libma5t_exp/PLAN.md 顶部）同一开关集
OPT="${MA5PLAY_OPT:--O2}"
CFLAGS="$OPT -w -I$SRC -include uc_shim.h -DI386_ENABLE_FPU -DI386_NATIVE_HOOKS -D_stricmp=strcasecmp"
WFLAGS="-sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sINITIAL_MEMORY=64MB \
  -sSTACK_SIZE=16MB \
  -sMODULARIZE=1 -sEXPORT_NAME=ma5play \
  -sEXPORTED_FUNCTIONS=_ma5w_set_preload,_ma5w_init,_ma5w_load,_ma5w_open_standby_start,_ma5w_pump_seq,_ma5w_pump_audio,_ma5w_take_pcm,_ma5w_last_error,_ma5w_compact_mode,_ma5w_loaded,_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=ccall,HEAPU8"

mkdir -p "$OUT"

if [ "$MODE" = "node" ]; then
  # node 冒烟: NODERAWFS 直接读盘上的 m5_snapshot.bin（cwd 需含该文件）
  /ucrt64/lib/emscripten/emcc $CFLAGS $WFLAGS -sENVIRONMENT=node -sNODERAWFS \
    $SRCS -o "$OUT/ma5play_node.js"
  echo "node 变体 -> $OUT/ma5play_node.js"
else
  # web: 快照走 Emscripten 打包文件系统（flat）；compact 路线以后可切 fetch+DecompressionStream
  /ucrt64/lib/emscripten/emcc $CFLAGS $WFLAGS -sENVIRONMENT=web,worker \
    --preload-file "$SRC/m5_snapshot.bin@/m5_snapshot.bin" \
    $SRCS -o "$OUT/ma5play.js"
  echo "web 变体 -> $OUT/ma5play.js(.wasm/.data)"
fi

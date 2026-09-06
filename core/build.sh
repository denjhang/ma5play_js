#!/bin/sh
# build.sh — emcc 编译 libma5t → ma5play wasm
# 用法: ./build.sh [node|web]   (默认 node；web 变体待 AudioWorklet 阶段定数据打包)
# 工具链: msys2 pacman mingw-w64-ucrt-x86_64-emscripten 6.0.9
#   PATH=/d/msys64/ucrt64/bin:$PATH; emcc 入口 /ucrt64/lib/emscripten/emcc
set -e
export PATH="/ucrt64/bin:$PATH"   # emcc 二进制工具（wasm-opt 等）在此
ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="${MA5T_SRC:-/d/working/vscode-projects/YM2163-Midi/Denjhang_Music_Player_v16/ma2play/libma5t_compact}"
OUT="$ROOT/build"
MODE="${1:-node}"

SRCS="$SRC/i386.c $SRC/fpu.c $SRC/pe_loader.c $SRC/win32_stubs.c $SRC/ma5t_host.c $ROOT/ma5play_shell.c"
# 与原生构建（libma5t_exp/PLAN.md 顶部）同一开关集
OPT="${MA5PLAY_OPT:--O2}"
# -fwrapv/-fno-strict-aliasing：核心原生链（flt_main 等）大量故意 int32 回绕 +
# uint32* 类型双关，gcc 行为是验收基准；clang 必须显式对齐语义，否则效果链数值
# 发散（melody05 哇音滤波器损坏，2026-09-06 实测 wasm≠原生 md5）
CFLAGS="$OPT -w -fwrapv -fno-strict-aliasing -I$SRC -include uc_shim.h -DI386_ENABLE_FPU -DI386_NATIVE_HOOKS -D_stricmp=strcasecmp"
WFLAGS="-sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sINITIAL_MEMORY=64MB \
  -sSTACK_SIZE=16MB \
  -sMODULARIZE=1 -sEXPORT_NAME=ma5play \
  -sEXPORTED_FUNCTIONS=_ma5w_set_preload,_ma5w_init,_ma5w_load,_ma5w_pump,_ma5w_pause,_ma5w_resume,_ma5w_seek_play,_ma5w_ended,_ma5w_played_ms,_ma5w_last_error,_ma5w_compact_mode,_ma5w_loaded,_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=ccall,HEAPU8"

mkdir -p "$OUT"

if [ "$MODE" = "node" ]; then
  # node 冒烟: NODERAWFS 直接读盘上的 m5_snapshot.bin（cwd 需含该文件）
  /ucrt64/lib/emscripten/emcc $CFLAGS $WFLAGS -sENVIRONMENT=node -sNODERAWFS \
    $SRCS -o "$OUT/ma5play_node.js"
  echo "node 变体 -> $OUT/ma5play_node.js"
else
  # web: DLL 打进 MEMFS（compact host 从 cwd 磁盘找 DLL）；预载映像走 fetch
  /ucrt64/lib/emscripten/emcc $CFLAGS $WFLAGS -sENVIRONMENT=web,worker \
    --preload-file "$SRC/M5_EmuSmw5.dll@/M5_EmuSmw5.dll" \
    --preload-file "$SRC/M5_EmuHw.dll@/M5_EmuHw.dll" \
    $SRCS -o "$OUT/ma5play.js"
  echo "web 变体 -> $OUT/ma5play.js(.wasm/.data)"
fi

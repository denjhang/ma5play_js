# ma5play_js

ma5t WASM 移植 + 网页版在线播放器。本项目独立于 dmplayer
（Denjhang_Music_Player_v16），立项背景见 dmplayer 仓库
`ma2play/DEVELOPMENT.md` 的「2026-09-05 立项」小节。
交接文档：[HANDOFF.md](HANDOFF.md)。

## 目标
- ma5t（DLL bit-exact）经 emcc 编译为 WASM（这是 ma5t 的 wasm 化，不是原生
  cpp 的 ymf825 核心）
- AudioWorklet 壳 + 通道状态快照（48 通道：16FM + 32PCM）
- 现代移动端 PWA UI，Modizer 式钢琴键盘可视化（FM/PCM 分色）
- 最终整个包部署上线

## 源码引用（不复制，直接引用 dmplayer）
- 四件套：`../YM2163-Midi/Denjhang_Music_Player_v16/ma2play/libma5t*/`
  （i386.c / ma5t_host.c / ma5t_player.c / win32_stubs.c）
- 预载映像：ma5_ds.bin.z（浏览器端 DecompressionStream("deflate") 解压）

## 目录规划
- `core/`    — emcc 构建脚本 + C 壳层
- `worklet/` — AudioWorklet 处理器
- `web/`     — 前端（键盘可视化、进度、PWA manifest）
- `docs/`    — 进度记录

## 落地顺序
1. emcc 工具链（msys2 pacman 装 mingw-w64-ucrt-x86_64-emscripten 6.0.9，
   msys2 在 D:\msys64）
2. 编译四件套（flat + ALLOW_MEMORY_GROWTH，失败切 compact 模式）
3. node 渲染冒烟测试：能出 PCM 即通过，**不做 wav/md5 对拍**
4. AudioWorklet + 通道状态导出
5. 键盘可视化 + 移动端 PWA UI

## 参考
- chip-player-js（mmontag）：player-emu WASM 壳可抄（本机已有
  `chip-player-js-master`）
- chiptune3（npm）：AudioWorklet 轻量骨架
- Modizer：交互概念参考，源码在
  `Reference_Project/vgm_libs/modizer-master-20260408`（Obj-C UI 不复用）

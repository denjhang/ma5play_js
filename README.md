# ma5play

ma5t WASM 移植 + 网页版在线播放器。独立于 dmplayer（Denjhang_Music_Player_v16），
立项背景见 dmplayer 仓库 `ma2play/DEVELOPMENT.md` 的「2026-09-05 立项」小节。
项目记忆：[AGENTS.md](AGENTS.md)；逐阶段进度：[docs/PROGRESS.md](docs/PROGRESS.md)。

## 目标
- ma5t（DLL bit-exact t386 仿真，libma5t_compact = GUI ma5t 后端同源）经 emcc
  编译为 WASM
- Worker 渲染 + 消息流控 PCM；AudioWorklet 壳 + 通道状态快照（48ch，规划中）
- 现代移动端 PWA UI；ma2play 式布局：文件浏览器（面包屑/文件夹历史/传输条）+
  Log + 钢琴键盘可视化（parser 时间轴，VIS_PARSER 同思路）
- 最终整包部署上线

## 当前能力
- 完整 MMF 解析（ymf825emu/src/mmf_parser.cpp 移植）：MA-1/2/3/5/7 版本判定、
  标题、音符时间轴、Huffman 压缩格式
- mel05 级别的效果链保真：泵块 960 帧与 GUI 一致，md5 与原生 tp5 逐字节一致
- 播放/暂停/停止/循环/音量、曲终自动下一曲、切曲无残留、预滚防卡顿

## 源码引用（不复制，直接引用 dmplayer）
- 核心：`../YM2163-Midi/Denjhang_Music_Player_v16/ma2play/libma5t_compact/`
  （i386.c / fpu.c / pe_loader.c / win32_stubs.c / ma5t_host.c）
- 预载映像：ma5_ds.bin.z（浏览器端 DecompressionStream 解压）

## 目录
- `core/`    — emcc 构建脚本 + C 壳层（ma5w_* 导出）+ 等价性验证脚本
- `web/`     — 前端（app.js / mmf.js / render-worker.js / server.mjs / prepare.sh）
- `worklet/` — AudioWorklet 处理器（待建）
- `docs/`    — 进度记录

## 运行
```sh
# 1. 构建（git bash；务必 -O1，默认 -O2 会撞本机 wasm-opt 挂死）
/d/msys64/usr/bin/bash -lc "cd core && MA5PLAY_OPT=-O1 sh build.sh web"
# 2. 资源
sh web/prepare.sh
# 3. 测试服务器
node web/server.mjs        # → http://127.0.0.1:8095
```

## 落地顺序（剩余）
1. 进度条 seek（ma5w_seek_play 已就绪，待接 UI）
2. AudioWorklet 壳 + 48ch 通道状态快照（钢琴升级为实时通道数据）
3. PWA manifest + 部署

## 参考
- chip-player-js（mmontag）：player-emu WASM 壳（本机 `chip-player-js-master`）
- ymf825emu（同作者）：MMF 解析器权威（`D:\working\vscode-projects\ymf825emu`）
- Modizer：键盘可视化交互概念参考

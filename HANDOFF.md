# HANDOFF — ma5play_js 交接（2026-09-05）

## 当前状态
- 项目刚独立创建于 `D:\working\vscode-projects\ma5play_js`（独立 git 仓库，
  与 dmplayer 分离；最终要整个包部署上线）。
- emscripten **尚未装好**：用户裁定用 msys2 pacman 安装，包名
  `mingw-w64-ucrt-x86_64-emscripten`（6.0.9，在 ucrt64 仓库；注意不是
  mingw-w64-x86_64- 前缀，那个不存在）。msys2 根在 `D:\msys64`，安装命令：
  `/d/msys64/usr/bin/bash -lc "pacman -S --noconfirm mingw-w64-ucrt-x86_64-emscripten"`
  （上次执行到一半被叫停去改需求，装完后 emcc 在 ucrt64 环境里）。
- 尚未写任何构建脚本或代码。

## 用户明确的要求（务必遵守）
1. **不做 wav/md5 对拍**——用户多次强调。node 渲染冒烟测试只要求能出 PCM。
2. 目标是 **ma5t 的 wasm 化**，不是原生 cpp 的 ymf825 核心（ymf825 路线已暂停）。
3. 工具链用 msys2 pacman 装，不用 emsdk。
4. 键盘可视化参考 Modizer 的交互概念（核心每周期上报通道 key-on/note/vel，
   UI 点亮钢琴键），外观不抄老 iOS。
5. 界面要手机端友好、现代化（PWA、深色、大触控目标）。

## 下一步（按序）
1. pacman 装 emscripten（命令见上）
2. 在 `core/` 写 emcc 构建脚本，编译 dmplayer 的 libma5t* 四件套
   （flat + ALLOW_MEMORY_GROWTH 优先，内存问题切 compact 模式）
3. node 冒烟：加载 wasm、渲染一段 PCM 确认出声
4. `worklet/` AudioWorklet 壳 + 通道状态快照导出
   （pump 末尾导出 `{keyOn,note,vel,type} ch[48]`，参考 MA5T_TRIGLOG 钩子先例）
5. `web/` 键盘可视化 + PWA UI

## 关键背景
- ma5t 源码（只引用不复制）：
  `D:\working\vscode-projects\YM2163-Midi\Denjhang_Music_Player_v16\ma2play\libma5t*`
- win32_stubs.c 已是自包含拦截层（CreateThread 短路、CRITICAL_SECTION no-op、
  dsound/winmm 桩）→ WASM 单线程可原样工作
- 性能预估：原生 2.1x realtime，wasm 桌面够用，低端手机贴线；
  缓解 = 块状按需渲染 + 预渲染缓冲
- 参考：chip-player-js（本机 `D:\working\vscode-projects\chip-player-js-master`）、
  chiptune3 npm 包、Modizer 源码
- dmplayer 侧立项记录：`ma2play/DEVELOPMENT.md:2830`（commit 016c89d）
- dmplayer 里误建的 `ma5play_js/` 目录已删除（README 移到本仓库）

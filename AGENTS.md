# ma5play_js 项目记忆（AGENTS.md）

> 本文件是项目记忆。每次会话开始先读本文件 + [docs/PROGRESS.md](docs/PROGRESS.md)。
> 逆向方法论、ma5t 历史细节在 dmplayer 仓库文档里（见"外部文档"），本文件只记
> 本项目直接相关的事实与规则，避免重复维护。

## 项目目标（用户裁定，不可动摇）

- **ma5t（DLL bit-exact t386 仿真）的 wasm 化** + 网页在线播放器，最终整包部署上线。
  不是原生 cpp 的 ymf825 核心（该路线已暂停）。
- 架构：ma5t C 核心 --emcc--> wasm → **Worker 渲染**（消息传递 PCM 流控）→
  主线程 ScriptProcessor 消费 → 现代 PWA UI；Modizer 式钢琴键盘可视化。
- **不做 wav/md5 对拍验收**（听感由用户做）；md5 只用于核心等价性定位。
- 工具链用 msys2 pacman 的 emscripten，不用 emsdk。

## 当前状态（2026-09-06）

- ✅ 工具链 / 构建 / node 冒烟（compact 核心全曲渲染）
- ✅ web 播放器：ma2play 布局（左 File Info | 右上 钢琴+48ch 占位 |
  右下 文件浏览器[传输条+面包屑+文件夹历史]+Log）
- ✅ Worker 渲染 + 消息流控；切曲干净（停泵→淡出→清缓冲→加载）
- ✅ 完整 MMF 解析器（web/mmf.js，ymf825emu/src/mmf_parser.cpp 逐函数移植）
- ✅ mel4ody05 效果器问题定位并修复（泵块尺寸，见铁律）
- ⬜ 进度条 seek（ma5w_seek_play 已就绪待接 UI）
- ⬜ AudioWorklet 壳 + 48ch 通道状态快照
- ⬜ -O2 wasm-opt 挂死排查（现用 -O1）

## 目录

```
core/   build.sh（emcc）、ma5play_shell.c（ma5w_* 导出）、smoke.mjs、
        switch_test.mjs、md5check.mjs、cmpflag.mjs（等价性工具，勿删）
worklet/  （待建）
web/    index.html / style.css / app.js / mmf.js / render-worker.js /
        server.mjs（静态+目录 API，127.0.0.1:8095）/ prepare.sh（资源再生）
docs/   PROGRESS.md
build/  构建产物（不入库）
```

## 核心技术事实（已验证，违反会复现 bug）

1. **源 = libma5t_compact（GUI ma5t 后端同源）**，不是 libma5t_exp（离线实验目录）。
   五件套 i386/fpu/pe_loader/win32_stubs/ma5t_host + 本项目 shell。
   曲终判定 = no_pcm_iters > 100（round19 教训，>5 会误杀短断流曲目）。
2. **泵块尺寸必须 960 帧（20ms）**——GUI/tp5 同款。2400 帧(50ms)会损坏
   DLL 效果链状态（melody05 哇音电吉他效果器损坏即此）。md5 已验证：
   960 帧 = 原生 tp5 逐字节一致（c6a66d26）。
3. 生命周期：一次 init 一次 load；切曲 = shutdown + 重新 init（DLL 状态机
   不可重入）。
4. 预载映像 = **ma5_ds.bin.z**（4B 小端原始长度 + zlib 流，4.3MB），
   worker 内 DecompressionStream('deflate') 解压为 18.9MB。
5. DLL 加载：compact host 从磁盘找 M5_EmuSmw5/Hw.dll——web 构建用
   --preload-file 打进 MEMFS；node 用 dir 参数指向 compact 目录；
   GetModuleFileNameA 在 shell 有桩。
6. CFLAGS 加 `-fwrapv -fno-strict-aliasing`（原生链大量故意回绕 + uint32*
   双关；实测不改变输出但保留是安全网）+ `-D_stricmp=strcasecmp`。
7. 音频管线：Worker pump → postMessage(transferable) → 主线程帧队列 →
   ScriptProcessor(48kHz)。**块内偏移 S.blkOff 跨回调持久**（半块重播
   = 细微卡顿+变慢的根因）。预滚 2s 门控 + 欠载回预滚。
8. 切曲顺序：stop worker → 淡出 120ms（等在途消息送达）→ suspend →
   清 queue/qFrames/blkOff → 才发 load；加载代际 id 过滤旧代消息。
9. 测试曲目用 `ma2play/bin/mmf/` 语料；melody05 标准文件 =
   Samsung SGH-D500 Pre-downloaded/Melody05.mmf。
10. 性能：node 全曲 1.7~2.9x realtime；MA-2 (Dot Beat) 4.2x。

## 外部依赖（只引用，绝不复制进本仓库）

| 内容 | 路径（MA5T_SRC 根 = `D:\working\vscode-projects\YM2163-Midi\Denjhang_Music_Player_v16\ma2play`） |
|---|---|
| 核心源 | `libma5t_compact\`（五件套 + ma5t_player.h） |
| 预载映像 | `libma5t_compact\ma5_ds.bin.z` |
| DLL | `libma5t_compact\M5_EmuSmw5.dll / M5_EmuHw.dll` |
| 图标 | `src\app_icon.ico`（prepare.sh 提取 256px PNG） |
| 原生对照 | `libma5t_compact\tp5.exe`（GUI 等效路径；`MA5T_BASSDUMP=1` 出逐通道 trace） |
| 曲库 | `bin\mmf\`（默认浏览目录） |

### 外部文档（dmplayer 仓库，改核心行为前必读）

| 文档 | 用途 |
|---|---|
| `libma5t_compact\PLAN.md` | ma5t 圣经：round 历史、flt_main 修复链、全部验证结论 |
| `libma5t_exp\PLAN.md` | flat/compact/preload 三模式与内存设计 |
| `DEVELOPMENT.md` | 全量进度史；2830 行起 = 本项目立项小节 |
| `AGENTS.md` / `HANDOFF.md` | dmplayer 工作规则 + round31 教训 |
| `ymf825emu\src\mmf_parser.cpp` | **MMF 解析器唯一权威参考**（用户指定） |

## 构建与运行

```sh
# 构建（git bash，emcc 是 msys2 sh 脚本；务必显式 -O1！默认 -O2 会撞 wasm-opt 挂死）
/d/msys64/usr/bin/bash -lc "cd core && MA5PLAY_OPT=-O1 sh build.sh node"   # 或 web
sh web/prepare.sh            # 刷新 assets（含 node 图标提取，需 git bash 的 node）
node web/server.mjs          # http://127.0.0.1:8095

# 等价性验证（核心改动后跑）
cd core && node md5check.mjs        # melody05 全曲 md5，期望 c6a66d26（960 帧泵）
node switch_test.mjs                # 连播四曲
```

### 工具链陷阱

- emcc 入口 `/ucrt64/lib/emscripten/emcc`，需 `export PATH=/ucrt64/bin:$PATH`。
- pacman 安装中断 = 部分升级 = 全工具 DLL 失败（修法 `pacman -Syu`，已发生过）。
- **-O2 的 wasm-opt 在本机挂死**（多小时无产物）；一律 MA5PLAY_OPT=-O1。
- 被 kill 的构建留孤儿 python3/wasm-opt 进程，重跑前清理。
- prepare.sh 的 node 图标提取：node 是 Windows 程序，路径须 cygpath 转换。

## 铁律（用户多次严令，违反 = 浪费一轮）

1. **ma2play（imgui GUI）里 ma5t 后端完全正常，禁止怀疑核心、禁止怀疑 GUI**。
   网页侧任何异常先怀疑自己的宿主层（泵尺寸/时序/管线）。
2. **test_render_p.exe（离线快泵）≠ GUI 行为**，不能当核心真值；原生对照用
   tp5.exe（player5 路径）。
3. **ma5t 泵块尺寸 = 960 帧（20ms）**，与 GUI 一致；改它破坏效果链。
4. 排查音色/效果问题：对比**调用链路**（MA5T_BASSDUMP 逐通道数值 trace、
   TRIGLOG），禁止对比 wav 听感、禁止猜。md5 只作最终验收。
5. **先看旧文档/旧记忆再动手**：dmplayer 的 PLAN.md/DEVELOPMENT.md/AGENTS.md
   记录了全部已踩坑；用户指了方向就看，不自行扩展战线。
6. MMF 解析以 ymf825emu/src/mmf_parser.cpp 为准（web/mmf.js 是其移植）。
7. 不用 subagent 做核心工作、不进计划模式；每步完成即提交 + 更新本文件；
   等 user 说"继续"再推进。
8. 不动 dmplayer 仓库任何文件（只读引用）。

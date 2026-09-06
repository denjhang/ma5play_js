# ma5play_js 项目记忆（AGENTS.md）

> 本文件是项目记忆。每次会话开始先读本文件 + [docs/PROGRESS.md](docs/PROGRESS.md)。
> 逆向方法论、ma5t 历史细节在 dmplayer 仓库文档里（见"外部文档"），本文件只记
> 本项目直接相关的事实与规则，避免重复维护。

## 项目目标（用户裁定，不可动摇）

- **ma5t（DLL bit-exact t386 仿真）的 wasm 化** + 网页在线播放器，最终整包部署上线。
  不是原生 cpp 的 ymf825 核心（该路线已暂停）。
- 架构：ma5t C 核心 --emcc--> wasm → AudioWorklet 块状渲染 + 每 128 样本导出
  48 通道（16FM+32PCM）状态快照 → 主线程现代 PWA（深色、大触控目标），
  Modizer 式钢琴键盘可视化（交互概念抄它，外观不抄老 iOS）。
- **不做 wav/md5 对拍**——用户多次明确。node 渲染冒烟只要求能出 PCM（非全零）。
- 工具链用 msys2 pacman 的 emscripten，不用 emsdk。

## 当前状态（2026-09-06）

- ✅ 工具链可用（修复过程见下）
- ✅ core/build.sh 编译五件套 + shell → wasm（-O1；-O2 的 wasm-opt 本机挂死待查）
- ✅ node 冒烟通过：Melody01 5s，peak=32767，2.90x realtime
- ⬜ worklet/ AudioWorklet 壳 + 通道状态快照导出
- ⬜ web/ 键盘可视化 + PWA UI

## 目录

```
core/   build.sh（emcc 构建）、ma5play_shell.c（ma5w_* 导出）、smoke.mjs（node 冒烟）
worklet/  （待建）AudioWorklet 处理器
web/      （待建）前端 + PWA
docs/     PROGRESS.md（逐阶段进度）
build/    构建产物（ma5play_node.js/.wasm 等，不入库）
```

## 外部依赖（只引用，绝不复制进本仓库）

| 内容 | 路径 |
|---|---|
| ma5t 源码五件套 | `D:\working\vscode-projects\YM2163-Midi\Denjhang_Music_Player_v16\ma2play\libma5t_exp\`（i386.c fpu.c pe_loader.c win32_stubs.c ma5t_host.c） |
| 预载映像（node 冒烟用） | 同目录 `compact_preload.bin`（5129476B） |
| 浏览器端预载映像 | `...\ma2play\libma5t_compact\ma5_ds.bin.z`（deflate，前端 DecompressionStream 解压） |
| MA-5 测试曲目 | `...\ma2play\bin\mmf\MMF's for Samsung phones (from the Samsung PC Studio)\64poly\Melody0*.mmf` |
| 原生对照 exe | libma5t_exp `test_render_p.exe`（用法见下） |
| 参考项目 | chip-player-js（本机 `D:\working\vscode-projects\chip-player-js-master`）、Modizer 源码 |

### 外部文档（dmplayer 仓库，改核心行为前必读）

| 文档 | 用途 |
|---|---|
| `ma2play\libma5t_exp\PLAN.md` | **libma5t 的圣经**：构建命令、运行模式（flat/compact/preload 三种）、内存设计、全部验证结论 |
| `ma2play\DEVELOPMENT.md` | 全量进度史；2830 行起 = 本项目立项小节（需求/性能评估/架构） |
| `ma2play\AGENTS.md` | dmplayer 工作规则 + round31 教训 |
| `YM2163-Midi\...\HANDOFF.md`、`AGENTS.md`、`PLAN.md` | dmplayer 项目级上下文 |

## 核心技术事实（已验证）

1. **唯一验证过的启动模式 = compact 预载**：
   `ma5t_set_preload_image(img,size)` → `ma5t_init(NULL,...)`（dir=NULL 即内嵌 DLL
   路径）→ `ma5t_preload_boot_mem` 解析 MAP5 头建块表，全程无 2GB flat 分配。
   全语料 931 首 md5 验证（PLAN.md M2-3 节）。
   - **flat 模式是过时用法**：不带环境变量裸跑 `test_render_p.exe <mmf>` 连原生都
     `MaSound_Load -> -1`。原生对照必须带：
     `MA5T_PRELOAD=compact_preload.bin MA5T_EMBED=1 ./test_render_p.exe <mmf> [sec]`
2. 渲染循环语义（同 test_render_t.c）：`pump_seq → pump_audio → take_pcm`，
   48kHz s16 立体声，192000 B/s；曲终判定 = 出声后连续 2s 全零或连续 5 轮取不到 PCM。
3. wasm 侧性能已实测够用：node -O1 = 2.90x realtime（原生预估才 2.1x）。
   低端手机的缓解方案（立项小节）：块状按需渲染 + 预渲染超前 10~20s 喂 ring buffer。
4. 测试曲目必须是 **MA-5 语料**；libma/mmf 的 gm6 文件、s14.mmf 属于别的后端，
   `MaSound_Load` 必 -1，不是 wasm 的 bug。
5. shell 层导出（core/ma5play_shell.c）：`ma5w_set_preload / init / load /
   open_standby_start / pump_seq / pump_audio / take_pcm / last_error /
   compact_mode / loaded`。`ma5w_load` 在宿主侧 malloc 保活副本（DLL 可能持指针）。

## 构建与测试（命令照抄）

```sh
# 构建（git bash 里跑，emcc 是 msys2 的 sh 脚本，cmd 跑不了）
/d/msys64/usr/bin/bash -lc "cd /d/working/vscode-projects/ma5play_js/core && sh build.sh node"
MA5PLAY_OPT=-O1 sh build.sh node        # 当前可用的优化级别
sh build.sh web                          # web 变体（数据打包方式待定稿）

# 冒烟
cd core && node smoke.mjs "D:\\...\\64poly\\Melody01.mmf" 5
# 预载映像路径可用 MA5PLAY_PRELOAD 环境变量覆盖
```

### 工具链陷阱（每次环境异常先查这里）

- emcc 入口在 **`/ucrt64/lib/emscripten/emcc`**（此包不在 /ucrt64/bin 放 emcc）；
  运行前必须 `export PATH=/ucrt64/bin:$PATH`（emcc 要找 /ucrt64/bin 下的
  wasm-opt 等二进制工具，否则报 BINARYEN_ROOT not set）。
- **pacman 安装中断 = 部分升级 = clang/node/wasm-opt 全部 DLL 入口点失败**
  （进程表里留僵尸条目，taskkill 报"没有此任务"）。修复 = `pacman -Syu`
  全量升级（20 分钟级，正常）。已发生过一次，症状记住。
- **-O2 的 wasm-opt 阶段在本机疑似挂死**（启动十几秒后 CPU 归零、无产物、
  多轮复现，包括干净单进程重跑）。-O1 一次通过。排查方向：binaryen 版本/
  Windows 大模块问题。在修好前构建默认用 -O1。
- 编译需 `-D_stricmp=strcasecmp`（win32_stubs.c 用了 MSVC 名，emcc 无此函数）。
- 被 kill 的构建会留孤儿 python3/wasm-opt 进程，重跑前先清：
  `ps -ef | grep -iE 'emcc|wasm' | grep -v grep | awk '{print $2}' | xargs -r kill -9`

## 用户工作规则（从 dmplayer 继承，必守）

1. 先抓文件/运行证据再改代码；不编造实现；禁止怀疑用户的听感描述。
2. 用户指了方向（看文档、看 git 日志）就照做，不自行扩展战线。
3. **不用 subagent 做核心工作**、不进计划模式。
4. 每步完成即提交 + 更新本文件/PROGRESS.md，等用户说"继续"再推进下一步。
5. 不动 dmplayer 仓库的任何文件（本项目只读引用）。
6. 浏览器端资源按 README：ma5_ds.bin.z 用 DecompressionStream("deflate") 解压。

## 铁律（用户 2026-09-06 严令）
- **ma2play（imgui GUI）里 ma5t 后端完全正常**——Sound_14 等任何曲目都能完整播放。
  **禁止怀疑 ma5t 核心、禁止怀疑 GUI 的正确性**。网页/wasm 侧任何"核心提前停"
  的现象，都是**宿主泵节奏问题**，不是核心 bug。
- `test_render_p.exe`（离线快泵渲染器）不等价于 GUI：它以最快速度泵，
  而 GUI 按实时节奏泵；DLL 音序器的字节发射节奏依赖实时交错（round5b
  已记录：waveOut 桩依赖真实时钟）。**离线渲染器停 ≠ 核心有问题**，
  用离线 exe 的结果下"核心也停"的结论是错误的（2026-09-06 我犯过，被用户纠正）。
- 唯一权威解析器 = ymf825emu/src/mmf_parser.cpp（用户指定参考），
  web/mmf.js 是它的逐函数移植。

## 铁律补充（2026-09-06 五）
- **ma5t 泵块尺寸 = 960 帧（20ms）**，与 GUI/ma5t_backend/tp5 一致。
  改大（如 2400/50ms）会破坏 DLL 效果链状态——melody05 哇音电吉他效果器
  损坏的根因（md5 验证：960=原生逐字节一致，2400 发散）。
- 排查此类问题的正确姿势（用户多次强调）：对比**调用链路**（MA5T_BASSDUMP
  逐通道数值 trace / TRIGLOG），禁止靠对比 wav 听感；md5 只作最终验收。

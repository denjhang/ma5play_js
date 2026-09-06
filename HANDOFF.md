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

## 仿真核心更新要点（2026-09-06，源自 dmplayer ma2play round33/34）

dmplayer 侧近期对 libma5t* 做了两轮大改（提速 + 保真修复），JS 端同步
核心时按此清单执行，完整细节见
`D:\working\vscode-projects\YM2163-Midi\Denjhang_Music_Player_v16\ma2play\DEVELOPMENT.md`
（搜 "round33" / "round34" / "round34 附注"）。

### 必须带上（已验证正确）
1. **MA-2 提速：SUBS 只关 bit9**（16238）——AOR/808 Kingdom/House
   ~30% 提速，G60 卡顿修复。bit8/10/11 bit-exact 安全。
   **g_sub163d8_mask / g_ma2_track 必须 per-load 复位**（粘性掩码
   bug：播过 MA-2 后 MA-5 曲全掉回解释器；JS 单进程连续换曲必踩）。
2. **循环/暂停/seek 语义**（如 worklet 壳需要）：DLL 状态轮询
   （MaSound_Control type=6：3=READY/4=PLAYING）判曲终 + 静音掐尾
   （250ms 窗口 + 状态门控防曲中静音误判）+ 抢占式切片控制调用
   （ma5t_call_ctl，2M 指令切片防 guest Stop/Seek 死锁）。
3. **按需映射 blk_demand()**（若用 compact 内存模型）：原机
   0x3f5xxxx~0x3f7xxxx 是 DS 芯片设备 RAM，预载快照只收非零块导致
   这片变黑洞。未映射 in-range 块首次写时补零块（i386.c 实现，
   接入 pstore8/gmp/uc_mem_write 三路）。**这是两个历史大坑
   （melody13 全曲无声、FM 音色偏离）的公共根源修复。**

### 明确不要带
- gmp() 未映射写落共享零页的别名行为（幻影内存，两 bug 根源）
- 0x29b13d0 填充批量钩子写零页的"意外正确"（巧合，demand-map 后失效）
- 1284 原生移植当前版本（默认关，真路径仍有 +0x834 填充语义分歧，
  修好 bit-exact 前别开等价优化）

### 模式选择建议
- **flat 模式优先**（无上述内存坑）；ALLOW_MEMORY_GROWTH。
- 若性能必须 compact：快照生成时把 0x3f500000~0x3f800000 设备 RAM
  区强制包含（哪怕全零），或运行时 demand-map——二选一。

### 冒烟验收曲目
- Melody13（Samsung D500 Pre-downloaded）：历史"全曲无声"案
- Red Leaf（Panasonic G60）：bit9 分歧案
- Beauty（DefleMask YMU759）：MA-3 PCM 重载、最慢曲目

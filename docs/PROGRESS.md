# ma5play_js 进度记录

## 2026-09-06 — WASM 编译 + node 冒烟通过

### 工具链
- msys2 emscripten 包本来就装好（mingw-w64-ucrt-x86_64-emscripten 6.0.9-2），
  但 clang/node/wasm-opt 全部启动失败（0xC0000139 类 DLL 版本不匹配）——
  根因是上次 pacman 安装中断造成系统部分升级。`pacman -Syu` 全量升级后修复。
- emcc 入口在 `/ucrt64/lib/emscripten/emcc`（该包不在 /ucrt64/bin 放 emcc），
  运行需 `export PATH=/ucrt64/bin:$PATH`（找 wasm-opt 等二进制工具）。

### 构建（core/build.sh）
- 源 = libma5t_exp 五件套（i386/fpu/pe_loader/win32_stubs/ma5t_host）
  + 本项目 `core/ma5play_shell.c`（导出 ma5w_* 扁平 API）。
- 开关与原生 PLAN.md 顶部一致：`-I$SRC -include uc_shim.h
  -DI386_ENABLE_FPU -DI386_NATIVE_HOOKS -D_stricmp=strcasecmp`（_stricmp 是
  MSVC 名，emcc 下映射 strcasecmp）。
- `-O2` 下 wasm-opt 在本机疑似挂死（CPU 十几秒即停、20 分钟无产物，多轮复现）；
  降 `-O1` 一次通过。构建脚本用 `MA5PLAY_OPT` 环境变量控制，默认 -O2。
- 模式：**compact 预载**（走 ma5t_set_preload_image → ma5t_preload_boot_mem），
  非 flat。flat 模式原生也失败且 wasm 不需要 2GB 缓冲；预载是文档里全语料
  931 首 md5 验证过的路径。sMODULARIZE + ALLOW_MEMORY_GROWTH(4GB)。

### 冒烟结果（core/smoke.mjs）
- 预载映像：`libma5t_exp/compact_preload.bin`（5129476B，与原生成功运行同源）。
  浏览器端最终用 libma5t_compact/ma5_ds.bin.z（DecompressionStream 解压）。
- Melody01.mmf 5s：`PASS: 960000 bytes, nonzero=478017, peak=32767,
  speed=2.90x realtime`（node -O1 wasm，比原生预估 2.1x 还快）。
- 踩坑记录：
  - 测试曲目必须用 MA-5 语料（`bin/mmf/.../64poly/Melody01.mmf`）；
    libma/mmf 下的 gm6 文件和 s14.mmf 是别的后端的，MaSound_Load -1。
  - flat 模式裸跑原生也 -1（test_render_p.exe 不带环境变量的用法已过时），
    对照实验要带 `MA5T_PRELOAD=compact_preload.bin MA5T_EMBED=1`。

### 下一步
1. worklet/ AudioWorklet 壳（块状渲染 + 预渲染缓冲）
2. pump 末尾导出 `{keyOn,note,vel,type} ch[48]` 快照（MA5T_TRIGLOG 钩子先例）
3. web/ 键盘可视化 + PWA

## 2026-09-06（晚）— 网页测试播放器

### 新增
- `web/`：完整测试播放器（index.html / style.css / app.js / server.mjs / prepare.sh）
  - 播放器控件：播放/暂停/停止/循环/音量/静音 + 波形示波器 + 时间 + 实时速度
  - 文件浏览器：MMF 文件选择 + 拖放 + 演示曲目 + **播放历史（IndexedDB 存 blob，
    跨会话可重播，可单条删除/清空）**
  - 深色现代 UI，移动端适配（flex 折叠 + 大触控目标）
- 音频管线（测试版）：主线程 wasm pump → Int32 环形帧缓冲（6s）→
  ScriptProcessorNode(48kHz) 拉取。AudioWorklet + 通道快照是下一阶段。

### 关键修复
- **切曲**：ma5t 核心 DLL 状态机不可重入，一次 init 只支持一次 load。
  shell 层 `ma5w_load` 现在 = shutdown + 重新 init（compact 预载下 init 廉价）。
  node 四连播测试（core/switch_test.mjs）+ 浏览器四连播均通过。
- app.js 必须以 ES module 加载（顶层 await）；模块作用域函数需显式
  `window.loadTrack = loadTrack` 才能被页面控制台/自动化访问。
- 播放时间按 onaudioprocess 实际消费帧计（预渲染缓冲会抵消"渲染秒数"）。

### 踩坑（重要）
- **一条命令串两次 emcc 构建时，第二次漏传 MA5PLAY_OPT=-O1 会用默认 -O2，
  撞上 wasm-opt 挂死，挂一小时**。build.sh 默认值仍为 -O2，跑之前务必显式传
  或先改默认。

### 运行
```
core: MA5PLAY_OPT=-O1 sh build.sh web && sh ../web/prepare.sh
web:  node web/server.mjs   → http://127.0.0.1:8095
```

### 切曲卡音修复（2026-09-06 夜，用户实测反馈）
- 根因一（爆音）：切曲瞬间清空环形缓冲，波形硬切。修复 = 切曲前 60ms gain
  线性淡出 → 停泵重载 → 开声时 40ms 淡入。
- 根因二（断续，用户明令"等缓冲满再播"）：开声时缓冲为空，渲染 2.5x 追不上
  实时消耗。修复 = **priming 预滚门控**：攒满 2s（PREROLL_FRAMES）才
  ctx.resume() 开声；播放中缓冲耗尽也回到 priming（欠载保护），不连续小口供声。
- 切曲期间 switching 标志停 fillLoop，杜绝在半初始化 ctx 上 pump。

## 2026-09-06（夜二）— 按 ma2play 布局重做文件浏览器

用户指出第一版"文件选择器+播放历史"不是 ma2play 的设计。读
smaf_window.cpp Render() 确认真实布局并复刻：

```
左栏 Controls(280px)：文件信息/播放控制/音量循环/本机文件打开
右上：Scope + 通道状态占位（48ch F0-F15/P0-P31，快照待 worklet 阶段）
右下左：文件浏览器 = 传输条(⏮▶⏭+时间+进度) + ‹›⌃导航 + 面包屑
        + 文件夹历史下拉(localStorage, 20条) + [DIR]/文件列表
右下右：Log 面板（时间戳，播放/导航/错误事件）
```

- server.mjs 加 /api/list（目录列表，目录在前）与 /api/file（仅 .mmf），
  默认目录 ma2play/bin/mmf；仅监听 127.0.0.1。
- 曲终自动下一曲（列表顺序）；循环开关优先。
- 浏览器实测：面包屑/后退前进上级/文件夹历史/播放/上一曲下一曲/高亮当前曲 全通过。

## 2026-09-06（三）— Worker 渲染 + MA 版本显示 + ma2play 钢琴复刻

用户反馈三条：ma2 曲目"速度不够"、侧栏要显示 MA-2/3/5、钢琴照抄 ma2play。

### 速度（渲染移入 Worker）
- 根因不是渲染速度（wasm 实测 2.5~2.9x rt 够用），是主线程 pump 被 UI/渲染
  竞争饿死。渲染移到 render-worker.js（独立线程），主线程只消费。
- SAB 方案在本机 IAB webview 不可用（COOP/COEP 头正确但 crossOriginIsolated
  仍 false）→ 改消息传递：worker 按块 postMessage（transferable），
  主线程消费后 ack，worker 以未确认帧数流控（4s 在途预算）。
- **worker 陷阱两枚**：① worker 里必须 importScripts('assets/ma5play.js')
  （主线程 script 标签进不去）；② pump 必须 trackLoaded 门控——空核心 pump
  会真的出 PCM 垃圾并占满流控预算，真曲目一块都发不出（症状：永远 buffering）。
- 状态机从 rAF 改 setInterval(100ms)：隐藏/后台面板 rAF 不触发，时间会冻住。
- 首载 'loaded' 时主动 ctx.suspend() 进预滚（否则 autoplay 策略下 ctx 已
  running，预滚门控失效）。

### MMF 解析器（web/mmf.js，移植 libymf825_ma5/src/mmf_parser.cpp）
- 容器：MMMD+u32 总长，块从偏移 8 起（从 4 起会全空）。
- MTR: fmt/durTb/gateTb + 通道状态(2/16/32B) + 子块 Mtsq/Mtsu/EXVO/Mtsp。
- 事件：HPS(fmt0，note=sig&15+((sig>>4&3)+3)*12+八度移位) /
  MobileNormal(fmt2) / Mobile32(fmt3)。
- SysEx = F0+长度varint+数据+F7；版本判定同 mmf_detect_version_from_exclusives
  （实测：Melody01→MA-5，Dot Beat→MA-2，BrilliantSnow→MA-2[文件自标 MA-2]）。
- CNTI 标题：code_type!=0 时是逗号分隔 "ST:title" 文本。

### 钢琴（照抄 RenderPianoArea）
- 音域 12..107(C0..B8)、白键先画黑键后画、黑键 x=(wk-1)*ww+ww-bw/2、
  bw=ww*0.65、黑键高 62%、blend=0.55+lv*0.45 向键底色混合、
  kChColors[16] 通道配色、C 音名、Ch 标签——逐项同源移植到 canvas。
- 数据源 = parser 时间轴（VIS_PARSER 同思路）：音符按 playedFrames 实际
  播放时钟点亮。48ch 快照仍待 AudioWorklet 阶段。

### 布局修正
- 播放/停止/循环/音量全部并入文件浏览器上方传输条（ma2play SMAF Player
  header 同位置）；侧栏只剩 File Info（File/Title/Version/Size/Length/
  Notes/Backend）+ 本机文件打开。

### 卡顿+变慢根因（用户报"细微卡顿和播放速度变慢"）
- 渲染速度无罪：node 实测 MA-2 Dot Beat 4.23x、MA-5 Melody01 2.80x realtime。
- 真凶 = 消费端半块重播 bug：onaudioprocess 每次回调把 bi 从 0 数起，
  上一回调没消费完的半块下一回调**从头重播** → 内容重复 = 细微卡顿 +
  乐曲推进变慢。修复 = S.blkOff 跨回调持久保存块内偏移。
- 验证：播放帧数/墙钟 = 1.006（修复前该比值虚高、内容落后）。

### 面包屑细节 + 图标（用户对照 ma2play 指出）
- 右优先：从右往左保留能放下的段（firstVisibleSegment 同算法），前导段
  折叠成 "..."——深层路径始终看得见当前目录；窗口 resize 重算。
- 点击 "..." 或面包屑空白处 → 路径输入模式（Enter 导航 / Esc 取消），
  同 ma2play s_pathEditMode。
- logo/favicon = ma2play app_icon.ico（ICO 内嵌 PNG，取 256px，prepare.sh 再生）。

## 2026-09-06（四）— 核心源切换 libma5t_compact（Sound_14 提前停曲修复）

### 根因（用户严令纠正后的结论）
- Sound_14（38.4s/97 音符）在 exp 源 wasm 只出 3.4s。真凶 = **用错了源**：
  libma5t_exp 是离线实验目录；GUI ma5t 后端（ma2play/src/core/ma5t_backend.cpp）
  用的是 **libma5t_compact**（ma5p_* API）。两者曲终判定不同：
  exp 的 test_render 用 no_pcm>5；compact 的 ma5p_pump 用 **no_pcm>100**
  （round19 教训：melody11 有 >5 泵的短暂断流，5 会误杀）。
- 另一条红线：test_render_p.exe（离线快泵）≠ GUI 行为，不能当核心真值
  （DLL 音序器节奏依赖实时交错）。GUI 完全正常 = 核心 ground truth。

### wasm 切换到 compact
- build.sh SRC → libma5t_compact 五件套；ma5t_player.c 不编（依赖 zlib/windows），
  其 ma5p_pump 语义照抄进 ma5play_shell.c（no_pcm>100 + 出声后 2s 全零丢尾）。
- compact host 从磁盘找 DLL → web 构建把 M5_EmuSmw5/Hw.dll 打进 MEMFS
  （--preload-file，.data 3.1MB）；GetModuleFileNameA 在 shell 补桩。
- 预载映像换 **ma5_ds.bin.z**（4B 小端原始长度 + zlib 流，4.3MB），
  worker 里 DecompressionStream('deflate') 解压（= zlib 包装，18.9MB）。
- shell 新增 ma5w_pause/resume/seek_play（compact host 有 ma5t_pause/seek/
  start_play——seek 能力的入口，后续进度条拖动用）。

### 完整 MMF 解析器（web/mmf.js 重写，用户指定参考 ymf825emu/src/mmf_parser.cpp）
- 逐函数移植：Huffman 解压(MobileCompressed)、HPS/SEQU 事件、MMMG 容器、
  独立 SEQU、EXVO/Mtsu SysEx、CNTI 标题(Mx: 版本/ST 标题/AN 艺术家)。
- 实测：Sound_14=MA-5 38.4s/97notes、Sound_15=MA-5 23.3s、Melody01=MA-5 49.8s、
  Dot Beat=MA-2 22.0s、BrilliantSnow=MA-2 44.2s。

### 验证
- node：Sound_14 **38.5s 完整渲染**（谱面 38.4s）1.76x rt；Melody01 51.5s 1.73x。
- 浏览器：Sound_14 连续播放 23s+（旧版 3.4s 即停），缓冲稳定 4s。
- HPS 多轨通道 = c + t*4（RenderPianoArea 同规则）。

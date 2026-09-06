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

### 切曲残留旧音根治（用户指出）
- 旧顺序：淡出期间旧泵仍在产数据 → 清缓冲后仍有在途旧块落进新曲队列。
- 新顺序（app.js loadTrack）：**postMessage stop 停旧泵 → 淡出已排队尾音
  （120ms，含在途消息送达窗口）→ suspend → 清 queue/qFrames/blkOff →
  才发 load**。清空后旧代字节不可能再进队（worker 已停 + id 过滤双保险）。

## 2026-09-06（五）— melody05 电吉他效果器损坏根因：泵块尺寸

### 排查过程（全程调用链路验证，遵守禁音频对拍规则）
1. wasm 全曲 md5(3ac28d5d) ≠ tp5 原生(c6a66d26)；-O0≡-O1、-fwrapv 无变化
   → 非编译器问题。
2. MA5T_BASSDUMP 逐通道 trace（wv/st/ph/step/env/gL/gR，全曲 151 行）
   wasm 与原生**完全一致** → 通道合成链（含哇音滤波参数）正确。
3. tp5 复跑两次 md5 相同 → 分歧真实存在于通道探针下游的效果/混音路径。
4. **真凶：worker 泵块尺寸 2400 帧(50ms)，GUI/tp5 均为 960 帧(20ms)**。
   DLL 效果链状态对泵粒度敏感，50ms 块损坏效果器输出。
   wasm 改 960 帧/泵后 md5 = c6a66d26 = tp5 原生**逐字节一致**。

### 修复
- render-worker.js CHUNK_FRAMES 2400 → 960。wasm 本体无需重编。

### 记入 AGENTS.md 铁律
- ma5t 泵块尺寸是行为参数：必须 960 帧/20ms（GUI 同款），改它会破坏
  效果链数值输出。

## 2026-09-06（六）— 核心加载进度条 + 手机竖屏布局

- 加载进度：worker 流式 fetch wasm（Content-Length 计 2~45%）+ 编译（46%）
  + 流式下载 ma5_ds.bin.z（50~70%）+ DecompressionStream 按已知原始长度计
  解压进度（70~95%）+ 初始化（96~100%）；进度条挂 header 下缘。
  wasm 预取后经 wasmBinary 传入工厂（绕开 emscripten 内部无进度 fetch）。
- 手机竖屏：传输条重排为两行（按钮行 + 进度/音量行），t-mid 时间右贴；
  @media≤820px 触控目标 ≥42px、钢琴 96px、导航键 40px。
  390px 视口自动检测：所有控件零重叠、零横向溢出。
- server.mjs 保持只绑 127.0.0.1（用户指示，不改 0.0.0.0）。

## 2026-09-06（七）— 布局重构 + 双后端同步可视化

- 布局：钢琴独占右上整行（到右边框）；通道状态与 Log 合并为一个面板
  （通道在上 / Log 在下）。手机端钢琴拆两行，键比例 = 真钢琴
  （白键长 6.2×白键宽，黑键 62% 长 / 65% 宽），行高上限 86px 防过长，
  DPR 适配。修复替换脚本截断错误导致 app.js 整体崩溃（钢琴消失）。
- 双后端同步可视化（用户需求：播放 ma5t / 可视化 parser，自动对齐）：
  汇合点 = 音频消费时钟 playedFrames；t = playedFrames/48000 −
  ctx.outputLatency（输出延迟自动补偿，无需手动 PianoSyncDelay）。
  音符按下满亮（vel→lv，0.15~1），释放后 0.12s 线性渐隐。

### 通道状态表（ma2play 可视化同款，parser 数据源）
- mmf.js 补全 CC/PC/Bend 事件捕获（此前丢弃）→ chEv 时间轴。
- chTable：16 通道 × Ch/Prog(GM 128 音色名)/Note(如 C4)/Vol/Pan(L/R 偏移)/
  Exp/Event 列，sticky 表头，活跃行高亮；与钢琴同一音频时钟
  （playedFrames − outputLatency）驱动，tick 100ms 更新。
- 替换原 48 格占位（48ch 快照仍留 AudioWorklet 阶段，届时换后端数据源）。

### 可视化实时性修复（用户：反应慢、不实时）
- 根因：可视化时钟直接读 playedFrames，只在 onaudioprocess（4096 帧 ≈ 85ms）
  更新 → 钢琴/通道表按 85ms 台阶跳。
- 修复 = ma2play 同步法：音频回调只做**锚点校准**（clkSong/clkAt），
  帧间用 ctx.currentTime（连续、微秒级）插值 → visClock()；suspend 时
  currentTime 冻结，画面自然停住。切曲/加载重置锚点。
- 通道表从 100ms 定时器改挂钢琴 rAF（~50ms 节流），与时钟同帧更新。
- 顺带修复：欠载时未填样本区显式补零。

### 钢琴/通道表全空根因：单位错误 + 可视化日志（用户要求静态对比法）
- 根因：mmf.js 音符/事件时间轴单位是 **ms**，可视化时钟是**秒**——所有
  t>0 的事件永远不触发，只有 t=0 的 CC 生效（症状：通道表有 Vol 但
  Note 恒 --，钢琴全空）。解析器本身正确（静态 dump 对照原生 bass
  trace：首批事件 20ms 吻合）。修复 = 解析输出统一转秒。
- 按用户要求加可视化日志（静态解析 ↔ 运行时对比闭环）：
  - `[vis] parsed: N notes, ch=[..], span 0→Xs`（载入即出）
  - `[vis] piano first key lit @ t=X.XXs note=C#4 ch=4`（首个点亮）
  - Sound_1 验证：解析首音符 t=2.0s ↔ 点亮 2.01s，吻合。
- __dbg 探针扩展：visT/clkSong/clkAt/now/latency/n0/chEv0。

### 通道表 ext/PCM 区 + 2s 延迟修复
- mmf.js 补 Mtsp/Mwa 波形块解析（type/stereo/Hz/size）+ SysEx 内嵌波检测
  （43 79 07 7F 03 waveId / 43 05 00 waveId）→ waves[]。
- 通道表追加 "PCM Waves (ext / Mwa / inline)" 分组行（W# / 来源 / 波号 /
  采样率 / 单声道立体 / 大小）；ch9 鼓音符显示 GM 打击鼓名（35-81 全表）。
- 2 秒延迟根因：outputLatency 在蓝牙/部分 webview 报 1~2s，全额扣减把
  画面拖后。封顶 150ms。
- 实测：LG Sound_1 waves=4 Inline、Melody05 waves=2（#1/#93 电吉他流）。

### 通道表细化 + 真实发声时刻对齐（用户三连指正）
- 波形类型按 MA-2 规范细分标注（ma2play GetWaveStates 语义）：
  Mwa chunk → Awa (stream)/Mwa stream/MSTR；SysEx 内嵌 →
  "ext PCM (43 79 07 7F 03)" / "wave data (43 05 00)"。
- GM 音色名换 ma2play kGmMelodic 同款**全名**（不缩写）。
- 延迟对齐弃用 outputLatency 扣减（蓝牙/webview 会报 1~2s，封顶也是猜），
  改用 **ctx.getOutputTimestamp().contextTime**——浏览器报告的"此刻正在
  发声"的真实音频时刻，直接作为可视化时钟基准（解析器对齐于此）。
- 通道表布局稳定：table-layout fixed + colgroup 固定列宽，Prog 列省略号。

### 通道表按 ma2play 设计原则重构
- **切曲全扫定型**：chOnTrack 一次性预扫——用到的通道集合（本曲行数
  定型，播放期间零增删）、各通道首个 PC/CC7 作初始显示；此前固定 16 行
  且乐器名等时间轴到达才出现。
- **乐器名粘性**：progShown 缓存，变化才写 DOM，永不回退默认"—"；
  Prog 列 title 悬浮全名。
- **通道色行**：每行左缘 3px 通道色（kChColors）；未激活行灰 #66748f，
  激活行亮色 + 行底高亮（ma2play StatusArea 同款）。
- **SysEx 注册表全细分**（ymf825emu parse_exclusive 全表移植）：
  MA-5 Voice (bank/PC/drum/FM|PCM)、MA-3 Voice、MA-3/5 PCM waveform、
  MA-5 FM/PCM Voice、Wave data、4-op compact、MA-2 VMA、SoftBank legacy；
  + Mwa 块 Awa/Mwa stream/MSTR。表内 Voices/Waves 两个 registry 区。
- 实测 Sound_1：11 行（仅用到通道）、7 voices + 4 waves 全部分类正确。

### 通道表对齐 ma2play 分版本设计（用户：MA-2/3/5 表完全不同）
研读 smaf_window.cpp RenderStatusArea 后照抄设计：
- **10 列**：Ch|Stat|Note|Voice|PC|Inst|Vol|Pan|Event|Mode。
- **分版本行组**：MA-2 = FM + ATR（ADPCM 音轨行，ATR0/1，parser 计数）；
  MA-3 = FM + PCM（P0-7 ROM 鼓槽，ch9 鼓音符静态枚举）+ MWA；
  MA-5 = FM + MWA（e#waveId / m#idx）。
- **粘性缓存**（ma2play s_lastNote/s_lastAlg 同款）：不活跃保留上次
  Note/Inst/Voice 灰色显示，不闪回 "--"；换曲清空。
- **多复音 Note 列**：MA-5 同通道最多 3 音符 "C4+E4"。
- **Voice 列**：PC(+bankL) → SysEx 注册表反查 FM/PCM。
- **Mode 列**：MTR 通道状态 channel_type（NoCare/Melody/NoMel/Rhythm，
  HPS 2 字节打包/其余每通道 1 字节，parser 新增解析）。
- Inst：GM 全名；ch9 按当前音高显 GM 鼓名。
- 实测 Sound_1(MA-5)：11 FM + 4 MWA(e1-e4) 行；Dot Beat(MA-2)：
  chTypes 含 Rhythm。

### 非 MIDI 通道行（完整研读 RenderStatusArea + ymf825_backend.h 后照抄）
设计事实（读码定案）：
- 数据模型 type：PcmState 1=ATR/2=ROM 鼓/3=ext 旋律/4=Mwa；WaveState
  1=Awa/2=Mwa 流/3=MSTR；MwaSlot kind 3=ext/4=Mwa chunk/5=stream（load 定型）。
- **MA-2 恒显 ATR0/1 两条**（showAtrRows = !(MA>=3)，不按 atrCount），
  adpcm/ADPCM Stream/#idx/Stream，琥珀色。
- **PCM P0-7 仅 MA-3**、只留 ROM 鼓（ext/Mwa 归 wave 行）、8 色板（与
  音量条同色系）、行出现后常驻（everActive）。
- **WAVE 行（MA≥3）**：Ch = e<waveID>/m<idx>/s<waveID>；Voice=ext/mwa/
  stream；Inst=Inline Wave/Mwa Chunk/Mwa Stream/MSTR Loop；Note=触发音符
  或同类内 #seq；ext 青/mwa 紫/stream 橙；stream 无 pan。
- ma2play **不显示 SysEx 注册表**——表只有通道行。
- 我们的 parser 数据源等价映射：ATR=版本判定+恒显两条；PCM=ch9 静态鼓音
  枚举；wave=Mwa 块 m<idx> + 内嵌 SysEx e<waveID>；stream 静态不可知不显示。
- 实测：Sound_1(MA-5) 显示 e1-e4 Inline Wave 行；Dot Beat(MA-2) 显示
  ATR0/1 ADPCM Stream 行。

## 2026-09-06（总结）— 会话全部成果

### 核心层
- 源从 libma5t_exp 切换 **libma5t_compact**（GUI 同源）；shell 照抄 ma5p_pump
  语义（no_pcm>100 曲终）；pause/resume/seek 导出就绪。
- **melody05 效果器损坏根因 = 泵块尺寸**（2400→960 帧，md5 与原生逐字节一致）。
- 等价性工具：md5check（melody05 全曲 c6a66d26）、switch_test、cmpflag。

### web 播放器（界面英文，面向 YouTube）
- 布局照抄 ma2play：左 File Info | 右上钢琴 | 右下文件浏览器(传输条+
  面包屑右优先+点击输入模式+文件夹历史+目录记忆)+Log；ma2play 图标。
- Worker 渲染（消息传递+代际 id）；切曲零残留（停泵→淡出→清缓冲→加载）；
  预滚 2s 门控+欠载回预滚；blkOff 持久化修半块重播（卡顿+变慢根因）。
- **mmf.js 完整解析器**（ymf825emu 逐函数移植）：Huffman/HPS/SEQU/MMMG/
  CNTI 标题/Mx: 版本/SysEx 全类型注册表（voices/waves）/通道类型/ATR 计数。
  输出单位=秒（ms 混用曾致可视化全空）。
- **钢琴**：ma2play RenderPianoArea 同款（比例/配色/blend/C 音名），
  parser 时间轴 + visClock（getOutputTimestamp 真实发声时刻）同步，
  多通道同音水平切分多色，手机两行（含横屏），键释放渐隐。
- **通道表**：ma2play 分版本设计——7 列、切曲全扫定型、粘性缓存、通道色；
  MA-2 恒显 ATR0/1；MA-3 PCM 8 色板 ROM 鼓行；MA≥3 wave 行 e<waveID>/m<idx>
  （ext 青/mwa 紫）；不显示 SysEx 注册表（10 列版被驳回回退）。
- 手机竖屏/横屏完整适配（横屏通道表上移钢琴上方）；加载百分比进度条；
  页脚手机隐藏；历史下拉溢出修复。

### 可视化调试方法论（用户确立）
- 载入即出解析摘要日志 + 首键点亮时间戳 → 静态↔运行对比闭环
  （Sound_1: 解析首音符 2.0s ↔ 点亮 2.01s）。
- __dbg 探针：队列/时钟锚点/延迟/首音符。

### 静态解析自查（用户方法论：本地 JS 解析、每版本 10 首自查）
- web/inspect10.mjs：MA-2/3/5 各 10 首全量 dump（notes/通道/atr/waves/
  voices/chTypes/title/dur），结论：三版本数据形态与 ma2play 设计一致。
- **修复 1（ATR 不显示根因）**：回退 3f6dffb 时 mmf.js 一并回退，把
  ATR 计数 + 通道类型解析（0706045 加的）丢了 → atrCount 恒 0。
  已补回。全语料验证：MA-2 423 首中 70 首有 ATR 音轨。
- **修复 2（注册表爆量）**：DefleMask 逐音符 SysEx 产生上万条重复注册
  （Mirror And Mirror voices=10664）→ 按 (kind,bank,pc,vtype) 去重 → 253。
- 版本分布（1484 首全语料，0 解析错误）：MA-2=423 / MA-3=431 /
  MA-5=516 / MA-7=2 / 未知=112。
- web/../core/bench_ma2.mjs：MA-2 速度基准工具就绪；
  **MA-2 播放卡顿排查移交 imgui（ma2play）窗口做**（用户指示）。

### MA-2 播放效率问题 —— 移交 imgui 窗口（用户裁定：涉及逆向，本窗口只做前端）
已测得的事实（core/bench_ma2.mjs，node 静态基准，供那边接手）：
- Melody A (G50, MA-2)：**0.20x rt**（卡顿实锤）；Sound_14 (LG, MA-5)：0.51x；
  Dot Beat 1.24x / Red Leaf 1.55x / Disney 2.08x / Melody03 1.09x。
- **速度与泵块尺寸强相关**：Sound_14 在 2400 帧块 = 1.76x，960 帧块 = 0.51x。
  但 960 帧是效果链正确性要求（melody05 md5 验证）——GUI 原生 960 能到 2.1x，
  wasm 960 却掉到 0.5x，矛盾点在每次 pump_seq/pump_audio 调用的固定开销
  （960 块 = 2.5 倍调用次数）。待逆向窗口定位每泵固定成本后回传参数/结论。
- bench_ma2.mjs 已参数化（node bench_ma2.mjs <chunk>）。

## 2026-09-06（深夜·第二轮）— 可视化对齐 ma2play 825 核心

### 解析器 bit 级对齐（1484/1484）
- `ref/` 建立三版 libymf825（ma2/ma3/ma5）源码副本 + voice 资产（DefMA3 ini、
  rom_wave_source.bin）+ midifile，本地 g++ 编译出 count_events / dump_events /
  test_vis_slots / test_vis5（ma5 版）基准工具。dmplayer 仓库零改动。
- A/B 对拍（C++ 全语料 TSV vs JS）揪出五处偏差并修复（5ff7b41）：
  1. evMobile 0x8X Note-Off 是 NOTE 事件（vel 继承 g_lastVelocity）——
     丢 ~80% 音符根因（sekai ni hana 507→2582 与 C++ 一致）
  2. fmt=1 Huffman 解压后未分发 evMobile（整曲 0 事件）
  3. MMMG 容器内 SEQU 未解析 + 时基取 MMMG 头字节
  4. evSequ 对照 create_event_sequ 逐分支重写（0x01-0x3B）+ 0xF0 漏 rest-- 越界
  5. evHps 补 type2 短格式表（SHORT_MOD/SHORT_EXP）/ bank=CC#32 / modulation
- 结论：全语料 1484 文件 notes/cc/pc/pb 总量 + 每通道音符数与 C++ 完全一致。

### noteOn 路由模型（bec00b2）
- routeNotes 静态预标记：fm / drum / ext / stream（时间轴重放通道 bank 状态；
  MA-5 默认 ch9=125、其余 124，同 C++ noteOn）。
- 用户裁定：**MA-5 鼓 = MA-3 同款 ROM 鼓 PCM**（bankMSB>=125；ymf825 关闭
  MA-5 鼓只是逆向未完成）。
- ext 行真键 waveID：MA-5 长格式（43 79 07 7F 01）vp=原始 16 字节取 d[25]；
  MA-3 为 7bit 打包 vp[15]。与 test_vis_slots 日志对拍一致。
- stream：MA-5 note < Mwa 条数（mwaStreams[note]，任意通道）。
- UI：MIDI 行只挂 FM 音符；P0-7=鼓键去重；WAVE=e<waveID>/s<idx>/m<idx>；
  分组标题（MIDI/ATR/PCM ROM Drums/WAVE）；单曲目内全表粘滞不复位。
- 已知边界：MA-3 mwa 行（drum-RAM 例外需 ini preset 表）用播放期常亮兜底，
  AudioWorklet 核心快照后消除。

### 其他
- 分阶段载入进度条（五阶段权重 + 原子阶段时间曲线 + 流光，0bb67ae）。
- 发布：README 重写为开源项目形态 + LICENSE + dist 打包脚本。

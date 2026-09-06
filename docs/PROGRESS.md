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

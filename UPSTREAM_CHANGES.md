# 上游组件与本地变更说明 (AGPL-3.0-only)

本独立应用 `音流独立接收器 (SoundFlow Receiver, io.soundflow.receiver)` 基于以下 GitHub 开源组件（均采用 AGPL-3.0-only 许可）独立构建，**不依赖、不调用任何小米专有系统服务（`com.milink.service` / `com.xiaomi.mi_connect_service`）**：

1. **核心接收协议栈**：`rosienosiesie/FusionPlay-Android` (`FusionPlay.MiPlaySdk` & `FusionPlay.AndroidIfAddrs`, commit `b43fe1130c3fdb233dcb41c2da174b858eb44cc2`)
   - **本地修改**：
     - `upstream/src/FusionPlay.MiPlaySdk/src/media.rs` & `lib.rs`：新增无锁原子音频输出实时峰值遥测函数 `live_output_peak()` 与 `reset_live_output_peak()`，用于驱动视觉歌词光环与极光流速，避免往事件队列发送高频 JSON 引发 GC/锁抖动。
     - `native/src/lib.rs`：全新编写 `io.soundflow.receiver.NativeBridge` JNI 接口，支持 `start`/`stop`/`poll`/`action`（增加上一首/下一首控制转发）及 `audioLevel`，并增加端口占用 (`EADDRINUSE`) 友好诊断提示。

2. **歌词动效核心引擎**：`amll-dev/applemusic-like-lyrics` (`@applemusic-like-lyrics/core` v0.6.0, `DomLyricPlayer` + `BackgroundRender` + `MeshGradientRenderer`)
   - **本地集成** (`web/src/stage-app.js` + `assets/ui/app.bundle.js`)：
     - 由 `build.py` 调用本地 `esbuild` 离线打包 `web/node_modules/@applemusic-like-lyrics/core` 及其 `@pixi/*`、`gl-matrix` 依赖为单文件 IIFE bundle，并实现双栏左唱片/右全幅歌词舞台（`AMLL 流体` + `QQ 黑胶水波` / `QQ 极光水波` / `QQ 逐字动感`），含同步时间微调（`-5.0s ~ +5.0s`）、无级动画速度（`0.25x ~ 2.50x`）、自动全屏展开与停播自动隐藏。
     - `LyricOrchestrator.java` 采用纯离线优先架构（LAN 内嵌 LRC → 本地导入 `.lrc` / 磁盘缓存 / 局域网实时流自动记忆 LRC → 默认关闭、按需手开的一键联网补全并永久离线保存）。

3. **新增独立原生模块与液态玻璃界面**：
   - **跨品牌开放投送扩展**：`DlnaRendererServer.java`（标准 SSDP + UPnP AVTransport/RenderingControl HTTP/SOAP MediaRenderer，1900 UDP + 8901 HTTP），允许非小米手机、音乐 App 或桌面客户端通过通用 DLNA 协议投送音频至同一套播放与歌词舞台。
   - **iOS 26/27 液态玻璃界面** (`assets/ui/{tokens.css,app.css,refraction.js,index.html}`)：按 GitHub `nextlevelbuilder/ui-ux-pro-max-skill` 与 `xenitV1/liquid-glass-skill` 实现自适应深/浅通透光学玻璃、Chromium SVG 边缘折射滤镜、胶囊液滴滑块与安全区边距响应。
   - **双进程防杀架构**：`ReceiverService` 运行于独立核心进程 `:receiver`，配合 `KeepAliveJobService`、低延迟 `WifiLock`、`MulticastLock`、`PARTIAL_WAKE_LOCK`、Android 15 合规双模式前台服务与 `MediaSession`。
   - **Root 系统服务固化模块**：`RootHardeningManager.java` 与内置 Kitsune Magisk 模块资产（支持热写入 `deviceidle`/`appops`/`perf_proc_protect_list` 以及一键安装/卸除 `/product/priv-app` 特权固化 + 开机 `service.d` 守护脚本）。

## v1.1 接手修正（2026-11-17）

- 内容区改为不透明分组表面，Liquid Glass 限定导航/控制层，新增通透度滑块、明暗/减少动态/高对比降级。预览采用紧凑布局，全屏舞台移动到页面根节点，稳定底部控制栏并提供顶部全屏入口。
- `web/src/ripple-field.mjs` 管理独立波纹寿命；缓存透明光影贴图，减少逐帧渐变和布局读取；歌曲节奏只控制新波生成，不裁掉已有外圈。保留屏幕刷新节奏，AMLL 背景低分辨率渲染，后台暂停视觉。
- 修复完整歌词超过 56 行被裁切；相同完整歌词不重建；DLNA 实际进度优先于旁路逐句词，歌曲时长更新不再被识别为换歌。
- 自动记录片段从完整词库中隔离，迁移旧 `_lanrec.lrc` 为 `.fragment`。导入/在线返回歌词使用当前播放位置，UI 切后台不冻结歌曲时间。
- DLNA 取得会话后独占播放状态/进度，匹配同一曲目的 MiPlay 只提供歌词补充，Rust 并行音频静音。停止后释放 HTTP/SSDP 和客户端连接，修复停止与 SSDP 线程初始化竞态。
- DLNA 节拍不再使用伪造计时器，新增可选 Visualizer 频谱；用户未授权也可正常播音和显示歌词。Rust Native 二进制与前版一致，本次未修改原生协议引擎。

## v1.1.1 设备查询兼容修正（2026-11-17）

`protocol.rs::build_device_info` 的首三字节是状态码和大端 u16 TLV 表长度，原固定 `0x0155` 会导致官方手机复制超出实际内容的数据并偶发 NegativeArraySizeException。现按实际 UTF-8 序列化字节数填入长度；单元回归覆盖长度变化与解析边界。Android 与固化模块版本更新为 1.1.1 / 111。只修改音流接收端，手机官方系统及应用不改动。

## v1.1.2 妙播接收重试（2026-11-18）

`lyra.rs::run_server` 将 `UdpSocket::recv_from` 返回的 `ErrorKind::Interrupted` 放入可重试分支。循环重新检查停止标志和待重发数据后继续接收，保留已有会话状态，并避免触发 `miplay_lyra_receive_failed` 和 100 毫秒退避。

## v1.1.7 输出缓冲与发现重试

Android 接收服务将手动偏移传入妙播的时间戳排程和缓冲排程。Oboe 以实际采样率请求 50 ms 输出缓冲，并记录音频驱动返回的帧数。`discovery.rs` 的 mDNS 响应循环和对端发现循环收到 `Interrupted` 后继续监听，与 Lyra UDP 的重试行为一致。

## v1.1.19 全栈代码审查修复（2026-11-26）

- **Rust JNI 与原生协议栈**：
  - 修复 `native/src/lib.rs::sanitize` 短子串 `"iv"` 误删 `session_active` / `receiver_name` 等字段的问题，并对 `lyrics`（64KB）与 `artwork`（256KB）豁免 2KB 截断；新增回归单元测试。
  - `protocol.rs::ControlHub` 与 `lyra.rs::LyraControlServer` 保存监听线程句柄并在 `Drop` 中同步回收端口（Lyra UDP 开启 `SO_REUSEADDR`）；`lyra.rs` 增加空闲对端会话淘汰与响应缓存上限防止常驻内存泄漏。
  - `media.rs` 中 Android Oboe 音频回调事件改由通道异步转发至 `miplay-audio-route` 监控线程发射，三频段分频滤波系数按实际硬件采样率动态计算。
- **Android 双进程服务与安全加固**：
  - 修复水波灵敏度误写 UI 进程 `.so` 的双进程 Bug，改经 Binder `Messenger` 同步至 `:receiver` 进程并持久化；已绑定状态下控制/配置指令优先走 `Messenger` IPC；使用进程级串行工作线程 + 实例代次防服务启停竞态。
  - `DlnaRendererServer` 增加 `http/https` 非回环 URI 校验、`Content-Length` 非负下界、有界线程池、SOAP 命名空间前缀兼容及统一的 `Visualizer` 同步采样；修复双协议切换静音残留、DLNA 误发切歌和缺失 `MediaSession` 元数据。
  - `MainActivity` 复用单线程池并节流单次 `su` 探测，加固 WebView 离线拦截与路径穿越防护，暂停态保留歌词舞台并持续同步曲目变更；`ReceiverService.readLogTail` 加锁并避免 UTF-8 截断乱码。
- **前端、Magisk 与构建**：
  - 修复完整 LRC 匹配实时句时重复叠加负同步偏移回跳上一句的问题，`setArtworkBase64` 兼容 JPEG/PNG/WebP；`ripple-water.mjs` 复用缓冲并支持 `webglcontextlost` 降级与恢复。
  - 去除 Magisk 全局 `service.d` 重复部署并在 `service.sh` 中等待 FBE 解锁；统一版本号至 `1.1.19` / `129`，支持 `build.py` 环境变量覆盖与跨分区移动。

## v1.1.20 水波鼓点声画对齐与全栈整改（2026-11-26）

- **原生音频延迟校准与因果鼓点识别（Rust）**：
  - `upstream/src/FusionPlay.MiPlaySdk/src/onset.rs`：步长加密至 `HOP = 256`（`5.33 ms`），预计算 2048 点 Hann 窗与位反转表；修复 `PendingBandPeak` 强度饱和导致重拍滞后的问题；重构因果多通道 `PercussionDetector`（底鼓/808 下扫频 + 锤头瞬态、军鼓/木鱼宽频冲击、高频打击乐，确认窗口收敛至 `≤ 16 ms`，抑制持续低音与纯音阶跃假阳性），校准 `-16 ms` 汉宁窗重心偏移，合成单击测试的输出时间戳误差通过 `±12 ms` 断言。
  - `upstream/src/FusionPlay.MiPlaySdk/src/media.rs`：记录 Oboe 实际输出缓冲帧数用于未收敛阶段的回退延迟估算，跳过回调前导静音帧并叠加对应延迟后再送入 `onset::tap_output`。
  - `native/src/lib.rs`：在 `emit()` 入口拦截高频诊断 trace 事件，防止高吞吐下冲掉 `media_info`/`playback_state`/`progress` 等关键控制事件。
- **Android 双进程服务、歌词时钟与 DLNA 频谱**：
  - `DlnaRendererServer.java`：将 `Visualizer` 缩放模式从 `SCALING_MODE_NORMALIZED`（逐帧自动增益抹平鼓点动态）修正为 `SCALING_MODE_AS_PLAYED`，补齐 `Seek` 即时状态广播。
  - `ReceiverService.java` & `LyricOrchestrator.java`：将 `dlnaServer.captureOnsetSpectrum()` 移入后台 `analysis` 线程并以 `12 ms` 节拍轮询；新增 `case "progress":` 实时锚定播放进度，消除 `MSG_AUDIO_LEVEL` 每 85 ms 用陈旧进度覆盖 90fps 歌词时钟导致的锯齿抖动；修复 DLNA 活跃期切回小米妙播的协议抢占与远端音量量化回声环路；统一使用 `SystemClock.elapsedRealtime()`。
- **前端水波零遮挡渲染与 `DESIGN.md` 合规**：
  - `web/src/{ripple-onsets.mjs,ripple-water.mjs,stage-app.js}`：默认启用 `percussion`（鼓点）模式并将 `classic`（融合节奏）接入原生起音队列，删除伪周期 `quietCadenceMs` 定时器；`rippleCoverRadius` 对齐 `#platter-disc` 真实外缘，WebGL 波包相位外移 `0.42 * width` 并缩短淡入至 `16 ms`，配合 `10 ms` 扫描提前量实现鼓点出声瞬间亮峰即刻跃出封面边缘；打通系统 `setReducedMotion` 与 WebGL 暂停联动。
  - `assets/ui/{index.html,tokens.css,redesign.css}`：按 `DESIGN.md` 将偏好设置重构为「歌词」「接收」「外观」三组双栏结构，补齐所有 `.lg-slider` 滑块的 `44 CSS px` 触控热区高度、浅色 `--label-2` 色值与 Android 系统优先字体栈；版本升至 `1.1.20`（`versionCode = 130`）。

## v1.1.21 实时链路、双协议与舞台修复（2026-11-27）

- Android 音频回调使用预分配 PCM 队列和有界时钟快照。路由线程负责诊断、Oboe XRun 与硬件帧时间映射。桌面继续使用 CPAL。
- 起音分析复用 FFT、参考谱和 JNI 缓冲，在积压时只处理最近窗口。可视需求控制频谱统计与采样，空批次不发往 UI。DLNA 复用同一次 Visualizer 采样，媒体控制与日志、歌词文件操作移出相应主线程。
- 目标拍点使用单调时钟时间戳，WebGL 用缓存几何与到期时间绘制。默认鼓点的 112 ms 合成双击回归通过，绝对声光偏差与有权限的 DLNA 真机播放由后续集成验收测定。


- 满环时只由 PCM 消费端裁剪超出目标窗的旧帧，真实 PTS 断层才重置播放。输出路由重开清理旧 PCM，并分别报告 XRun 初值与运行期增量。硬件帧时间映射可以重锚，检测器仍沿连续样本时钟推进。
- 新增合成混音起音回归，验证 90、112、125、140 ms 双底鼓与 55/110 ms 衰减回声。DLNA 频谱按 60 ms 估算输出延迟，等待有权限的设备测量。
- 暂停或停止 DLNA 后，已建立的妙播会话可通过明确的播放状态或有效元数据恢复。按通知内容和播放状态节流 Android 媒体更新。DLNA 将监听与 HTTP 请求拆到不同线程池，跳转发出后立刻更新位置查询，回调到达时与播放器实际位置对齐。
- AMLL 直接接收本地封面图片元素，避免库内部走 CSP 禁止的下载路径。调谐面板的开合只在布局变化时刷新水波圆心缓存。Pad 6 验证了底色和 DLNA 基本播控。

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

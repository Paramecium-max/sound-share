# MiPlay Multi-Room 全屋妙播发送端（Electron + C++20 跨平台核心）

一款不依赖 MIUI / HyperOS 系统组件的小米妙播（MiPlay）独立多设备全屋同步音频发送端。

- **表现层**：Electron + React + TypeScript + Vite（支持 Windows / macOS / Linux 三端桌面应用）
- **核心引擎**：独立无 GC 停顿的 C++20 `miplay-core`（CMake 构建，提供本地 WebSocket Sidecar 与跨平台 C-ABI，后续可无缝交叉编译复用到 Android / iOS）
- **协议文档**：
  - 总体架构设计：[docs/MIPLAY_SENDER_DESIGN.md](docs/MIPLAY_SENDER_DESIGN.md)
  - 实机抓包字节级协议规范：[docs/PROTOCOL_WIRE_SPEC.md](docs/PROTOCOL_WIRE_SPEC.md)

## 核心特性

1. **多设备毫秒级全屋同步（Multi-Room Sync）**
   - 内置实机实证的 **40 字节 UDP `TimerService` 主时钟校时服务器**，通过 RTSP M1 `wfd_timer_server_port` 下发给全屋所有接收端。
   - 统一主时钟 `MasterMediaClock` 与会话全局基准 `TIME_OFFSET`，保证中途热加入的音箱也能与现有音箱比特级对齐 PTS。
   - 支持 **单设备 `-300ms ~ +300ms` 延迟微调**（1ms 步进），消除不同型号音箱/电视的硬件 DAC 固有延迟差。
   - **一源多发零重复打包**：无偏置节点共享同一份 MPEG-TS（PAT `0x0000` / PMT `0x0100` / PCR `0x1000` / Audio PES `0x1100`）+ RTP-over-TCP（`PT=33` / `SSRC=0xDEADBEEF`），单节点异常自动隔离不卡顿其他房间。
2. **双模式音源采集**
   - **系统全局环回抓音**（Linux PipeWire/PulseAudio Monitor；Windows WASAPI / macOS ScreenCaptureKit 预留统一接口）
   - **内置无损 WAV 播放器** + **柔和全屋同步校准测试音** + 16 段实时频谱
3. **局域网发现 + 手动添加**
   - 自动在 UDP `5355`（`_miplay_lan._tcp.local.`）与 UDP `5353` 扫描设备，支持网卡选择与手动添加 IP:Port。

## 快速构建与运行

### 1. 编译 C++20 核心引擎与单元测试
```bash
cmake -S core -B core/build -DCMAKE_BUILD_TYPE=Release
cmake --build core/build -j$(nproc)
ctest --test-dir core/build --output-on-failure
```

### 2. 启动 Electron 全屋控制台
```bash
cd desktop
npm install
npm start
```

### 3. 全平台打包
```bash
cd desktop
npm run dist:linux   # AppImage / deb
npm run dist:win     # Windows NSIS 安装包
npm run dist:mac     # macOS DMG
```

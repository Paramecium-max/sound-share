# 小米妙播（MiPlay）字节级通信协议规范（实机实证版）

> **抓包基准环境**：  
> - 发送端（Source）：REDMI K90 Pro Max（`192.168.5.84`，HyperOS `OS3.0` / Android 16，`lib_version: miplaycast_os3_release1.5 3.1.6030516`）  
> - 接收端 1（Sink 1）：PC（`192.168.5.87`，开启 SP 流加密）  
> - 接收端 2（Sink 2）：`SoundFlow-Pad`（`192.168.5.95`，`lib_version: audio-display-release2.1 2.1.5071614`，明文 ADTS AAC 流）  
> - 抓包样本：`captures/20261124_114738_live_pc.pcap`、`captures/20261124_121010_multiroom_handshake.pcap`

---

## 1. 设备发现协议（mDNS）

小米妙播局域网发现**使用独立组播端口 UDP `5355`**（同时兼容标准 `5353`）：

| 参数 | 实机实证值 |
| :--- | :--- |
| 组播地址 | `224.0.0.251` |
| 妙播专用端口 | **UDP `5355`**（实测发送端查询 `224.0.0.251:5355`） |
| 服务类型（PTR Query） | **`_miplay_lan._tcp.local.`**（DNS Query Type = `12` PTR） |
| 辅助互联发现（UDP `5353`） | `_lyra-mdns._udp.local.`（同账号 PC/跨端设备）、`_rc._tcp.local.`（小米电视） |

---

## 2. 控制信令协议（TCP `8899`）

### 2.1 固定 9 字节二进制帧头（100% 实证）

所有 TCP `8899` 双向指令采用统一 9 字节定长头 + 变长载荷：

```text
 0       1       2       3       4       5       6       7       8
+-------+---------------+-------+---------------+-----------------------+
| 0x24  | cmd_type(u16) |  ver  |   seq (u16)   |  payload_len (u24)    |
| ('$') |  Big-Endian   | (u8)  | Little-Endian |      Big-Endian       |
+-------+---------------+-------+---------------+-----------------------+
|                     payload (payload_len bytes)                       |
+-----------------------------------------------------------------------+
```

- `magic`: 恒定 `0x24` (`'$'`)
- `cmd_type`: 2 字节大端无符号整数
- `ver`: 1 字节标志/版本号（实测常见 `0x00` 或 `0x01`）
- `seq`: 2 字节小端递增序列号（每条请求递增 1，对应 Ack 保持同一 `seq`）
- `payload_len`: **3 字节大端无符号整数**（`uint24`，实测最大含封面 `SetMediaInfo` 为 `0x005C89` = `23,689` 字节）

### 2.2 已实证命令编号对照表

| `cmd_type` | 命令名称 | 方向 | 载荷示例 / 说明 |
| :--- | :--- | :--- | :--- |
| `0x0000` | `Open` | Source → Sink | 拉起反向 WFD 连接，明文为 `wfd://<sender_ip>:<rtsp_port>?mirrorMode=1` |
| `0x0001` | `Open_Ack` | Sink → Source | 回复 5 字节结果码 |
| `0x0002` | `Close` | Source → Sink | 关闭当前投送会话 |
| `0x0006` | `Resume` | Source → Sink | 恢复播放（空明文，加密态 25B） |
| `0x0007` | `Resume_Ack` | Sink → Source | 恢复确认 |
| `0x0012` | `SetMediaInfo` | Source → Sink | 歌曲元数据（歌名、歌手、专辑、封面 Bitmap） |
| `0x0013` | `SetMediaInfo_Ack` | Sink → Source | 元数据确认 |
| `0x001A` | `HeartBeat` | Source → Sink | 每 5 秒一次心跳保活 |
| `0x001B` | `HeartBeat_Ack` | Sink → Source | 心跳应答 |
| `0x001C` | `GetVolume` / `GetState` | Source → Sink | 查询远端音量或状态 |
| `0x001E` | `GetDeviceInfo` | Source → Sink | 拉取接收端能力详情 |
| `0x001F` | `GetDeviceInfo_Ack` | Sink → Source | 返回设备能力 TLV/JSON（实测 354B） |
| `0x0022` | `Notify` | Sink → Source | 远端主动通知（明文态为键值 TLV，如 `mode`/`mediaInfoEx`/`state`） |
| `0x0028` | `Challenge` | Sink → Source | 建连后接收端主动下发 14B 挑战串（如 `13139245183899`） |
| `0x0029` | `Challenge_Ack` | Source → Sink | 40 字节十六进制应答串 |
| `0x0034` | `GetMirrorMode` | Source → Sink | 查询镜像模式 |
| `0x0035` | `GetMirrorMode_Ack` | Sink → Source | 返回镜像模式 |
| `0x0036` | `GetVersion` | Source → Sink | 上报发送端版本号（以 `\0` 结尾，如 `3.1.6030516\0`） |
| `0x0037` | `GetVersion_Ack` | Sink → Source | 返回接收端版本号（如 `2.1.5112421\0`） |
| `0x0040` | `SetPlaySource` | Source → Sink | 设置音源信息（加密态 105B） |
| `0x0041` | `SetPlaySource_Ack` | Sink → Source | 音源设置确认 |
| `0x0056` | `SetPosition` | Source → Sink | 每 1 秒同步一次播放毫秒进度（8 字节明文 → 加密态 25B） |
| `0x0057` | `SetPosition_Ack` | Sink → Source | 进度确认 |
| `0x0058` | `SetLocalDeviceInfo` | Source → Sink | 上报发送端设备名、蓝牙 MAC、同账号标志等 JSON |
| `0x0059` | `SetLocalDeviceInfo_Ack` | Sink → Source | 确认 |
| `0x1400` | `SafetyInfo` | Source → Sink | 协商安全通道能力（TLV 键 `"cmd"` + JSON） |
| `0x1401` | `SafetyInfo_Ack` | Sink → Source | 选定安全通道参数（TLV 键 `"ack"` + JSON） |
| `0x1402` | `SafetyAuth` | 双向 | 双向加密挑战认证（73B 加密帧） |
| `0x1403` | `SafetyAuth_Ack` | 双向 | 双向认证结果确认（137B 加密帧） |

### 2.3 首次建连握手时序与 TLV / Safety 封装（100% 实机实证）

1. **建连初始明文阶段**（`+2.603s ~ +2.655s` 实测）：
   - Sink → Source：`0x0028`（14B ASCII 挑战串）
   - Source → Sink：`0x0036` (`GetVersion`: `"3.1.6030516\0"`) + `0x0029` (40B Hex 应答) + `0x1400` (`SafetyInfo`)
   - Sink → Source：`0x0037` (`GetVersion_Ack`: `"2.1.5112421\0"`) + 明文 `0x0022` (`Notify`: `mode` / `mediaInfoEx` / `state`) + `0x1401` (`SafetyInfo_Ack`)
2. **内层键值 TLV 格式**（`0x1400` / `0x1401` / `0x0022` 实证）：
   - `[key_len:u8][key_ascii][type:u8][value...]`
   - 当 `type == 0x1E`（大块字符串/JSON）：后接 `[val_len:u32be][utf8_bytes]`，例如 `03 "cmd" 1e 00000078 "{...}"`；
   - 当 `type == 0x03`（单字节整数）：后接 `[val:u8]`，例如 `04 "mode" 03 02`、`05 "state" 03 00`。
3. **Safety 协商 JSON 实测样例**：
   - `0x1400` (`SafetyInfo`)：
     ```json
     {"aesIvTypes":"7","aesKeyTypes":"7","authAlgorithmTypes":"7","authKeyTypes":"3","integrityTypes":"1"}
     ```
   - `0x1401` (`SafetyInfo_Ack`)：
     ```json
     {"aesIvType":"4","aesKeyType":"4","authAlgorithmType":"4","authKeyType":"2","integrityType":"1","result":"0"}
     ```
4. **Safety 加密载荷封装结构（已实证）**：

```text
+---+---+---+---+---------+----------------------------------+------------------+
| 00| 07| 01| e0| pad_len |  AES-128-CBC Ciphertext (16 * N) |  4B Integrity/CRC|
+---+---+---+---+---------+----------------------------------+------------------+
  \_____ 4B 头 ___/  (1B)
```

- 验证示例 1（空明文 `HeartBeat` / `Resume`）：`pad_len = 0x10 (16)`，密文 16 字节，总长 `5 + 16 + 4 = 25` 字节（`0x000019`）。
- 验证示例 2（8 字节 `SetPosition`）：`pad_len = 0x08 (8)`，明文 8B + 填充 8B = 16B 密文，总长 `5 + 16 + 4 = 25` 字节。
- 验证示例 3（大包 `SetMediaInfo`，`plen = 23689`）：`23689 - 5 - 4 = 23680 = 16 × 1480`，`pad_len = 0x05`。
- 架构设计上，`CmdSession` 通过可插拔的 `ISafetyProvider` 接口隔离安全算法（支持 `PlaintextProvider` 明文兼容模式与自定义/外部安全提供者接入）。

---

## 3. 反向 WFD RTSP 媒体协商协议（100% 明文实证）

发送端每加入一台目标接收端，即在本地监听一个动态 TCP 端口（例如 `40089`），通过 `8899` 发送 `Open("wfd://<sender_ip>:<rtsp_port>?mirrorMode=1")`。  
接收端收到后，会向 `<sender_ip>:<rtsp_port>` 建立 **多条 TCP 连接**（一条 RTSP 控制连接 + 一条 `multi_port` 音频数据连接 + 可选 `image_port` 连接）。

### 3.1 完整 M1–M7 + `TIME_OFFSET` 交互原文（实机捕获）

1. **M1（Source → Sink）：宣告全屋主时钟端口 `wfd_timer_server_port`**
   ```http
   OPTIONS * RTSP/1.0
   Date: Tue, 24 Nov 2026 04:10:16 +0000
   Server: 
   CSeq: 2
   Require: org.wfa.wfd1.0
   wfd_timer_server_port:3232236884:44274
   lib_version: miplaycast_os3_release1.5 3.1.6030516
   ```
   - `3232236884` 为发送端 IPv4 地址的大端无符号 32 位整数：`192.168.5.84 = (192<<24)|(168<<16)|(5<<8)|84`。
   - `44274` 为发送端全局共享的 **`TimerService` UDP 校时端口**（所有加入全屋播放的音箱都收到同一个 `IP:Port`！）。

2. **M2（Sink → Source ↔ Source → Sink）**：标准双向 `OPTIONS` 确认。

3. **M3（Source → Sink）：查询音频编码、TCP 多会话与加密保护能力**
   ```http
   GET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0
   CSeq: 3
   Content-Type: text/parameters
   Content-Length: 347

   wfd_content_SP_protection
   wfd_video_formats
   wfd_video_enctype
   wfd_video_gamuttype
   wfd_video_bitrate
   wfd_dynamic_video_enable
   wfd_current_video_info
   wfd_audio_codecs_v2
   wfd_client_rtp_ports
   wfd_tcp_enable
   wfd_tcp_multi_session_enable
   wfd_support_secure_win
   wfd_mirror_control_enable
   wfd_standby_resume_capability
   wfd_image_enable_v2
   ```
   接收端应答示例（纯音频接收端）：
   ```http
   RTSP/1.0 200 OK
   CSeq: 3
   Content-Type: text/parameters

   wfd_audio_codecs_v2: 15 3 3
   wfd_video_formats: none
   wfd_client_rtp_ports: RTP/AVP/TCP;interleaved mode=play
   miplay_support_image: none
   wfd_standby_resume_capability: supported
   wfd_content_SP_protection: 4 1 256 3 1 1 0 0
   ```

4. **M4（Source → Sink）：选定音频编码与关闭/开启流加密**
   ```http
   SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0
   CSeq: 4
   Content-Type: text/parameters

   wfd_type_encryp: 0 1 1 0 0
   wfd_audio_codecs_v2: 1 1
   wfd_client_rtp_ports: RTP/AVP/TCP;interleaved mode=play
   wfd_presentation_URL: rtsp://192.168.5.84/wfd1.0/streamid=0 none
   ```
   - `wfd_type_encryp: 0 1 1 0 0` 首字段 `0` 表示**明文音频流（不加密）**！
   - `wfd_audio_codecs_v2: 1 1` 表示 **ADTS AAC / 48kHz Stereo**。

5. **M5（Source → Sink）**：`SET_PARAMETER` → `wfd_trigger_method: SETUP`。

6. **M6（Sink → Source）：绑定音频数据 TCP 连接（`MultiPort`）**
   ```http
   SETUP rtsp://192.168.5.84/wfd1.0/streamid=0 RTSP/1.0
   CSeq: 2
   Transport: RTP/AVP/TCP;interleaved=0-1
   MultiPort: image_port=37782;multi_port=37784
   ```
   - **极其关键**：发送端根据 `multi_port=37784` 在已 `accept()` 的 TCP 连接池中匹配对端源端口为 `37784` 的 Socket，后续所有音频 RTP-over-TCP 包均写入该专用数据连接！

7. **全屋时间基准下发（Source → Sink，在 M6 `200 OK` 同步发出）**
   ```http
   TIME_OFFSET rtsp://localhost/wfd1.0 RTSP/1.0
   CSeq: 6
   Content-Type: text/parameters
   TimeOffset:59209621361
   Content-Length: 0
   ```
   - `TimeOffset` 为本次全屋多设备播放会话的**全局基准主时钟起点（微秒 `int64`）**。无论新音箱何时中途加入全屋组，只要收到相同的 `TimeOffset` 并连接同一 `TimerService`，即可与正在播放的其他音箱精准毫秒级同步！

8. **M7（Sink → Source）**：`PLAY rtsp://192.168.5.84/wfd1.0/streamid=0 RTSP/1.0` → Source 返回 `200 OK; Range: npt=now-`，开始持续推流。

9. **运行期保活与状态上报**：
   - Sink 每 ~200ms 在 RTSP 连接上报 `VIDEO_LATENCY rtsp://localhost/wfd1.0 RTSP/1.0`（含 `latency`、`bitrate`、`rtpPacketNum`）。
   - Source/Sink 每 5~20 秒互发空体 `GET_PARAMETER`（M16）保活。

---

## 4. 全屋时钟同步协议（`TimerService` UDP，100% 明文实证）

- 发送端启动一个全局共享 UDP 监听端口（并在 RTSP M1 `wfd_timer_server_port` 中通告给全屋所有设备）。
- 每个接收端定期发送 **固定 40 字节（5 × Little-Endian `uint64`）** 的明文校时请求：

```c
struct MiPlayTimerPacket {
    uint64_t client_synced_us;  // Q[0]: 客户端按当前 offset 估算的主时钟 (us)，首次可为 0 或本地值
    uint64_t client_local_us;   // Q[1]: 客户端本地单调时钟 (us)
    uint64_t server_recv_us;    // Q[2]: 服务端收到包时覆写为自身单调主时钟 now_us()
    uint64_t server_send_us;    // Q[3]: 服务端发回包时覆写为自身单调主时钟 now_us()
    uint64_t seq;               // Q[4]: 递增校时序列号
}; // sizeof == 40 bytes, Little-Endian
```

- **发送端处理逻辑（实测验证）**：收到 40 字节包后，原样保留 `Q[0]`、`Q[1]`、`Q[4]`，将 `Q[2]` 与 `Q[3]` 覆写为发送端当前 `CLOCK_MONOTONIC` 微秒时间戳，直接 `sendto()` 原路发回！
- 实测 K90 上两台设备（`192.168.5.87` 与 `192.168.5.95`）同时向同一端口 `44274` 校时，单次校准残差仅 **1~3ms**。

---

## 5. 音频媒体流封装规范（100% 实证）

音频流写入 `MultiPort` 指定的专用 TCP 连接（`multi_port`），每帧采用标准 **RFC 2326 RTP-over-TCP + MPEG-TS**：

```text
+------+------+----------+------------------------+-----------------------------+
| 0x24 | 0x00 | len(u16) |  12B Standard RTP Hdr  |  N × 188B MPEG-TS Packets   |
| ('$')| ch=0 |   BE     | PT=33, SSRC=0xDEADBEEF | (PAT / PMT / PCR / Audio)   |
+------+------+----------+------------------------+-----------------------------+
```

- **RTP Header（12 字节）**：
  - `Byte 0`: `0x80`（V=2）
  - `Byte 1`: `0xA1`（M=1, `PT = 33` 即 MP2T）
  - `Byte 2..3`: 递增 RTP 序号（Big-Endian `uint16`）
  - `Byte 4..7`: 90kHz RTP 时间戳（48kHz AAC 每帧 1024 samples 严格递增 **`1920`** = 21.333ms）
  - `Byte 8..11`: `0xDEADBEEF`
- **MPEG-TS PID 分配（实机验证）**：
  - `PAT`: PID `0x0000` → 指向 PMT `0x0100`
  - `PMT`: PID `0x0100` → `PCR_PID = 0x1000`，音频流 `stream_type = 0x0F`（ISO/IEC 13818-7 ADTS AAC），`elementary_PID = 0x1100`
  - `PCR`: PID `0x1000`（周期与 PAT/PMT 一同发送）
  - `Audio PES`: PID `0x1100`，Stream ID `0xC0`：
    - 明文模式（`wfd_type_encryp: 0 1 1 0 0`）：`flags = 0x84 0x80`，`header_len = 0x07`（5 字节 90kHz PTS + 2 字节 `0xFF 0xFF` 填充），紧接标准 **ADTS AAC 帧**（`0xFF 0xF9 0x4C ...`）！
    - **全屋多设备同步铁律**：同一帧音频推送到全屋所有设备时，**`rtp_ts` 与 PES `PTS` 完全相同**！

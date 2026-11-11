# EdgeFusion Gateway（RK3576 智能视频网关）

运行在 **DshanPi-A1 / RK3576 Linux** 上的 C 多线程视频网关：USB / RTSP 输入 → H.264 分段录像 → Web 预览与回放 + RTSP 转发；通过 MQTT 接入端侧状态、移动事件和快照，通过云端视觉模型或独立 NPU 服务完成智能分析，统一写入 SQLite 事件库。

整体介绍与五个页面截图见[项目首页](../README.md)，PC 摄像头代理的使用方法见[端侧 README](../edgefusion_vision/README.md)。

---

## 1. 目录结构

```text
edgefusion_gateway/
├── src/
│   ├── main.c               # 模块生命周期、信号退出、录像到期扫描
│   ├── conf.*               # 配置解析、保存与部分参数热更新
│   ├── usb_source.*         # V4L2 MJPEG 采集、解码、libx264 编码
│   ├── stream_receiver.*    # 输入选择、录像与预览/转发分发
│   ├── recorder.*           # MP4 分段、空间清理与保留期限
│   ├── pkt_queue.*          # 有界 AVPacket 队列
│   ├── tx_queue.*           # 有界字节发送队列
│   ├── frame_grabber.*      # JPEG 缓存、RTSP 路径抽帧
│   ├── rtsp_server.*        # RTSP 协议、并发客户端与发送隔离
│   ├── mqtt_client.*        # MQTT 3.1.1 客户端
│   ├── mqtt_bridge.*        # 端侧接入、快照与命令桥接
│   ├── cloud_ai.*           # 云端图像请求、结构化结果处理
│   ├── event_bus.*          # SQLite 事件、证据图、订阅回调
│   ├── alarm.*              # GPIO sysfs 报警
│   └── web.*                # HTTP、MJPEG、API、MP4 Range 回放
├── web/                     # 实时预览 / 录像 / 事件 / 端侧设备 / 设置
├── config/rk3576-usb.conf    # USB 基础配置
├── deploy/                  # systemd 服务模板
├── scripts/                 # 启动、部署、NPU 服务与测量工具
├── tests/                   # C、Python、JavaScript 测试工具
└── Makefile.native          # Linux 板端原生编译
```

---

## 2. 数据流与线程模型

### 2.1 视频链路

- **USB 输入**：`ioctl + mmap + poll` 采集 MJPEG；原始 JPEG 交给预览缓存，另一条路径经 FFmpeg 解码、格式转换、libx264 编码后写入 MP4，并投递到预览/转发队列。
- **RTSP 输入**：读取 H.264 压缩包，直接封装录像；预览路径按需软解抽帧，本地 RTSP 服务转发 H.264。
- **录像处理**：使用实际输入时间基，各段内部时间戳归零，以 fragmented MP4 保存。录像写入仍在采集路径执行，并非独立异步写盘线程。
- **发送隔离**：有界包队列将采集与预览/RTSP 分发解耦；每个 RTSP 客户端有独立线程与有界发送队列，以 `poll(POLLOUT)` 驱动发送。

### 2.2 智能事件链路

| 来源 | 路径 |
|------|------|
| 端侧移动检测 | PC agent → MQTT → `mqtt_bridge` → 事件库 |
| 云端识别 | 本地 JPEG / 端侧快照 → `cloud_ai` → 视觉模型 → 事件库 |
| 板端 NPU | `/api/snapshot` → Python RKNN 服务 → `POST /api/events` → 事件库 |

事件包含类型、置信度、描述、建议动作及可用的证据图。事件入库后通过订阅回调交给报警模块；事件总线本身不是独立工作线程。

---

## 3. 依赖与编译

以下命令在 **RK3576 板端 Ubuntu / Armbian** 执行：

```bash
sudo apt install build-essential pkg-config ffmpeg v4l-utils \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
  libcurl4-openssl-dev libssl-dev libsqlite3-dev libjson-c-dev zlib1g-dev

cd ~/Program/EdgeFusion_Smart_NVR/edgefusion_gateway
make -f Makefile.native check-deps
make -f Makefile.native -j2
```

产物为 `build-native/edgefusion_gateway`。原生构建自动生成头文件依赖；如需强制重编可使用 `make -f Makefile.native -B -j2`。编译后需重启运行中的网关。

| 依赖 | 用途 |
|------|------|
| FFmpeg 开发库，运行时可用的 libx264 编码器 | 视频解码、缩放、软件编码和 MP4 封装 |
| libcurl / OpenSSL | 云端 HTTPS 请求 |
| SQLite / json-c | 事件存储及 JSON 处理 |
| pthread | 采集、预览、网络和 AI 线程 |
| Mosquitto（可选，独立进程） | MQTT Broker；网关客户端无需 libmosquitto |
| RKNNLite、OpenCV、NumPy（可选） | 独立 Python NPU 服务 |

---

## 4. 配置与启动

### 4.1 USB 基础配置

先确认摄像头设备和支持的 MJPEG 模式：

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video11 --list-formats-ext
```

`/dev/video11` 仅为示例，请替换为实际设备。编辑 `config/rk3576-usb.conf`：

```ini
source_type = v4l2
video_device = /dev/video11
capture_width = 640
capture_height = 480
capture_fps = 30
video_bitrate_kbps = 1500

storage_dir = data/recordings
sqlite_db_path = data/gateway.db
segment_seconds = 60
storage_min_free_mb = 500
storage_retention_days = 3
segment_prefix = rec

web_bind = 0.0.0.0
web_port = 8080
web_root = web
web_mjpeg_fps = 5

rtsp_server_enable = 1
rtsp_server_port = 8554
```

仓库基础配置的 `rtsp_server_enable` 实际默认为 `0`；上面示例将其打开。目录为相对路径时，应从 `edgefusion_gateway` 目录启动。

切换为 RTSP 输入时，设置 `source_type=rtsp`、实际 `rtsp_url` 和 `rtsp_transport=tcp`，然后重启。USB 与 RTSP 是网关主视频源的两种选择，不是同时录像的两路通道。

### 4.2 前台与 systemd

```bash
# 前台运行，Ctrl+C 停止
bash scripts/run-usb.sh

# 结束前台实例后，安装并启动 systemd 服务
bash scripts/install-service.sh
```

服务管理：

```bash
systemctl status edgefusion-gateway --no-pager
sudo systemctl restart edgefusion-gateway
sudo systemctl stop edgefusion-gateway
journalctl -u edgefusion-gateway -f
```

服务模板使用仓库内 `config/rk3576-usb.conf`，提供开机启动、异常退出重启及 SIGINT 退出收尾。

**已有演示部署**使用 `~/nvr-data/gw.conf`、`~/nvr-data/recordings` 和 `~/nvr-data/snapshots`。请先查看已安装服务：

```bash
systemctl cat edgefusion-gateway
```

若要使用该配置前台调试，先停止服务，再从网关目录执行：

```bash
./build-native/edgefusion_gateway -path ~/nvr-data/gw.conf
```

### 4.3 生效方式与录像保留

- AI、报警、预览帧率、分段时长等有热更新路径；并非所有页面配置都能立即生效。
- 输入类型、USB 参数、监听端口、MQTT 连接参数和录像保留天数等修改后应重启网关。
- 录像保留默认 3 天：启动时与之后每 60 秒按文件最后修改时间清理符合命名规则的已完成片段，跳过当前录像。
- 空间不足时还会清理最旧录像；网关停止期间不会扫描，到下次启动补清理。录像保留策略不等于事件图片保留策略。

---

## 5. MQTT 端侧接入

网关内实现了 MQTT 3.1.1 客户端，包括连接与订阅、QoS0/1、PUBACK、心跳、主题通配、粘包/半包处理、退避重连和未确认消息重发。Broker 作为独立进程运行。

### 5.1 Broker 与网关配置

板端安装：

```bash
sudo apt install mosquitto mosquitto-clients
```

在 Broker 配置目录（例如 `/etc/mosquitto/conf.d/edgefusion.conf`）添加局域网演示监听配置；已有监听时先核对，避免重复绑定：

```conf
listener 1883 0.0.0.0
allow_anonymous true
```

```bash
sudo systemctl restart mosquitto
```

该配置用于可信局域网演示，不提供设备认证。编辑实际使用的网关配置：

```ini
mqtt_enable = 1
mqtt_host = 127.0.0.1
mqtt_port = 1883
mqtt_client_id = edgefusion-edge
mqtt_prefix = edgefusion/device
```

重启网关，然后按[端侧 README](../edgefusion_vision/README.md)启动 PC agent。

### 5.2 网页与命令

「端侧设备」页显示在线状态、消息数量、事件入库数、快照数、AI 投递数及 PC 摄像头 HTTP 视频。按钮支持立即抓拍、暂停、恢复、2 秒/5 秒快照间隔。

也可在板端调用命令接口：

```bash
curl -fsS -X POST http://127.0.0.1:8080/api/mqtt/cmd \
  -H 'Content-Type: application/json' \
  -d '{"device":"A1","cmd":"snapshot"}'
```

`pause` 停止周期快照与移动事件上报，状态心跳和 HTTP 视频仍继续；`resume` 恢复。端侧视频通过浏览器访问 PC 的 HTTP 服务，不经过 MQTT 视频传输，也不自动进入网关录像与 RTSP 输出。

---

## 6. 云端 AI 与板端 NPU

### 6.1 云端视觉分析

在「系统设置」填写 OpenAI 兼容的**视觉模型**接口、Token、模型名与 Prompt：

| 配置项 | 含义 |
|--------|------|
| `ai_enable` | 云端 AI 开关 |
| `ai_source` | `local` 网关帧、`device` 端侧快照、`both` 两者 |
| `ai_fps` | 分析节奏配置，实际吞吐受请求耗时限制 |
| `ai_frame_width` / `ai_jpeg_quality` | 上传图片尺寸与 JPEG 质量 |
| `anthropic_base_url` / `anthropic_auth_token` / `anthropic_model` | 接口、凭证和模型；字段名虽如此，实际使用 OpenAI 兼容协议 |
| `ai_prompt` | 场景描述及结构化结果约束 |

Prompt 可要求返回：

```json
{"event_type":"blue_duck_alert","confidence":0.95,"description":"画面中出现蓝色鸭子玩偶","suggested_action":"record"}
```

`none` 不作为告警入库；自定义事件类型可在事件列表展示。分析来源选择不会切换实时预览或录像源。是否支持图像输入取决于实际服务，不能仅凭接口兼容就认定模型具备视觉能力。

### 6.2 板端 NPU

独立 `scripts/npu-infer-service.py` 从 `/api/snapshot` 取帧，经 RKNNLite 推理、检测框解码、阈值过滤和冷却去重后，向 `/api/events` 提交事件及 JPEG 证据图。

先准备匹配 RK3576 的模型、NPU 驱动与 Python 运行环境。以下使用演示环境中已有的路径：

```bash
~/venvs/rknn-lite2/bin/python scripts/npu-infer-service.py \
  --model ~/nvr-data/yolo11n_fp.rknn \
  --gateway http://127.0.0.1:8080 \
  --fps 2 --classes person --conf 0.35 --cooldown 20
```

成功检测时事件中心可见 `npu_person` 和证据图。停止该服务不会停止网关录像。

项目记录了 ONNX → RKNN 的模型转换与 int8/fp16 对照过程，当前演示选择 YOLO11n fp16。模型、转换环境和虚拟环境需单独准备，不由网关编译命令生成。

### 6.3 报警联动

GPIO 模块按 `alarm_trigger_types` 与 `alarm_min_confidence` 判断是否驱动 LED / 蜂鸣器，并保持 `alarm_active_s` 秒。引脚为 `-1` 时禁用；需要实际接线及 GPIO 访问权限。

事件中的 `suggested_action` 仅作展示，不会据此自动录像或发送通知。截图中的蓝色鸭子事件若需硬件联动，还需将 `blue_duck_alert` 加入触发类型。

---

## 7. Web 与接口

| 页面 / 入口 | 功能 |
|-------------|------|
| 实时预览 | MJPEG、OSD、抓拍、视频与录像指标 |
| 录像回放 | MP4 列表、Range 回放、下载 |
| 事件告警 | 类型筛选、事件描述、置信度和证据图 |
| 端侧设备 | HTTP 实时画面、最近快照、MQTT 状态与控制 |
| 系统设置 | 视频服务、存储、AI、报警等配置 |
| `GET /api/status` | 网关、录像、RTSP、MQTT、AI 等状态 |
| `GET /api/snapshot` | 当前网关 JPEG |
| `GET /api/events` | 事件列表 |
| `POST /api/events` | 外部分析服务提交事件 |
| `GET /api/config` | 配置查看（凭证脱敏） |
| `POST /api/mqtt/cmd` | 下发端侧命令 |
| `GET /api/mqtt/snapshot` | 最近端侧快照 |

Web 地址为 `http://<网关IP>:8080`；启用转发后可通过 `rtsp://<网关IP>:8554/track0` 拉流。

---

## 8. 测试与排错

在板端网关目录执行：

```bash
make -f Makefile.native test
make -f Makefile.native test-web test-events
make -f Makefile.native test-color
make -f Makefile.native test-retention
curl -fsS http://127.0.0.1:8080/api/status
```

`test` 覆盖录像、色彩、包队列、发送队列、MQTT 客户端及保留策略；`test-web` / `test-events` 针对 MQTT 网页接口和事件图片。MQTT 在线往返另可使用 `make -f Makefile.native test-mqtt`，需本地 Broker。

`scripts/smoke-usb.sh`、`measure-run.sh`、`monitor-run.sh` 提供设备冒烟与采样；运行前核对脚本的配置、设备和实例管理方式。

| 现象 | 排查方向 |
|------|----------|
| USB 摄像头打不开 | 核对设备节点、MJPEG 模式、video 组权限及是否已有实例占用 |
| 编译后页面行为未变 | 重启实际服务，确认 `ExecStart` 指向的二进制与配置 |
| RTSP 无法连接 | 基础配置默认关闭转发；启用并重启，核对 8554 端口 |
| MQTT 无数据或反复重连 | 检查 Broker 监听、网络、主题前缀和重复 client_id |
| 端侧视频不可见但事件正常 | 检查 PC HTTP 地址、8081 端口与防火墙，视频链路独立于 MQTT |
| 云端没有事件 | 检查开关、Token、视觉模型能力、来源是否有帧及日志 |
| NPU 没有事件 | 检查网关截图接口、模型兼容性、类别阈值和推理服务日志 |
| GPIO 未动作 | 检查引脚、权限、触发类型与置信度；建议动作字段不直接驱动 GPIO |

---

## 9. 当前边界

当前主视频源为单路，USB 使用软件 H.264 编码；PC 视频未自动接入统一录像。NPU 独立进程只从网关取帧。MQTT TLS、设备鉴权、QoS2、多设备规模化管理尚未完成或验证。慢客户端发送隔离已有实现与并发验证记录，但踢出兜底触发、真实 USB 拔插以及 24–72 小时组合故障测试仍不能作为已验证保证。

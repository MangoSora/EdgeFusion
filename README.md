# EdgeFusion — RK3576 单板智能 NVR（上游项目的二次开发）

## 这是什么

**基于开源项目 [dshanpi/EdgeFusion_Smart_NVR](https://github.com/dshanpi/EdgeFusion_Smart_NVR) 的二次开发。**
上游是「V821 端侧采集 + IMX6ULL 边侧录像 + 云端分析」的三层架构；本仓库把它改造成
**单块 DshanPi-A1（RK3576）+ 一个 USB 摄像头**就能独立运行，并保留录像、预览、回放与 RTSP 转发能力。

| 来源 | 内容 |
|---|---|
| **上游提供** | 网关主框架、录像分段封装、SQLite 事件库、Web 页面、自研轻量 RTSP 服务端、V821 端侧 MPP 工程 |
| **我做的改造** | USB(V4L2) 采集链路、MJPEG→H.264 软件编码、有界队列与线程解耦、预览直通、色彩范围缺陷修复、测试与度量工具、systemd 服务化 |

- 逐项改动、验证命令与板端原始证据：**[edgefusion_gateway/docs/RK3576_PORT.md](edgefusion_gateway/docs/RK3576_PORT.md)**
- "我做了什么 / 验证到什么程度 / 没做什么"：**[docs/MY_WORK.md](docs/MY_WORK.md)**

### 当前链路（本仓库实际形态）

```
USB UVC 摄像头 ──MJPEG──▶ V4L2 采集(mmap) ──▶ MJPEG 解码 ──▶ 色彩范围/尺度转换 ──▶ libx264 编码
                              │                                                      │
                              └── 原始 JPEG ──▶ 预览(Web MJPEG)         分段 MP4 录像 + RTSP 转发
                                                                                    │
                                                                          Web：预览/回放/事件
```

### 硬件与系统

| 项 | 值 |
|---|---|
| 开发板 | DshanPi-A1（RK3576，aarch64） |
| 系统 | 100ASK Armbian 25.11 / Ubuntu 24.04 noble，内核 6.1.115-vendor-rk35xx |
| 摄像头 | USB UVC（MJPEG，640×480@30fps 起，最高 2560×1440） |
| 构建 | 板端原生 `make -f Makefile.native`（gcc + FFmpeg/libx264 + sqlite3 + json-c） |

### 快速开始（板端）

```bash
cd edgefusion_gateway
make -f Makefile.native -j2
bash scripts/run-usb.sh          # 前台运行，Ctrl+C 优雅退出
# 浏览器打开 http://<板子IP>:8080 → 实时预览 / 录像回放 / 事件
```

常驻服务：`bash scripts/install-service.sh`（systemd 单元，含优雅退出与崩溃重启）

### 已验证结果（板端实测，非推测）

| 验证 | 命令 | 结果 |
|---|---|---|
| 单元与回归测试 | `make -f Makefile.native test` | **8 项 PASS**：录像时间基/分段、色彩范围、队列语义与并发 |
| USB 全链路冒烟 | `bash scripts/smoke-usb.sh` | **5 项 PASS**：出段可解码、预览异步零丢包、无弃用告警、5 秒内退出 |
| 60 秒性能采样 | `bash scripts/measure-run.sh 60` | CPU 均值 44.0%（峰 73%）、RSS ≤45 MB、24.80 包/秒、预览丢包 0 |

> 未验证的部分（摄像头热插拔、慢客户端隔离修复、24–72h 稳定性、MPP 硬编码）在
> [docs/MY_WORK.md](docs/MY_WORK.md) 中逐条列明，不写成已完成。

---

## 上游项目背景（V821 + IMX6ULL，非本仓库当前形态）

基于 **V821（端）+ IMX6ULL（边）+ 云端大模型（云）** 三层架构的智能网络视频录像机。V821 无线采集 H.264 推流，IMX6ULL 拉流录像并提取关键帧上云做视觉分析，低成本实现「会思考」的 NVR。

```
┌─────────────┐  Wi-Fi RTSP   ┌─────────────┐  HTTPS    ┌─────────────┐
│  V821 端侧  │ ────────────▶ │ IMX6ULL 边侧│ ────────▶ │  云侧 AI    │
│ 摄像头采集  │  H.264 1080P  │ 录像+上云+  │  关键帧    │ qwen3.6-plus│
│ H.264硬编码 │               │ Web+RTSP转发│  JSON事件  │ 视觉理解    │
└─────────────┘               └─────────────┘           └─────────────┘
   edgefusion_vision           edgefusion_gateway        阿里百炼
```
<img width="1905" height="928" alt="Snipaste_2026-07-02_15-27-31" src="https://github.com/user-attachments/assets/e2bc250e-7b21-4137-8bdd-02a9486d287f" />
<img width="1908" height="933" alt="Snipaste_2026-07-02_15-18-28" src="https://github.com/user-attachments/assets/8c357844-a62a-480e-a9b7-a3ff391e29e5" />
<img width="1888" height="937" alt="Snipaste_2026-07-02_15-07-36" src="https://github.com/user-attachments/assets/40976a5c-f92a-400b-b40c-09ff8eac16eb" />
<img width="1889" height="925" alt="Snipaste_2026-07-02_15-07-53" src="https://github.com/user-attachments/assets/60e104f3-a21d-484d-babe-3686270863cd" />

---

## 仓库结构

| 路径 | 说明 |
|------|------|
| [`edgefusion_vision/`](edgefusion_vision/README.md) | **端侧**：V821 sample 工程，MIPI CSI → H.264 硬编码 → RTSP 推流 `rtsp://<V821-IP>:8554/ch0` |
| [`edgefusion_gateway/`](edgefusion_gateway/README.md) | **边侧**：IMX6ULL 网关守护进程，拉流录像 + 关键帧上云 + 事件入库 + Web/RTSP |

> 日常开发用 `edgefusion_vision`（含调好的运行配置和 RTSP URL 打印）。

---

## 硬件平台

| 板子 | 芯片 | 角色 | OS |
|------|------|------|-----|
| 百问网 AvaotaF1 V821 | 全志 V821（RISC-V 1.2GHz，内置 Wi-Fi，H.264 硬编码） | 端侧摄像头 | Tina Linux 5.0（musl） |
| 百问网 IMX6ULL-Pro | NXP i.MX6ULL（ARM Cortex-A7 800MHz） | 边侧网关/AI NVR | Buildroot Linux（glibc） |
| 阿里百炼 | qwen3.6-plus 多模态视觉模型 | 云侧 AI | — |

完整硬件规格与外设清单见 [`IMX6ULL_V821_Project.md`](IMX6ULL_V821_Project.md) §4。

---

## 核心能力

- **无线采集推流**：V821 MIPI CSI → H.264 硬编码 → Wi-Fi RTSP 推流（1920×1080@15fps）
- **本地录像**：IMX6ULL 拉 RTSP，H.264 直接 remux 成 fragmented MP4 分段（不解码，断电安全，循环覆盖）
- **云端 AI 分析**：1fps 软解关键帧 → JPEG → HTTPS 上云 qwen3.6-plus → 结构化 JSON → 事件入库（已验证识别人脸/人员，置信度 0.95+）
- **报警联动**：GPIO 驱动 LED/蜂鸣器（引脚待定）
- **Web 管理**：MJPEG 实时预览 + OSD + 录像回放 + 事件告警列表 + 配置热加载 + 云端 AI 开关
- **RTSP 转发**：IMX6ULL 自研轻量 RTSP server，H.264 passthrough 供 VLC/NVR 拉流

---

## 快速开始

### 0. 前置条件（主机 Ubuntu）

```bash
sudo apt install android-tools-adb build-essential
sudo usermod -aG dialout $USER      # 访问串口，需重新登录生效
```

配置 udev 规则 `/etc/udev/rules.d/51-edgefusion-android.rules`：
```
SUBSYSTEM=="usb", ATTR{idVendor}=="1d6b", ATTR{idProduct}=="0104", MODE="0666"  # IMX6ULL
SUBSYSTEM=="usb", ATTR{idVendor}=="1f3a", ATTR{idProduct}=="0000", MODE="0666"  # V821
```
```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
adb devices   # 应同时看到 100ask_IMX6ULL 和 4c48410491c30a72512
```

两板出厂已烧好系统，无需重新烧录固件。

### 1. V821 端：编译并启动推流

```bash
cd ~/EdgeFusion/edgefusion_vision
./build.sh                                    # 产出 output/sample_rtsp_strip

adb -s 4c48410491c30a72512 push output/sample_rtsp_strip /mnt/extsd/sample_rtsp
adb -s 4c48410491c30a72512 push output/sample_rtsp.conf   /mnt/extsd/
adb -s 4c48410491c30a72512 shell 'chmod +x /mnt/extsd/sample_rtsp'
```

**必须从串口启动**（V821 无 setsid，adb 后台进程会随会话退出被杀）：
```bash
sudo stty -F /dev/ttyUSB0 115200 cs8 -cstopb -parenb -ixon -ixoff raw -echo
sudo bash -c 'printf "cd /mnt/extsd && ./sample_rtsp -path /mnt/extsd/sample_rtsp.conf > /mnt/extsd/run.log 2>&1 &\r\n" > /dev/ttyUSB0'
```

确认推流地址（IP 随 wlan0 DHCP 变）：
```bash
adb -s 4c48410491c30a72512 shell 'grep "rtsp://" /mnt/extsd/run.log'        # rtsp://<V821-IP>:8554/ch0
adb -s 4c48410491c30a72512 shell 'ip -4 addr show wlan0 | grep inet'          # 记下 V821 IP
```

> 详细步骤与排错见 [`edgefusion_vision/README.md`](edgefusion_vision/README.md)。

### 2. IMX6ULL 端：编译、配置、部署、启动

```bash
cd ~/EdgeFusion/edgefusion_gateway
# 编辑 config/gateway.conf，把 rtsp_url 改成上一步的 V821 地址，填入云端 token
#   rtsp_url          = rtsp://<V821-IP>:8554/ch0
#   anthropic_auth_token = sk-...

source toolchain.env && make && make deploy    # 交叉编译 + adb 部署到 /mnt/extsd/gateway/

adb -s 100ask_IMX6ULL shell 'mountpoint -q /mnt/extsd || mount -t vfat /dev/mmcblk0p1 /mnt/extsd'
adb -s 100ask_IMX6ULL shell 'killall edgefusion_gateway 2>/dev/null; sleep 1; \
  cd /mnt/extsd/gateway && \
  setsid sh -c "./edgefusion_gateway -path /mnt/extsd/gateway/config/gateway.conf > /mnt/extsd/gateway.log 2>&1" \
  </dev/null >/dev/null 2>&1 &'
```

> 详细步骤与排错见 [`edgefusion_gateway/README.md`](edgefusion_gateway/README.md)。

### 3. 验证

浏览器打开 **`http://192.168.1.59:8080`**（IMX6ULL IP）：
- **实时预览**：看到摄像头画面 + OSD（通道/REC/分辨率/码率/时间戳）
- **事件告警**：云端 AI 识别到的人脸/人员事件（带快照、置信度、描述）
- **录像回放**：60 秒分段的 MP4 列表
- **系统设置**：可热切换云端 AI 开关、改配置

```bash
curl -s http://192.168.1.59:8080/api/status    # 状态：stream/recording/cloud_ai
# RTSP 转发：rtsp://192.168.1.59:8554/track0（VLC 可拉）
```

---

## 板子访问速查

| 板子 | ADB 序列号 | 调试串口 | 默认 IP |
|------|-----------|----------|---------|
| IMX6ULL | `100ask_IMX6ULL` | `/dev/ttyACM0`（115200，免 sudo） | `192.168.1.59`（eth0 固定） |
| V821 | `4c48410491c30a72512` | `/dev/ttyUSB0`（115200，需 sudo） | DHCP 动态（wlan0，曾 .28/.44） |

> V821 走 wlan0 DHCP，**IP 每次上电可能变**，需重新查并更新 `gateway.conf` 的 `rtsp_url`（Web 设置页可热改）。

---

## 上游项目状态（截至上游最后一次更新）

| 阶段 | 状态 |
|------|------|
| P0 工程骨架 + 交叉编译 | ✅ |
| P1 RTSP 拉流 + MP4 分段录像 | ✅ |
| P2 关键帧上云（qwen3.6-plus 视觉分析） | ✅ 已验证 |
| P3 事件入库 + GPIO 报警订阅 | ✅ |
| P4 Web 预览/回放/事件/配置 + 云端 AI 开关 | ✅ |
| 本地 RTSP passthrough 转发 | ✅ |
| P5 MQTT 上报 | ⏳ 仅配置项，未实现（缺 libmosquitto） |
| 成本优化（V821 pdet 预筛选） | ⏳ 待办 |
| OTA / 多路接入 / 72h 稳定性 | ⏳ 待办 |

> 上表是**上游项目**在其自研硬件（V821 + IMX6ULL）上的状态，用于说明来源。
> 本仓库在 RK3576 单板上的实际状态与验证证据见 [docs/MY_WORK.md](docs/MY_WORK.md)。

---

## 文档导航

| 想了解 | 看这里 |
|--------|--------|
| **我的改造清单与验证边界** | [`docs/MY_WORK.md`](docs/MY_WORK.md) |
| **逐项改动的实现、验证命令与板端证据** | [`edgefusion_gateway/docs/RK3576_PORT.md`](edgefusion_gateway/docs/RK3576_PORT.md) |
| **面试自测问答卡**（自用） | [`docs/INTERVIEW_QA.md`](docs/INTERVIEW_QA.md) |
| 上游：项目全貌、架构、模块职责 | [`PROJECT_INTRO.md`](PROJECT_INTRO.md)（上游） |
| 上游：立项背景、硬件选型、实施计划 | [`IMX6ULL_V821_Project.md`](IMX6ULL_V821_Project.md)（上游） |
| 上游：V821 端编译/运行/排错 | [`edgefusion_vision/README.md`](edgefusion_vision/README.md)（上游） |
| 上游：IMX6ULL 端编译/运行/排错 | [`edgefusion_gateway/README.md`](edgefusion_gateway/README.md)（上游） |
| 上游：网关任务书与决策点 | [`edgefusion_gateway/docs/TASK.md`](edgefusion_gateway/docs/TASK.md)（上游） |

# RK3576 + USB 摄像头适配与验证

## 目标与来源

基于 dshanpi/EdgeFusion_Smart_NVR 二次开发，保留原作者署名及许可证。
原项目：V821 MIPI/全志 MPP 编码 → RTSP → IMX6ULL 网关。
本分支：DshanPi-A1 单板运行 USB 采集、软件 H.264 编码、录像和 Web 管理。

实际环境（用户于 2026-10-14 提供）：
- 100ASK Armbian 25.11.0-trunk noble，Ubuntu 24.04，aarch64。
- 内核 6.1.115-vendor-rk35xx。
- UVC Web Camera：/dev/video11 为 Video Capture，/dev/video12 为 Metadata Capture。
- MJPG：640×480 至 2560×1440，设备声明 30 fps；无 H.264 输出。
- YUYV：640×480 为 30 fps，1920×1080 为 5 fps；本版仅接入 MJPG。
- v4l2-ctl：640×480 MJPG 抓拍得到约 56 KB JPEG；300 帧采集后段约 30.06 fps。
- 上述结果仅证明独立采集可用，不代表网关录像/预览/NPU 性能。
- 已安装 FFmpeg 开发库（libavcodec 60.31.102），编码器列表包含 libx264。

## 第一版代码与原项目的区别

| 改动 | 原实现 | 本版实现 | 验证方式 |
|---|---|---|---|
| 视频源 | RTSP 输入 | source_type=rtsp/v4l2；保留 RTSP 路径 | USB 真机 + RTSP 单独回归 |
| USB 采集 | 无 | 自行调用 V4L2 ioctl、mmap、poll、DQBUF/QBUF | 抓拍、连续录像、拔插 |
| USB 编码 | 上游 V821 提供 H.264 | MJPEG 解码→格式转换→libx264 软件编码 | ffprobe 检查录像、测 CPU |
| 构建 | IMX6ULL 工具链/sysroot | 新增 Makefile.native，板端 gcc + pkg-config | 板端构建 |
| 录像时间戳 | 写死输入 1/90000 | 传递实际输入 time_base，每段归零 | 合成视频 1/1000 输入测试 |
| 码率统计 | muxer 接管 packet 后读取 size | 写入前保存 packet 大小 | 回归测试检查非零码率 |
| 重连 | 部分下游对象持续复用 | 关闭旧录像并重建采集/编解码/预览 | 拔插后产生可播新段 |
| 状态读取 | Web 读取共享对象指针 | 单个线程拥有处理对象，互斥锁保护值快照 | 并发查状态及拔插 |
| 退出 | 拉流阻塞可能阻止 join | RTSP 中断回调；USB 非阻塞读取和 200ms poll | 停流、断网时退出 |
| 预览 | 解码前创建转换器 | 根据实际解码帧延迟创建转换器；锁内固定 JPEG 长度 | 首帧、重连、并发抓图 |
| 观测 | 连接/录像状态 | 新增 source_type、frames、reconnects | /api/status |

编码后仍复用原来的解码预览流程，因此存在“编码后再次解码”的开销，尚未做性能优化。
本版没有应用层有界帧队列，也没有实现 MPP/RGA/NPU 或零拷贝。
V4L2 mmap 不等于整条链路零拷贝：当前明确复制 JPEG，确保 QBUF 后数据仍有效。

## 在开发板远程终端编译

所有下列命令都在通过 SSH 登录的开发板终端执行，无需桌面。
Ubuntu/Armbian 板端原生编译不需要 source 原 IMX6ULL 的 toolchain.env。

依赖（已安装则不用重复）：

~~~bash
sudo apt update && sudo apt install -y build-essential pkg-config ffmpeg libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev libcurl4-openssl-dev libssl-dev libsqlite3-dev libjson-c-dev zlib1g-dev
~~~

进入实际项目目录，以下假定仓库在 $HOME/Program 中：

~~~bash
cd "$HOME/Program/EdgeFusion_Smart_NVR/edgefusion_gateway" && make -f Makefile.native -j2 && make -f Makefile.native test
~~~

测试不占用摄像头。在 /tmp 下创建私有目录，生成两轮 H.264 视频，使用非零起点和 1/1000 时间基写入六个 MP4 分段。
检查首包关键帧、时间戳归零、DTS 递增、每段 30 包、180 包总数及非零码率。
成功时删除本测试产生的文件；失败时保留路径供排查。它不代替 USB/RTSP 真机测试。

## 启动与观察

配置：config/rk3576-usb.conf。第一版默认为 640×480、30 fps 请求值、1500 kbps、60 秒分段。
使用软件编码；默认关闭云端 AI、GPIO 和 RTSP 转发。

~~~bash
bash scripts/run-usb.sh
~~~

脚本切换到网关目录，创建 data/recordings、data/snapshots，并前台运行。
Ctrl+C 请求正常退出。再次启动仍使用同一配置和 data 目录。
前台运行验证期间，不要直接关闭 SSH 会话；后续再补 systemd 服务。
如果摄像头被 v4l2-ctl 或其他程序占用，先结束对应采集程序。

在第二个远程终端查询（需有 curl 命令）：

~~~bash
curl -s http://127.0.0.1:8080/api/status
~~~

在电脑浏览器访问 http://板子IP:8080，测试原项目的 MJPEG 预览和录像回放。
录像自动保存到板端 data/recordings，无需网页“保存”。Ctrl+C 会停止整个网关（采集、录像和 HTTP 服务），因此网页不能继续获取列表或回放，但已保存文件仍保留。
重新运行 scripts/run-usb.sh 后，在“录像回放”选择上一次运行的文件并点击“播放”；要在服务停止后观看，应先点击“下载”保存到电脑。
60 秒分段只在关键帧处切换，验证多个日常分段应连续运行超过两分钟。
frames 是累计送入下游的视频包数；reconnects 是重试次数，包含启动时打开失败的重试。
fps 是 USB 配置请求值（RTSP 为流声明值），不是性能实测值。
测处理吞吐应在稳定连接时用两次 frames 之差除以实际时间间隔。

USB 的捕获超时暂复用 rtsp_timeout_us（默认 5 秒）；重试间隔复用 rtsp_reconnect_s（默认 3 秒）。
USB 参数需要编辑配置并重启，尚未在原 Web 设置表单中增加 USB 输入控件。

## 板端验证记录

2026-10-14，用户通过开发板远程终端执行：
- 原生构建完成，FFmpeg/libx264 可链接。
- 录像回归测试通过：两轮共六段、180 个视频包；输入 1/1000 时间基和非零起点，段内时间戳归零、DTS 单调、码率非零。
- 首轮暴露的末包 duration 估算及写尾告警，在补充包时长和调整错误检查后复测未再出现。
- USB 短时集成测试通过（用户返回结果并已核对共享目录日志）：640×480 MJPEG → libx264 H.264 → MP4，HTTP 状态查询及正常退出通过。
- 测试目录：data/smoke-aDMnnX；两次状态快照的视频包数为 75 → 316，重试次数均为 0。脚本间隔约 8 秒，这可作接近 30 fps 的粗略观察，不是严格性能测量；状态 fps=30 仍表示请求值。
- 三个分段均通过 FFmpeg 解码检查，时长分别为 4.117367、3.989356、2.661333 秒。测试目标为 3 秒，但只在关键帧处分段；当前 GOP 为 60 帧，30 fps 下约 2 秒，因此完整段接近 4 秒，末段由停止时刻决定。
- 进程在脚本规定的 5 秒退出上限内正常退出；这不是精确退出时延测量。
- 用户后续确认：浏览器实时预览持续更新且主观流畅，录像列表可见。不能据此推断低延迟或长时间稳定性。
- 停止网关后，已核对 data/recordings/rec_20261014_124833_370352103_0.mp4 仍在，大小 5,933,389 字节；浏览器回放仍待用户验证。
- 本记录依据用户执行结果及实际保存的日志；真实拔插、RTSP 回归和长期稳定性尚未验证。

真实摄像头的短时集成测试（需要 curl、ffmpeg、ffprobe、python3）：

~~~bash
make -f Makefile.native -j2 && bash scripts/smoke-usb.sh
~~~

脚本使用 /dev/video11、127.0.0.1:18080，在 data/smoke-XXXXXX 下生成独立配置、日志和录像。
约十几秒后自动停止测试进程，验证状态包数增长、无重连、至少两个分段及所有录像可解码。
不会删除录像；不会修改日常运行配置。超出 5 秒未正常退出会标记失败并结束本次测试进程。
节点变化时可传入实际采集路径：bash scripts/smoke-usb.sh /dev/v4l/by-id/实际名称。
它尚未验证浏览器预览、真实拔插恢复或长时间稳定性。

## 真机验收清单（尚需逐项记录）

1. 【已通过】编译和无摄像头录像回归测试。
2. 【已通过】短时测试 source_type=v4l2、connected=true、frames 增长、录像码率非零。
3. 【短时通过】3 秒分段配置下产生三段，全部可解码；60 秒日常配置仍需验证。
4. 浏览器预览与录像并行，记录实际帧率、CPU/RSS，而非直接引用摄像头的 30 fps。
5. 拔出再插入 USB，记录重连耗时和新录像段可播放情况。
6. 稳定运行和摄像头断开时分别 Ctrl+C，观察退出耗时及再次启动是否可用。
7. RTSP 输入与 RTSP 转发分别回归，不因保留代码而标为已验证。

USB 节点编号在拔插/重启后可能变化。建议先执行：

~~~bash
ls -l /dev/v4l/by-id/
~~~

把其中指向采集节点（本次为 video11）的完整稳定路径填入 video_device。
若系统没有该目录，则用 v4l2-ctl --list-devices 重新确认；程序不会猜测并自动切到其他设备。

分段使用 fragmented MP4，能降低部分中断场景的数据损失，但不能承诺断电完全无损。
末尾未完成片段、内核页缓存和存储设备缓存仍可能造成丢失，需要另行测试。

## 后续优化按顺序推进

1. 有界队列与线程解耦：明确帧所有权、队列上限、背压/丢帧规则及退出唤醒，记录改造前后延迟和内存上限。
2. 原始 MJPEG 直接供预览：避免 H.264 编码后再解码生成 JPEG，比较 CPU 与预览延迟。
3. 慢客户端隔离：原 Web/RTSP 服务仍需完善短写、请求解析、连接资源及并发管理；先复现再修改。
4. Rockchip MPP 硬编码：检查板端驱动/库，使用相同分辨率、帧率和码率比较软件编码基线。
5. 移动检测或 RKNN 人员检测：复用事件数据库，增加冷却/去重规则；不同时开展多个模型。
6. systemd、磁盘写入失败恢复、长期运行及性能报告。

## 面试与贡献记录

每项改动保留：问题现象、代码位置、根因、修复方法、验证日志和局限。
可介绍为“基于开源项目完成 RK3576/USB 接入和应用层改造”，明确 Web/数据库基础来自原项目。
只有实际验证后，才在简历写“完成适配”“稳定运行 X 小时”“CPU 降低 X%”。
本轮新增代码由助手协助编写；学习时逐步解释 V4L2 缓冲区所有权、编解码数据流、重连清理顺序和锁的作用。

参考：
- https://github.com/dshanpi/EdgeFusion_Smart_NVR
- https://wiki.dshanpi.org/docs/DshanPi-A1/intro/
- https://docs.kernel.org/userspace-api/media/v4l/mmap.html
- https://ffmpeg.org/doxygen/6.1/group__lavc__encdec.html
- https://ffmpeg.org/doxygen/6.1/group__lavf__encoding.html

## 像素格式告警修复（待板端验证）

USB MJPEG 解码与 JPEG 预览原先直接给 swscale 传 YUVJ 格式，会打印 deprecated pixel format 告警。新增 image_scale.h：只把 scaler 输入/输出格式规范成布局相同的 YUV，用 color_range 或 YUVJ 标记显式设定 full/limited range；编码器和帧也设置对应范围元数据。复用转换上下文，保留现有颜色矩阵，不以关闭日志代替修复。

~~~bash
make -f Makefile.native -j2 && make -f Makefile.native test-color && bash scripts/smoke-usb.sh
~~~

test-color 检查全范围 0–255 与有限范围 16–235 的黑白电平转换、同参数上下文复用，以及不出现旧格式告警。USB 真机录像/预览仍须复测。

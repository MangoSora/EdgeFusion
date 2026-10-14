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

## 改动一：色彩范围与弃用告警修复（已板端验证）

USB MJPEG 解码与 JPEG 预览原先直接给 swscale 传 YUVJ 格式，会打印 deprecated pixel format 告警。
第一版新增 `image_scale.h` 把交给 scaler 的格式规范成布局相同的 YUV。**板端验证证明这一版只修好了
告警、没修好电平转换**：

| 项目 | 板端结果 |
|---|---|
| 弃用告警 | 已消失：新一轮 smoke 的 `gateway.log` 中 0 条（修复前同目录历史日志 369 条） |
| `make test-color` | **FAIL line 42**：黑白电平断言不成立 |

诊断（同一结论也写进了 `src/image_scale.h` 的注释）：用 5 组配置直接探测 libswscale 7.5.100（FFmpeg 6.1）：

~~~
YUVJ420P UNSPEC -> limited   black=  0(exp 16) white=255(exp 235) MISMATCH
YUV420P  MPEG   -> full      black= 16(exp  0) white=235(exp 255) MISMATCH
YUV420P  JPEG   -> limited   black=  0(exp 16) white=255(exp 235) MISMATCH
YUV420P  MPEG   -> limited   black= 16(exp 16) white=235(exp 235) OK
YUVJ422P UNSPEC -> limited   black= 16(exp 16) white=235(exp 235) OK
~~~

规律：**scaler 输入输出像素格式相同（含 YUVJ→YUV 归一化后相同）时，`sws_scale()` 完全不执行
srcRange↔dstRange 的电平转换**；只有格式真的不同（422→420）才顺带转换。上一版把范围转换交给
swscale，于是在同格式路径上静默失效——依赖库的内部快速路径属于未定义行为。

修复内容：

- `src/image_scale.h` 新增 `image_range_convert()`：**显式**做 YUV 电平转换（Y 16–235 ↔ 0–255，
  色度 128±112 ↔ 128±127），查表、就地；范围相同时是空操作。
- `image_scale_prepare()` 改为告诉 swscale「进出一致」：它只做格式/尺寸转换，电平由调用方负责，
  行为不再依赖 swscale 选哪条内部路径。
- `usb_source.c`：相机 JPEG 本来就是全范围，编码器也标记全范围（`AVCOL_RANGE_JPEG`），
  端到端保持一致，省掉一次有损往返。
- `frame_grabber.c`：预览按来源实际范围判断（JPEG 标记=全范围；MPEG 或未标注=限范围），
  限范围来源在编码 JPEG 前显式提升到全范围。
- `tests/test_image_scale.c` 重写：分别验证电平转换（两个方向 + 空操作）、端到端四条链路、
  上下文复用，以及不出现弃用告警。

板端结果（`make -f Makefile.native test-color`，rc=0）：

~~~
range_convert full=1->0  Y   0-> 16(exp  16)  U   0-> 16(exp  16)
range_convert full=1->0  Y 255->235(exp 235)  U 255->240(exp 240)
range_convert full=0->1  Y  16->  0(exp   0)  U 128->128(exp 128)
range_convert full=0->1  Y 235->255(exp 255)  U 240->255(exp 255)
range_convert full=0->0  Y 123->123(exp 123)  U  77-> 77(exp  77)
pipeline YUVJ420P full -> full            black=  0(exp   0) white=255(exp 255) OK
pipeline YUVJ422P full -> full            black=  0(exp   0) white=255(exp 255) OK
pipeline YUV420P limited -> full          black=  0(exp   0) white=255(exp 255) OK
pipeline YUV420P full -> full             black=  0(exp   0) white=255(exp 255) OK
context reuse OK: same sws pointer across 11 configures
PASS: range convert levels, cached scaler reuse, no deprecated-format warnings
~~~

局限：以上是合成图案 + 真机冒烟的结果；不同相机／不同 JPEG 色度采样下的观感差异仍需人眼确认。

## 改动二：预览/转发下到独立线程 + 有界队列（待板端验证）

### 问题

原实现是单线程串行：采集线程依次做 `usb_source_read()`（V4L2 取帧 + MJPEG 解码 + x264 编码）、
`recorder_write_packet()`（写盘）、`frame_grabber_feed()`（H.264 解码 + JPEG 编码）、
`rtsp_server_on_packet()`（向每个 RTSP 客户端发送）。
任一环节变慢都会拖住采集：V4L2 的 4 个缓冲区耗尽后由驱动层丢帧，应用侧既看不到计数，
也无法区分“摄像头没给帧”和“自己处理不过来”。

### 改动

- 新增 `src/pkt_queue.[ch]`：单生产者/单消费者有界包队列，容量 8 个 H.264 包（约 0.27 秒 @30fps）。
  队列满时**丢最旧**并计数；`push()` 无论入队还是丢弃都消费掉传入的包引用，`pop()` 返回的包归消费者所有，
  `stop()` 唤醒阻塞中的消费者且已入队的包仍会被取完。
- `src/stream_receiver.c`：新增预览线程，从队列取包后执行 `frame_grabber_feed()` 与
  `rtsp_server_on_packet()`；采集线程只做“取帧 → 编码 → 录像 → 入队”（`av_packet_ref()` 共享引用，不复制负载）。
- `/api/status` 的 `stream` 增加 `preview_queue:{depth,high,dropped,processed}`，队列指标可直接从 HTTP 读。
- 队列或线程创建失败时退回内联处理，不改变原有可用性。
- 新增 `tests/test_pkt_queue.c`（有界性、丢最旧顺序、停止唤醒、停止后仍排空）与 `scripts/measure-run.sh`
  （按秒采样 CPU/RSS/fd/帧率/队列指标）。

### 边界（明确没做的部分）

- 录像仍在采集线程内写盘，尚未独立成线程；RTSP 慢客户端隔离（每客户端队列、超时踢出）未做。
- 队列只按包数限界，没有字节上限；没有零拷贝、内存池或无锁结构。

### 验证方法

~~~bash
make -f Makefile.native -j2 && make -f Makefile.native test
bash scripts/smoke-usb.sh
bash scripts/measure-run.sh 60
~~~

改造前后各跑一次 `measure-run.sh 60`（相同分辨率、码率、预览帧率），对比 CPU 均值/峰值、RSS 峰值、
平均帧率与退出时延。压力验证：把 `web_mjpeg_fps` 提到 30 或人为放慢预览，观察 `frames_per_s` 是否保持、
`preview_queue.dropped` 是否增长、RSS 是否有界、退出是否仍在 5 秒内。

### 离线校验（已完成，非板端）

队列逻辑先在本机（MinGW + 最小 `AVPacket` 桩，**不是** FFmpeg）编译运行过一遍：

~~~bash
gcc -std=gnu11 -Wall -Wextra -I . -I stub pkt_queue.c test_pkt_queue.c stub/stub_avcodec.c -o qt -pthread
./qt
~~~

结果：`-Wall -Wextra` 零告警，5 个用例全部 PASS——

1. 有界丢最旧：容量 4 推 10 个，留下最后 4 个，`dropped=6`、`high_watermark=4`；
2. `stop()` 唤醒阻塞中的消费者（退出不卡死）；
3. `stop()` 之后已入队的包仍被取完；
4. 队列里还有包时 `destroy()` 正常释放；
5. 5 万包生产者/消费者并发：取出顺序单调、`consumed + dropped == pushed`。

局限：桩只替代 `AVPacket` 的分配/释放，**不覆盖真实 FFmpeg 语义与整机集成**，
板端仍须用 `make -f Makefile.native test` 复跑并记录结果。

### 板端结果（已通过）

~~~
$ make -f Makefile.native test          # rc=0
PASS: 1/1000 time base, timestamp reset, six playable segments, bitrate and reconnect sessions
PASS: range convert levels, cached scaler reuse, no deprecated-format warnings
PASS: bounded queue keeps the newest packets and drops the oldest
PASS: stop() woke the blocked consumer in 0 ms
PASS: queued packets are drained after stop()
PASS: destroy() with pending packets returns cleanly
PASS: concurrent producer/consumer kept order: pushed=50000 dropped=402 consumed=49598
PASS: pkt_queue bounded drop, ownership, stop wakeup, drain and concurrency
~~~

冒烟的队列断言也通过：`preview_queue:{depth:1,high:1,dropped:0,processed:227}` —— 预览线程确实在
消费且零丢包。改造前后各 60 秒、同分辨率/码率/预览帧率的对比（`scripts/measure-run.sh 60`）：

| 指标 | 改造前（d1325fa） | 改造后 |
|---|---|---|
| CPU 均值 / 峰值 | 43.4% / 59.0% | 44.0% / 73.0% |
| RSS 首 / 末 / 峰 (kB) | 47468 / 48484 / 48484 | 42720 / 45412 / 45412 |
| 帧率（包/秒） | 23.93 | 24.80 |
| 文件描述符峰值 | 7 | 7 |
| 退出时延 | < 1 s | < 1 s |
| 预览丢包 | 无此指标 | 0 |

结论要诚实：**CPU 均值与改造前基本持平（差异落在运行间波动范围内），RSS 峰值低约 3 MB，
吞吐 +3.6%**。这次改动的主要收益是结构性的——采集不再被预览/转发拖住、队列有界且可观测、
退出有明确的唤醒路径——而不是 CPU 数字上的大幅下降。软件编码（MJPEG 解码 + x264）仍是主要
开销，量级上的性能提升要等 MPP 硬件编码。

### 验收标准（待板端逐项记录）

1. `test_pkt_queue` 通过：有界性、丢最旧顺序、停止唤醒、停止后仍排空。
2. `smoke-usb.sh` 通过：预览线程 `processed > 0`、正常负载 `dropped == 0`、无 `deprecated pixel format` 告警。
3. 与改造前同参数对比：平均帧率不下降，CPU 变化有解释，RSS 峰值有界。
4. 压力场景下队列深度达到上限、`dropped` 增长，但录像分段仍可解码、退出仍在 5 秒内。

## 改动三：预览直通相机 MJPEG（待板端验证）

### 问题

预览原先走的是“相机 MJPEG → 解码 → x264 编码 → 再从 H.264 解码 → 编码 JPEG”的回路：
`frame_grabber_feed()` 每收到一个 H.264 包就解码一次，再缩放、再编码成 JPEG。
预览只是要看画面，中间这一整轮编解码是纯开销（30fps 输入时约 30 次 H.264 解码/秒）。

### 改动

- `src/usb_source.[ch]`：新增原始 JPEG 回调（`usb_source_set_jpeg_cb()`），在**解码前**把相机
  那一帧 JPEG 交给订阅者；回调在采集线程内同步执行，自身不做解码。
- `src/frame_grabber.[ch]`：新增 `frame_grabber_submit_jpeg()`，只做帧率节流 + 缓存（一次 `memcpy`），
  并把原来内联的节流逻辑提取成 `throttle_allow()`，两条产出路径共用同一时间基准。
- `src/stream_receiver.c`：USB 模式下注册回调并令预览线程不再解码 H.264（`decode_preview=0`），
  预览线程只负责 RTSP 转发；RTSP 输入（`source_type=rtsp`）没有原始 JPEG，仍走解码路径。
- `/api/status` 增加 `preview_raw_jpeg`，并新增日志字段 `jpeg camera-passthrough|h264-decode`。

### 行为变化与边界

- 预览分辨率 = `capture_width × capture_height`（不再套用 `ai_frame_width` 缩放）；
  当前配置两者都是 640，因此默认配置下无可见差异。
- 云端 AI 取到的快照同样是相机原始 JPEG（本版 `ai_enable=0`，未启用）。
- 队列不可用时的内联退路也遵循同一判断，不会出现“直通 + 解码”两条路同时喂图。

### 验证方法

~~~bash
make -f Makefile.native -j2 && bash scripts/smoke-usb.sh   # 断言 preview_raw_jpeg=true
bash scripts/measure-run.sh 60                              # 与改动二同参数对比 CPU/RSS
~~~

浏览器端确认预览仍持续更新。对比口径：相同分辨率/码率/预览帧率下，改动三的 CPU 均值应低于改动二，
RSS 不增长，退出时延仍在 5 秒内。

### 验收标准（待板端逐项记录）

### 板端结果（已通过）

`bash scripts/smoke-usb.sh`（rc=0）相关输出：

~~~
PASS: async preview consumed 227 packets, dropped 0
PASS: preview fed from camera MJPEG (no H.264 re-decode)
PASS: no deprecated pixel format warnings (0 found)
~~~

`/api/status` 实测：`"preview_queue":{"depth":1,"high":1,"dropped":0,"processed":227}`、
`"preview_raw_jpeg":true`；60 秒采样里预览线程处理 1487 个包、采集计数 1488 个包，两者一致，
说明预览没有落后于采集。

边界（已实测确认）：预览分辨率等于采集分辨率（640×480），USB 模式下 `ai_frame_width` 不再生效；
RTSP 输入仍走 H.264 解码预览（该场景下 `preview_raw_jpeg=false`）。

### 验收标准（原计划）

1. `/api/status`：`preview_raw_jpeg=true`、`preview_queue.processed>0`、`dropped==0`。
2. 浏览器预览持续更新，画面无异常（单一 JPEG 源，不再经过 H.264 回路）。
3. 与改动二对比：CPU 均值下降（幅度以实测为准），RSS 与退出时延不劣化。

## 慢客户端隔离：复现方案与代码位置（尚未修改）

“先复现再修改”的复现材料已经准备好，代码位置都已核对：

| 位置 | 事实 |
|---|---|
| `src/rtsp_server.c:183-192` | `send_all()` 是阻塞写循环，控制信令与 RTP 数据都用它 |
| `src/rtsp_server.c:374-375` | 客户端套接字设置 `SO_SNDTIMEO = 2s`，单次卡住上限 2 秒 |
| `src/rtsp_server.c:300-322` | `rtsp_server_on_packet()` **持 `g_mtx`** 遍历客户端逐个发送 |
| `src/rtsp_server.c:576-585` | `rtsp_server_client_count()` 需要同一把锁，而 `/api/status` 会调用它 |
| `src/rtsp_server.c:519-523` | 新客户端接入（加入链表）也需要同一把锁 |

推论（待板端确认）：一个“连上但不读数据”的客户端会让发送方在锁内阻塞，连带拖慢
`/api/status` 与新客户端接入；预览队列会因此丢包（`preview_queue.dropped` 增长），
但采集与录像不受影响——后者正是“改动二”要验证的隔离效果。

复现方法（需要 `rtsp_server_enable = 1`）：

~~~bash
python3 scripts/slow-rtsp-client.py            # 默认 127.0.0.1:8554 卡住 15 秒
python3 scripts/slow-rtsp-client.py 127.0.0.1 8554 30
~~~

脚本只做观察：建立 RTSP/TCP interleaved 会话后停止读取，期间探测 `/api/status` 耗时，
恢复读取后统计积压字节，并打印 `preview_queue` 指标。判定标准是卡住期间的
`/api/status` 耗时是否显著高于基线（脚本会打印 `REPRODUCED` / `NOT REPRODUCED`）。

修复方向（复现后再动手）：锁只保护客户端链表、不覆盖发送；每客户端独立发送队列与非阻塞写；
队列满时丢帧或踢出慢客户端；补空闲超时（当前 SDP 只声明 `timeout=60`，服务端并不执行）。

### 离线校验（已完成，非板端）

复现工具本身先在本机用模拟网关验证过——`tests/mock_gateway.py` 是一个最小 RTSP 服务端
加上“有停读客户端时状态接口就变慢”的 HTTP 端：

~~~bash
python3 tests/mock_gateway.py
~~~

结果：握手（OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN）全部走通；PLAY 请求带上了解析出来的
`Session` 头；`$` interleaved 帧被正确跳过（否则 TEARDOWN 不会到达服务端）；判定输出

~~~
REPRODUCED: /api/status 从 0.020s 恶化到 2.003s （102x）
~~~

细看时序：基线 0.001–0.056 s → 卡住期间 2.003 s → 恢复读取后回到 0.001–0.018 s，
期间读出约 1.19 MB 积压数据，末尾 `MOCK CHECK: PASS`。

局限：模拟网关只复刻了“发送阻塞连带拖慢状态接口”这一条因果链，**不能替代真机复现**；
板端仍须在 `rtsp_server_enable = 1` 下运行 `scripts/slow-rtsp-client.py` 并记录结果。

## 改动四：systemd 服务化与长期采样（待板端验证）

### 问题

此前是 SSH 前台运行（见上文“启动与观察”），SSH 一断进程就跟随 SIGHUP 结束；没有开机自启，
异常退出也不会重启；长期运行需要的 CPU / RSS / 文件描述符 / 重连数据也没有采集手段。

### 改动

- `deploy/edgefusion-gateway.service`：`Type=simple`，`KillSignal=SIGINT`（复用 `src/main.c` 已有的
  优雅退出路径，而不是 SIGTERM 直接杀），`Restart=on-failure` + `RestartSec=3`；
  `StartLimitIntervalSec` / `StartLimitBurst` 放在 `[Unit]`（systemd 229+ 的位置要求）；
  `SupplementaryGroups=video` 取得 UVC 节点权限；日志进 journald；
  `@USER@` / `@DIR@` 由安装脚本替换，因此整个仓库可以整体搬路径。
- `scripts/install-service.sh`：幂等安装（重复执行即更新单元），`--uninstall` 停止并移除单元，
  不动工程文件、录像与数据目录。
- `scripts/monitor-run.sh`：只采样**已在运行**的进程——CPU%、RSS、文件描述符数、帧率、队列深度、
  预览丢包、预览处理量、重连次数，按秒写 CSV 并打印汇总。它不启停网关，所以能挂在 24–72 小时
  稳定性测试旁边；与 `measure-run.sh`（自己启停网关、用于定点前后对比）分工不同。

### 验证方法

~~~bash
bash scripts/install-service.sh
systemctl --no-pager status edgefusion-gateway
curl -s http://127.0.0.1:8080/api/status
journalctl -u edgefusion-gateway -n 30 --no-pager
bash scripts/monitor-run.sh 5            # 另开一个终端采样，Ctrl+C 结束
sudo systemctl show -p NRestarts edgefusion-gateway
sudo systemctl stop edgefusion-gateway   # 记录退出耗时，应 ≤ 5 秒
bash scripts/install-service.sh --uninstall
~~~

### 验收标准（待板端逐项记录）

1. 安装后服务 `active`，`/api/status` 可访问，`journalctl` 能看到启动横幅。
2. `systemctl stop` 在 5 秒内退出（SIGINT → 主循环收尾），`NRestarts` 不增长。
3. 重启开发板后服务自动启动。
4. 摄像头拔出：进程不退出，`reconnects` 增长而 `NRestarts` 保持 0
   （证明重连发生在应用层，而不是靠 systemd 反复重启掩盖问题）。
5. 长期运行 24–72 小时：用 `monitor-run.sh` 记录 CPU 均值/峰值、RSS 首末与峰值、fd 首末、
   平均帧率、预览丢包与重连次数，形成表格后再回填本文档。

说明：本节的文件与流程已就绪，但**尚未在开发板上执行过**，各项结论以待板端结果为准。


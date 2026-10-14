#ifndef EDGEFUSION_RTSP_SERVER_H
#define EDGEFUSION_RTSP_SERVER_H

#include "libavcodec/avcodec.h"
#include "libavutil/rational.h"

/* rtsp_server: 自研轻量 RTSP server，把 stream_receiver 拉到的 H.264 包
 * 重新打包成 RTP 直接转发（passthrough，不转码不解码），供 VLC/ffmpeg/NVR 拉流预览。
 * 支持 TCP interleaved + UDP 两种传输。 */

/* 初始化：从 codecpar 提取 SPS/PPS 生成 SDP，启动监听线程。成功返回 0 */
int  rtsp_server_init(int port, const AVCodecParameters *codecpar);
void rtsp_server_deinit(void);

/* stream_receiver 每收到一个视频包调用：切 NALU → 打包 RTP → 放入各客户端发送队列。
 * 锁内不做 socket I/O：慢客户端只会让它自己的队列溢出，不阻塞推流与状态查询。 */
void rtsp_server_on_packet(const AVPacket *pkt, const AVRational *time_base);

/* 当前正在拉流的 RTSP 预览客户端数 */
int  rtsp_server_client_count(void);

/* 运行统计：累计踢出的慢客户端数、观测到的最大单客户端积压(KB) */
int  rtsp_server_slow_count(void);
int  rtsp_server_max_queued_kb(void);

#endif

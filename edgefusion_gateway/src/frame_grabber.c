#include "frame_grabber.h"
#include "log.h"
#include "image_scale.h"

#include "libavcodec/avcodec.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libswscale/swscale.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static AVCodecContext    *g_dec = NULL;
static AVCodecContext    *g_enc = NULL;        /* MJPEG 编码器 */
static struct SwsContext *g_sws = NULL;
static AVFrame           *g_dec_frame = NULL;  /* 解码输出帧 */
static AVFrame           *g_dst_frame = NULL;  /* 缩放后帧(YUVJ420P) */
static int                g_dst_w = 0, g_dst_h = 0;

static frame_grabber_cfg_t g_cfg;

/* 最新 JPEG 帧缓存 + 锁 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_jpeg = NULL;
static int       g_jpeg_size = 0;

/* 帧率节流 */
static int64_t g_last_emit_us = 0;

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int frame_grabber_init(const frame_grabber_cfg_t *cfg, AVCodecParameters *codecpar)
{
    pthread_mutex_lock(&g_lock);
    g_cfg = *cfg;
    pthread_mutex_unlock(&g_lock);
    g_last_emit_us = 0;

    /* 解码器 */
    const AVCodec *dec = avcodec_find_decoder(codecpar->codec_id);
    if (!dec) { LOG_ERR("frame_grabber: 找不到解码器 %d", codecpar->codec_id); return -1; }
    g_dec = avcodec_alloc_context3(dec);
    if (!g_dec) return -1;
    if (avcodec_parameters_to_context(g_dec, codecpar) < 0) { LOG_ERR("参数到上下文失败"); goto fail; }
    g_dec->flags2 |= AV_CODEC_FLAG2_FAST;
    if (avcodec_open2(g_dec, dec, NULL) < 0) { LOG_ERR("解码器打开失败"); goto fail; }

    g_dec_frame = av_frame_alloc();
    g_dst_frame = av_frame_alloc();
    if (!g_dec_frame || !g_dst_frame) goto fail;

    /* 输出尺寸 */
    g_dst_w = cfg->out_width > 0 ? cfg->out_width : codecpar->width;
    /* 高度按比例(对齐到2) */
    if (cfg->out_width > 0 && codecpar->width > 0) {
        g_dst_h = (codecpar->height * cfg->out_width / codecpar->width) & ~1;
    } else {
        g_dst_h = codecpar->height;
    }
    if (g_dst_h <= 0) g_dst_h = 480;

    /* MJPEG 编码器 */
    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!enc) { LOG_ERR("找不到 MJPEG 编码器"); goto fail; }
    g_enc = avcodec_alloc_context3(enc);
    if (!g_enc) goto fail;
    g_enc->width = g_dst_w;
    g_enc->height = g_dst_h;
    g_enc->pix_fmt = AV_PIX_FMT_YUVJ420P;
    g_enc->color_range = AVCOL_RANGE_JPEG;
    g_enc->time_base = (AVRational){1, 25};
    /* JPEG 质量通过 qmin/qmax 控制（取值 1-31，越小质量越高；映射 quality->qscale） */
    int qscale = 31 - (cfg->jpeg_quality * 30 / 100);
    if (qscale < 1) qscale = 1;
    if (qscale > 31) qscale = 31;
    g_enc->qmin = qscale;
    g_enc->qmax = qscale;
    AVDictionary *opts = NULL;
    if (avcodec_open2(g_enc, enc, &opts) < 0) {
        char err[128] = {0}; av_strerror(errno, err, sizeof(err));
        LOG_ERR("MJPEG 编码器打开失败 (errno:%s)", err);
        av_dict_free(&opts); goto fail;
    }
    av_dict_free(&opts);

    /* Decoder pixel format may be unknown before the first frame.
     * Initialize/reconfigure swscale from the decoded frame in emit_jpeg. */

    /* 给目标帧分配 buffer */
    if (av_image_alloc(g_dst_frame->data, g_dst_frame->linesize,
                       g_dst_w, g_dst_h, AV_PIX_FMT_YUVJ420P, 32) < 0) {
        LOG_ERR("av_image_alloc 失败"); goto fail;
    }
    g_dst_frame->width = g_dst_w;
    g_dst_frame->height = g_dst_h;
    g_dst_frame->format = AV_PIX_FMT_YUVJ420P;
    g_dst_frame->color_range = AVCOL_RANGE_JPEG;

    LOG_INF("frame_grabber 就绪: %dx%d -> %dx%d JPEG q=%d fps=%d",
            codecpar->width, codecpar->height, g_dst_w, g_dst_h, cfg->jpeg_quality, cfg->target_fps);
    return 0;
fail:
    frame_grabber_deinit();
    return -1;
}

void frame_grabber_deinit(void)
{
    if (g_dec) avcodec_free_context(&g_dec);
    if (g_enc) avcodec_free_context(&g_enc);
    if (g_sws) sws_freeContext(g_sws), g_sws = NULL;
    if (g_dec_frame) av_frame_free(&g_dec_frame);
    if (g_dst_frame) {
        if (g_dst_frame->data[0]) av_freep(&g_dst_frame->data[0]);
        av_frame_free(&g_dst_frame);
    }
    pthread_mutex_lock(&g_lock);
    if (g_jpeg) free(g_jpeg), g_jpeg = NULL;
    g_jpeg_size = 0;
    pthread_mutex_unlock(&g_lock);
}

/* 帧率节流：两条产出路径（H.264 解码 / 相机原始 JPEG）共用同一时间基准 */
static int throttle_allow(void)
{
    pthread_mutex_lock(&g_lock);
    int target_fps = g_cfg.target_fps;
    pthread_mutex_unlock(&g_lock);
    if (target_fps <= 0) return 1;
    int64_t interval = 1000000 / target_fps;
    int64_t now = now_us();
    if (now - g_last_emit_us < interval) return 0;
    g_last_emit_us = now;
    return 1;
}

/* 处理一个已解码帧：缩放+编码JPEG+缓存（带帧率节流） */
static void emit_jpeg_from_frame(void)
{
    if (!throttle_allow()) return;

    /* 解码帧的实际范围：显式标记 JPEG=全范围；MPEG 或未标注（H.264 默认）按限范围处理。
     * MJPEG 编码器按 YUVJ（全范围）解释数据，所以限范围来源必须显式转换——
     * swscale 在输入输出格式相同时不会做电平转换（见 image_scale.h 说明）。 */
    int src_full = (g_dec_frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    if (image_scale_prepare(&g_sws, g_dec_frame, g_dst_w, g_dst_h,
                            AV_PIX_FMT_YUV420P, src_full) < 0) {
        LOG_WRN("Cannot configure JPEG preview color range");
        return;
    }
    /* 缩放 */
    sws_scale(g_sws, (const uint8_t *const *)g_dec_frame->data, g_dec_frame->linesize,
              0, g_dec_frame->height, g_dst_frame->data, g_dst_frame->linesize);
    image_range_convert(g_dst_frame, src_full, 1);   /* 统一成 JPEG 的全范围 */
    g_dst_frame->pts = g_dec_frame->pts;

    /* 编码 JPEG */
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return;
    if (avcodec_send_frame(g_enc, g_dst_frame) < 0) { av_packet_free(&pkt); return; }
    if (avcodec_receive_packet(g_enc, pkt) < 0) { av_packet_free(&pkt); return; }

    /* 缓存最新帧 */
    pthread_mutex_lock(&g_lock);
    uint8_t *nb = realloc(g_jpeg, pkt->size);
    if (nb) {
        g_jpeg = nb;
        memcpy(g_jpeg, pkt->data, pkt->size);
        g_jpeg_size = pkt->size;
    }
    pthread_mutex_unlock(&g_lock);

    av_packet_free(&pkt);
}

/* 相机原始 MJPEG 直通：只节流 + 缓存，不做任何编解码。
 * 与解码路径互斥使用：USB 模式用本函数，RTSP 模式用 frame_grabber_feed()。 */
int frame_grabber_submit_jpeg(const uint8_t *data, int size)
{
    if (!data || size <= 0) return -1;
    if (!throttle_allow()) return 0;      /* 被帧率节流，不是错误 */

    pthread_mutex_lock(&g_lock);
    uint8_t *nb = realloc(g_jpeg, (size_t)size);
    if (nb) {
        g_jpeg = nb;
        memcpy(g_jpeg, data, (size_t)size);
        g_jpeg_size = size;
    }
    pthread_mutex_unlock(&g_lock);
    return nb ? size : -1;
}

int frame_grabber_feed(AVPacket *pkt)
{
    if (!g_dec) return 0;
    int ret = avcodec_send_packet(g_dec, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        return 0; /* 偶发坏包，忽略 */
    }
    /* 取出所有可解码帧 */
    while (1) {
        ret = avcodec_receive_frame(g_dec, g_dec_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;
        emit_jpeg_from_frame();
    }
    return 0;
}

int frame_grabber_get_jpeg(uint8_t **out_data)
{
    if (!out_data) return 0;
    *out_data = NULL;
    pthread_mutex_lock(&g_lock);
    if (!g_jpeg || g_jpeg_size <= 0) {
        pthread_mutex_unlock(&g_lock);
        *out_data = NULL;
        return 0;
    }
    uint8_t *buf = malloc(g_jpeg_size);
    if (!buf) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    int size = g_jpeg_size;
    memcpy(buf, g_jpeg, size);
    pthread_mutex_unlock(&g_lock);
    *out_data = buf;
    return size;
}

void frame_grabber_set_target_fps(int fps)
{
    pthread_mutex_lock(&g_lock);
    g_cfg.target_fps = fps;
    pthread_mutex_unlock(&g_lock);
}

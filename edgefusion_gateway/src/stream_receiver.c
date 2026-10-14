#include "stream_receiver.h"
#include "usb_source.h"
#include "pkt_queue.h"
#include "log.h"
#include "frame_grabber.h"
#include "rtsp_server.h"

#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_t g_tid;
static atomic_int g_running;
static atomic_int g_segment_seconds;
static gateway_conf_t g_cfg;
static pthread_mutex_t g_status_lock = PTHREAD_MUTEX_INITIALIZER;
static stream_status_t g_status;

/* 预览/转发队列（见 pkt_queue.h）。8 个包在 30fps 下约 0.27 秒缓冲：
 * 足以吸收磁盘与网络的短抖动，又不会让预览明显偏离实时。 */
#define PREVIEW_QUEUE_MAX_PACKETS 8
static pkt_queue_t  *g_preview_queue;      /* 指针仅由采集线程读写，供状态上报取统计 */
static atomic_ullong g_preview_processed;  /* 预览线程累计处理的包数 */
static int            g_preview_raw_jpeg;  /* 1=预览直通相机 MJPEG（USB 模式且预览可用） */

/* Only the receiver thread touches FFmpeg/V4L2/recorder objects.
 * Web reads a value snapshot, never a pointer that reconnect can free. */
static void publish_status(const AVCodecParameters *cp, int fps, recorder_t *rec,
                           unsigned long long frames, unsigned reconnects)
{
    stream_status_t st = {0};
    snprintf(st.source_type, sizeof(st.source_type), "%s", g_cfg.source_type);
    st.connected = cp != NULL;
    st.frames = frames;
    st.reconnects = reconnects;
    st.preview_processed = atomic_load(&g_preview_processed);
    st.preview_raw_jpeg = g_preview_raw_jpeg;
    if (g_preview_queue) {
        pkt_queue_stats_t qs;
        pkt_queue_get_stats(g_preview_queue, &qs);
        st.preview_queue_depth = qs.depth;
        st.preview_queue_high = qs.high_watermark;
        st.preview_dropped = qs.dropped;
    }
    if (cp) { st.width = cp->width; st.height = cp->height; st.fps = fps; }
    if (rec) {
        recorder_status_t rs;
        if (!recorder_get_status(rec, &rs)) {
            st.recording = rs.recording;
            st.bitrate_kbps = rs.bitrate_kbps;
            snprintf(st.cur_segment, sizeof(st.cur_segment), "%s", rs.cur_segment);
        }
    }
    pthread_mutex_lock(&g_status_lock);
    g_status = st;
    pthread_mutex_unlock(&g_status_lock);
}

typedef struct { int64_t deadline; } input_deadline_t;
static int interrupt_input(void *opaque)
{
    input_deadline_t *d = opaque;
    return !atomic_load(&g_running) || av_gettime_relative() >= d->deadline;
}

static int open_rtsp(const gateway_conf_t *cfg, AVFormatContext **pic, int *idx,
                     input_deadline_t *deadline)
{
    AVFormatContext *ic = avformat_alloc_context();
    if (!ic) return -1;
    ic->interrupt_callback = (AVIOInterruptCB){interrupt_input, deadline};
    deadline->deadline = av_gettime_relative() + cfg->rtsp_timeout_us;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "rtsp_transport", cfg->rtsp_transport, 0);
    av_dict_set(&opts, "analyzeduration", "5000000", 0);
    av_dict_set(&opts, "probesize", "10485760", 0);
    av_dict_set(&opts, "fflags", "+discardcorrupt", 0);
    int ret = avformat_open_input(&ic, cfg->rtsp_url, NULL, &opts);
    av_dict_free(&opts);
    if (ret < 0) goto fail;
    deadline->deadline = av_gettime_relative() + cfg->rtsp_timeout_us;
    ret = avformat_find_stream_info(ic, NULL);
    if (ret < 0) goto fail;
    *idx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (*idx < 0) { ret = *idx; goto fail; }
    *pic = ic;
    return 0;
fail: {
    char err[128]; av_strerror(ret, err, sizeof(err));
    LOG_WRN("RTSP open/probe failed: %s", err);
    avformat_close_input(&ic);
    return -1;
}}

/* 预览/转发线程：只从队列取包，做 H.264 解码预览与 RTSP 转发。
 * 这是“可丢路径”——它再慢也只是预览掉帧并被计数，不会拖住采集与录像。 */
typedef struct {
    pkt_queue_t *queue;
    AVRational   time_base;
    int          decode_preview;  /* 1=本线程解码 H.264 产 JPEG；0=预览已由相机 JPEG 直通 */
} preview_args_t;

static void *preview_thread(void *arg)
{
    /* 参数由采集线程 malloc 传来，本线程接手后立即释放，
     * 之后只用自己手里的 queue / tb，不依赖任何共享栈变量。 */
    preview_args_t *pa = arg;
    pkt_queue_t *queue = pa->queue;
    AVRational tb = pa->time_base;
    int decode_preview = pa->decode_preview;
    free(pa);
    for (;;) {
        AVPacket *pkt = NULL;
        int is_key = 0;
        if (pkt_queue_pop(queue, &pkt, &is_key) != 0) break; /* 已停止且排空 */
        (void)is_key;   /* 丢包后的重同步交给解码器等下一个关键帧 */
        if (decode_preview) frame_grabber_feed(pkt);
        rtsp_server_on_packet(pkt, &tb);
        av_packet_free(&pkt);
        atomic_fetch_add(&g_preview_processed, 1);
    }
    return NULL;
}

/* 相机 MJPEG 直通订阅者：在采集线程内被调用，只做节流 + 缓存（一次 memcpy），
 * 不做任何解码/编码，因此不会拖慢采集循环。 */
static void preview_jpeg_cb(void *ctx, const uint8_t *data, size_t size, int64_t ts_us)
{
    (void)ctx;
    (void)ts_us;
    frame_grabber_submit_jpeg(data, (int)size);
}

static void *receiver_thread(void *arg)
{
    const gateway_conf_t *cfg = arg;
    int is_usb = !strcmp(cfg->source_type, "v4l2");
    unsigned reconnects = 0;
    unsigned long long frames = 0;
    while (atomic_load(&g_running)) {
        AVFormatContext *ic = NULL;
        usb_source_t *usb = NULL;
        recorder_t *rec = NULL;
        AVPacket *pkt = av_packet_alloc();
        const AVCodecParameters *cp = NULL;
        AVRational tb = {0, 1};
        input_deadline_t deadline = {0};
        int vidx = -1, fg_ready = 0, rtsp_ready = 0, fps = 0;
        int applied_segment_seconds = atomic_load(&g_segment_seconds);
        pkt_queue_t *pq = NULL;
        pthread_t preview_tid;
        int preview_started = 0;
        int decode_preview = 1;      /* 预览线程是否需要在本地解码 H.264 */
        preview_args_t *pa = NULL;   /* 堆分配后交给预览线程，由线程负责释放 */
        if (!pkt) { LOG_ERR("Cannot allocate input packet"); break; }
        if (is_usb) {
            usb = usb_source_open(cfg);
            if (!usb) goto cleanup;
            cp = usb_source_codecpar(usb);
            tb = usb_source_time_base(usb);
            fps = cfg->capture_fps; /* Requested rate; frames is the measured output counter. */
        } else {
            if (open_rtsp(cfg, &ic, &vidx, &deadline) < 0) goto cleanup;
            cp = ic->streams[vidx]->codecpar;
            tb = ic->streams[vidx]->time_base;
            AVRational fr = ic->streams[vidx]->avg_frame_rate;
            fps = fr.den ? (int)av_q2d(fr) : 0;
        }
        if (!atomic_load(&g_running)) goto cleanup;
        recorder_cfg_t rcfg = {0};
        snprintf(rcfg.dir, sizeof(rcfg.dir), "%s", cfg->storage_dir);
        snprintf(rcfg.prefix, sizeof(rcfg.prefix), "%s", cfg->segment_prefix);
        rcfg.segment_seconds = applied_segment_seconds;
        rcfg.min_free_mb = cfg->storage_min_free_mb;
        rec = recorder_create(&rcfg, (AVCodecParameters *)cp);
        if (!rec) goto cleanup;
        frame_grabber_cfg_t fgc = {
            .target_fps = cfg->web_mjpeg_fps,
            .out_width = cfg->ai_frame_width,
            .jpeg_quality = cfg->ai_jpeg_quality
        };
        fg_ready = frame_grabber_init(&fgc, (AVCodecParameters *)cp) == 0;
        if (!fg_ready) LOG_WRN("Preview unavailable; recording continues");
        /* USB 源本来给的就是 MJPEG：预览直接用它，省掉“编码成 H.264 再解码回来”的整轮开销。
         * RTSP 输入没有原始 JPEG，仍走 H.264 解码预览。 */
        if (is_usb && fg_ready) {
            usb_source_set_jpeg_cb(usb, preview_jpeg_cb, NULL);
            decode_preview = 0;
        }
        if (cfg->rtsp_server_enable)
            rtsp_ready = rtsp_server_init(cfg->rtsp_server_port, cp) == 0;
        /* 预览与 RTSP 转发下到独立线程：解码变慢或客户端拖后腿时，
         * 采集与录像不再被连带阻塞（队列满只丢预览的旧包并计数）。 */
        if (fg_ready || rtsp_ready) {
            pq = pkt_queue_create(PREVIEW_QUEUE_MAX_PACKETS);
            if (pq) {
                pa = malloc(sizeof(*pa));
                if (pa) {
                    pa->queue = pq;
                    pa->time_base = tb;
                    pa->decode_preview = decode_preview;
                }
                if (pa && pthread_create(&preview_tid, NULL, preview_thread, pa) == 0) {
                    preview_started = 1;      /* 成功时由线程释放 pa */
                } else {
                    LOG_WRN("Cannot start preview thread; preview stays inline");
                    free(pa);
                    pkt_queue_destroy(pq);
                    pq = NULL;
                }
            } else {
                LOG_WRN("Cannot create preview queue; preview stays inline");
            }
        }
        g_preview_queue = pq;
        g_preview_raw_jpeg = (fg_ready && !decode_preview) ? 1 : 0;
        LOG_INF("Input connected: %s, %dx%d, %s (preview %s, jpeg %s)", cfg->source_type,
                cp->width, cp->height, avcodec_get_name(cp->codec_id),
                pq ? "async" : "inline",
                g_preview_raw_jpeg ? "camera-passthrough" : "h264-decode");
        publish_status(cp, fps, rec, frames, reconnects);
        while (atomic_load(&g_running)) {
            int ret;
            if (usb) {
                ret = usb_source_read(usb, pkt);
                if (ret == 0) continue;
            } else {
                deadline.deadline = av_gettime_relative() + cfg->rtsp_timeout_us;
                ret = av_read_frame(ic, pkt);
                if (ret >= 0 && pkt->stream_index != vidx) { av_packet_unref(pkt); continue; }
            }
            if (ret < 0) {
                char err[128]; av_strerror(ret, err, sizeof(err));
                LOG_WRN("Input read failed: %s; reconnecting", err);
                break;
            }
            int segment_seconds = atomic_load(&g_segment_seconds);
            if (segment_seconds != applied_segment_seconds) {
                recorder_set_segment_seconds(rec, segment_seconds);
                applied_segment_seconds = segment_seconds;
            }
            if (recorder_write_packet(rec, pkt, !!(pkt->flags & AV_PKT_FLAG_KEY), tb) < 0) {
                LOG_WRN("Recording failed; reopening pipeline");
                break;
            }
            if (pq) {
                /* 队列持有自己的引用（引用计数共享数据，不复制负载）；
                 * 生产者手里的 pkt 照旧在本线程回收。 */
                AVPacket *copy = av_packet_alloc();
                if (copy && av_packet_ref(copy, pkt) == 0)
                    pkt_queue_push(pq, copy, !!(pkt->flags & AV_PKT_FLAG_KEY));
                else
                    av_packet_free(&copy);
            } else {
                /* 队列不可用时的退路：相机 JPEG 直通已注册时无需再解码 H.264 */
                if (fg_ready && decode_preview) frame_grabber_feed(pkt);
                if (rtsp_ready) rtsp_server_on_packet(pkt, &tb);
            }
            ++frames;
            publish_status(cp, fps, rec, frames, reconnects);
            av_packet_unref(pkt);
        }
cleanup:
        /* 先把最后一次统计发出去（此时队列还在，丢弃计数可见），再拆队列 */
        publish_status(NULL, 0, NULL, frames, reconnects);
        g_preview_queue = NULL;
        g_preview_raw_jpeg = 0;
        av_packet_free(&pkt);
        if (pq) {
            pkt_queue_stop(pq);                  /* 唤醒可能阻塞的预览线程 */
            if (preview_started) pthread_join(preview_tid, NULL);
            pkt_queue_stats_t qs;
            pkt_queue_get_stats(pq, &qs);
            if (qs.dropped)
                LOG_WRN("Preview queue overflow: high=%d dropped=%llu packets",
                        qs.high_watermark, qs.dropped);
            pkt_queue_destroy(pq);
        }
        if (rtsp_ready) rtsp_server_deinit();
        if (fg_ready) frame_grabber_deinit();
        recorder_destroy(rec);
        usb_source_close(usb);
        avformat_close_input(&ic);
        if (atomic_load(&g_running)) {
            ++reconnects;
            publish_status(NULL, 0, NULL, frames, reconnects);
            LOG_INF("Retrying input in %d s (attempt %u)", cfg->rtsp_reconnect_s, reconnects);
            for (int i = 0; i < cfg->rtsp_reconnect_s * 10 && atomic_load(&g_running); ++i)
                usleep(100000);
        }
    }
    publish_status(NULL, 0, NULL, frames, reconnects);
    return NULL;
}

int stream_receiver_start(const gateway_conf_t *cfg)
{
    if (atomic_load(&g_running)) return 0;
    g_cfg = *cfg;
    atomic_store(&g_segment_seconds, cfg->segment_seconds);
    atomic_store(&g_preview_processed, 0);
    g_preview_queue = NULL;
    g_preview_raw_jpeg = 0;
    publish_status(NULL, 0, NULL, 0, 0);
    avformat_network_init();
    atomic_store(&g_running, 1);
    if (pthread_create(&g_tid, NULL, receiver_thread, &g_cfg) != 0) {
        atomic_store(&g_running, 0);
        return -1;
    }
    return 0;
}

void stream_receiver_stop(void)
{
    if (!atomic_exchange(&g_running, 0)) return;
    pthread_join(g_tid, NULL);
    LOG_INF("Input pipeline stopped");
}

int stream_receiver_get_status(stream_status_t *st)
{
    if (!st) return -1;
    pthread_mutex_lock(&g_status_lock);
    *st = g_status;
    pthread_mutex_unlock(&g_status_lock);
    return 0;
}

void stream_receiver_set_segment_seconds(int seconds)
{
    if (seconds > 0) atomic_store(&g_segment_seconds, seconds);
}

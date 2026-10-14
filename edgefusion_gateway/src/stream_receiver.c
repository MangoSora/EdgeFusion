#include "stream_receiver.h"
#include "usb_source.h"
#include "log.h"
#include "frame_grabber.h"
#include "rtsp_server.h"

#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_t g_tid;
static atomic_int g_running;
static atomic_int g_segment_seconds;
static gateway_conf_t g_cfg;
static pthread_mutex_t g_status_lock = PTHREAD_MUTEX_INITIALIZER;
static stream_status_t g_status;

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
        if (cfg->rtsp_server_enable)
            rtsp_ready = rtsp_server_init(cfg->rtsp_server_port, cp) == 0;
        LOG_INF("Input connected: %s, %dx%d, %s", cfg->source_type,
                cp->width, cp->height, avcodec_get_name(cp->codec_id));
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
            if (fg_ready) frame_grabber_feed(pkt);
            if (rtsp_ready) rtsp_server_on_packet(pkt, &tb);
            ++frames;
            publish_status(cp, fps, rec, frames, reconnects);
            av_packet_unref(pkt);
        }
cleanup:
        publish_status(NULL, 0, NULL, frames, reconnects);
        av_packet_free(&pkt);
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

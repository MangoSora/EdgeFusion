#include "recorder.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct recorder {
    recorder_cfg_t cfg;
    AVCodecParameters *codecpar;   /* 拷贝的源编解码参数 */
    AVFormatContext *ofmt;         /* 当前输出上下文 */
    AVStream        *ost;          /* 输出视频流 */
    char             cur_path[512];/* 当前段文件路径 */
    int64_t          segment_dts; /* Input timestamp origin, reset at each segment. */
    int              seg_index;    /* 段序号 */
    double           seg_start_ts; /* 当前段起始时间戳(秒) */
    int              header_written;
    int              waiting_key;  /* 新段是否在等首个关键帧 */
    long             bytes_written;     /* 当前段已写字节（算码率） */
    time_t           seg_start_wall;    /* 当前段起始墙钟（算码率/已录时长） */
    pthread_mutex_t  mtx;               /* 保护 bytes_written 等统计字段 */
};

/* 按段序号生成文件名：prefix_YYYYmmdd_HHMMSS_NN.mp4 */
static void make_path(recorder_t *r, char *out, size_t n)
{
    struct timespec stamp;
    clock_gettime(CLOCK_REALTIME, &stamp);
    time_t t = stamp.tv_sec;
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, n, "%s/%s_%04d%02d%02d_%02d%02d%02d_%09ld_%d.mp4",
             r->cfg.dir, r->cfg.prefix,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             stamp.tv_nsec, r->seg_index);
}

/* 关闭当前段 */
static void close_segment(recorder_t *r)
{
    if (!r->ofmt) return;
    if (r->header_written) {
        int ret = av_write_trailer(r->ofmt);
        if (ret < 0) {
            char err[128]; av_strerror(ret, err, sizeof(err));
            LOG_WRN("av_write_trailer failed: %s (%s, ret=%d)", r->cur_path, err, ret);
        }
    }
    if (!(r->ofmt->oformat->flags & AVFMT_NOFILE))
        avio_closep(&r->ofmt->pb);
    avformat_free_context(r->ofmt);
    r->ofmt = NULL;
    r->ost = NULL;
    r->header_written = 0;
    r->waiting_key = 1;
}

/* 打开新段 */
static int open_segment(recorder_t *r)
{
    /* 确保目录存在 */
    mkdir(r->cfg.dir, 0755);

    make_path(r, r->cur_path, sizeof(r->cur_path));

    AVFormatContext *octx = NULL;
    if (avformat_alloc_output_context2(&octx, NULL, "mp4", r->cur_path) < 0 || !octx) {
        LOG_ERR("无法创建输出上下文");
        return -1;
    }
    r->ofmt = octx;

    AVStream *st = avformat_new_stream(octx, NULL);
    if (!st) { LOG_ERR("avformat_new_stream 失败"); goto fail; }
    if (avcodec_parameters_copy(st->codecpar, r->codecpar) < 0) {
        LOG_ERR("复制 codecpar 失败"); goto fail;
    }
    st->codecpar->codec_tag = 0;
    r->ost = st;

    if (!(octx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&octx->pb, r->cur_path, AVIO_FLAG_WRITE) < 0) {
            LOG_ERR("打开录像文件失败: %s (%s)", r->cur_path, strerror(errno));
            goto fail;
        }
    }
    r->header_written = 0;
    r->waiting_key = 1;
    r->seg_index++;
    r->bytes_written = 0;
    r->seg_start_wall = time(NULL);
    LOG_INF("新录像段: %s", r->cur_path);
    return 0;
fail:
    if (octx->pb) avio_closep(&octx->pb);
    avformat_free_context(octx);
    r->ofmt = NULL;
    r->ost = NULL;
    return -1;
}

/* 比较函数用于 scandir 排序（按文件名，最旧的排前） */
static int name_cmp(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}

/* 检查并清理磁盘：删最旧段直到剩余空间足够 */
static void cleanup_disk(recorder_t *r)
{
    struct statvfs vfs;
    if (statvfs(r->cfg.dir, &vfs) != 0) return;
    unsigned long free_mb = (vfs.f_bavail * (unsigned long)vfs.f_frsize) / (1024 * 1024);
    if ((long)free_mb >= r->cfg.min_free_mb) return;

    LOG_WRN("磁盘剩余 %luMB < %ldMB，开始清理最旧录像", free_mb, r->cfg.min_free_mb);

    struct dirent **namelist = NULL;
    int n = scandir(r->cfg.dir, &namelist, NULL, name_cmp);
    if (n < 0) return;

    int deleted = 0;
    for (int i = 0; i < n && (long)((vfs.f_bavail * (unsigned long)vfs.f_frsize) / (1024 * 1024)) < r->cfg.min_free_mb; i++) {
        const char *name = namelist[i]->d_name;
        if (name[0] == '.') continue;
        size_t l = strlen(name);
        if (l < 5 || strcmp(name + l - 4, ".mp4")) continue;
        /* 跳过当前正在写的段 */
        if (strstr(r->cur_path, name)) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", r->cfg.dir, name);
        if (unlink(path) == 0) {
            deleted++;
            LOG_INF("删除旧录像: %s", name);
        }
        /* 重新统计 */
        if (statvfs(r->cfg.dir, &vfs) != 0) break;
    }
    for (int i = 0; i < n; i++) free(namelist[i]);
    free(namelist);
    if (deleted) LOG_INF("本次清理删除 %d 个旧段", deleted);
}

recorder_t *recorder_create(const recorder_cfg_t *cfg, AVCodecParameters *stream_codecpar)
{
    if (!stream_codecpar) { LOG_ERR("recorder 需要 stream_codecpar"); return NULL; }
    recorder_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->cfg = *cfg;
    r->codecpar = avcodec_parameters_alloc();
    avcodec_parameters_copy(r->codecpar, stream_codecpar);
    r->waiting_key = 1;
    pthread_mutex_init(&r->mtx, NULL);
    mkdir(r->cfg.dir, 0755);
    cleanup_disk(r);
    return r;
}

void recorder_destroy(recorder_t *r)
{
    if (!r) return;
    close_segment(r);
    if (r->codecpar) avcodec_parameters_free(&r->codecpar);
    pthread_mutex_destroy(&r->mtx);
    free(r);
}

int recorder_get_status(const recorder_t *r, recorder_status_t *st)
{
    if (!r || !st) return -1;
    memset(st, 0, sizeof(*st));
    pthread_mutex_lock((pthread_mutex_t *)&r->mtx);
    st->recording = (r->ofmt != NULL && r->header_written);
    st->seg_index = r->seg_index;
    /* cur_segment 取 basename */
    const char *base = strrchr(r->cur_path, '/');
    snprintf(st->cur_segment, sizeof(st->cur_segment), "%.*s",
             (int)sizeof(st->cur_segment) - 1, base ? base + 1 : r->cur_path);
    long bytes = r->bytes_written;
    time_t start = r->seg_start_wall;
    pthread_mutex_unlock((pthread_mutex_t *)&r->mtx);
    int elapsed = (int)(time(NULL) - start);
    if (elapsed < 0) elapsed = 0;
    st->seg_elapsed_s = elapsed;
    st->bitrate_kbps = (elapsed > 0) ? (int)((long long)bytes * 8 / 1000 / elapsed) : 0;
    return 0;
}

void recorder_set_segment_seconds(recorder_t *r, int seconds)
{
    if (!r || seconds <= 0) return;
    pthread_mutex_lock(&r->mtx);
    r->cfg.segment_seconds = seconds;
    pthread_mutex_unlock(&r->mtx);
    LOG_INF("分段时长热更新为 %d 秒(下一段生效)", seconds);
}

int recorder_write_packet(recorder_t *r, AVPacket *pkt, int is_key, AVRational source_time_base)
{
    if (!r || source_time_base.num <= 0 || source_time_base.den <= 0) return -1;
    int64_t input_dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
    if (input_dts == AV_NOPTS_VALUE) {
        LOG_WRN("Cannot record packet without PTS/DTS");
        return -1;
    }
    double ts = input_dts * av_q2d(source_time_base);

    /* 需要关键帧才开始/切换段，避免 MP4 损坏 */
    if (r->waiting_key) {
        if (!is_key) return 0; /* 丢弃直到关键帧 */
        /* 关键帧到达：若当前无段则开新段 */
        if (!r->ofmt) {
            if (open_segment(r) != 0) return -1;
            r->seg_start_ts = ts;
            r->segment_dts = input_dts;
        }
        r->waiting_key = 0;
    }

    /* 段时长到达则在下一个关键帧处切段 */
    if (r->header_written && is_key && (ts - r->seg_start_ts) >= r->cfg.segment_seconds) {
        close_segment(r);
        cleanup_disk(r);
        if (open_segment(r) != 0) return -1;
        r->seg_start_ts = ts;
        r->segment_dts = input_dts;
        r->waiting_key = 0;
    }

    /* 写头（首帧，必须在有有效帧时写） */
    if (!r->header_written) {
        /* Preserve the input time base; the muxer may adjust it in write_header. */
        r->ost->time_base = source_time_base;
        /* Fragmented MP4 reduces the amount at risk on interruption.
         * It does NOT guarantee power-loss safety: OS/device caches and the
         * incomplete final fragment can still lose data. */
        AVDictionary *opts = NULL;
        av_dict_set(&opts, "movflags", "+frag_keyframe+empty_moov+default_base_moof", 0);
        av_dict_set(&opts, "frag_duration", "10000000", 0); /* 10s 一个 fragment 兜底 */
        if (avformat_write_header(r->ofmt, &opts) < 0) {
            LOG_ERR("avformat_write_header 失败");
            av_dict_free(&opts);
            return -1;
        }
        av_dict_free(&opts);
        r->header_written = 1;
    }

    /* 重打时间戳到输出流时基 */
    AVPacket *out = av_packet_clone(pkt);
    if (!out) return -1;
    if (out->pts != AV_NOPTS_VALUE) out->pts -= r->segment_dts;
    if (out->dts != AV_NOPTS_VALUE) out->dts -= r->segment_dts;
    av_packet_rescale_ts(out, source_time_base, r->ost->time_base);
    out->stream_index = 0;

    int packet_size = out->size; /* muxer consumes and clears the packet */
    int ret = av_interleaved_write_frame(r->ofmt, out);
    if (ret >= 0) {
        pthread_mutex_lock(&r->mtx);
        r->bytes_written += packet_size;
        pthread_mutex_unlock(&r->mtx);
    }
    av_packet_free(&out);
    if (ret < 0) {
        LOG_WRN("写帧失败 ret=%d", ret);
    }
    return ret < 0 ? -1 : 0;
}

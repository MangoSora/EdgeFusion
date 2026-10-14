#include "usb_source.h"
#include "log.h"
#include "image_scale.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

struct usb_source {
    int fd, streaming;
    struct { void *data; size_t size; } buffers[4];
    unsigned count;
    AVCodecContext *decoder, *encoder;
    AVCodecParameters *codecpar;
    AVFrame *decoded, *converted;
    struct SwsContext *sws;
    int64_t first_ts, last_pts, last_frame_us;
    int64_t timeout_us;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;
    do { ret = ioctl(fd, request, arg); } while (ret < 0 && errno == EINTR);
    return ret;
}

static int codec_error(const char *op, int ret)
{
    char msg[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, msg, sizeof(msg));
    LOG_ERR("USB %s: %s", op, msg);
    return ret;
}

void usb_source_close(usb_source_t *s)
{
    if (!s) return;
    if (s->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(s->fd, VIDIOC_STREAMOFF, &type);
    }
    for (unsigned i = 0; i < s->count; ++i)
        if (s->buffers[i].data && s->buffers[i].data != MAP_FAILED)
            munmap(s->buffers[i].data, s->buffers[i].size);
    if (s->fd >= 0) close(s->fd);
    avcodec_free_context(&s->decoder);
    avcodec_free_context(&s->encoder);
    avcodec_parameters_free(&s->codecpar);
    av_frame_free(&s->decoded);
    av_frame_free(&s->converted);
    sws_freeContext(s->sws);
    free(s);
}

usb_source_t *usb_source_open(const gateway_conf_t *cfg)
{
    usb_source_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->fd = -1;
    s->first_ts = AV_NOPTS_VALUE;
    s->last_pts = AV_NOPTS_VALUE;
    s->timeout_us = cfg->rtsp_timeout_us;

    /* Explicit software baseline. Do not silently label this as MPP acceleration. */
    const AVCodec *enc = avcodec_find_encoder_by_name("libx264");
    const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!enc || !dec) {
        LOG_ERR("USB mode requires FFmpeg MJPEG decoder and libx264 encoder");
        goto fail;
    }
    s->fd = open(cfg->video_device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (s->fd < 0) goto device_fail;
    struct v4l2_capability cap = {0};
    if (xioctl(s->fd, VIDIOC_QUERYCAP, &cap) < 0) goto device_fail;
    unsigned caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        LOG_ERR("%s is not a single-plane streaming capture node", cfg->video_device);
        goto fail;
    }
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = cfg->capture_width;
    fmt.fmt.pix.height = cfg->capture_height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(s->fd, VIDIOC_S_FMT, &fmt) < 0) goto device_fail;
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG ||
        fmt.fmt.pix.width != (unsigned)cfg->capture_width ||
        fmt.fmt.pix.height != (unsigned)cfg->capture_height) {
        LOG_ERR("Camera did not accept requested MJPEG %dx%d; choose a listed mode",
                cfg->capture_width, cfg->capture_height);
        goto fail;
    }
    struct v4l2_streamparm parm = {0};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = cfg->capture_fps;
    if (xioctl(s->fd, VIDIOC_S_PARM, &parm) < 0) goto device_fail;
    LOG_INF("USB negotiated MJPEG %ux%u interval=%u/%u s",
            fmt.fmt.pix.width, fmt.fmt.pix.height,
            parm.parm.capture.timeperframe.numerator,
            parm.parm.capture.timeperframe.denominator);

    s->decoder = avcodec_alloc_context3(dec);
    s->encoder = avcodec_alloc_context3(enc);
    s->decoded = av_frame_alloc();
    s->converted = av_frame_alloc();
    s->codecpar = avcodec_parameters_alloc();
    if (!s->decoder || !s->encoder || !s->decoded || !s->converted || !s->codecpar) goto fail;
    s->decoder->thread_count = 1;
    int ret = avcodec_open2(s->decoder, dec, NULL);
    if (ret < 0) { codec_error("open MJPEG decoder", ret); goto fail; }
    s->encoder->width = cfg->capture_width;
    s->encoder->height = cfg->capture_height;
    s->encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    s->encoder->color_range = AVCOL_RANGE_MPEG;
    s->encoder->time_base = (AVRational){1, 90000};
    s->encoder->framerate = (AVRational){cfg->capture_fps, 1};
    s->encoder->bit_rate = (int64_t)cfg->video_bitrate_kbps * 1000;
    s->encoder->gop_size = cfg->capture_fps * 2;
    s->encoder->max_b_frames = 0;
    s->encoder->thread_count = 2;
    s->encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "preset", "ultrafast", 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);
    /* Annex B packets for the existing RTSP server; SPS/PPS also in codecpar. */
    av_dict_set(&opts, "x264-params", "repeat-headers=1:annexb=1", 0);
    ret = avcodec_open2(s->encoder, enc, &opts);
    av_dict_free(&opts);
    if (ret < 0) { codec_error("open libx264 encoder", ret); goto fail; }
    if (avcodec_parameters_from_context(s->codecpar, s->encoder) < 0) goto fail;
    s->converted->format = s->encoder->pix_fmt;
    s->converted->color_range = AVCOL_RANGE_MPEG;
    s->converted->width = s->encoder->width;
    s->converted->height = s->encoder->height;
    if (av_frame_get_buffer(s->converted, 32) < 0) goto fail;

    struct v4l2_requestbuffers req = {0};
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count = 4;
    if (xioctl(s->fd, VIDIOC_REQBUFS, &req) < 0) goto device_fail;
    if (req.count < 2 || req.count > 4) {
        LOG_ERR("Unsupported V4L2 buffer count: %u", req.count);
        goto fail;
    }
    s->count = req.count;
    for (unsigned i = 0; i < s->count; ++i) {
        struct v4l2_buffer buf = {0};
        buf.type = req.type;
        buf.memory = req.memory;
        buf.index = i;
        if (xioctl(s->fd, VIDIOC_QUERYBUF, &buf) < 0) goto device_fail;
        s->buffers[i].size = buf.length;
        s->buffers[i].data = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, s->fd, buf.m.offset);
        if (s->buffers[i].data == MAP_FAILED) goto device_fail;
        if (xioctl(s->fd, VIDIOC_QBUF, &buf) < 0) goto device_fail;
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(s->fd, VIDIOC_STREAMON, &type) < 0) goto device_fail;
    s->streaming = 1;
    s->last_frame_us = av_gettime_relative();
    LOG_INF("USB source ready: %s -> libx264 software H.264", cfg->video_device);
    return s;
device_fail:
    LOG_ERR("USB device %s: %s", cfg->video_device, strerror(errno));
fail:
    usb_source_close(s);
    return NULL;
}

const AVCodecParameters *usb_source_codecpar(const usb_source_t *s) { return s->codecpar; }
AVRational usb_source_time_base(const usb_source_t *s) { return s->encoder->time_base; }

static int finish_packet(usb_source_t *s, AVPacket *out)
{
    if (out->duration <= 0)
        out->duration = av_rescale_q(1, av_inv_q(s->encoder->framerate), s->encoder->time_base);
    return 1;
}

int usb_source_read(usb_source_t *s, AVPacket *out)
{
    /* Drain before sending another frame; no B frames/lookahead in this baseline. */
    int ret = avcodec_receive_packet(s->encoder, out);
    if (ret == 0) return finish_packet(s, out);
    if (ret != AVERROR(EAGAIN)) return codec_error("receive H.264", ret);

    if (av_gettime_relative() - s->last_frame_us > s->timeout_us) {
        LOG_WRN("USB capture timed out; reopening device");
        return AVERROR(ETIMEDOUT);
    }
    struct pollfd pfd = {.fd = s->fd, .events = POLLIN};
    ret = poll(&pfd, 1, 200);
    if (ret < 0) return errno == EINTR ? 0 : AVERROR(errno);
    if (!ret) return 0;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return AVERROR(EIO);
    if (!(pfd.revents & POLLIN)) return 0;
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(s->fd, VIDIOC_DQBUF, &buf) < 0)
        return errno == EAGAIN ? 0 : AVERROR(errno);
    if (buf.index >= s->count) return AVERROR(EIO);
    if (!buf.bytesused || buf.bytesused > s->buffers[buf.index].size ||
        buf.bytesused > INT_MAX || (buf.flags & V4L2_BUF_FLAG_ERROR)) {
        if (xioctl(s->fd, VIDIOC_QBUF, &buf) < 0) return AVERROR(errno);
        return 0;
    }
    AVPacket *jpeg = av_packet_alloc();
    if (!jpeg) return AVERROR(ENOMEM);
    ret = av_new_packet(jpeg, (int)buf.bytesused);
    if (ret < 0) { av_packet_free(&jpeg); return ret; }
    memcpy(jpeg->data, s->buffers[buf.index].data, buf.bytesused);
    /* Own a padded copy BEFORE requeueing: the driver may immediately overwrite mmap data. */
    int64_t capture_ts = (int64_t)buf.timestamp.tv_sec * 1000000 + buf.timestamp.tv_usec;
    if (capture_ts <= 0) capture_ts = av_gettime_relative();
    if (xioctl(s->fd, VIDIOC_QBUF, &buf) < 0) {
        ret = AVERROR(errno); av_packet_free(&jpeg); return ret;
    }
    ret = avcodec_send_packet(s->decoder, jpeg);
    av_packet_free(&jpeg);
    if (ret < 0) return codec_error("send MJPEG", ret);
    ret = avcodec_receive_frame(s->decoder, s->decoded);
    if (ret == AVERROR(EAGAIN)) return 0;
    if (ret < 0) return codec_error("decode MJPEG", ret);
    s->last_frame_us = av_gettime_relative();
    ret = av_frame_make_writable(s->converted);
    if (ret < 0) return ret;
    if (image_scale_prepare(&s->sws, s->decoded, s->encoder->width, s->encoder->height,
                            s->encoder->pix_fmt, 0) < 0)
        return codec_error("configure color range", AVERROR(EINVAL));
    ret = sws_scale(s->sws, (const uint8_t *const *)s->decoded->data, s->decoded->linesize,
                   0, s->decoded->height, s->converted->data, s->converted->linesize);
    av_frame_unref(s->decoded);
    if (ret <= 0) return AVERROR(EIO);
    if (s->first_ts == AV_NOPTS_VALUE) s->first_ts = capture_ts;
    int64_t pts = av_rescale_q(capture_ts - s->first_ts, AV_TIME_BASE_Q, s->encoder->time_base);
    if (s->last_pts != AV_NOPTS_VALUE && pts <= s->last_pts) pts = s->last_pts + 1;
    s->converted->pts = s->last_pts = pts;
    ret = avcodec_send_frame(s->encoder, s->converted);
    if (ret < 0) return codec_error("send frame to libx264", ret);
    ret = avcodec_receive_packet(s->encoder, out);
    if (ret == AVERROR(EAGAIN)) return 0;
    if (ret < 0) return codec_error("encode H.264", ret);
    return finish_packet(s, out);
}

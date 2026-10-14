/* Regression: non-90kHz input, nonzero origin, keyframe segmentation,
 * muxer packet ownership/bitrate, and repeated sessions in one directory.
 * Requires FFmpeg libx264; no camera or network required. */
#include "recorder.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static int write_packets(AVCodecContext *enc, recorder_t *rec)
{
    AVPacket *pkt = av_packet_alloc();
    CHECK(pkt);
    int ret;
    while ((ret = avcodec_receive_packet(enc, pkt)) >= 0) {
        if (pkt->duration <= 0) pkt->duration = 1; /* one frame at 1/30 */
        av_packet_rescale_ts(pkt, enc->time_base, (AVRational){1, 1000});
        pkt->pts += 123000;
        pkt->dts += 123000;
        CHECK(recorder_write_packet(rec, pkt, !!(pkt->flags & AV_PKT_FLAG_KEY),
                                    (AVRational){1, 1000}) == 0);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    CHECK(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    return ret;
}

static void session(const char *dir)
{
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    CHECK(codec);
    AVCodecContext *enc = avcodec_alloc_context3(codec);
    CHECK(enc);
    enc->width = 64; enc->height = 48;
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->time_base = (AVRational){1, 30};
    enc->framerate = (AVRational){30, 1};
    enc->gop_size = 30; enc->max_b_frames = 0;
    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "preset", "ultrafast", 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);
    av_dict_set(&opts, "x264-params", "scenecut=0", 0);
    CHECK(avcodec_open2(enc, codec, &opts) == 0);
    av_dict_free(&opts);
    AVCodecParameters *cp = avcodec_parameters_alloc();
    CHECK(cp && avcodec_parameters_from_context(cp, enc) == 0);
    recorder_cfg_t cfg = {.segment_seconds = 1, .min_free_mb = 0};
    snprintf(cfg.dir, sizeof(cfg.dir), "%s", dir);
    snprintf(cfg.prefix, sizeof(cfg.prefix), "test");
    recorder_t *rec = recorder_create(&cfg, cp);
    CHECK(rec);
    AVFrame *frame = av_frame_alloc();
    CHECK(frame);
    frame->format = enc->pix_fmt;
    frame->width = enc->width; frame->height = enc->height;
    CHECK(av_frame_get_buffer(frame, 32) == 0);
    for (int i = 0; i < 90; ++i) {
        CHECK(av_frame_make_writable(frame) == 0);
        for (int y = 0; y < frame->height; ++y)
            memset(frame->data[0] + y * frame->linesize[0], 32 + i, frame->width);
        for (int p = 1; p < 3; ++p)
            for (int y = 0; y < frame->height / 2; ++y)
                memset(frame->data[p] + y * frame->linesize[p], 128, frame->width / 2);
        frame->pts = i;
        CHECK(avcodec_send_frame(enc, frame) == 0);
        write_packets(enc, rec);
    }
    CHECK(avcodec_send_frame(enc, NULL) == 0);
    CHECK(write_packets(enc, rec) == AVERROR_EOF);
    sleep(1); /* recorder bitrate uses wall-clock elapsed seconds */
    recorder_status_t status;
    CHECK(recorder_get_status(rec, &status) == 0);
    CHECK(status.recording && status.bitrate_kbps > 0);
    recorder_destroy(rec);
    av_frame_free(&frame);
    avcodec_parameters_free(&cp);
    avcodec_free_context(&enc);
}

int main(void)
{
    char dir[] = "/tmp/edgefusion-recorder-test-XXXXXX";
    CHECK(mkdtemp(dir));
    printf("Test artifacts: %s\n", dir);
    session(dir);
    session(dir);
    DIR *dp = opendir(dir);
    CHECK(dp);
    struct dirent *ent;
    int segments = 0, total_packets = 0;
    while ((ent = readdir(dp))) {
        if (!strstr(ent->d_name, ".mp4")) continue;
        char filename[512];
        snprintf(filename, sizeof(filename), "%s/%s", dir, ent->d_name);
        AVFormatContext *ic = NULL;
        CHECK(avformat_open_input(&ic, filename, NULL, NULL) == 0);
        CHECK(avformat_find_stream_info(ic, NULL) >= 0);
        int video = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        CHECK(video >= 0);
        AVPacket *pkt = av_packet_alloc();
        CHECK(pkt);
        int count = 0;
        int64_t last = AV_NOPTS_VALUE;
        while (av_read_frame(ic, pkt) >= 0) {
            if (pkt->stream_index == video) {
                CHECK(pkt->dts != AV_NOPTS_VALUE);
                double seconds = pkt->dts * av_q2d(ic->streams[video]->time_base);
                if (!count) CHECK((pkt->flags & AV_PKT_FLAG_KEY) && seconds >= 0 && seconds < 0.01);
                CHECK(last == AV_NOPTS_VALUE || pkt->dts > last);
                CHECK(seconds < 1.1);
                last = pkt->dts;
                ++count;
            }
            av_packet_unref(pkt);
        }
        CHECK(count == 30);
        total_packets += count;
        ++segments;
        av_packet_free(&pkt);
        avformat_close_input(&ic);
    }
    closedir(dp);
    CHECK(segments == 6 && total_packets == 180);
    /* Remove only files generated in this test's private mkdtemp directory. */
    dp = opendir(dir);
    CHECK(dp);
    while ((ent = readdir(dp))) {
        if (!strstr(ent->d_name, ".mp4")) continue;
        char filename[512];
        snprintf(filename, sizeof(filename), "%s/%s", dir, ent->d_name);
        CHECK(unlink(filename) == 0);
    }
    closedir(dp);
    CHECK(rmdir(dir) == 0);
    puts("PASS: 1/1000 time base, timestamp reset, six playable segments, bitrate and reconnect sessions");
    return 0;
}

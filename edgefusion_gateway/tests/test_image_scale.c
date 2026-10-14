/* 色彩范围回归测试。
 *
 * 背景（板端实测，libswscale 7.5.100 / FFmpeg 6.1）：sws_scale() 在输入输出像素格式
 * 相同（含 YUVJ→YUV 归一化后相同）时不做 srcRange↔dstRange 的电平转换，且在 YUVJ 上
 * 会打印 deprecated 告警。因此本项目显式实现 image_range_convert()，本测试同时覆盖：
 *   1. image_range_convert 的 Y/U/V 电平（两个方向 + 空操作）；
 *   2. image_scale_prepare + sws_scale + image_range_convert 的端到端结果；
 *   3. 同参数重复配置必须复用同一个 sws 上下文；
 *   4. 全程不出现 deprecated pixel format 告警。
 * 不依赖摄像头与网络。 */
#include "image_scale.h"

#include <libavutil/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static int deprecated_warnings;

static void log_capture(void *ptr, int level, const char *fmt, va_list args)
{
    (void)ptr; (void)level;
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, args);
    if (strstr(msg, "deprecated pixel format")) ++deprecated_warnings;
}

/* 上半黑、下半白、色度中性 */
static AVFrame *make_src(enum AVPixelFormat fmt, int black, int white)
{
    AVFrame *f = av_frame_alloc();
    CHECK(f);
    f->format = fmt;
    f->width = 32;
    f->height = 32;
    CHECK(av_frame_get_buffer(f, 32) == 0);
    for (int y = 0; y < 32; ++y)
        memset(f->data[0] + y * f->linesize[0], y < 16 ? black : white, 32);
    int chroma_h = (fmt == AV_PIX_FMT_YUVJ422P || fmt == AV_PIX_FMT_YUV422P) ? 32 : 16;
    for (int p = 1; p < 3; ++p)
        for (int y = 0; y < chroma_h; ++y)
            memset(f->data[p] + y * f->linesize[p], 128, 16);
    return f;
}

/* 1) 只测 image_range_convert：三平面电平 */
static void check_range_convert(int src_full, int dst_full,
                                int y_in, int exp_y, int c_in, int exp_c)
{
    AVFrame *f = av_frame_alloc();
    CHECK(f);
    f->format = AV_PIX_FMT_YUV420P;      /* 与 YUVJ420P 布局相同 */
    f->width = f->height = 32;
    CHECK(av_frame_get_buffer(f, 32) == 0);
    for (int p = 0; p < 3; ++p) {
        int w = p ? 16 : 32, h = p ? 16 : 32;
        for (int y = 0; y < h; ++y)
            memset(f->data[p] + y * f->linesize[p], p ? c_in : y_in, w);
    }
    image_range_convert(f, src_full, dst_full);
    int got_y = f->data[0][0], got_u = f->data[1][0], got_v = f->data[2][0];
    printf("range_convert full=%d->%d  Y %3d->%3d(exp %3d)  U %3d->%3d(exp %3d)\n",
           src_full, dst_full, y_in, got_y, exp_y, c_in, got_u, exp_c);
    CHECK(abs(got_y - exp_y) <= 2);
    CHECK(abs(got_u - exp_c) <= 2);
    CHECK(abs(got_v - exp_c) <= 2);
    av_frame_free(&f);
}

/* 2) 端到端：prepare + sws_scale + range convert */
static void check_pipeline(const char *name, enum AVPixelFormat src_fmt, int src_full,
                           int black, int white, int exp_black, int exp_white)
{
    struct SwsContext *ctx = NULL;
    AVFrame *src = make_src(src_fmt, black, white);
    AVFrame *dst = av_frame_alloc();
    CHECK(dst);
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width = dst->height = 32;
    CHECK(av_frame_get_buffer(dst, 32) == 0);

    CHECK(image_scale_prepare(&ctx, src, 32, 32, dst->format, src_full) >= 0);
    CHECK(sws_scale(ctx, (const uint8_t *const *)src->data, src->linesize, 0, 32,
                    dst->data, dst->linesize) == 32);
    image_range_convert(dst, src_full, 1);

    int got_b = dst->data[0][4 * dst->linesize[0] + 4];
    int got_w = dst->data[0][24 * dst->linesize[0] + 4];
    int ok = abs(got_b - exp_black) <= 2 && abs(got_w - exp_white) <= 2;
    printf("pipeline %-32s black=%3d(exp %3d) white=%3d(exp %3d) %s\n",
           name, got_b, exp_black, got_w, exp_white, ok ? "OK" : "MISMATCH");
    CHECK(ok);

    sws_freeContext(ctx);
    av_frame_free(&src);
    av_frame_free(&dst);
}

/* 3) 同参数必须复用同一个上下文（否则每帧重建 scaler） */
static void check_context_reuse(void)
{
    struct SwsContext *ctx = NULL;
    AVFrame *src = make_src(AV_PIX_FMT_YUVJ420P, 0, 255);
    AVFrame *dst = av_frame_alloc();
    CHECK(dst);
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width = dst->height = 32;
    CHECK(av_frame_get_buffer(dst, 32) == 0);

    CHECK(image_scale_prepare(&ctx, src, 32, 32, dst->format, 1) >= 0);
    struct SwsContext *first = ctx;
    for (int i = 0; i < 10; ++i) {
        CHECK(image_scale_prepare(&ctx, src, 32, 32, dst->format, 1) >= 0);
        CHECK(ctx == first);
        CHECK(sws_scale(ctx, (const uint8_t *const *)src->data, src->linesize, 0, 32,
                        dst->data, dst->linesize) == 32);
    }
    printf("context reuse OK: same sws pointer across 11 configures\n");
    sws_freeContext(ctx);
    av_frame_free(&src);
    av_frame_free(&dst);
}

int main(void)
{
    av_log_set_callback(log_capture);

    /* full -> limited：Y 0→16、255→235；C 0→16、255→240 */
    check_range_convert(1, 0, 0, 16, 0, 16);
    check_range_convert(1, 0, 255, 235, 255, 240);
    /* limited -> full：Y 16→0、235→255；C 128→128、240→255 */
    check_range_convert(0, 1, 16, 0, 128, 128);
    check_range_convert(0, 1, 235, 255, 240, 255);
    /* 同范围：空操作 */
    check_range_convert(0, 0, 123, 123, 77, 77);

    /* USB 链路：相机 JPEG（全范围）→ 编码器（全范围），不应改电平 */
    check_pipeline("YUVJ420P full -> full", AV_PIX_FMT_YUVJ420P, 1, 0, 255, 0, 255);
    check_pipeline("YUVJ422P full -> full", AV_PIX_FMT_YUVJ422P, 1, 0, 255, 0, 255);
    /* 预览链路：限范围来源 → JPEG（全范围）必须提升电平 */
    check_pipeline("YUV420P limited -> full", AV_PIX_FMT_YUV420P, 0, 16, 235, 0, 255);
    /* 已是全范围的来源 → 空操作 */
    check_pipeline("YUV420P full -> full", AV_PIX_FMT_YUV420P, 1, 0, 255, 0, 255);

    check_context_reuse();

    av_log_set_callback(av_log_default_callback);
    CHECK(deprecated_warnings == 0);
    puts("PASS: range convert levels, cached scaler reuse, no deprecated-format warnings");
    return 0;
}

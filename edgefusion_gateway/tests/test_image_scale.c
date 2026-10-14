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

static void check_range(struct SwsContext **ctx, enum AVPixelFormat fmt,
                        enum AVColorRange range, int full_out,
                        int black, int white, int expected_black, int expected_white)
{
    AVFrame *src = av_frame_alloc(), *dst = av_frame_alloc();
    CHECK(src && dst);
    src->format = fmt; src->color_range = range;
    src->width = src->height = dst->width = dst->height = 32;
    dst->format = AV_PIX_FMT_YUV420P;
    CHECK(av_frame_get_buffer(src, 32) == 0);
    CHECK(av_frame_get_buffer(dst, 32) == 0);
    for (int y = 0; y < 32; ++y)
        memset(src->data[0] + y * src->linesize[0], y < 16 ? black : white, 32);
    int chroma_h = fmt == AV_PIX_FMT_YUVJ422P ? 32 : 16;
    for (int p = 1; p < 3; ++p)
        for (int y = 0; y < chroma_h; ++y)
            memset(src->data[p] + y * src->linesize[p], 128, 16);
    CHECK(image_scale_prepare(ctx, src, 32, 32, dst->format, full_out) >= 0);
    struct SwsContext *cached = *ctx;
    for (int i = 0; i < 10; ++i) {
        CHECK(image_scale_prepare(ctx, src, 32, 32, dst->format, full_out) >= 0);
        CHECK(*ctx == cached);
        CHECK(sws_scale(*ctx, (const uint8_t *const *)src->data, src->linesize,
                        0, 32, dst->data, dst->linesize) == 32);
        CHECK(abs(dst->data[0][4 * dst->linesize[0] + 4] - expected_black) <= 2);
        CHECK(abs(dst->data[0][24 * dst->linesize[0] + 4] - expected_white) <= 2);
    }
    av_frame_free(&src);
    av_frame_free(&dst);
}

int main(void)
{
    struct SwsContext *ctx = NULL;
    av_log_set_callback(log_capture);
    check_range(&ctx, AV_PIX_FMT_YUVJ420P, AVCOL_RANGE_UNSPECIFIED, 0, 0, 255, 16, 235);
    check_range(&ctx, AV_PIX_FMT_YUV420P, AVCOL_RANGE_MPEG, 1, 16, 235, 0, 255);
    check_range(&ctx, AV_PIX_FMT_YUV420P, AVCOL_RANGE_JPEG, 0, 0, 255, 16, 235);
    check_range(&ctx, AV_PIX_FMT_YUV420P, AVCOL_RANGE_MPEG, 0, 16, 235, 16, 235);
    check_range(&ctx, AV_PIX_FMT_YUVJ422P, AVCOL_RANGE_UNSPECIFIED, 0, 0, 255, 16, 235);
    sws_freeContext(ctx);
    av_log_set_callback(av_log_default_callback);
    CHECK(deprecated_warnings == 0);
    puts("PASS: full/limited range levels, cached scaler reuse, no deprecated-format warnings");
    return 0;
}

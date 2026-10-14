#ifndef EDGEFUSION_IMAGE_SCALE_H
#define EDGEFUSION_IMAGE_SCALE_H

#include <libavutil/frame.h>
#include <libswscale/swscale.h>

/* YUVJ has the same memory layout as YUV, but implies JPEG/full range.
 * Normalize only the scaler's format argument; preserve frame metadata.
 * Passing YUVJ repeatedly can defeat sws_getCachedContext's format check
 * because swscale normalizes its internal format to YUV during initialization. */
static inline enum AVPixelFormat image_scale_format(enum AVPixelFormat fmt)
{
    switch (fmt) {
    case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
    case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
    case AV_PIX_FMT_YUVJ411P: return AV_PIX_FMT_YUV411P;
    default: return fmt;
    }
}

/* Configure range explicitly: JPEG/full=1, MPEG/limited=0.
 * Leave the existing color matrices unchanged; this is a range conversion,
 * not an attempt to infer/convert BT.601 vs BT.709. */
static inline int image_scale_prepare(struct SwsContext **ctx, const AVFrame *src,
                                      int width, int height, enum AVPixelFormat dst,
                                      int dst_full_range)
{
    enum AVPixelFormat fmt = image_scale_format(src->format);
    int full = src->color_range == AVCOL_RANGE_JPEG ||
               (src->color_range == AVCOL_RANGE_UNSPECIFIED && fmt != src->format);
    *ctx = sws_getCachedContext(*ctx, src->width, src->height, fmt,
                               width, height, image_scale_format(dst),
                               SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!*ctx) return -1;
    int *inv_table, *table, old_src_range, old_dst_range;
    int brightness, contrast, saturation;
    if (sws_getColorspaceDetails(*ctx, &inv_table, &old_src_range, &table,
                                &old_dst_range, &brightness, &contrast, &saturation) < 0)
        return -1;
    return sws_setColorspaceDetails(*ctx, inv_table, full, table, dst_full_range,
                                   brightness, contrast, saturation);
}
#endif

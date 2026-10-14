#ifndef EDGEFUSION_IMAGE_SCALE_H
#define EDGEFUSION_IMAGE_SCALE_H

#include <libavutil/common.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <math.h>

/* YUVJ 与 YUV 内存布局相同，只是隐含 full range。
 * 只把交给 swscale 的格式参数规范化，避免反复传 YUVJ（否则 swscale 初始化时把内部格式
 * 归一成 YUV，会让 sws_getCachedContext 每次都重建上下文）。 */
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

/* 配置 scaler：只做格式/尺寸转换，**不做电平（range）转换**。
 *
 * 板端实测（libswscale 7.5.100 / FFmpeg 6.1）：当 scaler 输入输出像素格式相同
 * （含 YUVJ→YUV 归一化后相同）时，sws_scale() 根本不执行 srcRange↔dstRange 的电平转换；
 * 只有格式真的不同（如 4:2:2→4:2:0）时才顺带转换。依赖它做范围转换会静默失效，
 * 于是这里把 srcRange 与 dstRange 设成同一个值，把范围转换交给 image_range_convert()
 * 显式完成——行为与 swscale 的快速路径无关。
 *
 * 调用约定：先 image_scale_prepare() + sws_scale()，再对目标帧调用
 * image_range_convert(dst, src_full, dst_full)。 */
static inline int image_scale_prepare(struct SwsContext **ctx, const AVFrame *src,
                                      int width, int height, enum AVPixelFormat dst,
                                      int src_full_range)
{
    enum AVPixelFormat fmt = image_scale_format(src->format);
    *ctx = sws_getCachedContext(*ctx, src->width, src->height, fmt,
                                width, height, image_scale_format(dst),
                                SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!*ctx) return -1;

    int *inv_table, *table, old_src_range, old_dst_range;
    int brightness, contrast, saturation;
    if (sws_getColorspaceDetails(*ctx, &inv_table, &old_src_range, &table,
                                 &old_dst_range, &brightness, &contrast, &saturation) < 0)
        return -1;
    /* 同一范围进同一范围出：告诉 swscale 不需要做电平转换，保留原有颜色矩阵 */
    return sws_setColorspaceDetails(*ctx, inv_table, src_full_range, table, src_full_range,
                                    brightness, contrast, saturation);
}

/* 就地完成 YUV 电平转换（full 0–255 ↔ limited 16–235；色度 128±112 ↔ 128±127）。
 * src_full == dst_full 时是空操作。确定性实现，不依赖 swscale 的内部快速路径。 */
static inline void image_range_convert(AVFrame *f, int src_full, int dst_full)
{
    if (!f || src_full == dst_full || !f->data[0]) return;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(f->format);
    if (!desc || !(desc->flags & AV_PIX_FMT_FLAG_PLANAR)) return;

    uint8_t luma[256], chroma[256];
    for (int v = 0; v < 256; ++v) {
        double y, c;
        if (src_full && !dst_full) {                 /* full -> limited */
            y = 16.0 + v * 219.0 / 255.0;
            c = 128.0 + (v - 128) * 224.0 / 255.0;
        } else {                                     /* limited -> full */
            y = (v - 16.0) * 255.0 / 219.0;
            c = 128.0 + (v - 128) * 255.0 / 224.0;
        }
        int yi = (int)lrint(y);
        int ci = (int)lrint(c);
        luma[v]   = (uint8_t)(yi < 0 ? 0 : (yi > 255 ? 255 : yi));
        chroma[v] = (uint8_t)(ci < 0 ? 0 : (ci > 255 ? 255 : ci));
    }

    int has_chroma = desc->nb_components >= 3;
    for (int p = 0; p < 3; ++p) {
        if ((p == 1 || p == 2) && !has_chroma) break;
        if (!f->data[p] || f->linesize[p] == 0) break;
        int plane_w = p ? AV_CEIL_RSHIFT(f->width, desc->log2_chroma_w) : f->width;
        int plane_h = p ? AV_CEIL_RSHIFT(f->height, desc->log2_chroma_h) : f->height;
        const uint8_t *lut = p ? chroma : luma;
        uint8_t *row = f->data[p];
        for (int y = 0; y < plane_h; ++y) {
            for (int x = 0; x < plane_w; ++x) row[x] = lut[row[x]];
            row += f->linesize[p];
        }
    }
}

#endif

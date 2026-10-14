#ifndef EDGEFUSION_USB_SOURCE_H
#define EDGEFUSION_USB_SOURCE_H

#include "conf.h"
#include <libavcodec/avcodec.h>
#include <stddef.h>
#include <stdint.h>

/* Single-owner USB capture + MJPEG decode + H.264 software encode pipeline.
 * The receiver thread owns this object; no V4L2 buffer escapes read(). */
typedef struct usb_source usb_source_t;
usb_source_t *usb_source_open(const gateway_conf_t *cfg);
const AVCodecParameters *usb_source_codecpar(const usb_source_t *s);
AVRational usb_source_time_base(const usb_source_t *s);
/* 1 = encoded packet, 0 = retry (bounded poll), negative = reconnect.
 * Caller owns packet references and must av_packet_unref() after use. */
int usb_source_read(usb_source_t *s, AVPacket *out);
void usb_source_close(usb_source_t *s);

/* 相机原始 MJPEG 回调：在解码前调用，供预览直接使用相机 JPEG（省掉解码+重编码）。
 * 回调在采集线程内同步执行，必须尽快返回并自行拷贝需要的数据。传 NULL 取消注册。 */
typedef void (*usb_source_jpeg_cb_t)(void *ctx, const uint8_t *data, size_t size, int64_t ts_us);
void usb_source_set_jpeg_cb(usb_source_t *s, usb_source_jpeg_cb_t cb, void *ctx);

#endif

#ifndef EDGEFUSION_USB_SOURCE_H
#define EDGEFUSION_USB_SOURCE_H

#include "conf.h"
#include <libavcodec/avcodec.h>

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

#endif

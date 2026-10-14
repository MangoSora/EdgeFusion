#ifndef EDGEFUSION_TX_QUEUE_H
#define EDGEFUSION_TX_QUEUE_H

#include <stddef.h>

/* 有界字节发送队列（线程安全）。
 *
 * 用途：RTSP 发送侧把"待发送的字节"入队，由各客户端自己的连接线程在 socket 可写时排空。
 * 这样推流侧（预览线程）永远不会阻塞在 send() 上——慢客户端只会让它自己的队列溢出，
 * 不影响其他客户端，也不影响状态接口。
 *
 * 溢出策略：容量不足时先尝试压缩，仍放不下则丢弃**整个积压**再放入本次数据。
 *   理由：这是直播流而不是点播，积压本身就意味着延迟；丢弃后客户端会等到下一个关键帧
 *   重新同步（编码端配置了 repeat-headers，关键帧带 SPS/PPS）。溢出次数与字节数会累计，
 *   上层据此判定"慢客户端"并踢出。
 */

typedef struct tx_queue tx_queue_t;

typedef struct {
    size_t             pending;        /* 当前积压字节数 */
    size_t             high_watermark; /* 历史最大积压 */
    unsigned long long pushed_bytes;   /* 累计入队字节 */
    unsigned long long dropped_bytes;  /* 因溢出被丢弃的字节 */
    unsigned int       overflows;      /* 触发整段清空的次数 */
} tx_queue_stats_t;

/* 创建队列；capacity 为 0 时按 4096 处理。失败返回 NULL。 */
tx_queue_t *tx_queue_create(size_t capacity);

/* 销毁队列。允许传 NULL。 */
void tx_queue_destroy(tx_queue_t *q);

/* 入队（永不阻塞）。返回 0 表示已接受（含溢出丢弃的语义），-1 表示参数非法。 */
int tx_queue_push(tx_queue_t *q, const void *data, size_t len);

/* 取当前可直接发送的连续区段（内部地址）。返回长度，0 表示无数据。
 * 调用方发送成功后必须用 tx_queue_consume() 归还已发送字节。 */
size_t tx_queue_peek(tx_queue_t *q, const unsigned char **data);

/* 确认已发送 n 字节（n 超过当前积压时按积压长度处理）。 */
void tx_queue_consume(tx_queue_t *q, size_t n);

/* 丢弃全部积压，返回被丢弃的字节数。 */
size_t tx_queue_clear(tx_queue_t *q);

/* 取统计快照。 */
void tx_queue_get_stats(tx_queue_t *q, tx_queue_stats_t *out);

#endif

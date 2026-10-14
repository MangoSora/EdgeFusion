#ifndef EDGEFUSION_PKT_QUEUE_H
#define EDGEFUSION_PKT_QUEUE_H

#include <libavcodec/avcodec.h>

/* 有界包队列（单生产者 / 单消费者）。
 *
 * 用途：把“H.264 解码预览 + RTSP 转发”从采集线程剥离到独立线程。预览与转发
 * 属于可丢路径：消费端一慢，丢旧帧比拖住采集线程更划算——采集被拖住会让 V4L2
 * 缓冲区耗尽，驱动层直接丢帧，应用侧连计数都拿不到。
 *
 * 所有权约定（面试常问的三句）：
 *   - push() 无论入队成功还是因满被丢弃，都消费掉传入的包引用；调用者不得再
 *     使用或释放该指针。
 *   - pop() 返回的包归调用者所有，用完必须 av_packet_free()。
 *   - destroy() 释放队列里尚未取走的包，不会泄漏给调用者。
 *
 * 队列满时的策略是“丢最旧”（FIFO 溢出），因为预览关心的是最新画面而不是完整
 * 序列；录像路径不使用本队列，因此录像不会因预览变慢而丢包。
 */

typedef struct pkt_queue pkt_queue_t;

typedef struct {
    unsigned long long pushed;   /* 成功入队的包数（不含被丢弃的） */
    unsigned long long dropped;  /* 因队列满被丢弃的旧包数 */
    unsigned long long popped;   /* 已被消费者取走的包数 */
    int                depth;    /* 当前排队包数 */
    int                high_watermark; /* 队列曾达到的最大深度 */
} pkt_queue_stats_t;

/* 创建队列；capacity < 2 时按 2 处理。失败返回 NULL。 */
pkt_queue_t *pkt_queue_create(int capacity);

/* 销毁队列并释放其中未取走的包。允许传 NULL。 */
void pkt_queue_destroy(pkt_queue_t *q);

/* 入队，is_key 记录该包是否为关键帧（供消费端判断丢包后是否需要等关键帧）。
 * 返回 0 表示已入队；返回 -1 表示队列已停止，pkt 已被释放。 */
int pkt_queue_push(pkt_queue_t *q, AVPacket *pkt, int is_key);

/* 取包：无数据时阻塞等待。返回 0 表示 *out 有效；返回 1 表示队列已停止且已排空。 */
int pkt_queue_pop(pkt_queue_t *q, AVPacket **out, int *is_key);

/* 停止队列：唤醒阻塞中的消费者并拒绝后续入队；已入队的包仍会被消费者取完。 */
void pkt_queue_stop(pkt_queue_t *q);

/* 是否已停止。 */
int pkt_queue_stopped(pkt_queue_t *q);

/* 取统计快照（可在生产者线程调用，用于上报 /api/status）。 */
void pkt_queue_get_stats(pkt_queue_t *q, pkt_queue_stats_t *out);

#endif

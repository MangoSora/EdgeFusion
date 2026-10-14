#include "tx_queue.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct tx_queue {
    pthread_mutex_t    mtx;
    unsigned char     *buf;
    size_t             cap;
    size_t             head;   /* 已发送偏移 */
    size_t             len;    /* 未发送字节数 */
    tx_queue_stats_t   stats;
};

tx_queue_t *tx_queue_create(size_t capacity)
{
    if (capacity == 0) capacity = 4096;
    tx_queue_t *q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->buf = malloc(capacity);
    if (!q->buf) { free(q); return NULL; }
    q->cap = capacity;
    pthread_mutex_init(&q->mtx, NULL);
    return q;
}

void tx_queue_destroy(tx_queue_t *q)
{
    if (!q) return;
    pthread_mutex_destroy(&q->mtx);
    free(q->buf);
    free(q);
}

int tx_queue_push(tx_queue_t *q, const void *data, size_t len)
{
    if (!q || (!data && len > 0)) return -1;
    if (len == 0) return 0;

    pthread_mutex_lock(&q->mtx);

    if (len > q->cap) {
        /* 单个包比整个队列还大：正常 RTP 包不会出现，直接丢弃并计数 */
        q->stats.dropped_bytes += len;
        q->stats.overflows++;
        pthread_mutex_unlock(&q->mtx);
        return 0;
    }

    if (q->head + q->len + len > q->cap) {
        if (q->len + len <= q->cap) {
            /* 前移压缩，腾出连续空间 */
            memmove(q->buf, q->buf + q->head, q->len);
            q->head = 0;
        } else {
            /* 溢出：丢弃整个积压，保留本次数据（客户端等下一个关键帧重同步） */
            q->stats.dropped_bytes += q->len;
            q->stats.overflows++;
            q->head = 0;
            q->len = 0;
        }
    }

    memcpy(q->buf + q->head + q->len, data, len);
    q->len += len;
    q->stats.pushed_bytes += len;
    if (q->len > q->stats.high_watermark) q->stats.high_watermark = q->len;

    pthread_mutex_unlock(&q->mtx);
    return 0;
}

size_t tx_queue_peek(tx_queue_t *q, const unsigned char **data)
{
    if (!q || !data) return 0;
    pthread_mutex_lock(&q->mtx);
    size_t n = q->len;
    if (n > 0) *data = q->buf + q->head;
    pthread_mutex_unlock(&q->mtx);
    return n;
}

void tx_queue_consume(tx_queue_t *q, size_t n)
{
    if (!q) return;
    pthread_mutex_lock(&q->mtx);
    if (n > q->len) n = q->len;
    q->head += n;
    q->len -= n;
    if (q->len == 0) q->head = 0;   /* 空队列时回到缓冲起点，避免长跑后偏移漂移 */
    pthread_mutex_unlock(&q->mtx);
}

size_t tx_queue_clear(tx_queue_t *q)
{
    if (!q) return 0;
    pthread_mutex_lock(&q->mtx);
    size_t dropped = q->len;
    q->stats.dropped_bytes += dropped;
    q->head = 0;
    q->len = 0;
    pthread_mutex_unlock(&q->mtx);
    return dropped;
}

void tx_queue_get_stats(tx_queue_t *q, tx_queue_stats_t *out)
{
    if (!q || !out) return;
    pthread_mutex_lock(&q->mtx);
    *out = q->stats;
    out->pending = q->len;
    pthread_mutex_unlock(&q->mtx);
}

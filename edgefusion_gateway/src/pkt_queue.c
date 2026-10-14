#include "pkt_queue.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct pkt_queue {
    pthread_mutex_t   mtx;
    pthread_cond_t    cv;        /* 有新包或已停止时唤醒消费者 */
    AVPacket        **slots;
    int              *keys;
    int               capacity;
    int               head;      /* 下一个写入位置 */
    int               tail;      /* 下一个读取位置 */
    int               count;     /* 当前排队包数 */
    int               stopped;
    pkt_queue_stats_t stats;
};

static void pkt_release(AVPacket *pkt)
{
    if (pkt) av_packet_free(&pkt);
}

pkt_queue_t *pkt_queue_create(int capacity)
{
    if (capacity < 2) capacity = 2;
    pkt_queue_t *q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->slots = calloc((size_t)capacity, sizeof(*q->slots));
    q->keys  = calloc((size_t)capacity, sizeof(*q->keys));
    if (!q->slots || !q->keys) {
        free(q->slots);
        free(q->keys);
        free(q);
        return NULL;
    }
    q->capacity = capacity;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->cv, NULL);
    return q;
}

void pkt_queue_destroy(pkt_queue_t *q)
{
    if (!q) return;
    for (int i = 0; i < q->count; ++i)
        pkt_release(q->slots[(q->tail + i) % q->capacity]);
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->cv);
    free(q->slots);
    free(q->keys);
    free(q);
}

int pkt_queue_push(pkt_queue_t *q, AVPacket *pkt, int is_key)
{
    if (!q || !pkt) return -1;

    pthread_mutex_lock(&q->mtx);
    if (q->stopped) {
        pthread_mutex_unlock(&q->mtx);
        pkt_release(pkt);       /* 已停止：仍然消费掉引用，调用者不必关心 */
        return -1;
    }
    if (q->count == q->capacity) {
        /* 丢最旧：保持预览贴近实时，积压越少延迟越低 */
        pkt_release(q->slots[q->tail]);
        q->slots[q->tail] = NULL;
        q->tail = (q->tail + 1) % q->capacity;
        q->count--;
        q->stats.dropped++;
    }
    q->slots[q->head] = pkt;
    q->keys[q->head]  = is_key;
    q->head = (q->head + 1) % q->capacity;
    q->count++;
    q->stats.pushed++;
    if (q->count > q->stats.high_watermark) q->stats.high_watermark = q->count;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

int pkt_queue_pop(pkt_queue_t *q, AVPacket **out, int *is_key)
{
    if (!q || !out) return -1;
    *out = NULL;

    pthread_mutex_lock(&q->mtx);
    while (q->count == 0 && !q->stopped)
        pthread_cond_wait(&q->cv, &q->mtx);
    if (q->count == 0) {                 /* 已停止且排空 */
        pthread_mutex_unlock(&q->mtx);
        return 1;
    }
    *out = q->slots[q->tail];
    if (is_key) *is_key = q->keys[q->tail];
    q->slots[q->tail] = NULL;
    q->tail = (q->tail + 1) % q->capacity;
    q->count--;
    q->stats.popped++;
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

void pkt_queue_stop(pkt_queue_t *q)
{
    if (!q) return;
    pthread_mutex_lock(&q->mtx);
    q->stopped = 1;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mtx);
}

int pkt_queue_stopped(pkt_queue_t *q)
{
    if (!q) return 1;
    pthread_mutex_lock(&q->mtx);
    int stopped = q->stopped;
    pthread_mutex_unlock(&q->mtx);
    return stopped;
}

void pkt_queue_get_stats(pkt_queue_t *q, pkt_queue_stats_t *out)
{
    if (!q || !out) return;
    pthread_mutex_lock(&q->mtx);
    *out = q->stats;
    out->depth = q->count;
    pthread_mutex_unlock(&q->mtx);
}

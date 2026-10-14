/* tx_queue 回归测试：入队/排空/压缩、溢出策略、超大包、统计自洽与并发。
 * 纯逻辑，不依赖摄像头、网络与 FFmpeg。 */
#include "tx_queue.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

/* 用可校验的字节序列填充：第 i 个字节 = (seed + i) & 0xff */
static void fill(unsigned char *p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; ++i) p[i] = (unsigned char)((seed + i) & 0xff);
}

static void check_region(const unsigned char *p, size_t n, unsigned seed, int line)
{
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != (unsigned char)((seed + i) & 0xff)) {
            fprintf(stderr, "FAIL line %d: byte %zu mismatch (%u != %u)\n",
                    line, i, p[i], (unsigned char)((seed + i) & 0xff));
            exit(1);
        }
    }
}

/* 1) 基本入队 / peek+consume / 顺序 */
static void test_basic(void)
{
    tx_queue_t *q = tx_queue_create(256);
    CHECK(q);
    unsigned char a[32], b[16];
    fill(a, sizeof(a), 1);
    fill(b, sizeof(b), 100);
    CHECK(tx_queue_push(q, a, sizeof(a)) == 0);
    CHECK(tx_queue_push(q, b, sizeof(b)) == 0);

    tx_queue_stats_t st;
    tx_queue_get_stats(q, &st);
    CHECK(st.pending == 48);
    CHECK(st.high_watermark == 48);

    const unsigned char *p = NULL;
    CHECK(tx_queue_peek(q, &p) == 48);
    check_region(p, sizeof(a), 1, __LINE__);
    check_region(p + sizeof(a), sizeof(b), 100, __LINE__);

    /* 只确认前半，剩下的必须还在 */
    tx_queue_consume(q, 20);
    CHECK(tx_queue_peek(q, &p) == 28);
    CHECK(p[0] == (unsigned char)((1 + 20) & 0xff));

    tx_queue_consume(q, 28);
    CHECK(tx_queue_peek(q, &p) == 0);
    tx_queue_destroy(q);
    puts("PASS: tx_queue basic push/peek/consume keeps byte order");
}

/* 2) 压缩：部分发送后继续入队，数据不能被搬坏（容量 64，净积压 48，中途触发压缩且不溢出） */
static void test_compaction(void)
{
    tx_queue_t *q = tx_queue_create(64);
    CHECK(q);
    unsigned char chunk[16];
    for (unsigned seed = 0; seed < 6; ++seed) {
        fill(chunk, sizeof(chunk), (unsigned)(seed * 16));
        CHECK(tx_queue_push(q, chunk, sizeof(chunk)) == 0);
        tx_queue_consume(q, 8);            /* 制造 head 偏移，迫使后续压缩 */
    }
    tx_queue_stats_t st;
    tx_queue_get_stats(q, &st);
    CHECK(st.overflows == 0);              /* 压缩应当避免溢出 */
    CHECK(st.pending == 48);
    const unsigned char *p = NULL;
    size_t n = tx_queue_peek(q, &p);
    CHECK(n == 48);
    check_region(p + 32, 16, 5 * 16, __LINE__);   /* 末段数据仍是最后推入的那批 */
    tx_queue_destroy(q);
    puts("PASS: tx_queue compacts without corrupting pending data");
}

/* 3) 溢出：容量不足时丢弃整个积压并计数 */
static void test_overflow(void)
{
    tx_queue_t *q = tx_queue_create(100);
    CHECK(q);
    unsigned char a[60], b[60];
    fill(a, sizeof(a), 10);
    fill(b, sizeof(b), 200);
    CHECK(tx_queue_push(q, a, sizeof(a)) == 0);
    CHECK(tx_queue_push(q, b, sizeof(b)) == 0);   /* 放不下 a+b → 丢弃 a */

    tx_queue_stats_t st;
    tx_queue_get_stats(q, &st);
    CHECK(st.overflows == 1);
    CHECK(st.dropped_bytes == sizeof(a));
    CHECK(st.pending == sizeof(b));

    const unsigned char *p = NULL;
    CHECK(tx_queue_peek(q, &p) == sizeof(b));
    check_region(p, sizeof(b), 200, __LINE__);
    tx_queue_destroy(q);
    puts("PASS: tx_queue drops the whole backlog on overflow and counts it");
}

/* 4) 单个包大于容量：丢弃并计数，队列保持可用 */
static void test_oversized(void)
{
    tx_queue_t *q = tx_queue_create(64);
    CHECK(q);
    unsigned char big[128];
    fill(big, sizeof(big), 5);
    CHECK(tx_queue_push(q, big, sizeof(big)) == 0);
    tx_queue_stats_t st;
    tx_queue_get_stats(q, &st);
    CHECK(st.pending == 0);
    CHECK(st.overflows == 1);
    CHECK(st.dropped_bytes == sizeof(big));
    CHECK(tx_queue_push(q, big, 16) == 0);        /* 队列仍然可用 */
    const unsigned char *p = NULL;
    CHECK(tx_queue_peek(q, &p) == 16);
    check_region(p, 16, 5, __LINE__);
    tx_queue_destroy(q);
    puts("PASS: tx_queue rejects oversized packets but stays usable");
}

/* 5) clear 与空操作边界 */
static void test_clear_and_edges(void)
{
    tx_queue_t *q = tx_queue_create(32);
    CHECK(q);
    CHECK(tx_queue_push(q, NULL, 0) == 0);
    CHECK(tx_queue_push(NULL, "x", 1) == -1);
    CHECK(tx_queue_push(q, "x", 1) == 0);
    CHECK(tx_queue_clear(q) == 1);
    CHECK(tx_queue_clear(q) == 0);
    tx_queue_stats_t st;
    tx_queue_get_stats(q, &st);
    CHECK(st.pending == 0 && st.dropped_bytes == 1);
    tx_queue_destroy(NULL);                       /* 允许 NULL */
    tx_queue_destroy(q);
    puts("PASS: tx_queue clear and NULL/edge handling");
}

/* 6) 并发：生产者入队、消费者排空，守恒式 consumed + dropped == pushed */
typedef struct {
    tx_queue_t        *q;
    unsigned long long consumed;
    volatile int      *producer_done;
} cons_t;

static void *producer_main(void *arg)
{
    tx_queue_t *q = arg;
    unsigned char pkt[200];
    for (unsigned i = 0; i < 20000; ++i) {
        fill(pkt, sizeof(pkt), i);
        tx_queue_push(q, pkt, sizeof(pkt));
    }
    return NULL;
}

static void *consumer_main(void *arg)
{
    cons_t *c = arg;
    for (;;) {
        const unsigned char *p = NULL;
        size_t n = tx_queue_peek(c->q, &p);
        if (n > 0) {
            tx_queue_consume(c->q, n);
            c->consumed += n;
            continue;
        }
        if (*c->producer_done) break;     /* 生产者已结束且队列已排空 */
        usleep(500);
    }
    return NULL;
}

static void test_concurrent(void)
{
    tx_queue_t *q = tx_queue_create(4096);
    CHECK(q);
    volatile int done = 0;
    cons_t c = { .q = q, .producer_done = &done };
    pthread_t pt, ct;
    CHECK(pthread_create(&pt, NULL, producer_main, q) == 0);
    CHECK(pthread_create(&ct, NULL, consumer_main, &c) == 0);
    CHECK(pthread_join(pt, NULL) == 0);
    done = 1;
    CHECK(pthread_join(ct, NULL) == 0);

    tx_queue_stats_t st;
    tx_queue_get_stats(q, &st);
    printf("concurrent: pushed=%llu dropped=%llu consumed=%llu overflows=%u high=%zu\n",
           st.pushed_bytes, st.dropped_bytes, c.consumed, st.overflows, st.high_watermark);
    CHECK(st.pushed_bytes > 0);
    CHECK(c.consumed + st.dropped_bytes == st.pushed_bytes);
    tx_queue_destroy(q);
    puts("PASS: tx_queue concurrent accounting is consistent");
}

int main(void)
{
    test_basic();
    test_compaction();
    test_overflow();
    test_oversized();
    test_clear_and_edges();
    test_concurrent();
    puts("PASS: tx_queue bounded queue, overflow policy, compaction and concurrency");
    return 0;
}

/* pkt_queue 回归测试：有界性、丢最旧策略、所有权转移、停止唤醒、停止后仍排空，
 * 以及生产者/消费者并发压力下的顺序与计数自洽。
 * 不依赖摄像头与网络，可在板端直接运行。 */
#include "pkt_queue.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 每个测试包在 payload 首字节写入 id，用来验证取出的顺序 */
static AVPacket *make_pkt(int id)
{
    AVPacket *pkt = av_packet_alloc();
    CHECK(pkt);
    CHECK(av_new_packet(pkt, 8) == 0);
    memset(pkt->data, id & 0xff, 8);
    return pkt;
}

typedef struct {
    pkt_queue_t *q;
    int          first_id;
    int          count;
    int          rc;      /* 消费者退出码：1 = 队列已停止且排空 */
} consumer_t;

static void *consumer_main(void *arg)
{
    consumer_t *c = arg;
    for (;;) {
        AVPacket *pkt = NULL;
        int is_key = 0;
        int rc = pkt_queue_pop(c->q, &pkt, &is_key);
        if (rc != 0) { c->rc = rc; break; }
        if (c->count == 0) c->first_id = pkt->data[0];
        CHECK(pkt->size == 8);
        c->count++;
        av_packet_free(&pkt);
    }
    return NULL;
}

/* 队列满时丢最旧：容量 4 推 10 个，留下的应是最后 4 个 */
static void test_bounded_drop_oldest(void)
{
    pkt_queue_t *q = pkt_queue_create(4);
    CHECK(q);
    for (int i = 0; i < 10; ++i)
        CHECK(pkt_queue_push(q, make_pkt(i), i % 3 == 0) == 0);

    pkt_queue_stats_t st;
    pkt_queue_get_stats(q, &st);
    CHECK(st.pushed == 10);
    CHECK(st.dropped == 6);
    CHECK(st.depth == 4);
    CHECK(st.high_watermark == 4);

    for (int expect = 6; expect <= 9; ++expect) {
        AVPacket *pkt = NULL;
        int is_key = 0;
        CHECK(pkt_queue_pop(q, &pkt, &is_key) == 0);
        CHECK(pkt && pkt->data[0] == (uint8_t)expect);
        CHECK(is_key == (expect % 3 == 0));
        av_packet_free(&pkt);
    }
    pkt_queue_get_stats(q, &st);
    CHECK(st.depth == 0);
    CHECK(st.popped == 4);

    /* 停止后取包应立即返回 1（已排空），不再阻塞 */
    pkt_queue_stop(q);
    CHECK(pkt_queue_stopped(q) == 1);
    AVPacket *pkt = NULL;
    CHECK(pkt_queue_pop(q, &pkt, NULL) == 1);
    /* 停止后 push 必须消费掉引用并返回 -1 */
    CHECK(pkt_queue_push(q, make_pkt(99), 0) == -1);

    pkt_queue_destroy(q);
    puts("PASS: bounded queue keeps the newest packets and drops the oldest");
}

/* 消费者阻塞在空队列时，stop() 必须把它唤醒（退出不能卡死） */
static void test_stop_wakes_blocked_consumer(void)
{
    pkt_queue_t *q = pkt_queue_create(4);
    CHECK(q);
    consumer_t c = { .q = q, .first_id = -1 };
    pthread_t tid;
    CHECK(pthread_create(&tid, NULL, consumer_main, &c) == 0);

    usleep(300 * 1000);                 /* 让消费者进入等待 */
    CHECK(pkt_queue_push(q, make_pkt(42), 1) == 0);
    usleep(100 * 1000);

    int64_t t0 = now_ms();
    pkt_queue_stop(q);
    CHECK(pthread_join(tid, NULL) == 0);
    int64_t elapsed = now_ms() - t0;

    CHECK(c.count == 1);
    CHECK(c.first_id == 42);
    CHECK(c.rc == 1);
    CHECK(elapsed < 1000);
    pkt_queue_destroy(q);
    printf("PASS: stop() woke the blocked consumer in %lld ms\n", (long long)elapsed);
}

/* stop() 之后已入队的包仍会被消费者取完，一个都不丢 */
static void test_drain_after_stop(void)
{
    pkt_queue_t *q = pkt_queue_create(8);
    CHECK(q);
    CHECK(pkt_queue_push(q, make_pkt(5), 0) == 0);
    CHECK(pkt_queue_push(q, make_pkt(6), 1) == 0);
    pkt_queue_stop(q);

    consumer_t c = { .q = q, .first_id = -1 };
    pthread_t tid;
    CHECK(pthread_create(&tid, NULL, consumer_main, &c) == 0);
    CHECK(pthread_join(tid, NULL) == 0);

    CHECK(c.count == 2);
    CHECK(c.first_id == 5);
    CHECK(c.rc == 1);
    pkt_queue_destroy(q);
    puts("PASS: queued packets are drained after stop()");
}

/* 销毁时队列里还有包，也必须全部释放（配合 valgrind 可验证无泄漏） */
static void test_destroy_with_pending(void)
{
    pkt_queue_t *q = pkt_queue_create(4);
    CHECK(q);
    for (int i = 0; i < 4; ++i) CHECK(pkt_queue_push(q, make_pkt(i), 0) == 0);
    pkt_queue_destroy(q);
    puts("PASS: destroy() with pending packets returns cleanly");
}

/* 并发压力：生产者与消费者同时跑，验证丢最旧策略下顺序单调、计数自洽。
 * 计数恒等式（丢最旧策略下 push 不会被拒）：
 *   pushed == 总数；consumed + dropped == pushed。 */
#define STRESS_TOTAL 50000

static AVPacket *make_pkt_id(unsigned long long id)
{
    AVPacket *pkt = av_packet_alloc();
    CHECK(pkt);
    CHECK(av_new_packet(pkt, 8) == 0);
    memset(pkt->data, 0, 8);
    memcpy(pkt->data, &id, sizeof(id));
    return pkt;
}

typedef struct {
    pkt_queue_t       *q;
    unsigned long long consumed;
    int                order_ok;
} stress_t;

static void *stress_producer(void *arg)
{
    pkt_queue_t *q = arg;
    for (unsigned long long i = 0; i < STRESS_TOTAL; ++i)
        CHECK(pkt_queue_push(q, make_pkt_id(i), 0) == 0);
    return NULL;
}

static void *stress_consumer(void *arg)
{
    stress_t *c = arg;
    unsigned long long last = 0;
    int have_last = 0;
    for (;;) {
        AVPacket *pkt = NULL;
        int is_key = 0;
        if (pkt_queue_pop(c->q, &pkt, &is_key) != 0) break;
        unsigned long long id = 0;
        memcpy(&id, pkt->data, sizeof(id));
        if (have_last && id <= last) c->order_ok = 0;   /* 丢最旧也必须保持顺序 */
        last = id;
        have_last = 1;
        c->consumed++;
        av_packet_free(&pkt);
    }
    return NULL;
}

static void test_concurrent_producer_consumer(void)
{
    pkt_queue_t *q = pkt_queue_create(64);
    CHECK(q);
    stress_t c = { .q = q, .order_ok = 1 };
    pthread_t pt, ct;
    CHECK(pthread_create(&pt, NULL, stress_producer, q) == 0);
    CHECK(pthread_create(&ct, NULL, stress_consumer, &c) == 0);
    CHECK(pthread_join(pt, NULL) == 0);
    pkt_queue_stop(q);
    CHECK(pthread_join(ct, NULL) == 0);

    pkt_queue_stats_t st;
    pkt_queue_get_stats(q, &st);
    CHECK(c.order_ok == 1);
    CHECK(st.pushed == STRESS_TOTAL);
    CHECK(c.consumed + st.dropped == st.pushed);
    pkt_queue_destroy(q);
    printf("PASS: concurrent producer/consumer kept order: pushed=%llu dropped=%llu consumed=%llu\n",
           st.pushed, st.dropped, c.consumed);
}

int main(void)
{
    test_bounded_drop_oldest();
    test_stop_wakes_blocked_consumer();
    test_drain_after_stop();
    test_destroy_with_pending();
    test_concurrent_producer_consumer();
    puts("PASS: pkt_queue bounded drop, ownership, stop wakeup, drain and concurrency");
    return 0;
}

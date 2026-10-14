/* mqtt_client 测试：
 *   不带参数 —— 只跑纯逻辑（剩余长度编解码、主题通配匹配），不依赖网络
 *   带 host  —— 额外跑对真实 broker 的集成测试（订阅 + 发布 + 收包 + 大载荷 + QoS1）
 * 依赖：只依赖 src/mqtt_client.c 与 src/log.c（不依赖任何 MQTT 库）
 */
#include "mqtt_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

/* ---------- 1) 剩余长度变长编码 ---------- */
static void test_rem_len(void)
{
    const uint32_t vals[] = { 0, 1, 127, 128, 16383, 16384, 2097151, 2097152, 268435455 };
    const int    exps[] = { 1, 1,   1,   2,     2,     3,       3,       4,         4 };
    uint8_t buf[8];
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); ++i) {
        int n = mqtt_enc_rem_len(buf, vals[i]);
        CHECK(n == exps[i]);
        uint32_t back = 0;
        int used = mqtt_dec_rem_len(buf, (size_t)n, &back);
        CHECK(used == n);
        CHECK(back == vals[i]);
    }
    uint32_t v = 0;
    CHECK(mqtt_dec_rem_len(buf, 0, &v) == 0);                  /* 数据不足 */
    uint8_t bad[5] = { 0xff, 0xff, 0xff, 0xff, 0x7f };
    CHECK(mqtt_dec_rem_len(bad, sizeof(bad), &v) == -1);       /* 超过 4 字节：非法 */
    puts("PASS: mqtt remaining-length varint encode/decode (0..268435455)");
}

/* ---------- 2) 主题通配匹配 ---------- */
static void test_topic_match(void)
{
    struct { const char *f, *t; int want; } cases[] = {
        { "device/A1/status",   "device/A1/status",   1 },
        { "device/A1/status",   "device/A1/event",    0 },
        { "device/+/status",    "device/A1/status",   1 },
        { "device/+/status",    "device/A1/extra",    0 },
        { "device/+/status",    "device//status",     1 },   /* 空层也算一层 */
        { "device/#",           "device/A1/event",    1 },
        { "device/#",           "device/A1",          1 },
        { "device/#",           "devices/A1",         0 },
        { "#",                  "anything/at/all",    1 },
        { "a/+/c/#",            "a/b/c/d/e",          1 },
        { "a/+/c",              "a/b/c",              1 },
        { "a/+/c",              "a/b/c/d",            0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int got = mqtt_topic_match(cases[i].f, cases[i].t);
        if (got != cases[i].want) {
            fprintf(stderr, "FAIL topic_match(\"%s\",\"%s\") = %d, want %d\n",
                    cases[i].f, cases[i].t, got, cases[i].want);
            exit(1);
        }
    }
    printf("PASS: mqtt topic wildcard matching (%zu cases, + and #)\n",
           sizeof(cases) / sizeof(cases[0]));
}

/* ---------- 3) 集成：对真实 broker ---------- */
static struct {
    int    got_status, got_small, got_big;
    char   small[128];
    size_t big_len;
    int    big_ok;
} g_rx;

static void on_message(void *user, const char *topic, const uint8_t *payload, size_t len)
{
    (void)user;
    if (!strcmp(topic, "edgefusion/test/status")) {
        g_rx.got_status = 1;
    } else if (!strcmp(topic, "edgefusion/test/small")) {
        size_t n = len < sizeof(g_rx.small) - 1 ? len : sizeof(g_rx.small) - 1;
        memcpy(g_rx.small, payload, n);
        g_rx.small[n] = '\0';
        g_rx.got_small = 1;
    } else if (!strcmp(topic, "edgefusion/test/big")) {
        g_rx.big_len = len;
        g_rx.big_ok = 1;
        for (size_t i = 0; i < len; ++i) {                 /* 校验内容：byte = i & 0xff */
            if (payload[i] != (uint8_t)(i & 0xff)) { g_rx.big_ok = 0; break; }
        }
        g_rx.got_big = 1;
    }
}

static int test_broker(const char *host)
{
    const char *subs[] = { "edgefusion/test/#" };
    int qos[] = { 1 };
    mqtt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.host = host;
    cfg.port = 1883;
    cfg.client_id = "edgefusion-selftest";
    cfg.keepalive_s = 15;
    cfg.sub_topics[0] = subs[0];
    cfg.sub_qos[0] = qos[0];
    cfg.sub_count = 1;
    cfg.on_message = on_message;
    cfg.user = NULL;

    mqtt_client_t *c = mqtt_client_create(&cfg);
    CHECK(c);
    CHECK(mqtt_client_start(c) == 0);

    /* 等连接建立（最多 5 秒） */
    mqtt_stats_t st;
    for (int i = 0; i < 50; ++i) {
        mqtt_client_get_stats(c, &st);
        if (st.connected) break;
        usleep(100000);
    }
    CHECK(st.connected);
    printf("PASS: connected to broker %s:1883 (client_id=edgefusion-selftest)\n", host);

    /* QoS1 发布（retain），并等自己订阅到 */
    const char *msg = "hello-from-mqtt-client";
    CHECK(mqtt_client_publish(c, "edgefusion/test/small", msg, strlen(msg), 1, 0) == 0);
    for (int i = 0; i < 50 && !g_rx.got_small; ++i) usleep(100000);
    CHECK(g_rx.got_small);
    CHECK(!strcmp(g_rx.small, msg));
    printf("PASS: QoS1 publish + subscribe round-trip: %s\n", g_rx.small);

    /* 大载荷（模拟一张 100 KB 快照）——验证粘包/半包重组 */
    size_t big = 100 * 1024;
    uint8_t *buf = malloc(big);
    CHECK(buf);
    for (size_t i = 0; i < big; ++i) buf[i] = (uint8_t)(i & 0xff);
    CHECK(mqtt_client_publish(c, "edgefusion/test/big", buf, big, 0, 0) == 0);
    for (int i = 0; i < 50 && !g_rx.got_big; ++i) usleep(100000);
    CHECK(g_rx.got_big);
    CHECK(g_rx.big_len == big);
    CHECK(g_rx.big_ok);
    printf("PASS: large payload %zu bytes reassembled byte-exact (packet framing ok)\n", g_rx.big_len);
    free(buf);

    mqtt_client_get_stats(c, &st);
    printf("stats: reconnects=%d rx=%llu tx=%llu dropped=%llu connected=%d\n",
           st.reconnects, st.rx_messages, st.tx_messages, st.tx_dropped, st.connected);
    CHECK(st.rx_messages >= 2);
    CHECK(st.tx_messages >= 2);

    mqtt_client_destroy(c);
    puts("PASS: mqtt_client integration against real broker");
    return 0;
}

int main(int argc, char **argv)
{
    test_rem_len();
    test_topic_match();
    if (argc > 1) return test_broker(argv[1]);
    puts("(未指定 broker，跳过集成测试：./test_mqtt_client <broker-host>)");
    puts("PASS: mqtt_client pure-logic unit tests");
    return 0;
}

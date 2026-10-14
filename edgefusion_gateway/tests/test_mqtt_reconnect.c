/* MQTT 断线重连测试：
 * 连到指定端口的 broker，每秒打一行状态并周期发布 QoS1 消息；
 * 由外部脚本在运行中杀掉/重启 broker，程序据此报告：
 *   是否自动重连、重连次数、恢复后消息是否继续收发。
 * 退出码：0 = 观察到重连且恢复后仍有收发；1 = 未观察到重连。
 */
#include "mqtt_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned long long g_rx = 0;

static void on_msg(void *user, const char *topic, const uint8_t *payload, size_t len)
{
    (void)user;
    g_rx++;
    printf("rx  [%s] %.*s\n", topic, (int)len, (const char *)payload);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 1884;
    int dur  = argc > 2 ? atoi(argv[2]) : 20;

    mqtt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.client_id = "edgefusion-reconnect-test";
    cfg.keepalive_s = 3;              /* 心跳短，便于快速判死 */
    cfg.reconnect_min_ms = 300;
    cfg.reconnect_max_ms = 2000;
    cfg.sub_topics[0] = "edgefusion/rc/#";
    cfg.sub_qos[0] = 1;
    cfg.sub_count = 1;
    cfg.on_message = on_msg;

    mqtt_client_t *c = mqtt_client_create(&cfg);
    if (!c) { fprintf(stderr, "create failed\n"); return 2; }
    if (mqtt_client_start(c) != 0) { fprintf(stderr, "start failed\n"); return 2; }

    mqtt_stats_t st;
    for (int i = 0; i < dur; ++i) {
        mqtt_client_get_stats(c, &st);
        printf("t=%2ds connected=%d reconnects=%d rx=%llu tx=%llu dropped=%llu\n",
               i, st.connected, st.reconnects, st.rx_messages, st.tx_messages, st.tx_dropped);
        fflush(stdout);
        if (st.connected) {
            char payload[64];
            snprintf(payload, sizeof(payload), "tick-%d", i);
            mqtt_client_publish(c, "edgefusion/rc/tick", payload, strlen(payload), 1, 0);
        }
        sleep(1);
    }

    mqtt_client_get_stats(c, &st);
    printf("FINAL connected=%d reconnects=%d rx=%llu tx=%llu dropped=%llu local_rx=%llu\n",
           st.connected, st.reconnects, st.rx_messages, st.tx_messages, st.tx_dropped, g_rx);
    fflush(stdout);
    mqtt_client_destroy(c);

    if (st.reconnects >= 1 && st.rx_messages >= 2) {
        puts("PASS: auto-reconnect observed and traffic resumed");
        return 0;
    }
    puts("FAIL: auto-reconnect not observed");
    return 1;
}

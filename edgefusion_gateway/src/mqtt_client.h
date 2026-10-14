#ifndef EDGEFUSION_MQTT_CLIENT_H
#define EDGEFUSION_MQTT_CLIENT_H

#include <stddef.h>
#include <stdint.h>

/* 自研 MQTT 3.1.1 客户端（只实现项目需要的子集，不依赖任何 MQTT 库）：
 *
 *   报文：CONNECT / CONNACK、PUBLISH(QoS0、QoS1) / PUBACK、
 *         SUBSCRIBE / SUBACK、PINGREQ / PINGRESP、DISCONNECT
 *   特性：遗嘱消息(LWT)、主题通配匹配(+、#)、粘包与半包重组、
 *         心跳保活与超时判死、断线指数退避重连、QoS1 未确认消息重发
 *
 * 设计取向：网络线程只做"收发 + 心跳 + 重连"，业务回调在同一个线程里执行
 * （回调里不要做长阻塞；需要长时间工作请自行转交队列，例如项目的 event_bus）。
 */

typedef struct mqtt_client mqtt_client_t;

/* 收到订阅主题的消息时回调。topic 以 '\0' 结尾；payload 不保证以 '\0' 结尾 */
typedef void (*mqtt_msg_cb_t)(void *user, const char *topic,
                              const uint8_t *payload, size_t len);

typedef struct {
    const char  *host;
    int          port;
    const char  *client_id;       /* 为空时自动生成 edgefusion-<pid> */
    int          keepalive_s;     /* 默认 60 */
    int          reconnect_min_ms;/* 默认 1000 */
    int          reconnect_max_ms;/* 默认 30000 */

    const char  *will_topic;      /* 遗嘱主题，可为 NULL */
    const char  *will_payload;    /* 遗嘱内容（可为 NULL） */
    int          will_qos;        /* 0 或 1 */

    /* 连接建立后要订阅的主题（最多 8 条） */
    const char  *sub_topics[8];
    int          sub_qos[8];
    int          sub_count;

    mqtt_msg_cb_t on_message;
    void         *user;
} mqtt_config_t;

typedef struct {
    int                connected;     /* 1=已连接 */
    int                reconnects;    /* 累计重连次数 */
    unsigned long long rx_messages;   /* 收到的应用消息数 */
    unsigned long long tx_messages;   /* 成功提交发送的消息数 */
    unsigned long long tx_dropped;    /* 未连接时被丢弃的提交数 */
} mqtt_stats_t;

mqtt_client_t *mqtt_client_create(const mqtt_config_t *cfg);
void           mqtt_client_destroy(mqtt_client_t *c);

/* 启动后台线程（含首次连接），成功返回 0 */
int  mqtt_client_start(mqtt_client_t *c);
/* 停止并回收线程（内部会发 DISCONNECT） */
void mqtt_client_stop(mqtt_client_t *c);

/* 发布。qos 取 0 或 1；retain 非 0 时由 broker 保留。
 * 未连接时直接丢弃并计入 tx_dropped（返回 -1），不阻塞调用方。 */
int  mqtt_client_publish(mqtt_client_t *c, const char *topic,
                         const void *payload, size_t len, int qos, int retain);

/* 订阅（可在运行中追加）。成功返回 0，未连接返回 -1（下次连上后由内部表补订阅） */
int  mqtt_client_subscribe(mqtt_client_t *c, const char *topic, int qos);

void mqtt_client_get_stats(mqtt_client_t *c, mqtt_stats_t *out);

/* ---------- 纯逻辑工具（便于单元测试） ---------- */

/* MQTT 主题匹配：filter 支持 '+'（单层）与 '#'（多层，只能在末尾） */
int mqtt_topic_match(const char *filter, const char *topic);

/* 剩余长度变长编码/解码。编码返回写入字节数(1..4)；
 * 解码成功返回已消费字节数(1..4)，并把值写入 *out；数据不足返回 0；非法返回 -1。 */
int mqtt_enc_rem_len(uint8_t *buf, uint32_t len);
int mqtt_dec_rem_len(const uint8_t *buf, size_t avail, uint32_t *out);

#endif

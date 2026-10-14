#include "mqtt_client.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MQTT_OUTBOX_MAX   16          /* 待发消息槽位（业务线程提交、网络线程发送） */
#define MQTT_INFLIGHT_MAX 4           /* QoS1 已发未确认（断线后重发） */
#define MQTT_SUB_MAX      8
#define MQTT_TOPIC_MAX    128
#define MQTT_RXBUF_MAX    (300 * 1024)
#define MQTT_SEND_TIMEOUT_MS 2000

/* ---------- 纯逻辑工具 ---------- */

int mqtt_enc_rem_len(uint8_t *buf, uint32_t len)
{
    int n = 0;
    do {
        uint8_t b = (uint8_t)(len % 128);
        len /= 128;
        if (len) b |= 0x80;
        buf[n++] = b;
    } while (len);
    return n;
}

int mqtt_dec_rem_len(const uint8_t *buf, size_t avail, uint32_t *out)
{
    uint32_t value = 0, mult = 1;
    for (int i = 0; i < 4; ++i) {
        if ((size_t)i >= avail) return 0;              /* 数据不足 */
        uint8_t b = buf[i];
        value += (uint32_t)(b & 0x7f) * mult;
        if (!(b & 0x80)) { *out = value; return i + 1; }
        mult *= 128;
    }
    return -1;                                          /* 超过 4 字节：非法 */
}

int mqtt_topic_match(const char *filter, const char *topic)
{
    if (!filter || !topic) return 0;
    while (*filter) {
        if (*filter == '#') return 1;                   /* 多级通配，匹配剩余全部 */
        if (*filter == '+') {
            while (*topic && *topic != '/') topic++;    /* 吃掉一层 */
            filter++;
            if (*filter == '/') {
                if (*topic != '/') return 0;
                filter++; topic++;
            }
            continue;
        }
        if (*filter != *topic) return 0;
        filter++; topic++;
    }
    return *topic == '\0';
}

/* ---------- 内部结构 ---------- */

typedef struct {
    char      topic[MQTT_TOPIC_MAX];
    uint8_t  *payload;
    size_t    len;
    int       qos;
    int       retain;
    uint16_t  pid;
} mqtt_msg_t;

struct mqtt_client {
    mqtt_config_t cfg;
    char          client_id[64];
    char          host[64];
    char          will_topic[MQTT_TOPIC_MAX];
    char          will_payload[MQTT_TOPIC_MAX];
    struct { char topic[MQTT_TOPIC_MAX]; int qos; } subs[MQTT_SUB_MAX];
    int           sub_count;

    int           fd;
    int           connected;
    uint16_t      next_pid;
    pthread_t     tid;
    atomic_int    running;
    pthread_mutex_t lock;                                /* 保护 outbox / inflight / fd */

    mqtt_msg_t    outbox[MQTT_OUTBOX_MAX];
    int           out_count;
    mqtt_msg_t    inflight[MQTT_INFLIGHT_MAX];

    uint8_t      *rx;
    size_t        rx_len, rx_cap;

    int64_t       last_tx_ms, last_rx_ms;
    int           backoff_ms;
    mqtt_stats_t  stats;
};

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static int  put_str(uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    put_u16(p, (uint16_t)n);
    memcpy(p + 2, s, n);
    return (int)n + 2;
}

/* ---------- 套接字 ---------- */

static int tcp_connect(const char *host, int port)
{
    struct addrinfo hints, *res = NULL;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;

    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    /* 非阻塞 connect + 3s 超时，避免网络线程卡死 */
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    if (rc < 0 && errno == EINPROGRESS) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        if (poll(&pfd, 1, 3000) > 0) {
            int err = 0;
            socklen_t elen = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
            rc = err ? -1 : 0;
        } else {
            rc = -1;
        }
    }
    freeaddrinfo(res);
    if (rc < 0) { close(fd); return -1; }

    fcntl(fd, F_SETFL, fl);                       /* 恢复阻塞，配合 SO_SNDTIMEO */
    struct timeval tv = { .tv_sec = MQTT_SEND_TIMEOUT_MS / 1000,
                          .tv_usec = (MQTT_SEND_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static int send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* ---------- 报文构造 ---------- */

static int build_connect(const mqtt_client_t *c, uint8_t *buf, size_t cap)
{
    uint8_t body[512];
    int n = 0;
    n += put_str(body + n, "MQTT");
    body[n++] = 4;                                        /* 协议级别 3.1.1 */
    uint8_t flags = 0x02;                                 /* clean session */
    if (c->will_topic[0]) flags |= 0x04 | (uint8_t)((c->cfg.will_qos & 3) << 3);
    body[n++] = flags;
    put_u16(body + n, (uint16_t)c->cfg.keepalive_s); n += 2;
    n += put_str(body + n, c->client_id);
    if (c->will_topic[0]) {
        n += put_str(body + n, c->will_topic);
        n += put_str(body + n, c->will_payload);
    }
    if ((size_t)n + 5 > cap) return -1;
    int hdr = 1, rl = mqtt_enc_rem_len(buf + 1, (uint32_t)n);
    buf[0] = 0x10;
    memcpy(buf + 1 + rl, body, (size_t)n);
    return hdr + rl + n;
}

static int build_publish(uint8_t *buf, size_t cap, const char *topic,
                         const uint8_t *payload, size_t len, int qos, int retain, uint16_t pid)
{
    size_t tlen = strlen(topic);
    uint32_t rem = (uint32_t)(2 + tlen + (qos > 0 ? 2 : 0) + len);
    uint8_t rlbuf[4];
    int rl = mqtt_enc_rem_len(rlbuf, rem);
    if ((size_t)1 + (size_t)rl + rem > cap) return -1;
    uint8_t *p = buf;
    *p++ = (uint8_t)(0x30 | ((qos & 3) << 1) | (retain ? 1 : 0));
    memcpy(p, rlbuf, (size_t)rl); p += rl;
    p += put_str(p, topic);
    if (qos > 0) { put_u16(p, pid); p += 2; }
    memcpy(p, payload, len); p += len;
    return (int)(p - buf);
}

static int build_subscribe(const mqtt_client_t *c, uint8_t *buf, size_t cap, uint16_t pid)
{
    uint8_t body[1024];
    int n = 0;
    put_u16(body + n, pid); n += 2;
    for (int i = 0; i < c->sub_count; ++i) {
        n += put_str(body + n, c->subs[i].topic);
        body[n++] = (uint8_t)(c->subs[i].qos & 3);
    }
    uint8_t rlbuf[4];
    int rl = mqtt_enc_rem_len(rlbuf, (uint32_t)n);
    if ((size_t)1 + (size_t)rl + (size_t)n > cap) return -1;
    buf[0] = 0x82;
    memcpy(buf + 1, rlbuf, (size_t)rl);
    memcpy(buf + 1 + rl, body, (size_t)n);
    return 1 + rl + n;
}

/* ---------- 收包处理 ---------- */

static void send_simple(mqtt_client_t *c, uint8_t type, uint16_t pid)
{
    uint8_t pkt[4];
    if (type == 0xC0 || type == 0xE0) {           /* PINGREQ / DISCONNECT */
        pkt[0] = type; pkt[1] = 0;
        if (c->fd >= 0) send_all(c->fd, pkt, 2);
        c->last_tx_ms = now_ms();
        return;
    }
    pkt[0] = type; pkt[1] = 2;
    put_u16(pkt + 2, pid);
    if (c->fd >= 0) send_all(c->fd, pkt, 4);
    c->last_tx_ms = now_ms();
}

static void do_subscribe_all(mqtt_client_t *c)
{
    if (c->sub_count == 0) return;
    uint8_t pkt[1200];
    uint16_t pid = c->next_pid++;
    if (pid == 0) pid = c->next_pid++;
    int n = build_subscribe(c, pkt, sizeof(pkt), pid);
    if (n > 0 && c->fd >= 0 && send_all(c->fd, pkt, (size_t)n) == 0) {
        c->last_tx_ms = now_ms();
        LOG_INF("mqtt 已订阅 %d 个主题 (pid=%u)", c->sub_count, pid);
    }
}

static void retransmit_inflight(mqtt_client_t *c);

static void handle_publish(mqtt_client_t *c, int qos, const uint8_t *body, uint32_t rem)
{
    if (rem < 2) return;
    uint16_t tlen = (uint16_t)((body[0] << 8) | body[1]);
    if ((uint32_t)tlen + 2 > rem) return;
    char topic[MQTT_TOPIC_MAX];
    size_t n = tlen < sizeof(topic) - 1 ? tlen : sizeof(topic) - 1;
    memcpy(topic, body + 2, n);
    topic[n] = '\0';

    uint32_t off = 2u + tlen;
    uint16_t pid = 0;
    if (qos > 0) {
        if (rem < off + 2) return;
        pid = (uint16_t)((body[off] << 8) | body[off + 1]);
        off += 2;
    }
    const uint8_t *payload = body + off;
    size_t plen = rem - off;

    c->stats.rx_messages++;
    if (c->cfg.on_message) c->cfg.on_message(c->cfg.user, topic, payload, plen);
    if (qos > 0) send_simple(c, 0x40, pid);       /* PUBACK：不确认 broker 会重复投递 */
}

static void handle_packet(mqtt_client_t *c, uint8_t hdr, const uint8_t *body, uint32_t rem)
{
    switch (hdr & 0xF0) {
    case 0x20:                                     /* CONNACK */
        if (rem >= 2 && body[1] == 0) {
            c->connected = 1;
            c->backoff_ms = c->cfg.reconnect_min_ms;
            c->last_rx_ms = c->last_tx_ms = now_ms();
            LOG_INF("mqtt 已连接 %s:%d（client_id=%s）", c->host, c->cfg.port, c->client_id);
            do_subscribe_all(c);
            retransmit_inflight(c);        /* 断线前未确认的 QoS1 消息重发 */
        } else {
            LOG_WRN("mqtt CONNACK 被拒绝: rc=%u", rem >= 2 ? body[1] : 0xFF);
        }
        break;
    case 0x30:                                     /* PUBLISH */
        handle_publish(c, (hdr >> 1) & 3, body, rem);
        break;
    case 0x40: {                                   /* PUBACK：清掉 inflight */
        if (rem >= 2) {
            uint16_t pid = (uint16_t)((body[0] << 8) | body[1]);
            pthread_mutex_lock(&c->lock);
            for (int i = 0; i < MQTT_INFLIGHT_MAX; ++i) {
                if (c->inflight[i].pid == pid && c->inflight[i].payload) {
                    free(c->inflight[i].payload);
                    c->inflight[i].payload = NULL;
                    c->inflight[i].pid = 0;
                }
            }
            pthread_mutex_unlock(&c->lock);
        }
        break;
    }
    case 0x90:                                     /* SUBACK */
        break;
    case 0xD0:                                     /* PINGRESP */
        break;
    default:
        break;
    }
}

/* 从接收缓冲里按"粘包/半包"拆出完整报文 */
static void process_rx(mqtt_client_t *c)
{
    size_t off = 0;
    while (c->rx_len - off >= 2) {
        uint8_t hdr = c->rx[off];
        uint32_t rem = 0;
        int used = mqtt_dec_rem_len(c->rx + off + 1, c->rx_len - off - 1, &rem);
        if (used == 0) break;                       /* 长度字段未收全 */
        if (used < 0) { c->connected = 0; break; }  /* 非法长度：判为协议错误 */
        if (rem > MQTT_RXBUF_MAX) { c->connected = 0; break; }
        size_t total = 1 + (size_t)used + rem;
        if (c->rx_len - off < total) break;         /* 半包，等下次 */
        handle_packet(c, hdr, c->rx + off + 1 + used, rem);
        off += total;
    }
    if (off > 0) {
        memmove(c->rx, c->rx + off, c->rx_len - off);
        c->rx_len -= off;
    }
}

/* ---------- 发送侧 ---------- */

static void drain_outbox(mqtt_client_t *c)
{
    /* 没有待发消息时不分配缓冲（本函数每轮循环都会被调用） */
    pthread_mutex_lock(&c->lock);
    int pending = c->out_count;
    pthread_mutex_unlock(&c->lock);
    if (pending == 0 || c->fd < 0) return;

    uint8_t *pkt = malloc(MQTT_RXBUF_MAX);
    if (!pkt) return;
    for (;;) {
        mqtt_msg_t m;
        memset(&m, 0, sizeof(m));
        pthread_mutex_lock(&c->lock);
        if (c->out_count == 0 || c->fd < 0) { pthread_mutex_unlock(&c->lock); break; }
        m = c->outbox[0];
        memmove(&c->outbox[0], &c->outbox[1], sizeof(mqtt_msg_t) * (size_t)(c->out_count - 1));
        c->out_count--;
        pthread_mutex_unlock(&c->lock);

        uint16_t pid = 0;
        if (m.qos > 0) {
            pthread_mutex_lock(&c->lock);
            pid = c->next_pid++;
            if (pid == 0) pid = c->next_pid++;
            pthread_mutex_unlock(&c->lock);
        }
        int n = build_publish(pkt, MQTT_RXBUF_MAX, m.topic, m.payload, m.len, m.qos, m.retain, pid);
        if (n > 0 && send_all(c->fd, pkt, (size_t)n) == 0) {
            c->last_tx_ms = now_ms();
            c->stats.tx_messages++;
            if (m.qos > 0) {                        /* 记下来：断线后重发 */
                pthread_mutex_lock(&c->lock);
                for (int i = 0; i < MQTT_INFLIGHT_MAX; ++i) {
                    if (!c->inflight[i].payload) {
                        c->inflight[i] = m;
                        c->inflight[i].pid = pid;
                        m.payload = NULL;
                        break;
                    }
                }
                pthread_mutex_unlock(&c->lock);
            }
        } else {
            c->connected = 0;
        }
        free(m.payload);
        if (!c->connected) break;
    }
    free(pkt);
}

static void retransmit_inflight(mqtt_client_t *c)
{
    uint8_t *pkt = malloc(MQTT_RXBUF_MAX);
    if (!pkt) return;
    pthread_mutex_lock(&c->lock);
    for (int i = 0; i < MQTT_INFLIGHT_MAX; ++i) {
        mqtt_msg_t *m = &c->inflight[i];
        if (!m->payload) continue;
        int n = build_publish(pkt, MQTT_RXBUF_MAX, m->topic, m->payload, m->len, m->qos, 0, m->pid);
        if (n > 0 && send_all(c->fd, pkt, (size_t)n) != 0) break;
        LOG_INF("mqtt 重发未确认消息 pid=%u topic=%s", m->pid, m->topic);
    }
    pthread_mutex_unlock(&c->lock);
    free(pkt);
}

/* ---------- 网络线程 ---------- */

static void drop_connection(mqtt_client_t *c)
{
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    c->connected = 0;
    c->rx_len = 0;
}

/* 按需扩容接收缓冲：MQTT 单包可达 256 MB，快照可能几十~几百 KB，
 * 缓冲必须能装下整包，否则会在 recv 时误判断连。 */
static int rx_ensure(mqtt_client_t *c, size_t need)
{
    if (need > MQTT_RXBUF_MAX) return -1;
    if (need <= c->rx_cap) return 0;
    size_t cap = c->rx_cap ? c->rx_cap : 64 * 1024;
    while (cap < need) cap *= 2;
    if (cap > MQTT_RXBUF_MAX) cap = MQTT_RXBUF_MAX;
    uint8_t *p = realloc(c->rx, cap);
    if (!p) return -1;
    c->rx = p;
    c->rx_cap = cap;
    return 0;
}

static void *mqtt_loop(void *arg)
{
    mqtt_client_t *c = arg;
    uint8_t rbuf[4096];

    while (atomic_load(&c->running)) {
        if (c->fd < 0) {
            int fd = tcp_connect(c->host, c->cfg.port);
            if (fd < 0) {
                LOG_WRN("mqtt 连接 %s:%d 失败，%d ms 后重试", c->host, c->cfg.port, c->backoff_ms);
                for (int t = 0; t < c->backoff_ms / 100 && atomic_load(&c->running); ++t) usleep(100000);
                int nb = c->backoff_ms * 2;
                c->backoff_ms = nb > c->cfg.reconnect_max_ms ? c->cfg.reconnect_max_ms : nb;
                continue;
            }
            if (c->stats.reconnects > 0 || c->last_rx_ms) c->stats.reconnects++;
            c->fd = fd;
            c->last_rx_ms = c->last_tx_ms = now_ms();
            uint8_t pkt[512];
            int n = build_connect(c, pkt, sizeof(pkt));
            if (n <= 0 || send_all(c->fd, pkt, (size_t)n) != 0) { drop_connection(c); continue; }
        }

        if (c->connected) {
            drain_outbox(c);
            if (!c->connected) { drop_connection(c); continue; }
        }

        struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 200);
        if (ready < 0 && errno != EINTR) { drop_connection(c); continue; }

        if (ready > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = recv(c->fd, rbuf, sizeof(rbuf), 0);
            if (n <= 0) {
                LOG_WRN("mqtt 连接断开（recv=%zd, errno=%d），准备重连", n, errno);
                drop_connection(c);
                continue;
            }
            if (rx_ensure(c, c->rx_len + (size_t)n) != 0) {
                LOG_WRN("mqtt 接收缓冲扩展失败（需要 %zu 字节，上限 %d）",
                        c->rx_len + (size_t)n, MQTT_RXBUF_MAX);
                drop_connection(c);
                continue;
            }
            memcpy(c->rx + c->rx_len, rbuf, (size_t)n);
            c->rx_len += (size_t)n;
            c->last_rx_ms = now_ms();
            process_rx(c);
            if (!c->connected || c->fd < 0) {
                LOG_WRN("mqtt 协议处理异常，重连");
                drop_connection(c);
                continue;
            }
        }

        /* 心跳：空闲超过 keepalive 就 PINGREQ；超过 2.5 倍没收到任何数据判死 */
        int64_t now = now_ms();
        int ka = c->cfg.keepalive_s > 0 ? c->cfg.keepalive_s : 60;
        if (c->connected && now - c->last_tx_ms >= ka * 1000 / 2) send_simple(c, 0xC0, 0);
        if (c->last_rx_ms && now - c->last_rx_ms > (int64_t)ka * 2500) {
            LOG_WRN("mqtt 心跳超时（%lld ms 无数据），重连", (long long)(now - c->last_rx_ms));
            drop_connection(c);
            c->backoff_ms = c->cfg.reconnect_min_ms;
        }
    }
    return NULL;
}

/* ---------- 对外接口 ---------- */

mqtt_client_t *mqtt_client_create(const mqtt_config_t *cfg)
{
    if (!cfg || !cfg->host) return NULL;
    mqtt_client_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->cfg = *cfg;
    if (c->cfg.keepalive_s <= 0) c->cfg.keepalive_s = 60;
    if (c->cfg.reconnect_min_ms <= 0) c->cfg.reconnect_min_ms = 1000;
    if (c->cfg.reconnect_max_ms <= 0) c->cfg.reconnect_max_ms = 30000;
    snprintf(c->host, sizeof(c->host), "%s", cfg->host);
    snprintf(c->client_id, sizeof(c->client_id), "%s",
             cfg->client_id && cfg->client_id[0] ? cfg->client_id : "edgefusion-gw");
    if (cfg->will_topic) snprintf(c->will_topic, sizeof(c->will_topic), "%s", cfg->will_topic);
    if (cfg->will_payload) snprintf(c->will_payload, sizeof(c->will_payload), "%s", cfg->will_payload);
    c->fd = -1;
    c->next_pid = 1;
    c->backoff_ms = c->cfg.reconnect_min_ms;
    pthread_mutex_init(&c->lock, NULL);

    int n = cfg->sub_count < MQTT_SUB_MAX ? cfg->sub_count : MQTT_SUB_MAX;
    for (int i = 0; i < n; ++i) {
        if (!cfg->sub_topics[i]) continue;
        snprintf(c->subs[c->sub_count].topic, MQTT_TOPIC_MAX, "%s", cfg->sub_topics[i]);
        c->subs[c->sub_count].qos = cfg->sub_qos[i];   /* 数组必有，缺省为 0 */
        c->sub_count++;
    }
    return c;
}

void mqtt_client_destroy(mqtt_client_t *c)
{
    if (!c) return;
    mqtt_client_stop(c);
    for (int i = 0; i < c->out_count; ++i) free(c->outbox[i].payload);
    for (int i = 0; i < MQTT_INFLIGHT_MAX; ++i) free(c->inflight[i].payload);
    free(c->rx);
    pthread_mutex_destroy(&c->lock);
    free(c);
}

int mqtt_client_start(mqtt_client_t *c)
{
    if (!c) return -1;
    atomic_store(&c->running, 1);
    if (pthread_create(&c->tid, NULL, mqtt_loop, c) != 0) {
        atomic_store(&c->running, 0);
        return -1;
    }
    return 0;
}

void mqtt_client_stop(mqtt_client_t *c)
{
    if (!c || !atomic_load(&c->running)) return;
    atomic_store(&c->running, 0);
    pthread_join(c->tid, NULL);
    if (c->fd >= 0) {
        send_simple(c, 0xE0, 0);                    /* DISCONNECT */
        close(c->fd);
        c->fd = -1;
    }
    c->connected = 0;
}

int mqtt_client_publish(mqtt_client_t *c, const char *topic,
                        const void *payload, size_t len, int qos, int retain)
{
    if (!c || !topic || (!payload && len)) return -1;
    pthread_mutex_lock(&c->lock);
    if (!c->connected || c->out_count >= MQTT_OUTBOX_MAX) {
        c->stats.tx_dropped++;
        pthread_mutex_unlock(&c->lock);
        return -1;
    }
    mqtt_msg_t *m = &c->outbox[c->out_count];
    snprintf(m->topic, MQTT_TOPIC_MAX, "%s", topic);
    m->payload = malloc(len ? len : 1);
    if (!m->payload) {
        c->stats.tx_dropped++;
        pthread_mutex_unlock(&c->lock);
        return -1;
    }
    if (len) memcpy(m->payload, payload, len);
    m->len = len;
    m->qos = qos == 1 ? 1 : 0;
    m->retain = retain ? 1 : 0;
    m->pid = 0;
    c->out_count++;
    pthread_mutex_unlock(&c->lock);
    return 0;
}

int mqtt_client_subscribe(mqtt_client_t *c, const char *topic, int qos)
{
    if (!c || !topic) return -1;
    pthread_mutex_lock(&c->lock);
    if (c->sub_count >= MQTT_SUB_MAX) { pthread_mutex_unlock(&c->lock); return -1; }
    int dup = 0;
    for (int i = 0; i < c->sub_count; ++i)
        if (!strcmp(c->subs[i].topic, topic)) dup = 1;
    if (!dup) {
        snprintf(c->subs[c->sub_count].topic, MQTT_TOPIC_MAX, "%s", topic);
        c->subs[c->sub_count].qos = qos;
        c->sub_count++;
    }
    int connected = c->connected;
    pthread_mutex_unlock(&c->lock);
    if (connected) do_subscribe_all(c);
    return 0;
}

void mqtt_client_get_stats(mqtt_client_t *c, mqtt_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    *out = c->stats;
    out->connected = c->connected;
    pthread_mutex_unlock(&c->lock);
}

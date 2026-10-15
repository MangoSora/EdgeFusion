#include "mqtt_bridge.h"
#include "mqtt_client.h"
#include "event_bus.h"
#include "cloud_ai.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>   /* unlink() */
#include <time.h>

#define BRIDGE_MAX_DEVICES     8
#define BRIDGE_SNAPSHOT_CACHE  (128 * 1024)   /* 事件缩略图缓存上限 */
#define BRIDGE_THUMB_MAX_AGE_S 10             /* 事件与快照的最大关联合龄(秒) */

typedef struct {
    char    id[32];
    int     online;
    int64_t last_seen;
} device_state_t;

static mqtt_client_t   *g_client;
static int              g_running;
static char             g_prefix[128];
static char             g_snap_dir[256];
static char             g_sub_status[192], g_sub_event[192], g_sub_snapshot[192];
static char             g_will_topic[192];

static device_state_t   g_devices[BRIDGE_MAX_DEVICES];
static int              g_device_count;

static unsigned long      g_snap_seq;
static unsigned long long g_frames_to_ai;   /* 投递给分析流水线的端侧帧数 */
static unsigned long long g_events_stored;
static unsigned long long g_snapshots_saved;
static long               g_last_event_ts;
static char               g_last_event_device[32];
static char               g_stream_url[160];
static int                g_device_paused;

/* 最近一张端侧快照：用于给同设备、时间相近的事件附缩略图 */
static pthread_mutex_t  g_snap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t         *g_snap_buf;
static size_t           g_snap_len;
static int64_t          g_snap_ts;
static char             g_snap_device[32];

static int64_t now_s(void) { return (int64_t)time(NULL); }

/* 前向声明：定义在文件末尾（快照清理） */
static void prune_old_snapshots(const char *devdir);

static device_state_t *device_get(const char *id)
{
    for (int i = 0; i < g_device_count; ++i)
        if (!strcmp(g_devices[i].id, id)) return &g_devices[i];
    if (g_device_count >= BRIDGE_MAX_DEVICES) return NULL;
    device_state_t *d = &g_devices[g_device_count++];
    snprintf(d->id, sizeof(d->id), "%s", id);
    d->online = 0;
    d->last_seen = now_s();
    return d;
}

static void mkdir_p(const char *path)
{
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

/* ---------------- 上行处理 ---------------- */

static void handle_status(const char *dev, const uint8_t *payload, size_t len)
{
    char buf[512];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = '\0';

    int online = -1;
    struct json_object *root = json_tokener_parse(buf);
    if (root) {
        struct json_object *o = NULL;
        if (json_object_object_get_ex(root, "online", &o))
            online = json_object_get_boolean(o) ? 1 : 0;
        if (json_object_object_get_ex(root, "paused", &o)) {
            int p = json_object_get_boolean(o) ? 1 : 0;
            if (p != g_device_paused) LOG_INF("mqtt 端侧上报状态: %s", p ? "已暂停" : "已恢复");
            g_device_paused = p;
        }
        if (json_object_object_get_ex(root, "stream", &o) && json_object_get_string(o)) {
            const char *u = json_object_get_string(o);
            if (strcmp(u, g_stream_url) != 0) {
                snprintf(g_stream_url, sizeof(g_stream_url), "%s", u);
                LOG_INF("mqtt 端侧视频流地址: %s", g_stream_url);
            }
        }
        json_object_put(root);
    }
    device_state_t *d = device_get(dev);
    if (!d) return;
    if (online >= 0 && d->online != online)
        LOG_INF("mqtt 端侧设备 %s %s", dev, online ? "上线" : "离线（含 LWT）");
    if (online >= 0) d->online = online;
    d->last_seen = now_s();
}

static void handle_event(const char *dev, const uint8_t *payload, size_t len)
{
    char buf[512];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = '\0';

    char kind[32] = "event";
    double conf = 0.0;
    struct json_object *root = json_tokener_parse(buf);
    if (root) {
        struct json_object *o = NULL;
        if (json_object_object_get_ex(root, "event", &o))
            snprintf(kind, sizeof(kind), "%s", json_object_get_string(o));
        if (json_object_object_get_ex(root, "confidence", &o))
            conf = json_object_get_double(o);
        json_object_put(root);
    }

    /* 同设备、10 秒内的最近快照作为事件缩略图 */
    const uint8_t *jpeg = NULL;
    int jlen = 0;
    pthread_mutex_lock(&g_snap_lock);
    if (g_snap_len > 0 && !strcmp(g_snap_device, dev) &&
        now_s() - g_snap_ts <= BRIDGE_THUMB_MAX_AGE_S) {
        jpeg = g_snap_buf;
        jlen = (int)g_snap_len;
    }
    if (jpeg) {                                   /* 交给 event_bus 存一份副本 */
        uint8_t *copy = malloc((size_t)jlen);
        if (copy) memcpy(copy, jpeg, (size_t)jlen);
        jpeg = copy;
    }
    pthread_mutex_unlock(&g_snap_lock);

    event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.created_at = now_s();
    snprintf(ev.event_type, sizeof(ev.event_type), "device_%s", kind);
    ev.confidence = (float)conf;
    snprintf(ev.description, sizeof(ev.description),
             "端侧设备 %s 上报事件 %s (conf=%.2f)", dev, kind, conf);
    snprintf(ev.suggested_action, sizeof(ev.suggested_action), "record");

    long id = event_bus_publish(&ev, jpeg, jlen);
    free((void *)jpeg);
    if (id >= 0) {
        g_events_stored++;
        g_last_event_ts = (long)ev.created_at;
        snprintf(g_last_event_device, sizeof(g_last_event_device), "%s", dev);
        LOG_INF("mqtt 端侧事件入库: device=%s type=%s conf=%.2f → event_id=%ld（含缩略图=%s）",
                dev, kind, conf, id, jlen > 0 ? "是" : "否");
    } else {
        LOG_WRN("mqtt 端侧事件入库失败: device=%s type=%s", dev, kind);
    }
    device_get(dev)->last_seen = now_s();
}

static void handle_snapshot(const char *dev, const uint8_t *payload, size_t len)
{
    char dir[320], path[420];
    snprintf(dir, sizeof(dir), "%s/%s", g_snap_dir, dev);
    mkdir_p(dir);
    snprintf(path, sizeof(path), "%s/%lld_%lu.jpg", dir, (long long)now_s(), ++g_snap_seq);

    FILE *f = fopen(path, "wb");
    if (!f) {
        LOG_WRN("mqtt 快照写盘失败 %s: %s", path, strerror(errno));
        return;
    }
    size_t wr = fwrite(payload, 1, len, f);
    fclose(f);
    if (wr != len) { LOG_WRN("mqtt 快照写入不完整 %s", path); return; }
    g_snapshots_saved++;

    pthread_mutex_lock(&g_snap_lock);
    size_t keep = len < BRIDGE_SNAPSHOT_CACHE ? len : BRIDGE_SNAPSHOT_CACHE;
    if (!g_snap_buf) g_snap_buf = malloc(BRIDGE_SNAPSHOT_CACHE);
    if (g_snap_buf) {
        memcpy(g_snap_buf, payload, keep);
        g_snap_len = keep;
        g_snap_ts = now_s();
        snprintf(g_snap_device, sizeof(g_snap_device), "%s", dev);
    }
    pthread_mutex_unlock(&g_snap_lock);

    LOG_INF("mqtt 端侧快照存盘: %s (%zu 字节) → %s", dev, len, path);
    prune_old_snapshots(dir);

    /* 端侧画面送边端分析流水线（cloud_ai，来源含 device 时才会入队） */
    if (cloud_ai_submit_jpeg(payload, (int)len) == 0) {
        g_frames_to_ai++;
        LOG_DBG("mqtt 端侧快照已投递 AI 分析队列: %s (%zu 字节)", dev, len);
    }
    device_state_t *d = device_get(dev);
    if (d) d->last_seen = now_s();
}

static void on_message(void *user, const char *topic, const uint8_t *payload, size_t len)
{
    (void)user;
    const char *rest = topic + strlen(g_prefix);
    if (*rest == '/') rest++;
    const char *slash = strchr(rest, '/');
    if (!slash) return;
    size_t dl = (size_t)(slash - rest);
    if (dl == 0 || dl >= 32) return;

    char dev[32];
    memcpy(dev, rest, dl);
    dev[dl] = '\0';
    const char *kind = slash + 1;

    if (!strcmp(kind, "status"))        handle_status(dev, payload, len);
    else if (!strcmp(kind, "event"))    handle_event(dev, payload, len);
    else if (!strcmp(kind, "snapshot")) handle_snapshot(dev, payload, len);
}

/* ---------------- 对外接口 ---------------- */

int mqtt_bridge_init(const char *host, int port, const char *client_id,
                     const char *topic_prefix, const char *snapshot_dir)
{
    if (g_running) return 0;
    if (!host || !host[0]) host = "127.0.0.1";
    snprintf(g_prefix, sizeof(g_prefix), "%s", topic_prefix && topic_prefix[0]
             ? topic_prefix : "edgefusion/device");
    snprintf(g_snap_dir, sizeof(g_snap_dir), "%s", snapshot_dir && snapshot_dir[0]
             ? snapshot_dir : "data/remote");
    mkdir_p(g_snap_dir);

    snprintf(g_sub_status,   sizeof(g_sub_status),   "%s/+/status",   g_prefix);
    snprintf(g_sub_event,    sizeof(g_sub_event),    "%s/+/event",    g_prefix);
    snprintf(g_sub_snapshot, sizeof(g_sub_snapshot), "%s/+/snapshot", g_prefix);
    /* 边端也挂遗嘱：网关异常掉线时云端/其他订阅者能看到 */
    snprintf(g_will_topic,   sizeof(g_will_topic),   "%s/edge/status", g_prefix);

    mqtt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.host = host;
    cfg.port = port > 0 ? port : 1883;
    cfg.client_id = client_id && client_id[0] ? client_id : "edgefusion-edge";
    cfg.keepalive_s = 30;
    cfg.reconnect_min_ms = 1000;
    cfg.reconnect_max_ms = 30000;
    cfg.will_topic = g_will_topic;
    cfg.will_payload = "{\"online\":false,\"role\":\"edge\"}";
    cfg.will_qos = 1;
    cfg.sub_topics[0] = g_sub_status;
    cfg.sub_qos[0] = 0;
    cfg.sub_topics[1] = g_sub_event;
    cfg.sub_qos[1] = 1;
    cfg.sub_topics[2] = g_sub_snapshot;
    cfg.sub_qos[2] = 0;
    cfg.sub_count = 3;
    cfg.on_message = on_message;

    g_client = mqtt_client_create(&cfg);
    if (!g_client) {
        LOG_ERR("mqtt_bridge 创建客户端失败（host=%s port=%d）", host, cfg.port);
        return -1;
    }
    if (mqtt_client_start(g_client) != 0) {
        mqtt_client_destroy(g_client);
        g_client = NULL;
        LOG_ERR("mqtt_bridge 启动失败");
        return -1;
    }
    g_running = 1;
    LOG_INF("mqtt_bridge 就绪: broker=%s:%d prefix=%s 快照目录=%s（连接与重连在后台进行）",
            host, cfg.port, g_prefix, g_snap_dir);
    return 0;
}

void mqtt_bridge_deinit(void)
{
    if (!g_running) return;
    mqtt_client_destroy(g_client);
    g_client = NULL;
    g_running = 0;
    pthread_mutex_lock(&g_snap_lock);
    free(g_snap_buf);
    g_snap_buf = NULL;
    g_snap_len = 0;
    pthread_mutex_unlock(&g_snap_lock);
    LOG_INF("mqtt_bridge 已停止");
}

int mqtt_bridge_send_cmd(const char *device_id, const char *cmd_json)
{
    if (!g_running || !g_client || !device_id || !cmd_json) return -1;
    char topic[192];
    snprintf(topic, sizeof(topic), "%s/%s/cmd", g_prefix, device_id);
    int rc = mqtt_client_publish(g_client, topic, cmd_json, strlen(cmd_json), 1, 0);
    if (rc == 0) LOG_INF("mqtt 下发命令 → %s: %s", topic, cmd_json);
    else         LOG_WRN("mqtt 下发命令失败（未连接？）→ %s", topic);
    return rc;
}

void mqtt_bridge_get_stats(mqtt_bridge_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->running = g_running;
    out->devices_known = g_device_count;
    out->events_stored = g_events_stored;
    out->snapshots_saved = g_snapshots_saved;
    out->last_event_ts = g_last_event_ts;
    snprintf(out->last_event_device, sizeof(out->last_event_device), "%s", g_last_event_device);
    snprintf(out->stream_url, sizeof(out->stream_url), "%s", g_stream_url);
    out->device_paused = g_device_paused;
    out->frames_to_ai = g_frames_to_ai;

    int online = 0;
    for (int i = 0; i < g_device_count; ++i)
        if (g_devices[i].online) online++;
    out->devices_online = online;

    if (g_client) {
        mqtt_stats_t st;
        mqtt_client_get_stats(g_client, &st);
        out->connected = st.connected;
        out->reconnects = st.reconnects;
        out->rx_messages = st.rx_messages;
        out->tx_messages = st.tx_messages;
        out->tx_dropped = st.tx_dropped;
    }
}

/* 在快照目录下找最近修改的 .jpg（同秒多张时按文件名序号取最大） */
int mqtt_bridge_latest_snapshot(char *out, size_t cap)
{
    if (!out || cap == 0 || !g_snap_dir[0]) return -1;
    DIR *root = opendir(g_snap_dir);
    if (!root) return -1;

    char best_path[1024] = {0};
    long long best_key = -1;
    struct dirent *de;
    while ((de = readdir(root)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char devdir[512];
        snprintf(devdir, sizeof(devdir), "%s/%.200s", g_snap_dir, de->d_name);
        DIR *d = opendir(devdir);
        if (!d) continue;
        struct dirent *fe;
        while ((fe = readdir(d)) != NULL) {
            const char *dot = strrchr(fe->d_name, '.');
            if (!dot || strcasecmp(dot, ".jpg") != 0) continue;
            /* 文件名形如 <unix秒>_<序号>.jpg */
            long long secs = atoll(fe->d_name);
            long long seq = 0;
            const char *us = strchr(fe->d_name, '_');
            if (us) seq = atoll(us + 1);
            long long key = secs * 1000 + (seq % 1000);
            if (key > best_key) {
                best_key = key;
                snprintf(best_path, sizeof(best_path), "%s/%.200s", devdir, fe->d_name);
            }
        }
        closedir(d);
    }
    closedir(root);
    if (best_key < 0) return -1;
    snprintf(out, cap, "%s", best_path);
    return 0;
}

/* 端侧快照会持续累积（每几秒一张），这里按数量保留最近 N 张，避免把 /tmp 或磁盘写满。
 * 每写 50 张检查一次；按文件名里的时间戳排序，删最旧的。 */
#define BRIDGE_SNAPSHOT_KEEP 500
static unsigned g_prune_tick;

static void prune_old_snapshots(const char *devdir)
{
    if (++g_prune_tick % 50 != 0) return;

    DIR *d = opendir(devdir);
    if (!d) return;
    char names[BRIDGE_SNAPSHOT_KEEP + 64][64];
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *dot = strrchr(de->d_name, '.');
        if (!dot || strcasecmp(dot, ".jpg") != 0) continue;
        if (n < (int)(sizeof(names) / sizeof(names[0])))
            snprintf(names[n++], sizeof(names[0]), "%.40s", de->d_name);
    }
    closedir(d);
    if (n <= BRIDGE_SNAPSHOT_KEEP) return;

    /* 文件名形如 <unix秒>_<序号>.jpg，按字符串排序即按时间排序 */
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (strcmp(names[j], names[i]) < 0) {
                char tmp[64];
                memcpy(tmp, names[i], sizeof(tmp));
                memcpy(names[i], names[j], sizeof(names[j]));
                memcpy(names[j], tmp, sizeof(tmp));
            }

    int removed = 0;
    for (int i = 0; i < n - BRIDGE_SNAPSHOT_KEEP; ++i) {
        char path[640];
        snprintf(path, sizeof(path), "%s/%.200s", devdir, names[i]);
        if (unlink(path) == 0) removed++;
    }
    if (removed > 0)
        LOG_INF("mqtt 端侧快照清理: 删除最旧 %d 张（保留 %d 张）", removed, BRIDGE_SNAPSHOT_KEEP);
}

#ifndef EDGEFUSION_MQTT_BRIDGE_H
#define EDGEFUSION_MQTT_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

/* mqtt_bridge：网关（边）侧的 MQTT 桥接。
 *
 * 复用自研 mqtt_client（src/mqtt_client.c），把端侧设备
 * （edgefusion_vision/device_agent/mqtt_device_agent.py）接进网关：
 *
 *   <prefix>/<device>/status    设备心跳/状态（保留消息；含 LWT 离线）
 *   <prefix>/<device>/event     端侧事件 → 写入本地事件库 event_bus(SQLite)
 *   <prefix>/<device>/snapshot  JPEG 快照 → 存盘 <snapshot_dir>/<device>/
 *   <prefix>/<device>/cmd       边→端命令（由 Web 触发）
 *
 * 设计原则：broker 不可用时本模块只自己重连，不影响录像/预览主链路；
 * MQTT 只承载控制、事件与快照，视频仍走 RTSP/HTTP。
 */

typedef struct {
    int                running;         /* 模块是否已启动 */
    int                connected;       /* MQTT 连接状态 */
    int                reconnects;      /* 累计重连次数 */
    unsigned long long rx_messages;
    unsigned long long tx_messages;
    unsigned long long tx_dropped;      /* 未连接时提交失败(丢弃)的命令数 */
    unsigned long long events_stored;   /* 写入事件库的端侧事件数 */
    unsigned long long snapshots_saved; /* 存盘的端侧快照数 */
    int                devices_online;  /* 当前在线设备数 */
    int                devices_known;   /* 见过的设备数 */
    long               last_event_ts;   /* 最近一条端侧事件时间戳(0=无) */
    char               last_event_device[32];
    char               stream_url[160];     /* 端侧上报的视频流地址(HTTP MJPEG) */
    int                device_paused;   /* 端侧自报的暂停状态(1=已暂停上报) */
} mqtt_bridge_stats_t;

/* host/port：broker 地址；topic_prefix 例 "edgefusion/device"；
 * snapshot_dir 例 "data/remote"（不存在会自动创建）。成功返回 0。 */
int  mqtt_bridge_init(const char *host, int port, const char *client_id,
                      const char *topic_prefix, const char *snapshot_dir);
void mqtt_bridge_deinit(void);

/* 向指定端侧设备下发命令（cmd_json 为完整 JSON 文本，例 {"cmd":"snapshot"}）。
 * 未连接或参数非法返回 -1 */
int  mqtt_bridge_send_cmd(const char *device_id, const char *cmd_json);

void mqtt_bridge_get_stats(mqtt_bridge_stats_t *out);

/* 取最近一张端侧快照的完整路径（用于 Web 展示）。成功返回 0，无快照返回 -1 */
int  mqtt_bridge_latest_snapshot(char *out, size_t cap);

#endif

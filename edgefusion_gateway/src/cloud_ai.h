#ifndef EDGEFUSION_CLOUD_AI_H
#define EDGEFUSION_CLOUD_AI_H

#include "conf.h"
#include <stdint.h>

/* cloud_ai: 消费 frame_grabber 最新 JPEG，调云端(OpenAI 兼容端点) 做异常检测，
 * 解析返回的结构化 JSON，把事件 publish 到 event_bus（含快照）。
 * 启用条件：ai_enable=1 且 token 非空。任一不满足则进入纯本地模式（不调云）。
 * 支持热启停：reload 时若开关状态翻转，会自动启动/停止线程。 */

int  cloud_ai_start(const gateway_conf_t *cfg);
void cloud_ai_stop(void);

/* 运行状态快照 */
typedef struct {
    int  enabled;   /* token 已配置且线程在跑 */
    int  running;   /* 线程存活 */
} cloud_ai_status_t;
int  cloud_ai_get_status(cloud_ai_status_t *st);

/* 投递一帧（JPEG）给分析线程：端侧上报的快照走这里进来。
 * 线程未运行、或 ai_source 不含 device 时返回 -1（丢弃，不阻塞调用方）。
 * 队列有界（满则丢最旧），因此 MQTT 回调/录像主链路都不会被云端调用拖慢。 */
int  cloud_ai_submit_jpeg(const uint8_t *jpeg, int len);

/* 投递统计：成功入队帧数、因队列满丢弃帧数 */
void cloud_ai_submit_stats(unsigned long long *accepted, unsigned long long *dropped);

/* 热重载配置（ai_enable/ai_fps/ai_prompt/anthropic_model/anthropic_base_url）。
 * ai_enable 翻转时会热启停线程。 */
void cloud_ai_reload(const gateway_conf_t *cfg);

/* B3：分析流水线运行指标 */
typedef struct {
    int                configured;        /* ai_enable 且 token 非空 */
    int                src_local;         /* 当前来源含网关摄像头 */
    int                src_device;        /* 当前来源含端侧上报快照 */
    int                last_call_ms;      /* 最近一次云端调用耗时(ms) */
    unsigned long long frames_submitted;  /* 投递给分析的帧数 */
    unsigned long long frames_dropped;    /* 队列满丢弃的帧数 */
    char               source[16];        /* local|device|both|off */
} cloud_ai_stats_t;
void cloud_ai_get_stats(cloud_ai_stats_t *out);

#endif

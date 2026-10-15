#include "cloud_ai.h"
#include <stdatomic.h>
#include "log.h"
#include "frame_grabber.h"
#include "event_bus.h"

#include <curl/curl.h>
#include <json-c/json.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Base64 编码，返回 malloc 缓冲(无填充也加'='，NUL 结尾)。调用者 free */
static char *b64_encode(const uint8_t *src, int len)
{
    int out_len = 4 * ((len + 2) / 3) + 1;
    char *out = malloc(out_len);
    if (!out) return NULL;
    int o = 0;
    for (int i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < len) v |= (uint32_t)src[i + 1] << 8;
        if (i + 2 < len) v |= src[i + 2];
        out[o++] = B64[(v >> 18) & 0x3f];
        out[o++] = B64[(v >> 12) & 0x3f];
        out[o++] = (i + 1 < len) ? B64[(v >> 6) & 0x3f] : '=';
        out[o++] = (i + 2 < len) ? B64[v & 0x3f] : '=';
    }
    out[o] = 0;
    return out;
}

/* ---------- libcurl 响应缓冲 ---------- */
struct resp_buf {
    char  *data;
    size_t size;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *user)
{
    struct resp_buf *r = (struct resp_buf *)user;
    size_t n = size * nmemb;
    char *nb = realloc(r->data, r->size + n + 1);
    if (!nb) return 0;
    r->data = nb;
    memcpy(r->data + r->size, ptr, n);
    r->size += n;
    r->data[r->size] = 0;
    return n;
}

/* ---------- 配置缓存 ---------- */
static gateway_conf_t g_cfg;
static pthread_mutex_t g_cfg_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile bool  g_running = false;
static pthread_t      g_tid;

/* JSON 转义不必要——用 json-c 构造请求体，自动转义 prompt */

/* 构造 OpenAI Chat Completions 请求体(JSON 字符串)，图像走 image_url data URL。
 * 适用于阿里百炼/DashScope 的 OpenAI 兼容端点（qwen-vl / qwen3.x-plus 等多模态模型）。
 * 返回 malloc，失败 NULL */
static char *build_request_body(const char *prompt, const char *model, const char *b64_img)
{
    struct json_object *root = json_object_new_object();
    json_object_object_add(root, "model", json_object_new_string(model));
    json_object_object_add(root, "max_tokens", json_object_new_int(300));

    struct json_object *content = json_object_new_array();
    struct json_object *txt_blk = json_object_new_object();
    json_object_object_add(txt_blk, "type", json_object_new_string("text"));
    json_object_object_add(txt_blk, "text", json_object_new_string(prompt));
    json_object_array_add(content, txt_blk);

    /* data URL: data:image/jpeg;base64,<...> */
    char *data_url = malloc(strlen(b64_img) + 32);
    if (!data_url) { json_object_put(root); return NULL; }
    sprintf(data_url, "data:image/jpeg;base64,%s", b64_img);

    struct json_object *img_blk = json_object_new_object();
    json_object_object_add(img_blk, "type", json_object_new_string("image_url"));
    struct json_object *iu = json_object_new_object();
    json_object_object_add(iu, "url", json_object_new_string(data_url));
    json_object_object_add(img_blk, "image_url", iu);
    json_object_array_add(content, img_blk);

    struct json_object *msg = json_object_new_object();
    json_object_object_add(msg, "role", json_object_new_string("user"));
    json_object_object_add(msg, "content", content);

    struct json_object *messages = json_object_new_array();
    json_object_array_add(messages, msg);
    json_object_object_add(root, "messages", messages);

    /* 强制 JSON 输出：很多模型（含 deepseek-flash）不加这个会先输出一段解释文字，
     * 解析器就找不到 JSON，事件全部被丢掉。OpenAI 兼容端点普遍支持该字段。 */
    struct json_object *rf = json_object_new_object();
    json_object_object_add(rf, "type", json_object_new_string("json_object"));
    json_object_object_add(root, "response_format", rf);

    /* qwen3 系列默认开启思考(reasoning)，会拖慢响应且多耗 token；
     * 我们只要结构化 JSON，关闭思考以获得干净直接的 content 输出 */
    json_object_object_add(root, "enable_thinking", json_object_new_boolean(0));

    free(data_url);
    const char *s = json_object_to_json_string(root);
    char *out = strdup(s);
    json_object_put(root);
    return out;
}

/* 从模型文本输出中提取首个 {...} 子串(剥 ```json 围栏)。返回 malloc 或 NULL */

/* 把不可打印字节替换为 '.'（保留 >=0x20 的 UTF-8 字节），避免日志变成二进制文件 */
static void sanitize_into(const char *src, char *dst, size_t cap, size_t maxlen)
{
    size_t o = 0;
    if (!dst || cap < 2) return;
    for (size_t i = 0; src && src[i] && i < maxlen && o + 1 < cap; ++i) {
        unsigned char c = (unsigned char)src[i];
        dst[o++] = (c >= 0x20) ? (char)c : '.';
    }
    dst[o] = '\0';
}

/* 从模型输出里取出第一个"完整的 JSON 对象"。
 * 关键：必须识别字符串内的花括号（描述文本里可能出现 { 或 }），
 * 否则括号计数会错乱 —— 这是此前约 1/3 调用"未含 JSON"的根因。 */
static char *extract_json_object(const char *text)
{
    if (!text) return NULL;
    const char *p = strchr(text, '{');
    if (!p) return NULL;

    int depth = 0;
    int in_str = 0, esc = 0;
    const char *start = p;
    for (; *p; p++) {
        char c = *p;
        if (in_str) {
            if (esc)                esc = 0;
            else if (c == '\\')    esc = 1;
            else if (c == '"')      in_str = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                size_t len = (size_t)(p - start) + 1;
                char *out = malloc(len + 1);
                if (out) { memcpy(out, start, len); out[len] = 0; }
                return out;
            }
        }
    }
    return NULL;
}

/* 解析模型返回 JSON → 填充 ev。成功返回 0 */
static int parse_event_json(const char *json_str, event_t *ev)
{
    struct json_object *root = json_tokener_parse(json_str);
    if (!root) return -1;
    struct json_object *o;
    if (json_object_object_get_ex(root, "event_type", &o) && json_object_is_type(o, json_type_string))
        strncpy(ev->event_type, json_object_get_string(o), sizeof(ev->event_type) - 1);
    if (json_object_object_get_ex(root, "confidence", &o))
        ev->confidence = (float)json_object_get_double(o);
    if (json_object_object_get_ex(root, "description", &o) && json_object_is_type(o, json_type_string))
        strncpy(ev->description, json_object_get_string(o), sizeof(ev->description) - 1);
    if (json_object_object_get_ex(root, "suggested_action", &o) && json_object_is_type(o, json_type_string))
        strncpy(ev->suggested_action, json_object_get_string(o), sizeof(ev->suggested_action) - 1);
    json_object_put(root);
    if (ev->event_type[0] == 0) strcpy(ev->event_type, "none");
    return 0;
}

/* 解析 OpenAI Chat Completions 响应，取 choices[0].message.content。
 * content 可能是字符串，也可能是 content block 数组(兼容部分实现)。返回 malloc 文本或 NULL */
static char *parse_openai_response(const char *body)
{
    struct json_object *root = json_tokener_parse(body);
    if (!root) return NULL;
    char *text_out = NULL;
    struct json_object *choices;
    if (json_object_object_get_ex(root, "choices", &choices) && json_object_is_type(choices, json_type_array)) {
        struct json_object *choice = json_object_array_get_idx(choices, 0);
        struct json_object *message;
        if (choice && json_object_object_get_ex(choice, "message", &message)) {
            struct json_object *content;
            if (json_object_object_get_ex(message, "content", &content)) {
                if (json_object_is_type(content, json_type_string)) {
                    text_out = strdup(json_object_get_string(content));
                } else if (json_object_is_type(content, json_type_array)) {
                    /* 数组形式：取首个 type==text 的 text */
                    int n = json_object_array_length(content);
                    for (int i = 0; i < n; i++) {
                        struct json_object *blk = json_object_array_get_idx(content, i);
                        struct json_object *t, *txt;
                        if (json_object_object_get_ex(blk, "type", &t) &&
                            json_object_is_type(t, json_type_string) &&
                            strcmp(json_object_get_string(t), "text") == 0 &&
                            json_object_object_get_ex(blk, "text", &txt) &&
                            json_object_is_type(txt, json_type_string)) {
                            text_out = strdup(json_object_get_string(txt));
                            break;
                        }
                    }
                }
            }
        }
    }
    json_object_put(root);
    return text_out;
}

/* 调云端(OpenAI 兼容端点)，成功返回响应体(malloc)，失败 NULL。*http_code 回填 */
static char *call_cloud(const char *body, long *http_code)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    /* base_url 形如 https://.../compatible-mode/v1，拼 /chat/completions */
    char url[512];
    snprintf(url, sizeof(url), "%s/chat/completions", g_cfg.anthropic_base_url);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    char kh[256 + 64];
    snprintf(kh, sizeof(kh), "Authorization: Bearer %s", g_cfg.anthropic_auth_token);
    hdrs = curl_slist_append(hdrs, kh);

    struct resp_buf rbuf = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rbuf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)g_cfg.ai_http_timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    /* 板子 buildroot 默认无 CA 库，显式指定 CA bundle（由部署脚本推送） */
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    *http_code = code;
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        LOG_WRN("cloud_ai curl 失败: %s", curl_easy_strerror(rc));
        free(rbuf.data);
        return NULL;
    }
    return rbuf.data;
}

static void process_one_frame(uint8_t *jpeg, int sz)
{
    char *b64 = b64_encode(jpeg, sz);
    if (!b64) return;
    char *body = build_request_body(g_cfg.ai_prompt, g_cfg.anthropic_model, b64);
    free(b64);
    if (!body) return;

    long http_code = 0;
    char *resp = call_cloud(body, &http_code);
    free(body);
    if (!resp) return;

    if (http_code != 200) {
        LOG_WRN("cloud_ai HTTP %ld: %.200s", http_code, resp);
        free(resp);
        return;
    }

    /* 注意：先解析、后释放；日志必须在释放之前打印（此前顺序反了，打印的是已释放内存，
     * 表现为"乱码"，把真正的失败原因掩盖了）。 */
    char *model_text = parse_openai_response(resp);
    if (!model_text) {
        char b[300];
        sanitize_into(resp ? resp : "(null)", b, sizeof(b), 200);
        LOG_WRN("cloud_ai 响应无 content | HTTP=%ld 响应长度=%zu | 响应前200=%s",
                http_code, resp ? strlen(resp) : 0, b);
        free(resp);
        return;
    }
    char *json_str = extract_json_object(model_text);
    if (!json_str) {
        char a[300], b[300];
        size_t rl = resp ? strlen(resp) : 0;
        sanitize_into(model_text, a, sizeof(a), 220);
        sanitize_into(rl > 220 ? resp + rl - 220 : (resp ? resp : ""), b, sizeof(b), 220);
        LOG_WRN("cloud_ai 未含 JSON | HTTP=%ld 响应长度=%zu 末字符=%c | content长度=%zu 含左括号=%d"
                " | content=%.220s | 响应尾部=%.220s",
                http_code, rl, rl ? resp[rl - 1] : '?', strlen(model_text),
                strchr(model_text, '{') ? 1 : 0, a, b);
        free(model_text);
        free(resp);
        return;
    }
    free(model_text);
    free(resp);

    event_t ev;
    memset(&ev, 0, sizeof(ev));
    if (parse_event_json(json_str, &ev) == 0) {
        /* 只对真实事件入库（none 不存，避免刷库）；快照用触发帧 */
        if (strcmp(ev.event_type, "none") != 0) {
            long id = event_bus_publish(&ev, jpeg, sz);
            LOG_INF("cloud_ai 检测事件 id=%ld type=%s conf=%.2f desc=%s", id, ev.event_type, ev.confidence, ev.description);
        } else {
            LOG_DBG("cloud_ai 无异常: %s", ev.description);
        }
    } else {
        LOG_WRN("cloud_ai JSON 解析失败: %.200s", json_str);
    }
    free(json_str);
}

/* ---------- 待分析帧队列（端侧快照与本地帧的统一入口） ---------- */
#define AI_QUEUE_MAX 4

static uint8_t          *g_q_buf[AI_QUEUE_MAX];
static int               g_q_len[AI_QUEUE_MAX];
static int               g_q_count;
static pthread_mutex_t   g_q_mtx = PTHREAD_MUTEX_INITIALIZER;
static atomic_int        g_accept_device;          /* 由分析线程按 ai_source 设置 */
static unsigned long long g_q_accepted, g_q_dropped;
static atomic_int         g_src_local, g_src_device;
static atomic_int         g_last_call_ms;      /* 最近一次云端调用耗时(ms) */

int cloud_ai_submit_jpeg(const uint8_t *jpeg, int len)
{
    if (!g_running || !jpeg || len <= 0) return -1;
    if (!atomic_load(&g_accept_device)) return -1;   /* 当前来源不含端侧帧 */

    uint8_t *copy = malloc((size_t)len);
    if (!copy) return -1;
    memcpy(copy, jpeg, (size_t)len);

    pthread_mutex_lock(&g_q_mtx);
    if (g_q_count >= AI_QUEUE_MAX) {                 /* 满：丢最旧，保证延迟不堆积 */
        free(g_q_buf[0]);
        memmove(&g_q_buf[0], &g_q_buf[1], sizeof(g_q_buf[0]) * (size_t)(AI_QUEUE_MAX - 1));
        memmove(&g_q_len[0], &g_q_len[1], sizeof(g_q_len[0]) * (size_t)(AI_QUEUE_MAX - 1));
        g_q_count--;
        g_q_dropped++;
    }
    g_q_buf[g_q_count] = copy;
    g_q_len[g_q_count] = len;
    g_q_count++;
    g_q_accepted++;
    pthread_mutex_unlock(&g_q_mtx);
    return 0;
}

static uint8_t *ai_queue_take(int *len)
{
    uint8_t *out = NULL;
    pthread_mutex_lock(&g_q_mtx);
    if (g_q_count > 0) {
        out = g_q_buf[0];
        *len = g_q_len[0];
        memmove(&g_q_buf[0], &g_q_buf[1], sizeof(g_q_buf[0]) * (size_t)(AI_QUEUE_MAX - 1));
        memmove(&g_q_len[0], &g_q_len[1], sizeof(g_q_len[0]) * (size_t)(AI_QUEUE_MAX - 1));
        g_q_count--;
    }
    pthread_mutex_unlock(&g_q_mtx);
    return out;
}

void cloud_ai_submit_stats(unsigned long long *accepted, unsigned long long *dropped)
{
    pthread_mutex_lock(&g_q_mtx);
    if (accepted) *accepted = g_q_accepted;
    if (dropped)  *dropped  = g_q_dropped;
    pthread_mutex_unlock(&g_q_mtx);
}

static void *ai_thread(void *arg)
{
    (void)arg;
    /* 启用条件：ai_enable 且 token 非空。否则纯本地模式，不调云 */
    pthread_mutex_lock(&g_cfg_mtx);
    int ok = g_cfg.ai_enable && (g_cfg.anthropic_auth_token[0] != 0);
    pthread_mutex_unlock(&g_cfg_mtx);
    if (!ok) {
        LOG_WRN("cloud_ai: 未启用（ai_enable=0 或 token 为空），纯本地模式，不调云");
        return NULL;
    }
    LOG_INF("cloud_ai 线程启动: %s model=%s fps=%d", g_cfg.anthropic_base_url, g_cfg.anthropic_model, g_cfg.ai_fps);

    int interval_us = (g_cfg.ai_fps > 0) ? (1000000 / g_cfg.ai_fps) : 1000000;
    /* 帧来源：local=网关自己的摄像头；device=端侧上报的快照；both=两者都分析。
     * device/both 时由 mqtt_bridge 调 cloud_ai_submit_jpeg() 投递。 */
    int src_local  = strcmp(g_cfg.ai_source, "device") != 0;
    int src_device = strcmp(g_cfg.ai_source, "local")  != 0;
    atomic_store(&g_accept_device, src_device);
    atomic_store(&g_src_local, src_local);
    atomic_store(&g_src_device, src_device);
    LOG_INF("cloud_ai 帧来源: %s (local=%d device=%d)", g_cfg.ai_source, src_local, src_device);

    while (g_running) {
        int sz = 0;
        /* 1) 外部投递的帧（端侧快照）优先，保证端侧事件不被本地帧挤掉 */
        uint8_t *jpeg = src_device ? ai_queue_take(&sz) : NULL;

        /* 2) 本地源：取 frame_grabber 的最新 JPEG（返回值需 free） */
        if (!jpeg && src_local) {
            sz = frame_grabber_get_jpeg(&jpeg);
            if (sz <= 0 || !jpeg) {
                if (jpeg) free(jpeg);
                usleep(interval_us);
                continue;
            }
        }
        if (!jpeg) { usleep(interval_us); continue; }

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        process_one_frame(jpeg, sz);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        atomic_store(&g_last_call_ms,
                     (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
        free(jpeg);
        /* 调用可能耗时数秒，这里只做上限节流 */
        if (g_running) usleep(interval_us);
    }
    return NULL;
}

int cloud_ai_start(const gateway_conf_t *cfg)
{
    if (g_running) return 0;
    pthread_mutex_lock(&g_cfg_mtx);
    g_cfg = *cfg;
    pthread_mutex_unlock(&g_cfg_mtx);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_running = true;
    if (pthread_create(&g_tid, NULL, ai_thread, NULL) != 0) {
        g_running = false;
        LOG_ERR("创建 cloud_ai 线程失败");
        return -1;
    }
    return 0;
}

int cloud_ai_get_status(cloud_ai_status_t *st)
{
    if (!st) return -1;
    pthread_mutex_lock(&g_cfg_mtx);
    int enabled_cfg = g_cfg.ai_enable && (g_cfg.anthropic_auth_token[0] != 0);
    pthread_mutex_unlock(&g_cfg_mtx);
    st->running = g_running;
    /* enabled = 配置允许上云 且 线程在跑 */
    st->enabled = enabled_cfg && st->running;
    return 0;
}

void cloud_ai_reload(const gateway_conf_t *cfg)
{
    pthread_mutex_lock(&g_cfg_mtx);
    int was_enabled = g_cfg.ai_enable && (g_cfg.anthropic_auth_token[0] != 0);
    /* 仅热更新可热生效字段，保留不可热改的（如 token 用原值） */
    g_cfg.ai_enable = cfg->ai_enable;
    g_cfg.ai_fps = cfg->ai_fps;
    g_cfg.ai_http_timeout_s = cfg->ai_http_timeout_s;
    strncpy(g_cfg.ai_prompt, cfg->ai_prompt, sizeof(g_cfg.ai_prompt) - 1);
    g_cfg.ai_prompt[sizeof(g_cfg.ai_prompt) - 1] = 0;
    strncpy(g_cfg.anthropic_model, cfg->anthropic_model, sizeof(g_cfg.anthropic_model) - 1);
    g_cfg.anthropic_model[sizeof(g_cfg.anthropic_model) - 1] = 0;
    strncpy(g_cfg.anthropic_base_url, cfg->anthropic_base_url, sizeof(g_cfg.anthropic_base_url) - 1);
    /* token 也热更新：这样在网页上填完 Key 立即生效（此前只更新 url/模型/prompt）*/
    strncpy(g_cfg.anthropic_auth_token, cfg->anthropic_auth_token,
            sizeof(g_cfg.anthropic_auth_token) - 1);
    g_cfg.anthropic_auth_token[sizeof(g_cfg.anthropic_auth_token) - 1] = 0;
    g_cfg.anthropic_base_url[sizeof(g_cfg.anthropic_base_url) - 1] = 0;
    int now_enabled = g_cfg.ai_enable && (g_cfg.anthropic_auth_token[0] != 0);
    pthread_mutex_unlock(&g_cfg_mtx);

    /* 开关状态翻转 → 热启停线程。
     * 注意：cloud_ai_stop 会 join 线程并 curl_global_cleanup；重新 start 会重新 init。
     * reload 在 web 线程上下文调用，join 上云线程是安全的（上云线程无锁依赖 web 线程）。 */
    if (!was_enabled && now_enabled) {
        LOG_INF("cloud_ai 开关已开启，启动线程");
        cloud_ai_stop();
        cloud_ai_start(cfg);
    } else if (was_enabled && !now_enabled) {
        LOG_INF("cloud_ai 开关已关闭，停止线程");
        cloud_ai_stop();
    } else {
        LOG_INF("cloud_ai 配置已热重载");
    }
}

void cloud_ai_stop(void)
{
    if (!g_running) return;
    g_running = false;
    pthread_join(g_tid, NULL);
    curl_global_cleanup();
    atomic_store(&g_accept_device, 0);
    pthread_mutex_lock(&g_q_mtx);
    for (int i = 0; i < g_q_count; ++i) free(g_q_buf[i]);
    g_q_count = 0;
    pthread_mutex_unlock(&g_q_mtx);
    LOG_INF("cloud_ai 线程已停止");
}

/* B3：分析流水线运行指标（供 /api/status 与网页显示） */
void cloud_ai_get_stats(cloud_ai_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_cfg_mtx);
    out->configured = g_cfg.ai_enable && (g_cfg.anthropic_auth_token[0] != 0);
    snprintf(out->source, sizeof(out->source), "%s",
             atomic_load(&g_src_device) && atomic_load(&g_src_local) ? "both" :
             atomic_load(&g_src_device) ? "device" :
             atomic_load(&g_src_local)  ? "local"  : "off");
    pthread_mutex_unlock(&g_cfg_mtx);
    out->src_local  = atomic_load(&g_src_local);
    out->src_device = atomic_load(&g_src_device);
    out->last_call_ms = atomic_load(&g_last_call_ms);
    cloud_ai_submit_stats(&out->frames_submitted, &out->frames_dropped);
}

// main/podcast_api.c —— 中转服务 HTTP API 客户端实现。
#include "podcast_api.h"

#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "mdns.h"

static const char *TAG = "podcast_api";

// 按 wyy-musicplayer 的经验:固件缓冲区是最后一道防线,服务端响应预算才是
// 第一道。中转的 /api/latest 只有 6 个小字段,4KB 上限绰绰有余,超了直接拒收。
#define API_RESPONSE_MAX_BYTES 4096

typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
    bool    overflow;
} api_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    api_response_t *resp = (api_response_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && resp && !resp->overflow) {
        if (resp->len + evt->data_len >= resp->cap) {
            resp->overflow = true;
            ESP_LOGE(TAG, "响应超过 %u 字节上限,拒收", API_RESPONSE_MAX_BYTES);
            return ESP_FAIL;
        }
        memcpy(resp->buf + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
    }
    return ESP_OK;
}

// 取 published_at 的前 10 个字符作日期 "YYYY-MM-DD";格式不对就留空。
static void extract_date(const char *iso, char *out, size_t out_len) {
    out[0] = '\0';
    if (!iso || strlen(iso) < 10) return;
    // 形如 2026-10-11T... 或 2026-10-11,前 10 字节必须是数字和 '-'。
    for (int i = 0; i < 10; i++) {
        char c = iso[i];
        if (!((c >= '0' && c <= '9') || c == '-')) return;
    }
    if (iso[4] != '-' || iso[7] != '-') return;
    strncpy(out, iso, out_len - 1);
    out[out_len - 1] = '\0';
}

esp_err_t podcast_api_resolve_base(char *out, size_t out_len) {
    if (!out || out_len < 24) return ESP_ERR_INVALID_ARG;

    const char *host = CONFIG_PODCAST_RELAY_HOST;
    const int port = CONFIG_PODCAST_RELAY_PORT;
    char ip_str[32] = {0};

    // 优先 mDNS:中转服务上线时会广播 wj-relay.local,省掉手动配 IP。
    if (strstr(host, ".local") != NULL) {
        esp_ip4_addr_t addr = {0};
        esp_err_t err = mdns_query_a(host, 2000, &addr);
        if (err == ESP_OK && addr.addr != 0) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&addr));
            ESP_LOGI(TAG, "mDNS 解析 %s -> %s", host, ip_str);
            host = ip_str;
        } else {
            ESP_LOGW(TAG, "mDNS 解析 %s 失败(%s),回退直连主机名",
                     host, esp_err_to_name(err));
        }
    }

    int n = snprintf(out, out_len, "http://%s:%d", host, port);
    if (n <= 0 || (size_t)n >= out_len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

esp_err_t podcast_api_fetch_latest(const char *base, podcast_episode_t *out) {
    if (!base || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    char url[320];
    int n = snprintf(url, sizeof(url), "%s/api/latest", base);
    if (n <= 0 || (size_t)n >= sizeof(url)) return ESP_ERR_INVALID_SIZE;

    char *buf = malloc(API_RESPONSE_MAX_BYTES);
    if (!buf) return ESP_ERR_NO_MEM;
    api_response_t resp = { .buf = buf, .cap = API_RESPONSE_MAX_BYTES, .len = 0 };

    esp_http_client_config_t cfg = {0};
    cfg.url = url;
    cfg.timeout_ms = 8000;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 512;
    cfg.user_agent = "WJ-Podcast/1.0";
    cfg.event_handler = http_event_handler;
    cfg.user_data = &resp;

    esp_err_t err = ESP_FAIL;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client 创建失败");
        goto done;
    }
    if (esp_http_client_perform(client) != ESP_OK) {
        ESP_LOGW(TAG, "GET %s 失败", url);
        goto cleanup;
    }
    if (resp.overflow) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "/api/latest HTTP %d", status);
        goto cleanup;
    }
    buf[resp.len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "JSON 解析失败");
        goto cleanup;
    }
    const cJSON *j_id = cJSON_GetObjectItem(root, "video_id");
    const cJSON *j_title = cJSON_GetObjectItem(root, "title");
    const cJSON *j_pub = cJSON_GetObjectItem(root, "published_at");
    const cJSON *j_dur = cJSON_GetObjectItem(root, "duration_sec");
    const cJSON *j_url = cJSON_GetObjectItem(root, "audio_url");
    if (!cJSON_IsString(j_id) || !cJSON_IsString(j_title) ||
        !cJSON_IsString(j_url)) {
        ESP_LOGW(TAG, "JSON 缺少必要字段");
        cJSON_Delete(root);
        goto cleanup;
    }
    strncpy(out->video_id, j_id->valuestring, sizeof(out->video_id) - 1);
    strncpy(out->title, j_title->valuestring, sizeof(out->title) - 1);
    strncpy(out->audio_url, j_url->valuestring, sizeof(out->audio_url) - 1);
    extract_date(cJSON_IsString(j_pub) ? j_pub->valuestring : NULL,
                 out->date, sizeof(out->date));
    if (cJSON_IsNumber(j_dur)) out->duration_sec = (uint32_t)j_dur->valuedouble;
    cJSON_Delete(root);

    if (out->video_id[0] == '\0' || out->audio_url[0] == '\0') {
        ESP_LOGW(TAG, "节目信息为空");
        goto cleanup;
    }
    ESP_LOGI(TAG, "最新一期: %s (%s, %us)", out->title, out->date,
             (unsigned)out->duration_sec);
    err = ESP_OK;

cleanup:
    if (client) esp_http_client_cleanup(client);
done:
    free(buf);
    return err;
}

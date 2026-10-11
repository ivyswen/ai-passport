// main/podcast_api.h —— 中转服务 HTTP API 客户端。
//
// 只做三件事(都只在后台任务里调用,不要在按键回调/LVGL 回调里调):
//   1. 解析中转服务地址:优先 mDNS(wj-relay.local),失败回退 Kconfig 配置。
//   2. GET /api/latest 拿最新一期节目信息(小 JSON,上限 4KB)。
//   3. GET /api/episodes 拿节目列表(发布时间倒序,上限 8KB)。
#pragma once

#include "esp_err.h"
#include "podcast.h"

// 把中转服务解析成 "http://<ip或主机>:<port>" 写入 out(保证 NUL 结尾)。
// 成功返回 ESP_OK;DNS/mDNS 都失败返回 ESP_FAIL。
esp_err_t podcast_api_resolve_base(char *out, size_t out_len);

// 拉取最新一期。base 由 podcast_api_resolve_base 得到。
// 成功返回 ESP_OK 且填好 out;失败返回错误码并打日志(调用方显示状态即可)。
esp_err_t podcast_api_fetch_latest(const char *base, podcast_episode_t *out);

// 拉取节目列表(发布时间倒序,最多 max_out 期)。成功返回实际期数(>=0),
// 失败返回 -1(此时 out 内容不可用,调用方保留旧列表)。
int podcast_api_fetch_episodes(const char *base, podcast_episode_t *out, int max_out);

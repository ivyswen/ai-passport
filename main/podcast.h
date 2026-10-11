// main/podcast.h —— 王剑每日观察播客机：共享类型定义。
//
// 整机只有一件事：从局域网中转服务拉取 YouTube 频道「王剑每日观察」
// 最新一期的音频(MP3 流)，解码后经 ES8311 播放。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PODCAST_TITLE_MAX    192   // UTF-8 节目标题(含 NUL)
#define PODCAST_VIDEO_ID_MAX 32
#define PODCAST_URL_MAX      256   // 音频相对路径 "/audio/<id>.mp3"
#define PODCAST_DATE_MAX     16    // "YYYY-MM-DD" + NUL
#define PODCAST_EPISODES_MAX 24    // 设备端节目单上限(内存与单次解析开销的折中)

// 中转服务返回的单期节目信息(对应 GET /api/latest 的 JSON)。
typedef struct {
    char     video_id[PODCAST_VIDEO_ID_MAX];
    char     title[PODCAST_TITLE_MAX];
    char     date[PODCAST_DATE_MAX];
    char     audio_url[PODCAST_URL_MAX];
    uint32_t duration_sec;          // 0 表示未知
} podcast_episode_t;

typedef enum {
    PODCAST_STATE_IDLE,          // 未开始(等待网络或节目单)
    PODCAST_STATE_NO_WIFI,       // 无 WiFi(配网模式或断线)
    PODCAST_STATE_FETCHING,      // 正在拉取节目单
    PODCAST_STATE_CONNECTING,    // 正在连接音频流
    PODCAST_STATE_BUFFERING,     // 正在缓冲
    PODCAST_STATE_PLAYING,       // 正在播放
    PODCAST_STATE_PAUSED,        // 已暂停(保留断点)
    PODCAST_STATE_RECONNECTING,  // 断流重连中(断点续播)
    PODCAST_STATE_FINISHED,      // 本期播完
    PODCAST_STATE_ERROR,         // 出错(看日志定位)
} podcast_state_t;

// 人类可读的状态文案(中文,已在字库覆盖范围内)。
static inline const char *podcast_state_text(podcast_state_t s) {
    switch (s) {
    case PODCAST_STATE_IDLE:         return "待机";
    case PODCAST_STATE_NO_WIFI:      return "等待网络…";
    case PODCAST_STATE_FETCHING:     return "获取节目单…";
    case PODCAST_STATE_CONNECTING:   return "连接中…";
    case PODCAST_STATE_BUFFERING:    return "缓冲中…";
    case PODCAST_STATE_PLAYING:      return "正在播放";
    case PODCAST_STATE_PAUSED:       return "已暂停";
    case PODCAST_STATE_RECONNECTING: return "断线重连…";
    case PODCAST_STATE_FINISHED:     return "播放完毕";
    case PODCAST_STATE_ERROR:        return "出错了";
    default:                         return "";
    }
}

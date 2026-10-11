// main/podcast_player.h —— 播客音频流播放器。
//
// 后台任务:HTTP 拉 MP3 流 → esp_audio_simple_dec 解码 → 单声道 → bsp_audio 写出。
// UI 只读状态(见下面的 getter),不要从 LVGL 任务调控制接口之外的函数。
// 按键回调里只调 podcast_player_*_async 系列(非阻塞);重活都在播放任务里。
#pragma once

#include "esp_err.h"
#include "podcast.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化:建互斥、读 NVS 音量、初始化音频、注册解码器、启动播放任务。
// 必须在 bsp_audio 可用之后、网络就绪之前调用一次。
esp_err_t podcast_player_init(void);

// 网络状态变化时由 WiFi 层调用。断网会停掉当前流(保留断点),恢复后自动续播。
void podcast_player_set_network(bool connected);

// 播放指定节目(拷贝一份)。打断当前流,断点清零。
void podcast_player_play_episode(const podcast_episode_t *ep);

// 播放/暂停切换。暂停保留断点(字节偏移+已播秒数),恢复时断点续播。
void podcast_player_toggle(void);

// 音量 0..100,持久化到 NVS。
void podcast_player_set_volume(uint8_t volume);
uint8_t podcast_player_get_volume(void);

// 只读状态,供 UI 轮询(LVGL 定时器里调,线程安全)。
podcast_state_t podcast_player_state(void);
uint32_t podcast_player_elapsed_sec(void);
// 当前节目拷贝(线程安全,失败返回 false)。
bool podcast_player_current_episode(podcast_episode_t *out);

#ifdef __cplusplus
}
#endif

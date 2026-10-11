// main/podcast_player.c —— 播客音频流播放器实现。
//
// 链路:esp_http_client 流式读 → esp_audio_simple_dec 解 MP3
//      → 立体声转单声道 → bsp_audio_write(ES8311)。
// 按 radio_player.cc 在同款硬件上验证过的参数起步:
//   输入缓冲 1KB、PCM 输出 8KB、播放任务栈 7168、优先级 6。
// 内存是整体账:输入/PCM/解码器/任务栈/HTTP 缓冲/LVGL 池/DMA 同时争内部 RAM,
// 改任何一个值都要看 free heap 和最大连续块,不要只看总量。
#include "podcast_player.h"

#include <string.h>

#include "bsp_audio.h"
#include "decoder/esp_audio_dec_default.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "podcast_api.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"

static const char *TAG = "podcast_player";

#define PLAYER_INPUT_BYTES  1024
#define PLAYER_PCM_BYTES    8192
#define PLAYER_TASK_STACK   7168
#define PLAYER_TASK_PRIO    6
#define HTTP_TIMEOUT_MS     7000

// 暂停后保留断点:字节偏移(供 Range 续播)+已播秒数(供进度条)。
typedef struct {
    podcast_episode_t ep;
    bool              has_episode;
    uint32_t          byte_offset;   // 流中已消费的字节数
    uint32_t          elapsed_sec;   // 已播秒数(按解出样本累计)
} resume_point_t;

static SemaphoreHandle_t s_mutex;
static TaskHandle_t      s_task;
static volatile uint32_t s_generation = 1;   // 换台/暂停/断网时 +1,让旧流退出
static volatile bool     s_network = false;
static volatile bool     s_want_play = false;
static volatile podcast_state_t s_state = PODCAST_STATE_IDLE;
static volatile uint8_t  s_volume = 60;
static resume_point_t    s_resume;           // 受 s_mutex 保护
static volatile uint32_t s_elapsed_shadow = 0; // UI 轮询用(任务内更新)

// 流缓冲:初始化时一次分配、永久复用。运行时的堆碎片曾导致每播必 malloc 失败
// (2026-10-11),改在堆最干净的启动阶段分配。
static uint8_t *s_stream_input;   // PLAYER_INPUT_BYTES
static uint8_t *s_stream_output;  // PLAYER_PCM_BYTES

// 节目单:发布时间倒序(新→旧),与 /api/episodes 一致。受 s_mutex 保护。
// 上=往新(下标-1),下=往旧(下标+1),到头回绕。
// 精简条目省内存(24×180B≈4.3KB);播放时由 video_id 现场拼 audio_url。
static podcast_playlist_item_t s_playlist[PODCAST_EPISODES_MAX];
static int                     s_playlist_count;

static void nvs_save_u8(const char *key, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open("podcast", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static uint8_t nvs_load_u8(const char *key, uint8_t fallback) {
    nvs_handle_t h;
    uint8_t v = fallback;
    if (nvs_open("podcast", NVS_READONLY, &h) != ESP_OK) return fallback;
    if (nvs_get_u8(h, key, &v) != ESP_OK) v = fallback;
    nvs_close(h);
    return v;
}

static void set_state(podcast_state_t s) {
    s_state = s;
}

static bool request_current(uint32_t gen) {
    return s_network && s_want_play && s_generation == gen;
}

// 立体声→单声道,(L+R)/2,中间用 32 位防溢出。返回单声道字节数。
static size_t downmix_to_mono(uint8_t *pcm, size_t bytes, uint8_t channels) {
    if (!pcm || channels == 0) return 0;
    if (channels == 1) return bytes & ~(size_t)1;
    int16_t *smp = (int16_t *)pcm;
    const size_t frames = bytes / (sizeof(int16_t) * channels);
    for (size_t f = 0; f < frames; f++) {
        int32_t mixed = 0;
        for (uint8_t c = 0; c < channels; c++) mixed += smp[f * channels + c];
        smp[f] = (int16_t)(mixed / channels);
    }
    return frames * sizeof(int16_t);
}

// 播一期节目。从 s_resume.byte_offset 处开始(支持断点续播)。
// 返回 true 表示至少解出过音频(流正常);false 表示连接/解码根本没起来。
static bool stream_episode(const podcast_episode_t *ep, uint32_t start_offset,
                           uint32_t start_elapsed, uint32_t gen,
                           const char *base_url) {
    char url[512];
    int n = snprintf(url, sizeof(url), "%s%s", base_url, ep->audio_url);
    if (n <= 0 || (size_t)n >= sizeof(url)) return false;

    set_state(start_offset > 0 ? PODCAST_STATE_RECONNECTING : PODCAST_STATE_CONNECTING);

    esp_http_client_config_t cfg = {0};
    cfg.url = url;
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 512;
    cfg.user_agent = "WJ-Podcast/1.0";
    // 中转在局域网,plain HTTP,不走 TLS,省内存。
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client 创建失败");
        return false;
    }
    if (start_offset > 0) {
        char range[48];
        snprintf(range, sizeof(range), "bytes=%u-", (unsigned)start_offset);
        esp_http_client_set_header(client, "Range", range);
        ESP_LOGI(TAG, "断点续播: %s", range);
    }

    bool played = false;
    esp_audio_simple_dec_handle_t decoder = NULL;
    // 用初始化时预分配的流缓冲(堆干净时分配,避开运行时碎片)。
    uint8_t *input = s_stream_input;
    uint8_t *output = s_stream_output;
    uint32_t consumed_total = start_offset;  // 本次连接内已消费字节(含起始偏移)
    uint32_t elapsed_samples = 0;
    uint32_t elapsed_base_sec = start_elapsed;  // 断点续播时,已播秒数的起点
    uint32_t sample_rate = 0;

    do {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            ESP_LOGW(TAG, "HTTP open 失败: %s", url);
            break;
        }
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        // 断点续播成功是 206;普通播放是 200。其它一律按失败处理,
        // 不要把中转的 JSON 错误页当 MP3 解(见 wyy-musicplayer 的教训)。
        if (status != 200 && status != 206) {
            ESP_LOGW(TAG, "HTTP 状态 %d", status);
            break;
        }
        if (start_offset > 0 && status != 206) {
            ESP_LOGW(TAG, "服务端不支持 Range(状态 %d),从头播", status);
            consumed_total = 0;
        }

        esp_audio_simple_dec_cfg_t dc = {
            .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
            .dec_cfg = NULL,
            .cfg_size = 0,
            .use_frame_dec = false,
        };
        if (esp_audio_simple_dec_open(&dc, &decoder) != ESP_AUDIO_ERR_OK || !decoder) {
            ESP_LOGE(TAG, "MP3 解码器打开失败");
            break;
        }
        if (!input || !output) {
            ESP_LOGE(TAG, "流缓冲不可用");
            break;
        }

        set_state(PODCAST_STATE_BUFFERING);
        bool format_ready = false;
        uint8_t src_channels = 1;
        int empty_reads = 0;

        while (request_current(gen)) {
            const int got = esp_http_client_read(client, (char *)input, PLAYER_INPUT_BYTES);
            if (got < 0) {
                ESP_LOGW(TAG, "流读取错误");
                break;
            }
            if (got == 0) {
                if (++empty_reads > 3) break;  // 流正常结束(播完)
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            empty_reads = 0;

            esp_audio_simple_dec_raw_t raw = {
                .buffer = input,
                .len = (uint32_t)got,
                .eos = false,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };
            const uint32_t chunk_start = consumed_total;
            while (raw.len > 0 && request_current(gen)) {
                esp_audio_simple_dec_out_t frame = {
                    .buffer = output, .len = PLAYER_PCM_BYTES,
                    .needed_size = 0, .decoded_size = 0,
                };
                const uint32_t before = raw.len;
                const esp_audio_err_t r = esp_audio_simple_dec_process(decoder, &raw, &frame);
                if (r != ESP_AUDIO_ERR_OK) {
                    if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                        ESP_LOGE(TAG, "解码器要 %u 字节,输出缓冲只有 %u",
                                 (unsigned)frame.needed_size, PLAYER_PCM_BYTES);
                    } else {
                        ESP_LOGW(TAG, "MP3 解码失败: %d", r);
                    }
                    raw.len = 0;
                    break;
                }
                if (raw.consumed > raw.len) raw.consumed = raw.len;
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;

                if (frame.decoded_size > 0) {
                    if (!format_ready) {
                        esp_audio_simple_dec_info_t info = {0};
                        if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK ||
                            info.bits_per_sample != 16 ||
                            info.sample_rate < 8000 || info.sample_rate > 48000 ||
                            bsp_audio_set_format(info.sample_rate, 16, 1) != ESP_OK) {
                            ESP_LOGE(TAG, "不支持的音频格式");
                            raw.len = 0;
                            break;
                        }
                        src_channels = (info.channel >= 1 && info.channel <= 2)
                                       ? info.channel : 1;
                        sample_rate = info.sample_rate;
                        bsp_audio_set_volume(s_volume);
                        format_ready = true;
                        ESP_LOGI(TAG, "播放: %luHz %uch -> 单声道",
                                 (unsigned long)info.sample_rate, src_channels);
                    }
                    if (format_ready) {
                        const size_t mono = downmix_to_mono(frame.buffer,
                                                            frame.decoded_size,
                                                            src_channels);
                        if (mono > 0) {
                            if (bsp_audio_write(frame.buffer, mono) != ESP_OK) continue;
                            elapsed_samples += (uint32_t)(mono / sizeof(int16_t));
                            s_elapsed_shadow = elapsed_base_sec +
                                               elapsed_samples / sample_rate;
                            if (!played) {
                                played = true;
                                set_state(PODCAST_STATE_PLAYING);
                            }
                        }
                    }
                }
                if (raw.len == before || raw.consumed == 0) break;  // 等下一块网络数据
            }
            consumed_total = chunk_start + ((uint32_t)got - raw.len);

            // 同步断点(约 1 秒一次,够用了):锁内只做赋值,不做重活。
            // 必须检查 generation:切台后旧流的 consumed_total 不能覆盖新节目的断点。
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (s_generation == gen) {
                    s_resume.byte_offset = consumed_total;
                    s_resume.elapsed_sec = elapsed_base_sec +
                        (sample_rate ? elapsed_samples / sample_rate : 0);
                }
                xSemaphoreGive(s_mutex);
            }
        }
    } while (0);

    if (decoder) esp_audio_simple_dec_close(decoder);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return played;
}

static void player_task(void *arg) {
    (void)arg;
    char base[96] = {0};
    uint8_t fail_count = 0;

    for (;;) {
        if (!s_network) {
            set_state(PODCAST_STATE_NO_WIFI);
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        if (!s_want_play) {
            // 暂停:不断流重连,只等用户恢复(断点已在 s_resume 里)。
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        podcast_episode_t ep;
        uint32_t start_offset, start_elapsed;
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) continue;
        if (!s_resume.has_episode) {
            xSemaphoreGive(s_mutex);
            set_state(PODCAST_STATE_IDLE);
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        ep = s_resume.ep;
        start_offset = s_resume.byte_offset;
        start_elapsed = s_resume.elapsed_sec;
        xSemaphoreGive(s_mutex);

        const uint32_t gen = s_generation;
        if (base[0] == '\0') {
            set_state(PODCAST_STATE_FETCHING);
            if (podcast_api_resolve_base(base, sizeof(base)) != ESP_OK) {
                ESP_LOGW(TAG, "中转地址解析失败,5s 后重试");
                set_state(PODCAST_STATE_ERROR);
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }

        s_elapsed_shadow = start_elapsed;
        const bool played = stream_episode(&ep, start_offset, start_elapsed,
                                           gen, base);

        // 流返回了:先看是不是用户/网络主动打断的。
        if (s_generation != gen || !s_network || !s_want_play) continue;

        if (played) {
            // 播过声音后流结束:大概率是本期播完(也可能是掉线,区分不了就按播完处理,
            // 用户长按 OK 可重播)。不断自动重连,避免把"播完"当"掉线"死循环。
            ESP_LOGI(TAG, "本期播放结束");
            set_state(PODCAST_STATE_FINISHED);
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                s_resume.byte_offset = 0;
                s_resume.elapsed_sec = 0;
                xSemaphoreGive(s_mutex);
            }
            s_want_play = false;
            fail_count = 0;
            continue;
        }

        // 连上但一秒都没响:重试几次,一直不行就报错停下(不要死循环烧流量)。
        if (++fail_count >= 3) {
            ESP_LOGE(TAG, "连续 %d 次无法播放,停下", fail_count);
            set_state(PODCAST_STATE_ERROR);
            s_want_play = false;
            fail_count = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        set_state(PODCAST_STATE_RECONNECTING);
        ESP_LOGW(TAG, "播放失败,第 %d 次重试…", fail_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t podcast_player_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    memset(&s_resume, 0, sizeof(s_resume));

    s_volume = nvs_load_u8("volume", 60);
    if (s_volume > 100) s_volume = 60;

    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init 失败");
        return ESP_FAIL;
    }
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
        esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "音频解码器注册失败");
        return ESP_FAIL;
    }
    bsp_audio_set_volume(s_volume);

    // 流缓冲尽早分配:此时堆最干净。失败也不致命,播放时会再试(并打日志)。
    s_stream_input = malloc(PLAYER_INPUT_BYTES);
    s_stream_output = malloc(PLAYER_PCM_BYTES);
    if (!s_stream_input || !s_stream_output) {
        ESP_LOGW(TAG, "流缓冲预分配失败(%u+%u 字节),播放时再试",
                 PLAYER_INPUT_BYTES, PLAYER_PCM_BYTES);
    }

    if (xTaskCreate(player_task, "podcast_stream", PLAYER_TASK_STACK,
                    NULL, PLAYER_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "播放任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "播放器就绪,音量 %d", s_volume);
    return ESP_OK;
}

void podcast_player_set_network(bool connected) {
    const bool was = s_network;
    s_network = connected;
    s_generation++;  // 让旧流立刻退出,不断网残留
    if (!connected && was) {
        ESP_LOGI(TAG, "网络断开,保留断点");
    }
}

void podcast_player_play_episode(const podcast_episode_t *ep) {
    if (!ep) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
    s_resume.ep = *ep;
    s_resume.has_episode = true;
    s_resume.byte_offset = 0;
    s_resume.elapsed_sec = 0;
    // generation 在锁内递增:避免与播放任务的断点同步竞态(旧偏移覆盖新节目的 0)。
    s_generation++;
    xSemaphoreGive(s_mutex);
    s_elapsed_shadow = 0;
    s_want_play = true;
    ESP_LOGI(TAG, "开始播放: %s", ep->title);
}

void podcast_player_toggle(void) {
    s_want_play = !s_want_play;
    s_generation++;
    set_state(s_want_play ? PODCAST_STATE_CONNECTING : PODCAST_STATE_PAUSED);
    ESP_LOGI(TAG, "%s", s_want_play ? "继续播放" : "暂停");
}

void podcast_player_set_volume(uint8_t volume) {
    if (volume > 100) volume = 100;
    s_volume = volume;
    bsp_audio_set_volume(volume);
    nvs_save_u8("volume", volume);
}

uint8_t podcast_player_get_volume(void) {
    return s_volume;
}

podcast_state_t podcast_player_state(void) {
    return s_state;
}

uint32_t podcast_player_elapsed_sec(void) {
    return s_elapsed_shadow;
}

bool podcast_player_current_episode(podcast_episode_t *out) {
    if (!out) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    const bool ok = s_resume.has_episode;
    if (ok) *out = s_resume.ep;
    xSemaphoreGive(s_mutex);
    return ok;
}

// 在节目单里找 video_id 的下标,找不到返回 -1。调用方需已持有 s_mutex。
static int playlist_index_of_locked(const char *video_id) {
    if (!video_id || !video_id[0]) return -1;
    for (int i = 0; i < s_playlist_count; i++) {
        if (strcmp(s_playlist[i].video_id, video_id) == 0) return i;
    }
    return -1;
}

// 切台核心:下标 step(+1 往旧,-1 往新),到头回绕。调用方需已持有 s_mutex。
// 从精简条目还原完整 episode(audio_url 按中转约定现场拼)。
static bool switch_locked(int step) {
    if (s_playlist_count == 0) return false;
    int idx = playlist_index_of_locked(s_resume.has_episode ? s_resume.ep.video_id : NULL);
    if (idx < 0) idx = (step > 0) ? -1 : 0;  // 当前不在单里:下=从最新开始,上=从最旧开始
    idx = (idx + step + s_playlist_count) % s_playlist_count;
    const podcast_playlist_item_t *it = &s_playlist[idx];
    podcast_episode_t ep;
    memset(&ep, 0, sizeof(ep));
    strncpy(ep.video_id, it->video_id, sizeof(ep.video_id) - 1);
    strncpy(ep.title, it->title, sizeof(ep.title) - 1);
    strncpy(ep.date, it->date, sizeof(ep.date) - 1);
    snprintf(ep.audio_url, sizeof(ep.audio_url), "/audio/%s.mp3", it->video_id);
    ep.duration_sec = it->duration_sec;
    s_resume.ep = ep;
    s_resume.has_episode = true;
    s_resume.byte_offset = 0;
    s_resume.elapsed_sec = 0;
    return true;
}

void podcast_player_set_playlist(const podcast_playlist_item_t *eps, int count) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
    if (!eps || count <= 0) {
        s_playlist_count = 0;
    } else {
        if (count > PODCAST_EPISODES_MAX) count = PODCAST_EPISODES_MAX;
        memcpy(s_playlist, eps, (size_t)count * sizeof(s_playlist[0]));
        s_playlist_count = count;
    }
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "节目单更新: %d 期", s_playlist_count);
}

int podcast_player_playlist_count(void) {
    return s_playlist_count;  // int 读写原子,无需加锁
}

int podcast_player_playlist_index(void) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return -1;
    const int idx = playlist_index_of_locked(s_resume.has_episode ? s_resume.ep.video_id : NULL);
    xSemaphoreGive(s_mutex);
    return idx;
}

bool podcast_player_play_next(void) {
    char title[PODCAST_TITLE_MAX];
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    const bool ok = switch_locked(+1);
    title[0] = '\0';
    if (ok) {
        strncpy(title, s_resume.ep.title, sizeof(title) - 1);
        // 锁内递增 generation,防播放任务的断点同步把旧偏移写回来。
        s_generation++;
    }
    xSemaphoreGive(s_mutex);
    if (!ok) return false;
    s_elapsed_shadow = 0;
    s_want_play = true;
    ESP_LOGI(TAG, "下一期: %s", title);
    return true;
}

bool podcast_player_play_prev(void) {
    char title[PODCAST_TITLE_MAX];
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    const bool ok = switch_locked(-1);
    title[0] = '\0';
    if (ok) {
        strncpy(title, s_resume.ep.title, sizeof(title) - 1);
        // 锁内递增 generation,防播放任务的断点同步把旧偏移写回来。
        s_generation++;
    }
    xSemaphoreGive(s_mutex);
    if (!ok) return false;
    s_elapsed_shadow = 0;
    s_want_play = true;
    ESP_LOGI(TAG, "上一期: %s", title);
    return true;
}

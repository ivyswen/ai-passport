// main/main.c —— 王剑每日观察播客机。
//
// 一台只干一件事的设备:从局域网中转服务拉取「王剑每日观察」最新一期的
// 音频(MP3 流),解码播放。UI 是播客机自己的设计(非 demo 菜单)。
//
// 按键语义:
//   OK 短按    播放/暂停
//   上/下短按  切节目(上=更新的一期,下=更旧的一期,到头回绕)
//   上/下长按  音量 +/-5
//   OK 长按    检查更新(有新节目自动切换)
#include <string.h>
#include <time.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mdns.h"

#include "key_guard.h"
#include "podcast.h"
#include "podcast_api.h"
#include "podcast_player.h"
#include "podcast_ui.h"
#include "podcast_wifi.h"

static const char *TAG = "podcast_main";

typedef enum {
    EVT_WIFI_CONNECTED,
    EVT_WIFI_DISCONNECTED,
    EVT_WIFI_CONFIG_AP,
    EVT_KEY,
} app_evt_kind_t;

typedef struct {
    app_evt_kind_t kind;
    bsp_btn_t      btn;
    bsp_btn_ev_t   ev;
    char           ap_ssid[40];
} app_evt_t;

static QueueHandle_t s_evt_queue;   // 按键 + WiFi 事件 → 主循环
static QueueHandle_t s_net_queue;   // 抓取指令 → net_task(独立队列,不竞态)
static volatile bool s_net_up;
static volatile bool s_sntp_started;
static volatile bool s_input_ready; // 按键分发就绪(队列+按钮初始化完成后置 true)

// 节目单缓冲:24 期 × 约 440 字节 ≈ 10KB,net_task 栈只有 6KB,放文件作用域。
static podcast_episode_t s_episode_list[PODCAST_EPISODES_MAX];

// 深睡按键唤醒保护:唤醒那颗键在整个启动期间还按着,先挡住它的动作,
// 直到松手(或 10 秒超时),否则一开机就会误触发一次播放/暂停。
// 状态机在 key_guard.h,契约由 tests/test_key_wake_contract.py 锁定。
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

typedef enum {
    NET_FETCH,   // 去拉最新节目(有新就切)
} net_cmd_t;

// 后台网络任务:等网络 → 同步时间 → 拉最新节目 → 播放；
// 收到 NET_FETCH 时重新拉一次,有新节目就切过去。
static void net_task(void *arg) {
    (void)arg;
    char base[96] = {0};

    for (;;) {
        net_cmd_t cmd;
        if (xQueueReceive(s_net_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        if (cmd != NET_FETCH || !s_net_up) continue;

        if (!s_sntp_started) {
            s_sntp_started = true;
            ESP_LOGI(TAG, "启动 SNTP 对时");
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "ntp.ntsc.ac.cn");
            esp_sntp_setservername(1, "pool.ntp.org");
            esp_sntp_init();
        }

        if (base[0] == '\0' &&
            podcast_api_resolve_base(base, sizeof(base)) != ESP_OK) {
            ESP_LOGW(TAG, "中转地址解析失败,稍后重试");
            continue;
        }

        podcast_episode_t latest;
        if (podcast_api_fetch_latest(base, &latest) != ESP_OK) {
            ESP_LOGW(TAG, "拉取节目单失败");
            continue;
        }

        // 刷新节目单(供上下键切台);失败则保留旧单,不影响播放。
        const int n_eps = podcast_api_fetch_episodes(base, s_episode_list,
                                                     PODCAST_EPISODES_MAX);
        if (n_eps >= 0) {
            podcast_player_set_playlist(s_episode_list, n_eps);
        }

        podcast_episode_t cur;
        const bool has_cur = podcast_player_current_episode(&cur);
        if (!has_cur || strcmp(cur.video_id, latest.video_id) != 0) {
            ESP_LOGI(TAG, "新节目:%s", latest.title);
            podcast_player_play_episode(&latest);
        } else {
            ESP_LOGI(TAG, "已是最新一期");
            // 如果当前是暂停/播完状态,检查更新顺手恢复播放
            const podcast_state_t st = podcast_player_state();
            if (st == PODCAST_STATE_PAUSED ||
                st == PODCAST_STATE_FINISHED ||
                st == PODCAST_STATE_IDLE) {
                podcast_player_play_episode(&latest);
            }
        }
    }
}

// WiFi 事件回调(跑在 WiFi 事件任务,只发队列)。
static void on_wifi_event(bool connected, bool config_ap, const char *ap_ssid,
                          void *user) {
    (void)user;
    app_evt_t evt = {0};
    if (config_ap) {
        evt.kind = EVT_WIFI_CONFIG_AP;
        if (ap_ssid) strncpy(evt.ap_ssid, ap_ssid, sizeof(evt.ap_ssid) - 1);
    } else if (connected) {
        evt.kind = EVT_WIFI_CONNECTED;
    } else {
        evt.kind = EVT_WIFI_DISCONNECTED;
    }
    xQueueSend(s_evt_queue, &evt, 0);
}

// 按键回调(跑在 esp_timer 任务,只入队)。注意顺序:先消费唤醒保护,
// 再做就绪检查 —— 松手事件可能在分发就绪前到达,直接丢弃会让保护
// 一直卡着吞掉下一次真正的按键。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready) return;
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) return;
    app_evt_t evt = { .kind = EVT_KEY, .btn = btn, .ev = ev };
    xQueueSend(s_evt_queue, &evt, 0);
}

static void handle_key(const app_evt_t *evt) {
    const net_cmd_t fetch = NET_FETCH;
    if (evt->ev == BSP_BTN_CLICK) {
        if (evt->btn == BSP_BTN_OK) {
            podcast_episode_t cur;
            if (!podcast_player_current_episode(&cur) && s_net_up) {
                // 还没节目:手动触发一次拉取
                xQueueSend(s_net_queue, &fetch, 0);
            } else {
                podcast_player_toggle();
            }
        } else if (evt->btn == BSP_BTN_UP) {
            if (!podcast_player_play_prev()) {
                ESP_LOGI(TAG, "无节目单,无法切换");
            }
        } else if (evt->btn == BSP_BTN_DOWN) {
            if (!podcast_player_play_next()) {
                ESP_LOGI(TAG, "无节目单,无法切换");
            }
        }
    } else if (evt->ev == BSP_BTN_LONG) {
        if (evt->btn == BSP_BTN_OK) {
            ESP_LOGI(TAG, "手动检查更新");
            xQueueSend(s_net_queue, &fetch, 0);
        } else if (evt->btn == BSP_BTN_UP) {
            const uint8_t v = podcast_player_get_volume() + 5;
            podcast_player_set_volume(v);
            podcast_ui_show_volume(podcast_player_get_volume());
        } else if (evt->btn == BSP_BTN_DOWN) {
            const uint8_t v = podcast_player_get_volume();
            const uint8_t nv = v >= 5 ? v - 5 : 0;
            podcast_player_set_volume(nv);
            podcast_ui_show_volume(nv);
        }
    }
}

// LVGL 定时器(跑在 LVGL 任务):刷新界面。
static void ui_timer_cb(lv_timer_t *t) {
    (void)t;
    podcast_ui_tick();
}

void app_main(void) {
    ESP_LOGI(TAG, "王剑每日观察播客机启动");

    // 按键唤醒时,那颗键在整个启动期间还按着。先武装保护,挡住它的动作(见 on_key)。
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败");
        return;
    }
    bsp_display_backlight(100);

    if (bsp_audio_init() != ESP_OK) ESP_LOGW(TAG, "音频初始化失败,继续(稍后重试)");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计初始化失败");

    // 中国时区,给顶栏时钟用。
    setenv("TZ", "CST-8", 1);
    tzset();
    mdns_init();

    s_evt_queue = xQueueCreate(16, sizeof(app_evt_t));
    s_net_queue = xQueueCreate(4, sizeof(net_cmd_t));
    if (!s_evt_queue || !s_net_queue) {
        ESP_LOGE(TAG, "事件队列创建失败");
        return;
    }

    if (bsp_lvgl_lock(1000)) {
        podcast_ui_init();
        lv_timer_create(ui_timer_cb, 500, NULL);
        bsp_lvgl_unlock();
    }

    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
        return;
    }
    s_input_ready = true;  // 此后 on_key 才开始向主循环投递按键事件

    if (podcast_player_init() != ESP_OK) {
        ESP_LOGE(TAG, "播放器初始化失败");
        return;
    }

    podcast_wifi_init(on_wifi_event, NULL);
    podcast_wifi_start();

    xTaskCreate(net_task, "podcast_net", 6144, NULL, 4, NULL);

    // 主循环:处理按键与 WiFi 事件(网络抓取在 net_task)。
    app_evt_t evt;
    for (;;) {
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) != pdTRUE) continue;
        switch (evt.kind) {
        case EVT_KEY:
            handle_key(&evt);
            break;
        case EVT_WIFI_CONNECTED:
            s_net_up = true;
            podcast_player_set_network(true);
            if (bsp_lvgl_lock(500)) {
                podcast_ui_show_main();
                bsp_lvgl_unlock();
            }
            {  // 唤醒 net_task 去抓取
                const net_cmd_t fetch = NET_FETCH;
                xQueueSend(s_net_queue, &fetch, 0);
            }
            break;
        case EVT_WIFI_DISCONNECTED:
            s_net_up = false;
            podcast_player_set_network(false);
            break;
        case EVT_WIFI_CONFIG_AP:
            s_net_up = false;
            podcast_player_set_network(false);
            if (bsp_lvgl_lock(500)) {
                podcast_ui_show_config_ap(evt.ap_ssid);
                bsp_lvgl_unlock();
            }
            break;
        }
    }
}

// main/podcast_wifi.cpp —— 78/esp-wifi-connect 的 C 胶水层。
#include "podcast_wifi.h"

#include <string>

#include "esp_log.h"
#include "ssid_manager.h"
#include "wifi_manager.h"

static const char *TAG = "podcast_wifi";

static podcast_wifi_event_cb_t s_cb;
static void *s_user;
static bool s_initialized;

void podcast_wifi_init(podcast_wifi_event_cb_t cb, void *user) {
    s_cb = cb;
    s_user = user;
    if (s_initialized) return;

    WifiManagerConfig config;
    config.ssid_prefix = "WJ-Podcast";
    config.fixed_ap_ssid = "WJ-Podcast";
    config.language = "zh-CN";
    config.station_hostname = "wj-podcast";
    config.station_scan_min_interval_seconds = 10;
    config.station_scan_max_interval_seconds = 60;

    auto &manager = WifiManager::GetInstance();
    if (!manager.Initialize(config)) {
        ESP_LOGE(TAG, "WiFi 组件初始化失败");
        return;
    }
    manager.SetEventCallback([](WifiEvent event, const std::string &data) {
        (void)data;
        auto &manager = WifiManager::GetInstance();
        if (!s_cb) return;
        switch (event) {
        case WifiEvent::Connected:
            ESP_LOGI(TAG, "WiFi 已连接: %s", manager.GetSsid().c_str());
            s_cb(true, false, nullptr, s_user);
            break;
        case WifiEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi 断开");
            s_cb(false, manager.IsConfigMode(), nullptr, s_user);
            break;
        case WifiEvent::ConfigModeEnter:
            ESP_LOGI(TAG, "进入配网模式: %s", manager.GetApSsid().c_str());
            s_cb(false, true, manager.GetApSsid().c_str(), s_user);
            break;
        case WifiEvent::ConfigModeExit:
            ESP_LOGI(TAG, "退出配网模式");
            s_cb(manager.IsConnected(), false, nullptr, s_user);
            if (!manager.IsConnected()) manager.StartStation();
            break;
        default:
            break;
        }
    });
    s_initialized = true;
}

void podcast_wifi_start(void) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "先调 podcast_wifi_init");
        return;
    }
    auto &manager = WifiManager::GetInstance();
    const auto &saved = SsidManager::GetInstance().GetSsidList();
    if (saved.empty()) {
        ESP_LOGI(TAG, "无保存的网络,进入配网模式");
        manager.StartConfigAp();
    } else {
        ESP_LOGI(TAG, "有 %u 个保存的网络,开始连接",
                 (unsigned)saved.size());
        manager.StartStation();
    }
}

bool podcast_wifi_connected(void) {
    if (!s_initialized) return false;
    return WifiManager::GetInstance().IsConnected();
}

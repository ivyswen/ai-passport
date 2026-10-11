// main/podcast_wifi.h —— WiFi 连接/配网胶水层(C 接口)。
//
// 底层用 78/esp-wifi-connect 组件(与 LEO RADIO 同款):
//   - 有保存过的网络:STA 直连,断线自动重连;
//   - 无保存网络:自动开 AP 配网热点,手机连上后浏览器打开 192.168.4.1 配网。
// 事件通过回调抛给应用层(回调里只置标志/发队列,不要做重活)。
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*podcast_wifi_event_cb_t)(bool connected, bool config_ap,
                                        const char *ap_ssid, void *user);

// 初始化组件并注册回调(幂等的可重复调用会更新回调)。
void podcast_wifi_init(podcast_wifi_event_cb_t cb, void *user);

// 开始连接:有保存网络则 STA,没有则进配网 AP 模式。非阻塞。
void podcast_wifi_start(void);

// 当前是否已连上 WiFi(含 IP)。
bool podcast_wifi_connected(void);

#ifdef __cplusplus
}
#endif

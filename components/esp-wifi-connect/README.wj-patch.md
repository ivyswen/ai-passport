# 本地 vendor 的 esp-wifi-connect（含 wj-podcast 补丁）

来源：`78/esp-wifi-connect` v3.3.1（上游 https://github.com/78/esp-wifi-connect）。

## 为什么 vendor

`managed_components/` 是 gitignored 的，直接改那里推不上去。
本地 `components/` 覆盖托管依赖，保证 fresh clone 也能编出带补丁的固件。

## 本地补丁

1. `wifi_configuration_ap.cc` — `WifiEventHandler` 的 `WIFI_EVENT_SCAN_DONE`
   分支：`ap_records_.resize(ap_num)` 前把 `ap_num` 上限卡到 32。
   原因（2026-10-11）：AP 数量多时 vector 需要连续大内存，堆分配失败抛
   `std::bad_alloc`，而固件未启用 C++ 异常 → 直接 `abort()` 崩溃重启。
   配网页面列 32 个 SSID 足够用。

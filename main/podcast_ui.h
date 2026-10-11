// main/podcast_ui.h —— 播客机 LVGL 界面。
//
// 视觉语言参考 LEO RADIO(深海军蓝底 + 橙/青点缀 + 中文大标题 + 底部按键提示),
// 版式是播客机自己的设计,不是 demo 菜单的改色。
// 所有函数都必须在 LVGL 任务内或持有 bsp_lvgl_lock() 时调用。
#pragma once

// 初始化主界面(标题/进度条/状态/提示)。LVGL 初始化完成后调一次。
void podcast_ui_init(void);

// 500ms 定时器回调里调:从播放器读状态刷新界面(内部做变化检测,不闪)。
void podcast_ui_tick(void);

// 配网模式界面:显示热点名,提示手机连 192.168.4.1。
void podcast_ui_show_config_ap(const char *ap_ssid);

// 回到主界面(配网完成后)。
void podcast_ui_show_main(void);

// main/podcast_ui.c —— 播客机 LVGL 界面实现。
#include "podcast_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp_battery.h"
#include "lvgl.h"
#include "podcast_player.h"

// 字库由 lv_font_conv 从 Noto Sans SC 按播客词表生成(见 tools/ 里的生成脚本)。
// 16px 正文/提示,20px 节目标题。英文/数字回退到 Montserrat(在 init 里接 fallback)。
LV_FONT_DECLARE(podcast_font_16);
LV_FONT_DECLARE(podcast_font_20);

static lv_font_t s_font_16, s_font_20;

#define C_BG      lv_color_hex(0x0A1030)
#define C_ORANGE  lv_color_hex(0xFFB84D)
#define C_CYAN    lv_color_hex(0x4DD8FF)
#define C_TEXT    lv_color_hex(0xE8ECFF)
#define C_DIM     lv_color_hex(0x8A93C8)

static lv_obj_t *s_screen;
static lv_obj_t *s_time_label, *s_batt_label;
static lv_obj_t *s_date_label, *s_title_label;
static lv_obj_t *s_bar, *s_elapsed_label, *s_total_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_config_screen, *s_config_label;

// 变化检测缓存,避免每 500ms 重设相同文本造成闪烁。
static char     s_last_title[PODCAST_TITLE_MAX];
static char     s_last_status[32];
static int      s_last_elapsed = -1;
static uint32_t s_last_total = 0xFFFFFFFF;
static int      s_last_min = -1;
static int      s_last_batt = -2;

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            lv_color_t color, int x, int y, int w) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static void fmt_time(char *out, size_t n, uint32_t sec) {
    snprintf(out, n, "%02u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
}

void podcast_ui_init(void) {
    // fallback 链:中文字库 -> Montserrat(英文数字符号),缺字显示占位框。
    s_font_16 = podcast_font_16;
    s_font_16.fallback = &lv_font_montserrat_16;
    s_font_20 = podcast_font_20;
    s_font_20.fallback = &lv_font_montserrat_20;

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, C_BG, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    // 顶栏:左 "王剑每日观察",右 时间 + 电量。
    lv_obj_t *brand = make_label(s_screen, &s_font_16, C_ORANGE, 12, 8, 150);
    lv_label_set_text(brand, "王剑每日观察");
    s_time_label = make_label(s_screen, &s_font_16, C_TEXT, 150, 8, 60);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_RIGHT, 0);
    s_batt_label = make_label(s_screen, &s_font_16, C_CYAN, 196, 8, 44);
    lv_obj_set_style_text_align(s_batt_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *line = lv_obj_create(s_screen);
    lv_obj_set_size(line, 216, 2);
    lv_obj_set_pos(line, 12, 38);
    lv_obj_set_style_bg_color(line, C_DIM, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);

    // 日期 + 节目标题(超长自动滚动)。
    s_date_label = make_label(s_screen, &s_font_16, C_CYAN, 12, 48, 216);
    s_title_label = make_label(s_screen, &s_font_20, C_TEXT, 12, 76, 216);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);

    // 进度条 + 时间。
    s_bar = lv_bar_create(s_screen);
    lv_obj_set_size(s_bar, 216, 10);
    lv_obj_set_pos(s_bar, 12, 190);
    lv_obj_set_style_bg_color(s_bar, C_DIM, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, C_ORANGE, LV_PART_INDICATOR);
    lv_bar_set_range(s_bar, 0, 1000);
    s_elapsed_label = make_label(s_screen, &s_font_16, C_TEXT, 12, 206, 100);
    s_total_label = make_label(s_screen, &s_font_16, C_DIM, 128, 206, 100);
    lv_obj_set_style_text_align(s_total_label, LV_TEXT_ALIGN_RIGHT, 0);

    // 状态行。
    s_status_label = make_label(s_screen, &s_font_16, C_ORANGE, 12, 244, 216);

    // 底部按键提示。
    lv_obj_t *hint1 = make_label(s_screen, &s_font_16, C_DIM, 12, 272, 216);
    lv_label_set_text(hint1, "OK 播放/暂停·上下 音量");
    lv_obj_t *hint2 = make_label(s_screen, &s_font_16, C_DIM, 12, 294, 216);
    lv_label_set_text(hint2, "长按OK 检查更新");

    lv_scr_load(s_screen);
}

void podcast_ui_tick(void) {
    // 时间(顶栏):SNTP 没同步就显示 --:--。
    time_t now = time(NULL);
    struct tm tm_now;
    char tbuf[8];
    if (now > 1000000000 && localtime_r(&now, &tm_now)) {
        const int mins = tm_now.tm_hour * 60 + tm_now.tm_min;
        if (mins != s_last_min) {
            s_last_min = mins;
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
            lv_label_set_text(s_time_label, tbuf);
        }
    } else if (s_last_min != -2) {
        s_last_min = -2;
        lv_label_set_text(s_time_label, "--:--");
    }

    // 电量。
    const int soc = bsp_battery_soc();
    if (soc != s_last_batt) {
        s_last_batt = soc;
        char bbuf[16];
        if (soc >= 0) snprintf(bbuf, sizeof(bbuf), "%d%%", soc);
        else snprintf(bbuf, sizeof(bbuf), "--");
        lv_label_set_text(s_batt_label, bbuf);
    }

    // 节目信息。
    podcast_episode_t ep;
    const bool has_ep = podcast_player_current_episode(&ep);
    if (has_ep && strcmp(ep.title, s_last_title) != 0) {
        strncpy(s_last_title, ep.title, sizeof(s_last_title) - 1);
        lv_label_set_text(s_title_label, ep.title[0] ? ep.title : "…");
        lv_label_set_text(s_date_label, ep.date);
        s_last_total = 0xFFFFFFFF;  // 强制刷新总时长
    } else if (!has_ep && s_last_title[0] != '\0') {
        s_last_title[0] = '\0';
        lv_label_set_text(s_title_label, "等待节目…");
        lv_label_set_text(s_date_label, "");
    }

    // 进度。
    const uint32_t elapsed = podcast_player_elapsed_sec();
    const uint32_t total = has_ep ? ep.duration_sec : 0;
    if ((int)elapsed != s_last_elapsed) {
        s_last_elapsed = (int)elapsed;
        char ebuf[8];
        fmt_time(ebuf, sizeof(ebuf), elapsed);
        lv_label_set_text(s_elapsed_label, ebuf);
        if (total > 0) lv_bar_set_value(s_bar, (int)(elapsed * 1000 / total), LV_ANIM_OFF);
    }
    if (total != s_last_total) {
        s_last_total = total;
        char xbuf[12];
        if (total > 0) {
            fmt_time(xbuf, sizeof(xbuf), total);
            char full[16];
            snprintf(full, sizeof(full), "/ %s", xbuf);
            lv_label_set_text(s_total_label, full);
        } else {
            lv_label_set_text(s_total_label, "");
        }
        if (total == 0) lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    }

    // 状态。
    const podcast_state_t st = podcast_player_state();
    const char *txt = podcast_state_text(st);
    if (strcmp(txt, s_last_status) != 0) {
        strncpy(s_last_status, txt, sizeof(s_last_status) - 1);
        char sbuf[40];
        const char *icon = (st == PODCAST_STATE_PLAYING) ? "▶ " :
                           (st == PODCAST_STATE_PAUSED) ? "⏸ " : "… ";
        snprintf(sbuf, sizeof(sbuf), "%s%s", icon, txt);
        lv_label_set_text(s_status_label, sbuf);
    }
}

void podcast_ui_show_config_ap(const char *ap_ssid) {
    if (s_config_screen) {
        lv_scr_load(s_config_screen);
        return;
    }
    s_config_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_config_screen, C_BG, 0);

    lv_obj_t *t = make_label(s_config_screen, &s_font_20, C_ORANGE, 12, 40, 216);
    lv_label_set_text(t, "配网模式");
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);

    s_config_label = make_label(s_config_screen, &s_font_16, C_TEXT, 12, 100, 216);
    lv_obj_set_style_text_align(s_config_label, LV_TEXT_ALIGN_CENTER, 0);
    char buf[160];
    snprintf(buf, sizeof(buf),
             "手机连接热点\n%s\n\n浏览器打开\n192.168.4.1\n\n配置家中 WiFi",
             ap_ssid ? ap_ssid : "");
    lv_label_set_text(s_config_label, buf);

    lv_scr_load(s_config_screen);
}

void podcast_ui_show_main(void) {
    if (s_screen) lv_scr_load(s_screen);
    // 强制下一次 tick 全量刷新。
    s_last_title[0] = '\0';
    s_last_status[0] = '\0';
    s_last_elapsed = -1;
    s_last_total = 0xFFFFFFFF;
    s_last_min = -1;
    s_last_batt = -2;
}

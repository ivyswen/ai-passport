# main/CMakeLists.txt —— 王剑每日观察播客机(替代 demo 菜单的应用入口)。
#
# 注意:demo_*.c / ui_pixel*.c / demo_navigation.c 是上游硬件测试基线的
# 文件,本应用不需要,不编译。BSP(components/bsp)原样复用。
idf_component_register(
    SRCS "main.c"
         "podcast_api.c"
         "podcast_player.c"
         "podcast_ui.c"
         "podcast_wifi.cpp"
         # 中文字库(tools/gen_podcast_font.py 生成,见 README):
         "podcast_font_16.c"
         "podcast_font_20.c"
    INCLUDE_DIRS "."
    REQUIRES bsp
             esp_event
             esp_http_client
             esp-wifi-connect
             esp_netif
             esp_timer
             esp_wifi
             json
             mdns
             nvs_flash
)

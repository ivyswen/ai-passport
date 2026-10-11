<p align="right">
  <strong>简体中文</strong> · <a href="podcast-player.md">English</a>
</p>

# 王剑每日观察播客机（分支 `feature/wj-podcast-player`）

把 FoloToy AI Passport 变成一台只干一件事的播客机：从局域网中转服务
拉取 YouTube 频道「王剑每日观察」最新一期的音频（MP3 流），解码播放。
只听声音，不要画面。

## 系统组成

```
YouTube「王剑每日观察」
   ↓  RSS/yt-dlp 轮询 + 抽音频转 MP3（30 分钟一轮）
OEC-T 盒子：wj-relay（Docker，https://github.com/ivyswen/wj-relay）
   ↓  局域网 HTTP：GET /api/latest → JSON，/audio/<id>.mp3 → 音频流
   ↓  mDNS 广播 wj-relay.local，设备免配置发现
AI Passport（本分支固件）：WiFi → 拉流 → MP3 解码 → ES8311 播放
```

## 按键

| 操作 | 动作 |
|---|---|
| OK 短按 | 播放 / 暂停 |
| 上 / 下短按 | 音量 +/-5（NVS 记忆） |
| OK 长按 | 检查更新（有新节目自动切换播放） |

断网/断流自动保留断点，恢复后断点续播（HTTP Range）。

## 首次使用

1. 中转服务先在 OEC-T 上跑起来（见 wj-relay 仓库 README）。
2. 给设备刷本分支固件（见下）。
3. 设备首次开机无保存的 WiFi 时会自动开配网热点 `WJ-Podcast-XXXX`
  （XXXX 为 MAC 后缀，屏幕上会显示完整名称），
   手机连上后浏览器打开 `192.168.4.1`，在网页填家中 WiFi。
4. 连网后自动对时（SNTP）、拉最新一期、开始播放。

## 从源码构建

需要 ESP-IDF v5.5.3（`./install.sh esp32c3`，本分支关掉了蓝牙，
`IDF_SKIP_CHECK_SUBMODULES=1` 可跳过 BT 大库子模块）。

```bash
# 1. 生成中文字库（Noto Sans SC，OFL 授权，需自行下载 TTF）
npm i -g lv_font_conv
python3 tools/gen_podcast_font.py --font /path/to/NotoSansSC-VF.ttf --compress
# 生成 main/podcast_font_16.c / main/podcast_font_20.c（git 不跟踪，构建机本地生成）

# 2. 构建
. $IDF_PATH/export.sh
idf.py set-target esp32c3
idf.py build

# 3. 合并刷机包（从 0x0 烧录）
idf.py merge-bin -o build/wj-podcast-full.bin
python -m esptool --chip esp32c3 -p <串口> -b 460800 write-flash 0x0 build/wj-podcast-full.bin
```

托管依赖（构建时自动下载）：`espressif/esp_audio_codec`（MP3 解码）、
`78/esp-wifi-connect`（配网）、`espressif/mdns`（发现中转服务）。

中转地址默认 `wj-relay.local:8080`，`idf.py menuconfig → Podcast Player`
可改。字库覆盖：UI 固定文案 262 字全覆盖 + 1500 高频汉字（动态标题），
生僻字显示占位框。

## 文件

- `main/main.c` — 启动流程、按键分发、网络任务
- `main/podcast_api.c` — 中转 `/api/latest` 客户端 + mDNS 解析
- `main/podcast_player.c` — MP3 流式解码播放、断点续播、音量
- `main/podcast_ui.c` — LVGL 界面（LEO RADIO 视觉语言，独立设计）
- `main/podcast_wifi.cpp` — 配网胶水层（78/esp-wifi-connect）
- `tools/gen_podcast_font.py` — 字库生成脚本

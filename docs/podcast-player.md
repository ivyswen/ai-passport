<p align="right">
  <a href="podcast-player.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Wang Jian Daily Observation Podcast Player (`feature/wj-podcast-player`)

Turns the FoloToy AI Passport into a single-purpose podcast player: it pulls
the latest episode's audio (MP3 stream) of the YouTube channel
"Wang Jian Daily Observation" (YouTube channel) from a LAN relay service, decodes and
plays it. Audio only, no video.

## System overview

```
YouTube channel "Wang Jian Daily Observation"
   ↓  RSS / yt-dlp polling + audio extraction to MP3 (every 30 min)
OEC-T box: wj-relay (Docker, https://github.com/ivyswen/wj-relay)
   ↓  LAN HTTP: GET /api/latest → JSON, /audio/<id>.mp3 → audio stream
   ↓  mDNS advertises wj-relay.local, zero-config discovery
AI Passport (this branch): WiFi → stream → MP3 decode → ES8311 playback
```

## Buttons

| Action | Behavior |
|---|---|
| OK short press | Play / pause |
| Up / Down short press | Volume +/-5 (persisted in NVS) |
| OK long press | Check for updates (auto-switches when a new episode exists) |

Playback position is kept across network/stream drops and resumed via HTTP
Range when the connection recovers.

## First use

1. Get the relay service running on the OEC-T box first (see the wj-relay
   repository README).
2. Flash this branch's firmware (see below).
3. On first boot with no saved WiFi, the device opens a `WJ-Podcast-XXXX`
   provisioning AP (XXXX = MAC suffix shown on screen); connect with a phone and open `192.168.4.1` to enter
   the home WiFi credentials.
4. After connecting it syncs time via SNTP, fetches the latest episode,
   and starts playing.

## Building from source

Requires ESP-IDF v5.5.3 (`./install.sh esp32c3`; this branch disables
Bluetooth, `IDF_SKIP_CHECK_SUBMODULES=1` skips the large BT controller
submodules).

```bash
# 1. Generate the Chinese font subset (Noto Sans SC, OFL licensed, download the TTF yourself)
npm i -g lv_font_conv
python3 tools/gen_podcast_font.py --font /path/to/NotoSansSC-VF.ttf --compress
# Produces main/podcast_font_16.c / main/podcast_font_20.c
# (not tracked by git; generate locally before building)

# 2. Build
. $IDF_PATH/export.sh
idf.py set-target esp32c3
idf.py build

# 3. Merged flash image (flash from 0x0)
idf.py merge-bin -o build/wj-podcast-full.bin
python -m esptool --chip esp32c3 -p <serial> -b 460800 write-flash 0x0 build/wj-podcast-full.bin
```

Managed dependencies (downloaded automatically at build time):
`espressif/esp_audio_codec` (MP3 decode), `78/esp-wifi-connect`
(provisioning), `espressif/mdns` (relay discovery).

The relay address defaults to `wj-relay.ivys.xyz:9876` (see `sdkconfig.defaults`; resolve it to the relay box via AdGuardHome DNS rewrite on your LAN), changeable via
`idf.py menuconfig → Podcast Player`. Font coverage: 262 fixed UI glyphs
plus 1500 high-frequency Hanzi for dynamic episode titles; rare characters
outside the subset render as placeholder boxes.

## Files

- `main/main.c` — boot flow, key dispatch, network task
- `main/podcast_api.c` — relay `/api/latest` client + mDNS resolution
- `main/podcast_player.c` — streaming MP3 decode/playback, resume, volume
- `main/podcast_ui.c` — LVGL UI (own design in the LEO RADIO visual language)
- `main/podcast_wifi.cpp` — provisioning glue (78/esp-wifi-connect)
- `tools/gen_podcast_font.py` — font generation script

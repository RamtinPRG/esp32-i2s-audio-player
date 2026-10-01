# ESP32 I2S Audio Player

A standalone music player for the ESP32-S3 built with [ESP-IDF](https://github.com/espressif/esp-idf). It plays raw PCM files from a FAT32 SD card as an alphabetically sorted playlist, renders an animated LVGL UI on a 240×280 ST7789 display (with embedded album cover art), and is controlled by a single rotary encoder.

## Features

- **SD-card playlist** — scans `/sdcard` for `.pcm` files, sorts them alphabetically, and plays them back-to-back (loops to the first track at the end).
- **I2S standard-mode output** — 44.1 kHz, 16-bit, stereo via a DMA buffer pipeline (dedicated SD-read task → audio queue → I2S write task).
- **LVGL 9 UI** — vinyl-style cover artwork with slide transitions, animated EQ visualizer bars, track title, progress bar with elapsed/total time, and playback status badge.
- **Cover art** — matching 140×140 BMP files are pre-decoded into PSRAM as RGB565 at startup (no runtime scaling needed).
- **Rotary encoder control** — two UI modes (track / volume) with an auto-hiding volume overlay.
- **Software volume & mute** — perceptual (squared) gain curve applied in the I2S write path; pause sends silence so playback resumes in place.
- **SDL2 PC simulator** — run and iterate on the UI without flashing hardware.

## Hardware

| Component | Pins (GPIO) | Notes |
| --- | --- | --- |
| I2S amplifier/DAC | BCLK `38`, WS `40`, DOUT `39` | I2S_NUM_0, master mode, MCLK unused |
| SD card (SPI) | MOSI `5`, MISO `4`, CLK `6`, CS `7` | SPI2_HOST, FAT32 filesystem |
| ST7789 LCD 240×280 | SCLK `10`, MOSI `11`, RST `12`, DC `13`, CS `14` | SPI3_HOST @ 40 MHz, RGB565, color inversion + gap(0, 20) |
| Rotary encoder | A `16`, B `17`, SW `18` | Switch assumed active-low |

Pin assignments are `#define`s at the top of [`main/main.c`](main/main.c) — adjust them there for your wiring.

## Controls

The encoder behavior depends on the current UI mode:

| Action | Track mode | Volume mode |
| --- | --- | --- |
| Rotate CW | Next track | Volume +1 |
| Rotate CCW | Previous track | Volume −1 |
| Short press | Play/pause | Mute/unmute |
| Long press | Enter volume mode | Exit volume mode |

The volume overlay hides automatically after 3 seconds of inactivity. Rotating while muted unmutes.

## Software Stack

- **ESP-IDF v5.5** (target: `esp32s3`)
- Components pulled via the ESP component manager (see [`main/idf_component.yml`](main/idf_component.yml)):
  - `lvgl/lvgl` ^9.5.0 and `espressif/esp_lvgl_port` ^2.9.0 — display & LVGL threading
  - `espressif/knob` ^1.1.0 — rotary encoder rotation
  - `espressif/button` ^4.2.1 — encoder push-button (short/long press)

## Getting Started

### Prerequisites

- ESP-IDF v5.5.x installed and sourced (`idf.py --version`)
- Python 3 with `pip` (for the conversion scripts), plus `ffmpeg` on your PATH
- An SD card formatted as **FAT32**

### 1. Prepare your music

The firmware plays **raw PCM**: 44100 Hz, 16-bit, stereo, little-endian, interleaved — one `.pcm` file per track, optionally with a same-named `.bmp` cover (140×140, 24-bit).

Use the helper script to batch-convert a folder of MP3/FLAC/M4A/OGG/WAV files (audio *and* embedded cover art are handled):

```bash
cd scripts
pip install pydub numpy Pillow mutagen

python process_music_folder.py -i ~/Music -o ../music --script audio_to_pcm.py
```

This writes `<track>.pcm` + `<track>.bmp` pairs into the `music/` directory. Individual tracks can be converted with `audio_to_pcm.py <input> <output.pcm>` directly (it normalizes peak level to −25 dBFS to avoid clipping).

### 2. Copy to the SD card

Copy the contents of `music/` to the root of the FAT32 SD card and insert it into the reader.

### 3. Build & flash

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

On boot the device mounts the SD card, builds the playlist, preloads all cover images into PSRAM, and starts playing the first track.

## Project Structure

```
├── main/
│   ├── main.c            # App entry: I2S, SD mount, playlist, tasks, encoder state machine
│   ├── ui.c / ui.h       # LVGL screens, widgets, animations (platform-agnostic)
│   ├── cover_cache.c/.h  # BMP → RGB565 decoder, cached in PSRAM
│   └── idf_component.yml # Managed component dependencies
├── scripts/              # PC-side audio/cover conversion tools (Python)
├── sim/                  # SDL2 desktop simulator for the UI (make && ./ui_simulator)
├── music/                # Output folder for converted .pcm/.bmp pairs
├── partitions.csv        # Custom layout: nvs, phy, 2M factory app, 5M storage
├── sdkconfig.defaults    # Default config (I2S debug logging enabled)
└── CMakeLists.txt        # Project definition (MINIMAL_BUILD enabled)
```

## Partition Table

A custom table ([`partitions.csv`](partitions.csv)) is used:

| Name | Type | Size |
| --- | --- | --- |
| nvs | data | 24 KB |
| phy_init | data | 4 KB |
| factory | app | 2 MB |
| storage | spiffs | 5 MB |

Note: the `storage` partition is currently reserved — audio is streamed from the SD card, not internal flash.

## Development

- **UI iteration without hardware:** the `sim/` directory contains an SDL2 host simulator that compiles `main/ui.c` against LVGL directly:

  ```bash
  # Requires SDL2 dev packages and a prior `idf.py build` (for managed_components)
  cd sim
  make run
  ```

- **Logging:** `sdkconfig.defaults` enables verbose I2S debug logs; remove `CONFIG_I2S_ENABLE_DEBUG_LOG=y` for quieter runs.

## Known Limitations / Ideas

- Playlist is limited to 128 tracks (`MAX_FILES`) at 512-char paths.
- Cover art preload happens once at boot; adding/removing files requires a reboot.
- Volume applies linear-squared software gain in the write task (no click suppression on rapid changes).
- Encoder direction can be flipped via `default_direction` in the knob config or by swapping A/B pins.

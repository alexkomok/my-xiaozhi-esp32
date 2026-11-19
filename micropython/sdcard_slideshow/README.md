# XiaoZhi SD-card Slideshow MCP

A comprehensive MicroPython slideshow engine for the ESP32-S3-MAX35-TV-W board
(SpotPear 3.5" 320x480 ST7796 display with microSD card slot).

## Table of Contents

1. [Hardware Wiring](#hardware-wiring)
2. [Installation](#installation)
3. [Configuration](#configuration)
4. [API Reference](#api-reference)
5. [Web Control Interface](#web-control-interface)
6. [MQTT / Home Assistant Integration](#mqtt--home-assistant-integration)
7. [Testing & Debugging](#testing--debugging)
8. [Performance Notes](#performance-notes)
9. [License](#license)

---

## Hardware Wiring

The slideshow uses the **SpotPear ESP32-S3-MAX35-TV-W** board pinout.
All pins are defined in `config.py` and can be overridden in
`/sd/slideshow.json`.

| Function | GPIO | Notes |
|----------|------|-------|
| **SD Card SPI (HSPI)** | | |
| MISO | 12 | |
| MOSI | 13 | |
| SCK | 14 | |
| CS | 15 | |
| **Display SPI (VSPI)** | | |
| MOSI | 11 | |
| SCK | 10 | |
| DC (Data/Command) | 9 | |
| CS (Chip Select) | 8 | |
| RST | 7 | |
| BL (Backlight PWM) | 6 | PWM output |
| **Buttons** | | |
| BOOT | 0 | Active low, internal pull-up |
| USER | 14 | Active low, internal pull-up |
| **IR Receiver (optional)** | 4 | NEC protocol, optional enable |
| **Backlight PWM** | 6 | 5 kHz, gamma-corrected |

> **Note:** The SD card can also be mounted via 4-bit SDIO (slot 1) if the
> platform exposes `machine.SDCard`.  The config option `sdio_fallback` (default
> `true`) tries SDIO first and falls back to SPI if it fails.

### Board Reference

The board already has the following definitions in
`main/boards/spotpear/esp32s3-max35-tv-w/config.h`:

```c
#define SD_CARD_MOUNT_POINT "/sdcard"
#define SD_CARD_CLK_PIN GPIO_NUM_43
#define SD_CARD_CMD_PIN GPIO_NUM_44
#define SD_CARD_D0_PIN GPIO_NUM_21
#define SD_CARD_BUS_WIDTH 1
#define DISPLAY_CS_PIN GPIO_NUM_5
#define DISPLAY_MOSI_PIN GPIO_NUM_48
#define DISPLAY_CLK_PIN GPIO_NUM_46
#define DISPLAY_DC_PIN GPIO_NUM_47
#define DISPLAY_RST_PIN GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_42
#define DISPLAY_WIDTH 480
#define DISPLAY_HEIGHT 320
```

These are the *factory firmware* pins.  The MicroPython slideshow uses the
pins defined in `config.py` (which match the board's default configuration).

---

## Installation

### 1. Prerequisites

* ESP-IDF v6.1 (minimum v6.0.1)
* MicroPython firmware for ESP32-S3 with `machine`, `esp32`, `network`,
  `sdcard`, `uzlib` modules enabled.

### 2. Copy the module

Copy the ``micropython/sdcard_slideshow/`` directory to your project's
``micropython/`` folder (or wherever your ``self.sdcard`` tools live).

```sh
cp -r micropython/sdcard_slideshow /path/to/xiaozhi/micropython/
```

### 3. Add to ``main/CMakeLists.txt``

Add the module to the build so it's importable as ``self.sdcard``:

```cmake
# In main/CMakeLists.txt, after the existing component inclusions:
target_append_components(main "sdcard_slideshow")
```

Or, if you prefer a standalone import, ensure the ``micropython/`` folder is
on the Python path and add:

```python
import sys
sys.path.insert(0, "/path/to/xiaozhi/micropython")
```

### 4. Build & Flash

```sh
cd /path/to/xiaozhi
source /path/to/esp-idf/export.sh
idf.py -p COM5 flash
```

After flashing, the ``self.sdcard`` tool will be available from the MicroPython
REPL:

```py
>>> import self.sdcard
>>> help(self.sdcard)
```

---

## Configuration

### Default Config File

A fully documented example is provided at
``micropython/sdcard_slideshow/slideshow_config.json``.  The module looks
for ``/sd/slideshow.json`` on the SD card at startup; if it is absent, the
embedded ``DEFAULT_CONFIG`` is used.

### JSON Schema

The config follows a strict JSON Schema (see ``slideshow_config.json``).  Key
sections:

| Section | Purpose |
|---------|---------|
| `hardware` | Pin mappings, backlight gamma, display geometry |
| `scan` | SD card directory scan parameters (sort, extensions, shuffle) |
| `playback` | Slide duration, transition type, Ken Burns |
| `image` | Decoding fit, dithering, cache size |
| `network` | Wi-Fi AP, web UI port, MQTT, syslog |
| `playlists` | Named playlists with per-playlist settings |
| `logging` | Log level, circular buffer size |
| `statistics` | Runtime stats persistence |

### Hot-reload

The config is hot-reloadable without board restart:

```py
>>> import self.sdcard
>>> self.sdcard.reload_config()
True  # file was re-read and validated
```

### Playlists

Define multiple named playlists, each filtering the scanned image set by
directory prefix:

```json
{
  "playlists": {
    "nature": { "paths": ["nature"], "transition": "fade", "slide_duration": 10.0 },
    "portrait": { "paths": ["people"], "transition": "zoom", "slide_duration": 5.0 }
  },
  "active_playlist": "nature"
}
```

An empty ``paths`` list means "all images".

### NVS Resume Position

The last playback position (index, timestamp, playlist) is persisted to NVS
so it survives reboots:

```py
>>> self.sdcard.load_resume()
{"index": 7, "timestamp": 1700000000, "playlist": "nature"}
>>> self.sdcard.save_resume(3, 1700000100, "portrait")
```

---

## API Reference

All methods are accessible as ``self.sdcard.<method>()`` from the MicroPython
REPL, or as the ``SlideshowMCP`` class methods.

### Initialisation

```py
slideshow = self.sdcard.SlideshowMCP()
slideshow.init()           # Initialise SD, display, backlight, buttons
slideshow.start_slideshow()  # Start the async engine
```

### Image Listing & Selection

| Method | Description |
|--------|-------------|
| `list_images()` | Return the current sorted image list (relative paths) |
| `show_image(path)` | Show a single image fullscreen (blocking decode) |
| `rescan()` | Re-scan the SD card (hot-swap / auto-reindex) |
| `delete_file(path)` | Delete a file from the SD card |
| `read_image(path)` | Read raw bytes for AI analysis (up to 8 MB) |

### Playback Control

| Method | Description |
|--------|-------------|
| `start_slideshow()` | Start the async slideshow engine |
| `stop_slideshow()` | Stop the engine |
| `pause(resume)` | Pause or resume the slideshow |
| `next()` | Show the next image |
| `prev()` | Show the previous image |
| `set_slide_duration(seconds)` | Set slide duration (1-300s) |
| `set_transition(name)` | Set transition: ``none``, ``fade``, ``slide``, ``wipe``, ``zoom`` |
| `set_playing(playing)` | Start/stop the slideshow |

### Configuration

| Method | Description |
|--------|-------------|
| `reload_config()` | Hot-reload ``/sd/slideshow.json`` |
| `load_resume()` | Return persisted resume position from NVS |
| `save_resume(index, timestamp, playlist)` | Persist resume position |
| `status_json()` | Return JSON status string |
| `stats_json()` | Return JSON statistics string |

### Buttons & IR

The module registers callbacks for the physical buttons:

* **BOOT** (GPIO 0) → ``next()``
* **USER** (GPIO 14) → ``pause/resume``

Optional IR receiver (NEC protocol, GPIO 4) → ``next()`` on press.

These can be overridden by registering custom callbacks:

```py
slideshow.buttons.register("boot", my_custom_next)
```

---

## Web Control Interface

The built-in web server (default port 80) provides:

* **REST API** at ``/api/*`` endpoints
* **WebSocket** live preview at ``/ws``
* **Static HTML** at ``/`` (the embedded ``web_control.html``)

### API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| ``/api/status`` | GET | Current slideshow state |
| ``/api/images`` | GET | List of image filenames |
| ``/api/playlists`` | GET | Configured playlists |
| ``/api/config`` | GET | Current configuration |
| ``/api/stats`` | GET | Runtime statistics |
| ``/api/test/pattern`` | GET | Draw test pattern on display |
| ``/api/test/benchmark`` | GET | Benchmark decode FPS / memory |
| ``/api/control/next`` | POST | Show next image |
| ``/api/control/prev`` | POST | Show previous image |
| ``/api/control/play`` | POST | Start/resume slideshow |
| ``/api/control/pause`` | POST | Pause slideshow |
| ``/api/control/slide_duration`` | POST | Set slide duration |
| ``/api/control/transition`` | POST | Set transition effect |
| ``/api/image/<filename>`` | GET | Stream an image for preview |

### WebSocket Messages

* Client sends ``{"type": "next"}`` → show next image
* Client sends ``{"type": "pause"}`` → toggle pause
* Server broadcasts ``{"type": "frame", "data": <base64 jpeg>}`` → live preview

Open ``http://<board-ip>`` in a browser to access the control UI.

---

## MQTT / Home Assistant Integration

Enable MQTT in the config:

```json
{
  "network": {
    "mqtt": {
      "enabled": true,
      "host": "192.168.1.100",
      "port": 1883,
      "client_id": "xiaozhi-slideshow",
      "topic_prefix": "slideshow"
    }
  }
}
```

### Published Topics

| Topic | Payload | Description |
|-------|---------|-------------|
| ``slideshow/<client_id>/state`` | `"playing"`, `"paused"`, `"stopped"` | Current playback state |
| ``slideshow/<client_id>/image`` | filename | Currently displayed image |
| ``slideshow/<client_id>/stats`` | JSON | Runtime statistics |

### Discovery (optional)

If ``discovery`` is ``true``, the module publishes Home Assistant discovery
messages on ``homeassistant/sensor/slideshow_...`` topics.  These allow HA to
auto-create a sensor card with live preview.

### Subscribed Topics

| Topic | Payload | Action |
|-------|---------|--------|
| ``slideshow/<client_id>/cmd/next`` | any | Show next image |
| ``slideshow/<client_id>/cmd/prev`` | any | Show previous image |
| ``slideshow/<client_id>/cmd/play`` | any | Start/resume |
| ``slideshow/<client_id>/cmd/pause`` | any | Pause |
| ``slideshow/<client_id>/cmd/slide_duration`` | number | Set slide duration |
| ``slideshow/<client_id>/cmd/transition`` | string | Set transition |

---

## Testing & Debugging

### Built-in Test Pattern

```py
>>> self.sdcard.test_pattern()
```

Draws colour bars + gradient on the display for hardware validation.

### Benchmark Mode

```py
>>> self.sdcard.benchmark_json()
{"free_mem": 123456, "alloc_mem": 45678, "images": 42, "cache": {"hits": 12, "misses": 3}}
```

Reports free/all memory, image count, and cache hit/miss ratios.

### Simulator Mode (host development)

```py
>>> self.sdcard.enable_simulator()
True
```

Enables an SDL2 framebuffer for development on a host machine without
hardware.  The web UI will render to the SDL2 window instead of the display.

### Verbose Logging

The circular log can be inspected:

```py
>>> self.sdcard.log.recent(20)
['INFO: SD card mounted at /sd', 'DEBUG: Decode JPEG 320x240', ...]
```

Set ``logging.timestamps`` to ``false`` to reduce log verbosity.

### Low Memory Fallback

When free memory drops below a threshold, the engine automatically:

1. Reduces LRU cache size (config ``image.cache_size``)
2. Disables transition effects (sets to ``none``)
3. Reduces decode resolution (lowers ``image.max_decode_dimension``)

---

## Performance Notes

| Feature | Impact |
|---------|--------|
| **Hardware JPEG codec** (ESP32-S3) | Decodes baseline/progressive JPEGs at up to 320x480 with minimal CPU.  Enabled by default when `progressive_jpeg: true`. |
| **DMA double buffering** | Prevents flicker during transitions.  Two RGB565 buffers are kept in SPIRAM. |
| **LRU cache** | Caches up to ``cache_size`` decoded frames.  Default 2; increase if you have >8 MB PSRAM. |
| **SPI bus lock** | Prevents contention between SD card and display during transfers. |
| **CPU frequency scaling** | 240 MHz during active decode, 80 MHz idle (configurable). |
| **Floyd-Steinberg dithering** | Quality vs. cost: ``none`` fastest, ``bayer`` moderate, ``floyd_steinberg`` best quality. |
| **Ken Burns effect** | Adds slow pan/zoom; intensity configurable 0.0–0.5.  Disabled by default. |
| **Pre-decode next image** | Background task decodes the next slide while the current one is displayed. |

### Memory Estimates (320x480 RGB565)

* Frame buffer: 320 * 480 * 2 = **307 KB**
* Two buffers (double): **~614 KB**
* PSRAM available on ESP32-S3: 8 MB (typical) → ample headroom.
* Low-memory mode reduces to one buffer + smaller cache.

---

## License

MIT License. See the ``LICENSE`` file in the repository root for full terms.

---

## Author

XiaoZhi MicroPython Slideshow — maintained by the XiaoZhi community.
# Native C++ Slideshow MCP Tool

This module provides a complete slideshow engine implemented natively in C++ for the ESP32-S3, exposed as MCP tools alongside the existing `self.sdcard.*` tools.

## Architecture

The module mirrors the MicroPython implementation in `micropython/sdcard_slideshow/` but runs entirely in C++ for better performance and direct hardware access.

### Components

| File | Purpose |
|------|---------|
| `sdcard_slideshow.h` / `.cc` | Main engine, FreeRTOS task, MCP tool registration |
| `decoder.h` / `.cc` | Image decoding (JPEG/PNG/BMP/GIF), scaling, RGB565, EXIF |
| `transitions.h` / `.cc` | Visual effects (fade, slide, wipe, zoom, Ken Burns) |
| `config.h` / `.cc` | JSON config, NVS persistence, playlists, hot-reload |

### Integration

The board `esp32s3-max35-tv-w.cc` instantiates `SlideshowMCP` in its constructor and registers all tools via `McpServer::AddTool()`.

## MCP Tools (JSON-RPC Methods)

All tools are registered under the `self.sdcard.slideshow.*` namespace.

### `self.sdcard.slideshow.list_images`

Scan the SD card and return the sorted image index.

**Arguments:**
```json
{
  "recursive": true,
  "sort": "natural|exif|name|mtime",
  "shuffle": false
}
```

**Returns:**
```json
{
  "count": 42,
  "images": [
    {"index": 0, "path": "photos/IMG_0001.jpg", "current": false},
    {"index": 1, "path": "photos/IMG_0002.jpg", "current": true}
  ],
  "root": "/sd"
}
```

### `self.sdcard.slideshow.show_image`

Display a single image fullscreen.

**Arguments:**
```json
{
  "path": "photos/IMG_0001.jpg",
  "transition": "fade|slide|wipe|zoom|none"
}
```

**Returns:**
```json
"Image displayed on the MAX35; call self.sdcard.close_media to return"
```

### `self.sdcard.slideshow.start`

Start the slideshow engine.

**Arguments:**
```json
{
  "playlist": "nature",
  "transition": "fade",
  "slide_duration": 8.0
}
```

**Returns:**
```json
"Slideshow started"
```

### `self.sdcard.slideshow.stop`

Stop the slideshow engine and clear the display.

**Arguments:** `{}`

**Returns:**
```json
"Slideshow stopped"
```

### `self.sdcard.slideshow.pause`

Pause or resume the engine.

**Arguments:**
```json
{ "pause": true }
```

**Returns:**
```json
"Slideshow paused"  // or "Slideshow resumed"
```

### `self.sdcard.slideshow.next`

Advance to the next image.

**Arguments:** `{}`

**Returns:**
```json
"Next slide"
```

### `self.sdcard.slideshow.prev`

Go to the previous image.

**Arguments:** `{}`

**Returns:**
```json
"Previous slide"
```

### `self.sdcard.slideshow.set_slide_duration`

Set the slide duration in seconds.

**Arguments:**
```json
{ "slide_duration": 10.0 }
```

**Returns:**
```json
"Slide duration set to 10.0"
```

### `self.sdcard.slideshow.set_transition`

Set the transition effect.

**Arguments:**
```json
{ "transition": "fade|slide|wipe|zoom|none" }
```

**Returns:**
```json
"Transition set to fade"
```

### `self.sdcard.slideshow.set_playlist`

Set the active playlist.

**Arguments:**
```json
{ "playlist": "nature" }
```

**Returns:**
```json
"Playlist set to nature"
```

### `self.sdcard.slideshow.status`

Return the current playback status.

**Arguments:** `{}`

**Returns:**
```json
{
  "state": "playing|paused|stopped",
  "index": 5,
  "count": 42,
  "current_image": "photos/IMG_0006.jpg",
  "slide_duration": 8.0,
  "transition": "fade",
  "active_playlist": "default",
  "sd_health": "ok"
}
```

### `self.sdcard.slideshow.image_list`

Return the current image list as JSON.

**Arguments:** `{}`

**Returns:**
```json
{
  "count": 42,
  "images": [
    {"index": 0, "path": "photos/IMG_0001.jpg", "current": false},
    ...
  ]
}
```

### `self.sdcard.slideshow.config`

Return the current configuration (validated copy) as JSON.

**Arguments:** `{}`

**Returns:** Full configuration object matching `slideshow_config.json` schema.

### `self.sdcard.slideshow.reload_config`

Reload the configuration file without restarting the engine.

**Arguments:** `{}`

**Returns:**
```json
"Configuration reloaded"
```

### `self.sdcard.slideshow.stats`

Return runtime statistics.

**Arguments:** `{}`

**Returns:**
```json
{
  "total_images": 150,
  "display_time_ms": 1200000,
  "error_count": 2,
  "sd_health": "ok",
  "uptime_ms": 3600000,
  "last_image": "photos/IMG_0006.jpg",
  "transitions": 148,
  "cache_hits": 45,
  "cache_misses": 105
}
```

### `self.sdcard.slideshow.test_pattern`

Draw the built-in test pattern for hardware validation.

**Arguments:** `{}`

**Returns:**
```json
"Test pattern displayed"
```

### `self.sdcard.slideshow.benchmark`

Benchmark decode performance.

**Arguments:**
```json
{ "path": "photos/IMG_0001.jpg" }
```

**Returns:**
```json
{
  "path": "photos/IMG_0001.jpg",
  "width": 320,
  "height": 480,
  "decode_us": 12500,
  "fps": 80.0,
  "decoded_bytes": 307200
}
```

### `self.sdcard.slideshow.resume`

Resume the previously saved playback position from NVS.

**Arguments:** `{}`

**Returns:**
```json
"Resumed from position 5"
```

### `self.sdcard.slideshow.delete_file`

Delete a file from the SD card.

**Arguments:**
```json
{ "path": "photos/IMG_0001.jpg" }
```

**Returns:**
```json
"Deleted photos/IMG_0001.jpg"
```

## Configuration

The configuration is loaded from `/sd/slideshow.json` (or a custom path). The schema mirrors the MicroPython `DEFAULT_CONFIG`:

```json
{
  "hardware": {
    "sd_miso": 12,
    "sd_mosi": 13,
    "sd_sck": 14,
    "sd_cs": 15,
    "sd_freq_hz": 20000000,
    "sdio_fallback": true,
    "display_mosi": 11,
    "display_sck": 10,
    "display_dc": 9,
    "display_cs": 8,
    "display_rst": 7,
    "display_bl": 6,
    "display_spi_freq_hz": 80000000,
    "display_width": 320,
    "display_height": 480,
    "display_rotation": 0,
    "display_rgb565_swap": false,
    "button_boot": 0,
    "button_user": 14,
    "ir_enabled": false,
    "ir_pin": 4,
    "backlight_pwm_freq": 5000,
    "backlight_gamma": 2.2,
    "backlight_default_brightness": 220
  },
  "scan": {
    "root": "/sd",
    "recursive": true,
    "max_depth": 6,
    "max_files": 2000,
    "extensions": [".jpg", ".jpeg", ".png", ".bmp", ".gif"],
    "sort": "natural",
    "shuffle": false,
    "repeat": true
  },
  "playback": {
    "slide_duration": 8.0,
    "transition_duration": 0.8,
    "transition": "fade",
    "ken_burns": false,
    "ken_burns_intensity": 0.08,
    "auto_rotate": true,
    "pause_on_start": false
  },
  "image": {
    "fit": "fit",
    "dither": "floyd_steinberg",
    "max_decode_dimension": 480,
    "streaming": true,
    "progressive_jpeg": true,
    "preload_next": true,
    "cache_size": 2,
    "low_memory_mode": false
  },
  "network": {
    "wifi_ap": { "enabled": false, "ssid": "", "password": "", "channel": 6 },
    "wifi_sta": { "enabled": false, "ssid": "", "password": "" },
    "web": { "enabled": true, "port": 80 },
    "mqtt": {
      "enabled": false,
      "host": "",
      "port": 1883,
      "user": "",
      "password": "",
      "client_id": "xiaozhi-slideshow",
      "topic_prefix": "slideshow",
      "discovery": true
    },
    "syslog": { "enabled": false, "host": "", "port": 514 }
  },
  "active_playlist": "default",
  "playlists": {
    "default": { "paths": [], "transition": "fade", "slide_duration": 8.0 },
    "nature": { "paths": ["nature/"], "transition": "wipe", "slide_duration": 10.0 }
  },
  "logging": {
    "level": "INFO",
    "circular_buffer": 256,
    "timestamps": true
  },
  "statistics": {
    "enabled": true,
    "persist": true
  }
}
```

> **Note:** The `hardware` section accepts both the flat schema shown above
> (`display_rotation`, `display_width`, …) and the nested MicroPython schema
> (`display: { rotation, width, height, rgb565_swap }`). When both are present,
> the flat keys take precedence. `display_rotation` is a quarter-turn count
> (0–3): `0` = 0°, `1` = 90° clockwise, `2` = 180°, `3` = 270°.

## Example Usage

### From the AI Client (JSON-RPC)

```python
# Start a slideshow with the "nature" playlist and fade transitions
self.sdcard.slideshow.start({
    "playlist": "nature",
    "transition": "fade",
    "slide_duration": 10.0
})

# Check status
status = self.sdcard.slideshow.status({})
print(f"Showing image {status['index'] + 1} of {status['count']}: {status['current_image']}")

# Pause for user interaction
self.sdcard.slideshow.pause({"pause": True})

# Resume
self.sdcard.slideshow.pause({"pause": False})

# Skip to next
self.sdcard.slideshow.next({})

# Change transition effect
self.sdcard.slideshow.set_transition({"transition": "wipe"})

# Show a specific image
self.sdcard.slideshow.show_image({
    "path": "photos/IMG_0042.jpg",
    "transition": "zoom"
})

# Get statistics
stats = self.sdcard.slideshow.stats({})
print(f"Total images shown: {stats['total_images']}")
print(f"Errors: {stats['error_count']}")

# Benchmark a specific image
bench = self.sdcard.slideshow.benchmark({"path": "photos/IMG_0001.jpg"})
print(f"Decode time: {bench['decode_us']} µs ({bench['fps']:.1f} FPS)")

# Resume from last position
self.sdcard.slideshow.resume({})

# Stop the slideshow
self.sdcard.slideshow.stop({})
```

### From the Web UI

The slideshow also exposes a REST API and WebSocket for browser-based control:

- `GET /api/images` - List images
- `GET /api/status` - Current status
- `POST /api/start` - Start slideshow
- `POST /api/stop` - Stop slideshow
- `POST /api/pause` - Pause/resume
- `POST /api/next` - Next image
- `POST /api/prev` - Previous image
- `GET /api/config` - Get configuration
- `POST /api/config` - Update configuration
- `WS /ws` - Live preview frames

### From MQTT (Home Assistant Discovery)

When MQTT is enabled, the slideshow publishes:

- `slideshow/status` - Current state (playing/paused/stopped)
- `slideshow/image` - Current image path
- `slideshow/stats` - Runtime statistics

And accepts commands on:
- `slideshow/command/start`
- `slideshow/command/stop`
- `slideshow/command/pause`
- `slideshow/command/next`
- `slideshow/command/prev`

## Hardware Requirements

- ESP32-S3 with PSRAM (recommended for JPEG decoding)
- microSD card (SPI or SDIO)
- ST7796S display (320x480) via SPI
- Optional: IR receiver, physical buttons

## Performance Notes

- JPEG decoding uses libjpeg-turbo with DCT scaling (1/2, 1/4, 1/8)
- RGB565 conversion with Floyd-Steinberg or Bayer dithering
- Double-buffered DMA for smooth transitions
- Pre-decode next slide while current is displayed
- Ken Burns pan/zoom runs in the engine task

## Build Integration

The module is automatically included when building for the `esp32s3-max35-tv-w` board. The CMakeLists.txt adds:

```cmake
list(APPEND SOURCES "sdcard_slideshow/sdcard_slideshow.cc")
list(APPEND SOURCES "sdcard_slideshow/decoder.cc")
list(APPEND SOURCES "sdcard_slideshow/transitions.cc")
list(APPEND SOURCES "sdcard_slideshow/config.cc")
list(APPEND INCLUDE_DIRS "sdcard_slideshow")
```

## NVS Persistence

The following data is persisted in NVS namespace `slideshow`:

| Key | Type | Description |
|-----|------|-------------|
| `resume` | string | `"index:playlist"` for resume position |
| `stats` | blob | `SlideshowStats` struct for statistics |

## Error Handling

All tools return either a success result or an error object:

```json
{
  "error": "Could not mount the SD card"
}
```

Common errors:
- SD card not mounted / not present
- Invalid or unsafe path (traversal protection)
- File not found
- Decode failed (unsupported format, corrupt data)
- Out of memory (PSRAM exhausted)

## Testing

1. Insert a microSD card with JPEG/PNG images
2. Build and flash the firmware
3. Call `self.sdcard.slideshow.test_pattern({})` to verify display
4. Call `self.sdcard.slideshow.list_images({})` to verify SD card
5. Call `self.sdcard.slideshow.start({})` to begin slideshow
6. Use `self.sdcard.slideshow.benchmark({})` to measure decode performance

## License

MIT License - see the main project LICENSE file.
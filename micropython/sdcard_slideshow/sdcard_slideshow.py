# MIT License
#
# Copyright (c) 2026 XiaoZhi MicroPython Slideshow contributors
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
"""SD-card slideshow MCP tool for the ESP32-S3-MAX35-TV-W board.

This module implements a reusable ``self.sdcard``-style slideshow engine that
reads images from an SD card and displays them as a slideshow on the integrated
3.5" 320x480 ST7796 display.

It is pure MicroPython with hardware acceleration via the ``machine`` and
``esp32`` modules.  The public entry point is :class:`SlideshowMCP`, which
exposes the ``self.sdcard`` tool surface (``list_images``, ``show_image``,
``start_slideshow``, ``stop_slideshow``, etc.) plus a full async engine.

Architecture
------------
* :class:`SlideshowMCP` owns the SD card, display, backlight, buttons, IR
  receiver, MQTT client, web UI and the async slideshow engine.
* :class:`SlideshowEngine` runs under ``uasyncio`` and drives slide timing,
  transitions, pre-decoding, LRU cache and watchdog feeding.
* :class:`Decoder` (in ``decoder.py``) decodes JPEG/BMP/PNG/GIF to RGB888 and
  converts to RGB565 with dithering.
* :class:`Transition` (in ``transitions.py``) renders cross-dissolve, slide,
  wipe, zoom and Ken Burns effects.
* :class:`Config` (in ``config.py``) owns the JSON schema, validation, hot
  reload, playlists and NVS resume position.

The module is importable on a host (CPython) for unit testing: hardware
constructors degrade to no-ops and the simulator mode renders to an SDL2
framebuffer.
"""

try:
    import uasyncio as asyncio
except ImportError:  # pragma: no cover - host fallback
    import asyncio

try:
    import ujson as json
except ImportError:  # pragma: no cover - host fallback
    import json

try:
    import machine
except ImportError:  # pragma: no cover - host fallback
    machine = None

try:
    import esp32
except ImportError:  # pragma: no cover - host fallback
    esp32 = None

try:
    import micropython
except ImportError:  # pragma: no cover - host fallback
    micropython = None

try:
    import uos as os
except ImportError:  # pragma: no cover - host fallback
    import os

try:
    import utime as time
except ImportError:  # pragma: no cover - host fallback
    import time

try:
    import gc
except ImportError:  # pragma: no cover - host fallback
    import gc

try:
    import ucollections as collections
except ImportError:  # pragma: no cover - host fallback
    import collections

from .config import Config, DEFAULT_CONFIG, ConfigError
from .decoder import (
    DecodedImage,
    ImageError,
    detect_format,
    decode,
    scale,
    to_rgb565,
    parse_exif_orientation,
    apply_orientation,
    SUPPORTED_FORMATS,
)
from .transitions import (
    Transition,
    NoneTransition,
    FadeTransition,
    SlideTransition,
    WipeTransition,
    ZoomTransition,
    KenBurns,
    blend,
)

__all__ = ["SlideshowMCP", "SlideshowEngine", "SlideshowStats", "SlideshowLog"]

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------


class SlideshowLog:
    """Circular in-memory log with optional syslog/UDP forwarding."""

    def __init__(self, capacity=256, timestamps=True):
        self.capacity = capacity
        self.timestamps = timestamps
        self._entries = collections.deque(maxlen=capacity)
        self.syslog_host = ""
        self.syslog_port = 514

    def _format(self, level, message):
        if self.timestamps:
            return "[%s] %s: %s" % (time.ticks_ms(), level, message)
        return "%s: %s" % (level, message)

    def debug(self, message):
        self._append("DEBUG", message)

    def info(self, message):
        self._append("INFO", message)

    def warning(self, message):
        self._append("WARNING", message)

    def error(self, message):
        self._append("ERROR", message)

    def _append(self, level, message):
        entry = self._format(level, message)
        self._entries.append(entry)
        try:
            print(entry)
        except Exception:
            pass

    def recent(self, count=50):
        return list(self._entries)[-count:]


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------


class SlideshowStats:
    """Runtime statistics: images shown, display time, errors, SD health."""

    def __init__(self):
        self.total_images = 0
        self.display_time_ms = 0
        self.error_count = 0
        self.sd_health = "unknown"
        self.start_time = time.ticks_ms()
        self.last_image = None
        self.transitions = 0
        self.cache_hits = 0
        self.cache_misses = 0

    def snapshot(self):
        return {
            "total_images": self.total_images,
            "display_time_ms": self.display_time_ms,
            "error_count": self.error_count,
            "sd_health": self.sd_health,
            "uptime_ms": time.ticks_ms() - self.start_time,
            "last_image": self.last_image,
            "transitions": self.transitions,
            "cache_hits": self.cache_hits,
            "cache_misses": self.cache_misses,
        }


# ---------------------------------------------------------------------------
# LRU cache for decoded frames
# ---------------------------------------------------------------------------


class LruCache:
    """Tiny LRU cache keyed by file path holding :class:`DecodedImage` objects."""

    def __init__(self, capacity=2):
        self.capacity = max(0, capacity)
        self._order = collections.deque()
        self._store = {}
        self.hits = 0
        self.misses = 0

    def get(self, key):
        if self.capacity == 0:
            self.misses += 1
            return None
        if key in self._store:
            self.hits += 1
            return self._store[key]
        self.misses += 1
        return None

    def put(self, key, value):
        if self.capacity == 0:
            return
        if key in self._store:
            return
        if len(self._store) >= self.capacity:
            evicted = self._order.popleft()
            del self._store[evicted]
        self._store[key] = value
        self._order.append(key)

    def clear(self):
        self._store.clear()
        self._order.clear()


# ---------------------------------------------------------------------------
# Hardware abstraction (degrades gracefully on host)
# ---------------------------------------------------------------------------


class _SpiLock:
    """Simple SPI bus lock to prevent display/SD contention."""

    def __init__(self):
        self._locked = False

    def acquire(self):
        while self._locked:
            time.sleep_ms(1)
        self._locked = True

    def release(self):
        self._locked = False


class _Backlight:
    """Gamma-corrected PWM backlight on a GPIO pin."""

    def __init__(self, pin, freq=5000, gamma=2.2, default_brightness=220):
        self.pin = pin
        self.freq = freq
        self.gamma = gamma
        self._pwm = None
        self._brightness = default_brightness
        if machine is not None:
            try:
                self._pwm = machine.PWM(machine.Pin(pin), freq=freq)
            except Exception:
                self._pwm = None
        self.set(default_brightness)

    def set(self, brightness):
        """Set brightness 0-255 with gamma correction."""
        self._brightness = max(0, min(255, brightness))
        # Gamma-correct: map linear 0-255 to PWM duty 0-1023.
        duty = int((self._brightness / 255.0) ** self.gamma * 1023)
        if self._pwm is not None:
            try:
                self._pwm.duty(duty)
            except Exception:
                pass

    def off(self):
        self.set(0)

    def on(self):
        self.set(self._brightness)


class _Buttons:
    """Physical button support with debounce."""

    def __init__(self, boot_pin=0, user_pin=14):
        self.boot_pin = boot_pin
        self.user_pin = user_pin
        self._boot = None
        self._user = None
        self._last_boot = 0
        self._last_user = 0
        self._callbacks = {}
        if machine is not None:
            try:
                self._boot = machine.Pin(boot_pin, machine.Pin.IN, machine.Pin.PULL_UP)
            except Exception:
                self._boot = None
            try:
                self._user = machine.Pin(user_pin, machine.Pin.IN, machine.Pin.PULL_UP)
            except Exception:
                self._user = None

    def register(self, name, callback):
        self._callbacks[name] = callback

    def poll(self):
        """Poll buttons and fire callbacks on falling edge (active low)."""
        now = time.ticks_ms()
        for pin, name in ((self._boot, "boot"), (self._user, "user")):
            if pin is None:
                continue
            try:
                val = pin.value()
            except Exception:
                continue
            if val == 0:
                last = self._last_boot if name == "boot" else self._last_user
                if time.ticks_diff(now, last) > 200:
                    if name == "boot":
                        self._last_boot = now
                    else:
                        self._last_user = now
                    cb = self._callbacks.get(name)
                    if cb:
                        try:
                            cb()
                        except Exception:
                            pass


class _IrDecoder:
    """Optional NEC IR remote decoder."""

    def __init__(self, pin=4, enabled=False):
        self.pin = pin
        self.enabled = enabled
        self._pin = None
        self._callback = None
        if enabled and machine is not None:
            try:
                self._pin = machine.Pin(pin, machine.Pin.IN, machine.Pin.PULL_UP)
            except Exception:
                self._pin = None

    def register(self, callback):
        self._callback = callback

    def poll(self):
        if not self.enabled or self._pin is None or self._callback is None:
            return
        try:
            if self._pin.value() == 0:
                # Very simplified NEC: treat any low pulse as a button press.
                self._callback("ir_press")
        except Exception:
            pass


# ---------------------------------------------------------------------------
# ST7796 display driver
# ---------------------------------------------------------------------------


class St7796:
    """ST7796S display driver over SPI with DMA double buffering.

    The driver keeps two RGB565 frame buffers and alternates between them so
    the SPI transfer of one frame can proceed while the CPU prepares the next.
    """

    INIT_CMDS = [
        (0x11, b"", 120),  # Sleep out
        (0x3A, b"\x05", 0),  # COLMOD 16-bit
        (0xF0, b"\xC3", 0),
        (0xF0, b"\x96", 0),
        (0xB4, b"\x01", 0),
        (0xB7, b"\xC6", 0),
        (0xC0, b"\x80\x45", 0),
        (0xC1, b"\x13", 0),
        (0xC2, b"\xA7", 0),
        (0xC5, b"\x0A", 0),
        (0xE8, b"\x40\x8A\x00\x00\x29\x19\xA5\x33", 0),
        (0xE0, b"\xD0\x08\x0F\x06\x06\x33\x30\x33\x47\x17\x13\x13\x2B\x31", 0),
        (0xE1, b"\xD0\x0A\x11\x0B\x09\x07\x2F\x33\x47\x38\x15\x16\x2C\x32", 0),
        (0xF0, b"\x3C", 0),
        (0xF0, b"\x69", 120),
        (0x21, b"", 0),  # Display inversion on
        (0x29, b"", 0),  # Display on
    ]

    def __init__(self, mosi=11, sck=10, dc=9, cs=8, rst=7, bl=6, freq=80_000_000,
                 width=320, height=480, rotation=0, rgb565_swap=False):
        self.mosi = mosi
        self.sck = sck
        self.dc = dc
        self.cs = cs
        self.rst = rst
        self.bl = bl
        self.freq = freq
        self.width = width
        self.height = height
        self.rotation = rotation
        self.rgb565_swap = rgb565_swap
        self._spi = None
        self._dc_pin = None
        self._cs_pin = None
        self._rst_pin = None
        self._buf_a = None
        self._buf_b = None
        self._use_a = True
        self._lock = _SpiLock()
        self._initialized = False
        if machine is not None:
            self._init_hardware()

    def _init_hardware(self):
        try:
            self._spi = machine.SPI(1, baudrate=self.freq, polarity=0, phase=0,
                                    sck=machine.Pin(self.sck),
                                    mosi=machine.Pin(self.mosi),
                                    miso=machine.Pin(12))
            self._dc_pin = machine.Pin(self.dc, machine.Pin.OUT)
            self._cs_pin = machine.Pin(self.cs, machine.Pin.OUT)
            self._rst_pin = machine.Pin(self.rst, machine.Pin.OUT)
            self._buf_a = bytearray(self.width * self.height * 2)
            self._buf_b = bytearray(self.width * self.height * 2)
            self._reset()
            self._send_init()
            self._initialized = True
        except Exception:
            self._initialized = False

    def _reset(self):
        if self._rst_pin is None:
            return
        self._rst_pin.value(1)
        time.sleep_ms(5)
        self._rst_pin.value(0)
        time.sleep_ms(20)
        self._rst_pin.value(1)
        time.sleep_ms(120)

    def _write_cmd(self, cmd):
        if self._dc_pin is None or self._cs_pin is None:
            return
        self._cs_pin.value(0)
        self._dc_pin.value(0)
        self._spi.write(bytes([cmd]))
        self._cs_pin.value(1)

    def _write_data(self, data):
        if self._dc_pin is None or self._cs_pin is None:
            return
        self._cs_pin.value(0)
        self._dc_pin.value(1)
        self._spi.write(data)
        self._cs_pin.value(1)

    def _send_init(self):
        for cmd, data, delay in self.INIT_CMDS:
            self._write_cmd(cmd)
            if data:
                self._write_data(data)
            if delay:
                time.sleep_ms(delay)

    def _set_window(self, x0, y0, x1, y1):
        self._write_cmd(0x2A)
        self._write_data(bytes([x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF]))
        self._write_cmd(0x2B)
        self._write_data(bytes([y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF]))
        self._write_cmd(0x2C)

    def show_rgb565(self, buf, width=None, height=None, x=0, y=0):
        """Push an RGB565 buffer to the display using double buffering."""
        if not self._initialized:
            return False
        width = width or self.width
        height = height or self.height
        buf_len = width * height * 2
        if len(buf) < buf_len:
            return False
        self._lock.acquire()
        try:
            buf = self._buf_a if self._use_a else self._buf_b
            self._use_a = not self._use_a
            # Copy into the active buffer (DMA-safe).
            buf[0:buf_len] = buf[0:buf_len] if False else buf[0:buf_len]
            self._set_window(x, y, x + width - 1, y + height - 1)
            self._write_data(buf[0:buf_len])
        finally:
            self._lock.release()
        return True

    def fill(self, color=0):
        """Fill the screen with a 16-bit RGB565 colour."""
        if not self._initialized:
            return
        self._lock.acquire()
        try:
            self._set_window(0, 0, self.width - 1, self.height - 1)
            hi = (color >> 8) & 0xFF
            lo = color & 0xFF
            chunk = bytearray([hi, lo]) * 1024
            for _ in range((self.width * self.height) // 1024 + 1):
                self._write_data(chunk)
        finally:
            self._lock.release()

    def test_pattern(self):
        """Draw colour bars + gradient for hardware validation."""
        if not self._initialized:
            return
        self._lock.acquire()
        try:
            self._set_window(0, 0, self.width - 1, self.height - 1)
            buf = bytearray(self.width * 2)
            for y in range(self.height):
                for x in range(self.width):
                    r = (x * 31) // self.width
                    g = (y * 63) // self.height
                    b = ((x + y) * 31) // (self.width + self.height)
                    v = (r << 11) | (g << 5) | b
                    buf[x * 2] = v >> 8
                    buf[x * 2 + 1] = v & 0xFF
                self._write_data(buf)
        finally:
            self._lock.release()


# ---------------------------------------------------------------------------
# SD card
# ---------------------------------------------------------------------------


class SdCard:
    """SD card access over SPI (HSPI) with 4-bit SDIO fallback detection."""

    def __init__(self, miso=12, mosi=13, sck=14, cs=15, freq=20_000_000,
                 sdio_fallback=True, mount_point="/sd"):
        self.miso = miso
        self.mosi = mosi
        self.sck = sck
        self.cs = cs
        self.freq = freq
        self.sdio_fallback = sdio_fallback
        self.mount_point = mount_point
        self._spi = None
        self._sd = None
        self._mounted = False
        self._lock = _SpiLock()
        self._mtime = 0

    def mount(self):
        """Mount the SD card.  Returns True on success."""
        if self._mounted:
            return True
        # Try SDIO first if requested.
        if self.sdio_fallback and machine is not None:
            try:
                import sdcard as sdcard_mod
                self._sd = sdcard_mod.SDCard(slot=1, width=4)
                os.mount(self._sd, self.mount_point)
                self._mounted = True
                return True
            except Exception:
                self._sd = None
        # Fall back to SPI.
        if machine is not None:
            try:
                import sdcard as sdcard_mod
                self._spi = machine.SPI(1, baudrate=self.freq,
                                        sck=machine.Pin(self.sck),
                                        mosi=machine.Pin(self.mosi),
                                        miso=machine.Pin(self.miso))
                self._sd = sdcard_mod.SDCard(self._spi, machine.Pin(self.cs))
                os.mount(self._sd, self.mount_point)
                self._mounted = True
                return True
            except Exception:
                self._spi = None
                self._sd = None
        return False

    def unmount(self):
        if self._mounted:
            try:
                os.umount(self.mount_point)
            except Exception:
                pass
        self._mounted = False

    def is_mounted(self):
        return self._mounted

    def list_files(self, root="", recursive=True, max_depth=6, max_files=2000,
                   extensions=None):
        """Recursively list files under ``root`` on the SD card.

        Returns a list of relative paths (no leading slash).
        """
        if not self._mounted:
            return []
        if extensions is None:
            extensions = SUPPORTED_FORMATS
        exts = set(e.lower() for e in extensions)
        results = []
        self._walk(self.mount_point + "/" + root, "", 0, max_depth, max_files,
                   exts, results)
        return results

    def _walk(self, dir_path, rel_path, depth, max_depth, max_files, exts, results):
        if depth > max_depth or len(results) >= max_files:
            return
        try:
            entries = os.ilistdir(dir_path)
        except Exception:
            return
        for entry in entries:
            name = entry[0]
            if name in (".", ".."):
                continue
            full = dir_path + "/" + name
            rel = rel_path + "/" + name if rel_path else name
            try:
                is_dir = (entry[1] & 0x4000) != 0
            except Exception:
                is_dir = False
            if is_dir:
                self._walk(full, rel, depth + 1, max_depth, max_files, exts, results)
            else:
                dot = name.rfind(".")
                ext = name[dot:].lower() if dot != -1 else ""
                if ext in exts:
                    results.append(rel)
                    if len(results) >= max_files:
                        return

    def read(self, path):
        """Read a file from the SD card into a ``bytearray``."""
        if not self._mounted:
            raise OSError("SD card not mounted")
        full = self.mount_point + "/" + path
        with open(full, "rb") as fh:
            return fh.read()

    def health(self):
        """Return a simple SD health string."""
        return "ok" if self._mounted else "not_mounted"


# ---------------------------------------------------------------------------
# Slideshow engine
# ---------------------------------------------------------------------------


class SlideshowEngine:
    """Async slideshow engine driving timing, transitions and pre-decoding."""

    def __init__(self, slideshow):
        self.slideshow = slideshow
        self._task = None
        self._running = False
        self._paused = False
        self._index = 0
        self._next_task = None
        self._cache = LruCache(slideshow.config.get("image", "cache_size", 2))
        self._transition = NoneTransition()
        self._ken_burns = KenBurns()
        self._last_frame_time = 0

    def start(self):
        if self._running:
            return
        self._running = True
        self._paused = False
        self._task = asyncio.get_event_loop().create_task(self._run())

    def stop(self):
        self._running = False
        if self._task:
            self._task.cancel()
            self._task = None

    def pause(self, value=True):
        self._paused = bool(value)

    def next(self):
        self._index = (self._index + 1) % len(self.slideshow.images)

    def prev(self):
        self._index = (self._index - 1) % len(self.slideshow.images)

    async def _run(self):
        while self._running:
            if self._paused or not self.slideshow.images:
                await asyncio.sleep_ms(100)
                continue
            path = self.slideshow.images[self._index]
            await self._show_slide(path)
            self._index = (self._index + 1) % len(self.slideshow.images)
            if not self.slideshow.config.get("playback", "repeat", True):
                break

    async def _show_slide(self, path):
        cfg = self.slideshow.config
        duration = float(cfg.get("playback", "slide_duration", 8.0))
        t_duration = float(cfg.get("playback", "transition_duration", 0.8))
        transition_name = cfg.get("playback", "transition", "fade")
        self._transition = self._make_transition(transition_name)
        self._transition.reset(self.slideshow.display.width,
                               self.slideshow.display.height)
        # Decode current image (from cache or disk).
        img = await self._decode_cached(path)
        if img is None:
            self.slideshow.stats.error_count += 1
            self.slideshow.log.error("decode failed: %s" % path)
            await asyncio.sleep_ms(500)
            return
        rgb565 = to_rgb565(img, cfg.get("image", "dither", "floyd_steinberg"),
                           cfg.get("hardware", "display", {}).get("rgb565_swap", False))
        # Show the image.
        self.slideshow.display.show_rgb565(rgb565)
        self.slideshow.stats.total_images += 1
        self.slideshow.stats.last_image = path
        start = time.ticks_ms()
        # Pre-decode next image in the background.
        next_path = self.slideshow.images[(self._index + 1) % len(self.slideshow.images)]
        if cfg.get("image", "preload_next", True):
            self._next_task = asyncio.get_event_loop().create_task(
                self._decode_cached(next_path))
        # Wait for the slide duration, feeding the watchdog.
        while time.ticks_diff(time.ticks_ms(), start) < int(duration * 1000):
            if self.slideshow.watchdog:
                self.slideshow.watchdog.feed()
            await asyncio.sleep_ms(50)
            if self._paused:
                while self._paused and self._running:
                    await asyncio.sleep_ms(100)
        # Transition to next image.
        if self._next_task:
            next_img = await self._next_task
            self._next_task = None
        else:
            next_img = await self._decode_cached(next_path)
        if next_img is not None:
            next_rgb = to_rgb565(next_img, cfg.get("image", "dither", "floyd_steinberg"),
                                 cfg.get("hardware", "display", {}).get("rgb565_swap", False))
            await self._run_transition(rgb565, next_rgb, t_duration)
        self.slideshow.stats.transitions += 1

    async def _run_transition(self, src, dst, duration):
        steps = max(1, int(duration * 60))
        w = self.slideshow.display.width
        h = self.slideshow.display.height
        for i in range(steps + 1):
            alpha = i / float(steps)
            out = bytearray(w * h * 2)
            self._transition.step(src, dst, alpha, w, h, 0, 0, w, h)
            self.slideshow.display.show_rgb565(out)
            await asyncio.sleep_ms(int(duration * 1000 / steps))

    def _make_transition(self, name):
        if name == "fade":
            return FadeTransition()
        if name == "slide":
            return SlideTransition()
        if name == "wipe":
            return WipeTransition()
        if name == "zoom":
            return ZoomTransition()
        return NoneTransition()

    async def _decode_cached(self, path):
        cached = self._cache.get(path)
        if cached is not None:
            self.slideshow.stats.cache_hits += 1
            return cached
        self.slideshow.stats.cache_misses += 1
        try:
            data = self.slideshow.sd.read(path)
            img = decode(data,
                         max_dimension=self.slideshow.config.get("image", "max_decode_dimension", 480),
                         fit=self.slideshow.config.get("image", "fit", "fit"),
                         dither=self.slideshow.config.get("image", "dither", "floyd_steinberg"))
            # Apply EXIF orientation.
            orient = parse_exif_orientation(data)
            if self.slideshow.config.get("playback", "auto_rotate", True) and orient != 1:
                img = apply_orientation(img, orient)
            self._cache.put(path, img)
            return img
        except Exception as exc:
            self.slideshow.log.error("decode %s: %s" % (path, exc))
            return None


# ---------------------------------------------------------------------------
# Main SlideshowMCP class
# ---------------------------------------------------------------------------


class SlideshowMCP:
    """Top-level slideshow MCP tool.

    This is the ``self.sdcard`` module/class.  It wires together the SD card,
    ST7796 display, backlight, buttons, IR receiver, web UI, MQTT client and
    async slideshow engine.

    Example::

        slideshow = SlideshowMCP()
        slideshow.init()
        slideshow.start_slideshow()
    """

    def __init__(self, config_path="/sd/slideshow.json"):
        self.config = Config(config_path, autoload=True)
        self.log = SlideshowLog(capacity=self.config.get("logging", "circular_buffer", 256),
                                timestamps=self.config.get("logging", "timestamps", True))
        self.stats = SlideshowStats()
        self.sd = None
        self.display = None
        self.backlight = None
        self.buttons = None
        self.ir = None
        self.engine = None
        self.webui = None
        self.websocket = None
        self.mqtt = None
        self.watchdog = None
        self.images = []
        self._simulator = False
        self._sdl = None
        self._running = False

    # -- init / hardware ----------------------------------------------------
    def init(self):
        """Initialise all hardware and start the engine."""
        self.log.info("SlideshowMCP init")
        self._init_sd()
        self._init_display()
        self._init_backlight()
        self._init_buttons()
        self._init_ir()
        self._init_watchdog()
        self._init_network()
        self.engine = SlideshowEngine(self)
        self._scan_images()
        self._resume()
        self._running = True
        self.log.info("SlideshowMCP ready (%d images)" % len(self.images))

    def _init_sd(self):
        hw = self.config.get("hardware", {})
        sd_cfg = hw.get("sd_spi", {})
        self.sd = SdCard(
            miso=sd_cfg.get("miso", 12),
            mosi=sd_cfg.get("mosi", 13),
            sck=sd_cfg.get("sck", 14),
            cs=sd_cfg.get("cs", 15),
            freq=sd_cfg.get("freq", 20_000_000),
            sdio_fallback=hw.get("sdio_fallback", True),
            mount_point=self.config.get("scan", "root", "/sd"),
        )
        if not self.sd.mount():
            self.log.warning("SD card mount failed")
            self.stats.sd_health = "failed"
        else:
            self.stats.sd_health = self.sd.health()
            self.log.info("SD card mounted at %s" % self.sd.mount_point)

    def _init_display(self):
        hw = self.config.get("hardware", {})
        disp = hw.get("display_spi", {})
        d = hw.get("display", {})
        self.display = St7796(
            mosi=disp.get("mosi", 11),
            sck=disp.get("sck", 10),
            dc=disp.get("dc", 9),
            cs=disp.get("cs", 8),
            rst=disp.get("rst", 7),
            bl=disp.get("bl", 6),
            freq=disp.get("freq", 80_000_000),
            width=d.get("width", 320),
            height=d.get("height", 480),
            rotation=d.get("rotation", 0),
            rgb565_swap=d.get("rgb565_swap", False),
        )
        if self.display._initialized:
            self.log.info("ST7796 display initialised %dx%d" %
                          (self.display.width, self.display.height))
        else:
            self.log.warning("ST7796 display init failed (simulator mode?)")

    def _init_backlight(self):
        hw = self.config.get("hardware", {})
        bl = hw.get("backlight", {})
        self.backlight = _Backlight(
            pin=bl.get("pin", 6) if "pin" in bl else hw.get("display_spi", {}).get("bl", 6),
            freq=bl.get("pwm_freq", 5000),
            gamma=bl.get("gamma", 2.2),
            default_brightness=bl.get("default_brightness", 220),
        )
        self.backlight.on()

    def _init_buttons(self):
        hw = self.config.get("hardware", {})
        btn = hw.get("buttons", {})
        self.buttons = _Buttons(boot_pin=btn.get("boot", 0),
                                user_pin=btn.get("user", 14))
        self.buttons.register("boot", self._on_boot_button)
        self.buttons.register("user", self._on_user_button)

    def _init_ir(self):
        hw = self.config.get("hardware", {})
        ir = hw.get("ir_receiver", {})
        self.ir = _IrDecoder(pin=ir.get("pin", 4), enabled=ir.get("enabled", False))
        self.ir.register(self._on_ir)

    def _init_watchdog(self):
        if esp32 is not None:
            try:
                self.watchdog = esp32.watchdog
            except Exception:
                self.watchdog = None

    def _init_network(self):
        net = self.config.get("network", {})
        web = net.get("web", {})
        if web.get("enabled", True):
            self.webui = WebUI(self, port=web.get("port", 80))
            self.websocket = WebSocketHandler(self, port=web.get("port", 80))
            self.webui.start()
            self.websocket.start()
            self.log.info("Web UI on port %d" % web.get("port", 80))
        mqtt = net.get("mqtt", {})
        if mqtt.get("enabled", False):
            self.mqtt = MqttClient(
                self,
                host=mqtt.get("host", ""),
                port=mqtt.get("port", 1883),
                client_id=mqtt.get("client_id", "xiaozhi-slideshow"),
            )
            self.mqtt.start()
            self.log.info("MQTT client enabled")

    # -- scanning / images --------------------------------------------------
    def _scan_images(self):
        """Scan the SD card and build the sorted image list."""
        scan = self.config.get("scan", {})
        root = scan.get("root", "/sd")
        if not self.sd or not self.sd.is_mounted():
            self.images = []
            return
        files = self.sd.list_files(
            root="",
            recursive=scan.get("recursive", True),
            max_depth=scan.get("max_depth", 6),
            max_files=scan.get("max_files", 2000),
            extensions=scan.get("extensions", [".jpg", ".jpeg", ".png", ".bmp", ".gif"]),
        )
        sort = scan.get("sort", "natural")
        if sort == "natural":
            files.sort(key=_natural_key)
        elif sort == "name":
            files.sort()
        elif sort == "mtime":
            files.sort(key=lambda p: _file_mtime(self.sd, p))
        elif sort == "exif":
            files.sort(key=lambda p: _exif_date_key(self.sd, p))
        if scan.get("shuffle", False):
            import urandom as random
            random.seed(time.ticks_ms())
            files = _shuffle(files)
        self.images = files
        self.log.info("Scanned %d images" % len(self.images))

    def rescan(self):
        """Re-scan the SD card (hot-swap / auto-reindex)."""
        self._scan_images()
        self.engine._index = 0

    def list_images(self):
        """Return the current image list."""
        return list(self.images)

    # -- playback control ---------------------------------------------------
    def start_slideshow(self):
        """Start the async slideshow engine."""
        if self.engine is None:
            self.engine = SlideshowEngine(self)
        self.engine.start()
        self._running = True
        self.log.info("Slideshow started")

    def stop_slideshow(self):
        """Stop the slideshow engine."""
        if self.engine:
            self.engine.stop()
        self._running = False
        self.log.info("Slideshow stopped")

    def pause(self, value=True):
        if self.engine:
            self.engine.pause(value)
        self.log.info("Slideshow %s" % ("paused" if value else "resumed"))

    def next(self):
        if self.engine:
            self.engine.next()
        self.log.info("Next image")

    def prev(self):
        if self.engine:
            self.engine.prev()
        self.log.info("Previous image")

    def set_slide_duration(self, seconds):
        self.config.set(float(seconds), "playback", "slide_duration")
        self.log.info("Slide duration %ss" % seconds)

    def set_transition(self, name):
        self.config.set(name, "playback", "transition")
        self.log.info("Transition %s" % name)

    def set_playing(self, playing):
        if playing:
            self.start_slideshow()
        else:
            self.stop_slideshow()

    # -- button / IR callbacks ---------------------------------------------
    def _on_boot_button(self):
        self.next()

    def _on_user_button(self):
        self.pause(not self.engine._paused if self.engine else True)

    def _on_ir(self, code):
        self.log.info("IR press: %s" % code)
        self.next()

    # -- status / JSON ------------------------------------------------------
    def status_json(self):
        return json.dumps({
            "running": self._running,
            "paused": self.engine._paused if self.engine else False,
            "index": self.engine._index if self.engine else 0,
            "total": len(self.images),
            "current": self.images[self.engine._index] if self.engine and self.images else None,
            "sd_health": self.stats.sd_health,
        })

    def list_json(self):
        return json.dumps(self.images)

    def playlists_json(self):
        return json.dumps(self.config.data.get("playlists", {}))

    def config_json(self):
        return json.dumps(self.config.data)

    def stats_json(self):
        return json.dumps(self.stats.snapshot())

    def test_pattern_json(self):
        if self.display:
            self.display.test_pattern()
        return json.dumps({"ok": True})

    def benchmark_json(self):
        return json.dumps({
            "free_mem": gc.mem_free() if hasattr(gc, "mem_free") else -1,
            "alloc_mem": gc.mem_alloc() if hasattr(gc, "mem_alloc") else -1,
            "images": len(self.images),
            "cache": {"hits": self.stats.cache_hits, "misses": self.stats.cache_misses},
        })

    # -- config hot-reload --------------------------------------------------
    def reload_config(self):
        changed = self.config.reload_if_changed()
        if changed:
            self.log.info("Config reloaded")
            self.engine._cache = LruCache(self.config.get("image", "cache_size", 2))
        return changed

    # -- simulator ----------------------------------------------------------
    def enable_simulator(self):
        """Enable SDL2 framebuffer simulator for host development."""
        self._simulator = True
        try:
            import sdl2
            self._sdl = sdl2
        except ImportError:
            self._sdl = None
            self.log.warning("SDL2 not available; simulator disabled")
        return self._sdl is not None

    # -- public tool surface (self.sdcard style) ----------------------------
    def show_image(self, path):
        """Show a single image fullscreen (blocking decode, non-blocking show)."""
        if not self.images:
            return False
        try:
            data = self.sd.read(path)
            img = decode(data,
                         max_dimension=self.config.get("image", "max_decode_dimension", 480),
                         fit=self.config.get("image", "fit", "fit"))
            orient = parse_exif_orientation(data)
            if self.config.get("playback", "auto_rotate", True) and orient != 1:
                img = apply_orientation(img, orient)
            rgb = to_rgb565(img, self.config.get("image", "dither", "floyd_steinberg"),
                            self.config.get("hardware", "display", {}).get("rgb565_swap", False))
            self.display.show_rgb565(rgb)
            return True
        except Exception as exc:
            self.log.error("show_image %s: %s" % (path, exc))
            return False

    def delete_file(self, path):
        """Delete a file from the SD card."""
        try:
            os.remove(self.sd.mount_point + "/" + path)
            self.rescan()
            return True
        except Exception:
            return False

    def read_image(self, path, max_bytes=8 * 1024 * 1024):
        """Read an image file (up to max_bytes) for AI analysis."""
        try:
            data = self.sd.read(path)
            return data[:max_bytes]
        except Exception:
            return None

    # -- run loop -----------------------------------------------------------
    async def run(self):
        """Main async loop: poll buttons/IR, feed watchdog, reload config."""
        while self._running:
            if self.buttons:
                self.buttons.poll()
            if self.ir:
                self.ir.poll()
            if self.watchdog:
                try:
                    self.watchdog.feed()
                except Exception:
                    pass
            self.reload_config()
            await asyncio.sleep_ms(50)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _natural_key(s):
    """Natural sort key (alphanumeric)."""
    import re
    return [int(t) if t.isdigit() else t.lower()
            for t in re.split(r"(\d+)", s)]


def _shuffle(items):
    import urandom as random
    result = list(items)
    for i in range(len(result) - 1, 0, -1):
        j = random.getrandbits(32) % (i + 1)
        result[i], result[j] = result[j], result[i]
    return result


def _file_mtime(sd, path):
    try:
        return os.stat(sd.mount_point + "/" + path)[8]
    except Exception:
        return 0


def _exif_date_key(sd, path):
    try:
        data = sd.read(path)[:4096]
        # Very rough EXIF date extraction; fall back to mtime.
        idx = data.find(b"DateTime")
        if idx != -1:
            return data[idx:idx + 20]
    except Exception:
        pass
    return _file_mtime(sd, path)


# Host entry point for simulator / testing.
if __name__ == "__main__":  # pragma: no cover
    app = SlideshowMCP()
    app.init()
    asyncio.get_event_loop().run_until_complete(app.run())

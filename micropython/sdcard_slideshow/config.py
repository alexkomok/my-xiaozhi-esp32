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
"""Configuration management for the SD-card slideshow.

This module owns the on-disk JSON schema, default values, validation, hot
reload, playlist handling and NVS-backed persistence of the last playback
position.  It is intentionally dependency free (only ``ujson``/``json`` and the
optional ``esp32`` NVS helpers) so it can be imported on the device and on a
host for unit testing.

The public surface is the :class:`Config` class plus the module level
:func:`validate` helper and :data:`DEFAULT_CONFIG` dictionary.
"""

try:
    import ujson as json
except ImportError:  # pragma: no cover - host fallback
    import json

try:
    from esp32 import NVS
except ImportError:  # pragma: no cover - host / simulator
    NVS = None

__all__ = ["Config", "DEFAULT_CONFIG", "validate", "ConfigError"]

# NVS namespace used for the resume position.  Changing this key is a breaking
# change for existing deployments (see AGENTS.md NVS policy).
_NVS_NAMESPACE = "slideshow"
_NVS_KEY = "resume"

# Supported values for enumerated options.  Keeping these in one place makes the
# schema self documenting and lets the web UI render dropdowns.
_TRANSITIONS = ("none", "fade", "slide", "wipe", "zoom")
_SLIDE_DIRECTIONS = ("left", "right", "up", "down")
_FIT_MODES = ("fit", "fill", "stretch")
_DITHER_MODES = ("none", "floyd_steinberg", "bayer")
_SORT_MODES = ("natural", "exif", "name", "mtime")
_THEMES = ("dark", "light")


class ConfigError(ValueError):
    """Raised when a configuration value fails validation."""


# ---------------------------------------------------------------------------
# Default configuration
# ---------------------------------------------------------------------------
# Every key is documented inline.  The web UI and README mirror this structure.
DEFAULT_CONFIG = {
    # -- Hardware pin map ---------------------------------------------------
    # SD card over SPI (HSPI).  ``sdio_fallback`` tries the 4-bit SDIO bus
    # first when the platform exposes ``machine.SDCard``.
    "hardware": {
        "sd_spi": {"miso": 12, "mosi": 13, "sck": 14, "cs": 15, "freq": 20_000_000},
        "sdio_fallback": True,
        "display_spi": {"mosi": 11, "sck": 10, "dc": 9, "cs": 8, "rst": 7, "bl": 6, "freq": 80_000_000},
        "display": {"width": 320, "height": 480, "rotation": 0, "rgb565_swap": False},
        "buttons": {"boot": 0, "user": 14},
        "ir_receiver": {"enabled": False, "pin": 4},
        "backlight": {"pwm_freq": 5000, "gamma": 2.2, "default_brightness": 220},
    },
    # -- Filesystem / scanning ---------------------------------------------
    "scan": {
        "root": "/sd",
        "recursive": True,
        "max_depth": 6,
        "max_files": 2000,
        "extensions": [".jpg", ".jpeg", ".png", ".bmp", ".gif"],
        "sort": "natural",
        "shuffle": False,
        "repeat": True,
    },
    # -- Playback timing ----------------------------------------------------
    "playback": {
        "slide_duration": 8.0,
        "transition_duration": 0.8,
        "transition": "fade",
        "ken_burns": False,
        "ken_burns_intensity": 0.08,
        "auto_rotate": True,
        "pause_on_start": False,
    },
    # -- Image processing ---------------------------------------------------
    "image": {
        "fit": "fit",
        "dither": "floyd_steinberg",
        "max_decode_dimension": 480,
        "streaming": True,
        "progressive_jpeg": True,
        "preload_next": True,
        "cache_size": 2,
        "low_memory_mode": False,
    },
    # -- Networking ---------------------------------------------------------
    "network": {
        "wifi_ap": {"enabled": False, "ssid": "Slideshow", "password": "slideshow", "channel": 6},
        "wifi_sta": {"enabled": False, "ssid": "", "password": ""},
        "web": {"enabled": True, "port": 80},
        "mqtt": {
            "enabled": False,
            "host": "",
            "port": 1883,
            "user": "",
            "password": "",
            "client_id": "xiaozhi-slideshow",
            "topic_prefix": "slideshow",
            "discovery": True,
        },
        "syslog": {"enabled": False, "host": "", "port": 514},
    },
    # -- Playlists ----------------------------------------------------------
    # Each playlist is a named filter over the scanned set.  ``paths`` is a
    # list of directory prefixes (relative to ``scan.root``); an empty list
    # means "all images".
    "playlists": {
        "default": {"paths": [], "transition": "fade", "slide_duration": 8.0},
    },
    "active_playlist": "default",
    # -- Logging / debug ----------------------------------------------------
    "logging": {"level": "INFO", "circular_buffer": 256, "timestamps": True},
    # -- Statistics ---------------------------------------------------------
    "statistics": {"enabled": True, "persist": True},
}


def _deep_merge(base, override):
    """Recursively merge ``override`` into ``base`` and return a new dict."""
    out = dict(base)
    for key, value in override.items():
        if key in out and isinstance(out[key], dict) and isinstance(value, dict):
            out[key] = _deep_merge(out[key], value)
        else:
            out[key] = value
    return out


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def _is_num(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _check_enum(value, allowed, name):
    if value not in allowed:
        raise ConfigError("%s must be one of %s, got %r" % (name, list(allowed), value))


def _check_range(value, low, high, name):
    if not _is_num(value) or not (low <= value <= high):
        raise ConfigError("%s must be a number in [%s, %s], got %r" % (name, low, high, value))


def validate(cfg):
    """Validate a configuration mapping in place.

    Args:
        cfg: Configuration dictionary (already merged with defaults).

    Raises:
        ConfigError: If any value is out of range or of the wrong type.
    """
    hw = cfg["hardware"]
    for bus in ("sd_spi", "display_spi"):
        for pin in ("miso", "mosi", "sck", "cs", "dc", "rst", "bl"):
            if pin in hw[bus] and not _is_int(hw[bus][pin]):
                raise ConfigError("hardware.%s.%s must be an int" % (bus, pin))
    _check_range(hw["display"]["width"], 1, 4096, "hardware.display.width")
    _check_range(hw["display"]["height"], 1, 4096, "hardware.display.height")
    _check_range(hw["backlight"]["default_brightness"], 0, 255, "hardware.backlight.default_brightness")
    _check_range(hw["backlight"]["gamma"], 0.5, 3.0, "hardware.backlight.gamma")

    scan = cfg["scan"]
    _check_enum(scan["sort"], _SORT_MODES, "scan.sort")
    _check_range(scan["max_depth"], 1, 32, "scan.max_depth")
    _check_range(scan["max_files"], 1, 100000, "scan.max_files")
    if not isinstance(scan["extensions"], list) or not scan["extensions"]:
        raise ConfigError("scan.extensions must be a non-empty list")

    pb = cfg["playback"]
    _check_range(pb["slide_duration"], 1.0, 300.0, "playback.slide_duration")
    _check_range(pb["transition_duration"], 0.0, 5.0, "playback.transition_duration")
    _check_enum(pb["transition"], _TRANSITIONS, "playback.transition")
    _check_range(pb["ken_burns_intensity"], 0.0, 0.5, "playback.ken_burns_intensity")

    img = cfg["image"]
    _check_enum(img["fit"], _FIT_MODES, "image.fit")
    _check_enum(img["dither"], _DITHER_MODES, "image.dither")
    _check_range(img["max_decode_dimension"], 32, 1024, "image.max_decode_dimension")
    _check_range(img["cache_size"], 0, 8, "image.cache_size")

    net = cfg["network"]
    _check_range(net["web"]["port"], 1, 65535, "network.web.port")
    _check_range(net["mqtt"]["port"], 1, 65535, "network.mqtt.port")

    if cfg["active_playlist"] not in cfg["playlists"]:
        raise ConfigError("active_playlist %r not defined in playlists" % cfg["active_playlist"])
    for name, pl in cfg["playlists"].items():
        if not isinstance(pl, dict):
            raise ConfigError("playlist %r must be an object" % name)
        if "transition" in pl:
            _check_enum(pl["transition"], _TRANSITIONS, "playlists.%s.transition" % name)
        if "slide_duration" in pl:
            _check_range(pl["slide_duration"], 1.0, 300.0, "playlists.%s.slide_duration" % name)


class Config:
    """Load, validate, persist and hot-reload the slideshow configuration.

    The configuration lives in a JSON file on the SD card (default
    ``/sd/slideshow.json``).  Missing keys are filled from
    :data:`DEFAULT_CONFIG`.  The last playback position is stored separately in
    NVS so it survives a config reload.

    Attributes:
        path: Filesystem path of the JSON config file.
        data: The merged, validated configuration dictionary.
    """

    def __init__(self, path="/sd/slideshow.json", autoload=True):
        self.path = path
        self.data = _deep_merge(DEFAULT_CONFIG, {})
        self._mtime = 0
        if autoload:
            self.load()

    # -- loading / saving ---------------------------------------------------
    def load(self):
        """Load and validate the config file, falling back to defaults."""
        try:
            with open(self.path, "r") as handle:
                raw = json.load(handle)
            self._mtime = _safe_mtime(self.path)
        except (OSError, ValueError) as exc:
            # Missing or corrupt file: keep defaults, remember the failure.
            self.data = _deep_merge(DEFAULT_CONFIG, {})
            self.last_error = str(exc)
            return False
        self.data = _deep_merge(DEFAULT_CONFIG, raw)
        validate(self.data)
        self.last_error = ""
        return True

    def save(self):
        """Persist the current configuration back to disk."""
        with open(self.path, "w") as handle:
            handle.write(json.dumps(self.data, indent=2))
        self._mtime = _safe_mtime(self.path)

    def reload_if_changed(self):
        """Hot-reload the file if its mtime changed.  Returns True on reload."""
        mtime = _safe_mtime(self.path)
        if mtime and mtime != self._mtime:
            return self.load()
        return False

    # -- typed accessors ----------------------------------------------------
    def get(self, *keys, default=None):
        """Return a nested value, e.g. ``cfg.get("playback", "slide_duration")``."""
        node = self.data
        for key in keys:
            if not isinstance(node, dict) or key not in node:
                return default
            node = node[key]
        return node

    def set(self, value, *keys):
        """Set a nested value and validate the whole document."""
        node = self.data
        for key in keys[:-1]:
            node = node.setdefault(key, {})
        node[keys[-1]] = value
        validate(self.data)
        return True

    # -- playlists ----------------------------------------------------------
    def playlist_names(self):
        """Return the list of configured playlist names."""
        return list(self.data["playlists"].keys())

    def active_playlist(self):
        """Return the active playlist definition (merged with defaults)."""
        name = self.data["active_playlist"]
        return self.data["playlists"].get(name, {})

    def set_active_playlist(self, name):
        """Switch the active playlist after validating it exists."""
        if name not in self.data["playlists"]:
            raise ConfigError("unknown playlist %r" % name)
        self.data["active_playlist"] = name
        return True

    # -- NVS resume position ------------------------------------------------
    def load_resume(self):
        """Return the persisted ``(index, timestamp, playlist)`` tuple or None."""
        if NVS is None:
            return None
        try:
            nvs = NVS(_NVS_NAMESPACE)
            blob = nvs.get_blob(_NVS_KEY)
            return json.loads(blob)
        except Exception:
            return None

    def save_resume(self, index, timestamp, playlist):
        """Persist the current playback position to NVS."""
        if NVS is None:
            return False
        try:
            nvs = NVS(_NVS_NAMESPACE)
            nvs.set_blob(_NVS_KEY, json.dumps({"index": index, "timestamp": timestamp, "playlist": playlist}))
            nvs.commit()
            return True
        except Exception:
            return False


def _safe_mtime(path):
    """Return file mtime, or 0 if it cannot be read (host/simulator safe)."""
    try:
        import os

        return os.stat(path)[8]
    except Exception:
        return 0
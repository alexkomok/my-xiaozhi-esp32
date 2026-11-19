// MIT License
//
// Copyright (c) 2026 XiaoZhi MicroPython Slideshow contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
/**
 * @file config.cc
 * @brief Configuration management implementation.
 */

#include "config.h"

#include <cJSON.h>
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace slideshow {

static const char* TAG = "SlideshowConfig";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string ReadFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return {};
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static bool WriteFile(const std::string& path, const std::string& content) {
    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }
    file << content;
    return file.good();
}

static std::string Trim(const std::string& str) {
    const size_t first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";
    const size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, last - first + 1);
}

static std::string ToLower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

TransitionType ParseTransition(const std::string& str) {
    const std::string lower = ToLower(str);
    if (lower == "none") return TransitionType::kNone;
    if (lower == "fade") return TransitionType::kFade;
    if (lower == "slide") return TransitionType::kSlide;
    if (lower == "wipe") return TransitionType::kWipe;
    if (lower == "zoom") return TransitionType::kZoom;
    return TransitionType::kFade;
}

std::string TransitionToString(TransitionType type) {
    switch (type) {
        case TransitionType::kNone: return "none";
        case TransitionType::kFade: return "fade";
        case TransitionType::kSlide: return "slide";
        case TransitionType::kWipe: return "wipe";
        case TransitionType::kZoom: return "zoom";
        default: return "fade";
    }
}

FitMode ParseFit(const std::string& str) {
    const std::string lower = ToLower(str);
    if (lower == "fit") return FitMode::kFit;
    if (lower == "fill") return FitMode::kFill;
    if (lower == "stretch") return FitMode::kStretch;
    return FitMode::kFit;
}

std::string FitToString(FitMode mode) {
    switch (mode) {
        case FitMode::kFit: return "fit";
        case FitMode::kFill: return "fill";
        case FitMode::kStretch: return "stretch";
        default: return "fit";
    }
}

DitherMode ParseDither(const std::string& str) {
    const std::string lower = ToLower(str);
    if (lower == "none") return DitherMode::kNone;
    if (lower == "bayer") return DitherMode::kBayer;
    if (lower == "floyd_steinberg" || lower == "floyd-steinberg") return DitherMode::kFloydSteinberg;
    return DitherMode::kFloydSteinberg;
}

std::string DitherToString(DitherMode mode) {
    switch (mode) {
        case DitherMode::kNone: return "none";
        case DitherMode::kBayer: return "bayer";
        case DitherMode::kFloydSteinberg: return "floyd_steinberg";
        default: return "floyd_steinberg";
    }
}

SortMode ParseSort(const std::string& str) {
    const std::string lower = ToLower(str);
    if (lower == "natural") return SortMode::kNatural;
    if (lower == "exif") return SortMode::kExif;
    if (lower == "name") return SortMode::kName;
    if (lower == "mtime") return SortMode::kMtime;
    return SortMode::kNatural;
}

std::string SortToString(SortMode mode) {
    switch (mode) {
        case SortMode::kNatural: return "natural";
        case SortMode::kExif: return "exif";
        case SortMode::kName: return "name";
        case SortMode::kMtime: return "mtime";
        default: return "natural";
    }
}

// ---------------------------------------------------------------------------
// Config implementation
// ---------------------------------------------------------------------------

void Config::ApplyDefaults() {
    // All defaults are set in the struct initializers.
    // This method is a placeholder for any runtime defaults that need
    // to be applied after parsing.
}

void Config::Load(const std::string& path, bool validate) {
    config_path_ = path;

    const std::string json_str = ReadFile(path);
    if (json_str.empty()) {
        ESP_LOGW(TAG, "Config file not found or empty: %s, using defaults", path.c_str());
        ApplyDefaults();
        return;
    }

    ParseJson(json_str, validate);

    // Update the stat cache.
    struct ::stat st = {};
    if (::stat(path.c_str(), &st) == 0) {
        last_stat_ = st;
    }
}

void Config::ParseJson(const std::string& json_str, bool validate) {
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse config JSON");
        if (validate) {
            // Exception handling is disabled on this target; report via log only.
            ESP_LOGE(TAG, "Invalid JSON in config file");
        }
        return;
    }

    // Hardware.
    cJSON* hw = cJSON_GetObjectItem(root, "hardware");
    if (hw != nullptr) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(hw, "sd_miso")) != nullptr) hw_.sd_miso = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "sd_mosi")) != nullptr) hw_.sd_mosi = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "sd_sck")) != nullptr) hw_.sd_sck = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "sd_cs")) != nullptr) hw_.sd_cs = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "sd_freq_hz")) != nullptr) hw_.sd_freq_hz = static_cast<uint32_t>(item->valueint);
        if ((item = cJSON_GetObjectItem(hw, "sdio_fallback")) != nullptr) hw_.sdio_fallback = item->valueint != 0;
        if ((item = cJSON_GetObjectItem(hw, "display_mosi")) != nullptr) hw_.display_mosi = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "display_sck")) != nullptr) hw_.display_sck = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "display_dc")) != nullptr) hw_.display_dc = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "display_cs")) != nullptr) hw_.display_cs = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "display_rst")) != nullptr) hw_.display_rst = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "display_bl")) != nullptr) hw_.display_bl = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "display_spi_freq_hz")) != nullptr) hw_.display_spi_freq_hz = static_cast<uint32_t>(item->valueint);
        if ((item = cJSON_GetObjectItem(hw, "display_width")) != nullptr) hw_.display_width = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "display_height")) != nullptr) hw_.display_height = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "display_rotation")) != nullptr) hw_.display_rotation = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "display_rgb565_swap")) != nullptr) hw_.display_rgb565_swap = item->valueint != 0;
        if ((item = cJSON_GetObjectItem(hw, "button_boot")) != nullptr) hw_.button_boot = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "button_user")) != nullptr) hw_.button_user = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "ir_enabled")) != nullptr) hw_.ir_enabled = item->valueint != 0;
        if ((item = cJSON_GetObjectItem(hw, "ir_pin")) != nullptr) hw_.ir_pin = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "backlight_pwm_freq")) != nullptr) hw_.backlight_pwm_freq = item->valueint;
        if ((item = cJSON_GetObjectItem(hw, "backlight_gamma")) != nullptr) hw_.backlight_gamma = static_cast<float>(item->valuedouble);
        if ((item = cJSON_GetObjectItem(hw, "backlight_default_brightness")) != nullptr) hw_.backlight_default_brightness = item->valueint;

        // Also support nested MicroPython schema: hardware.display.{width,height,rotation,rgb565_swap}
        cJSON* display = cJSON_GetObjectItem(hw, "display");
        if (display != nullptr) {
            if ((item = cJSON_GetObjectItem(display, "width")) != nullptr) hw_.display_width = item->valueint;
            if ((item = cJSON_GetObjectItem(display, "height")) != nullptr) hw_.display_height = item->valueint;
            if ((item = cJSON_GetObjectItem(display, "rotation")) != nullptr) hw_.display_rotation = item->valueint;
            if ((item = cJSON_GetObjectItem(display, "rgb565_swap")) != nullptr) hw_.display_rgb565_swap = item->valueint != 0;
        }
    }

    // Scan.
    cJSON* scan = cJSON_GetObjectItem(root, "scan");
    if (scan != nullptr) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(scan, "root")) != nullptr && cJSON_IsString(item)) scan_.root = item->valuestring;
        if ((item = cJSON_GetObjectItem(scan, "recursive")) != nullptr) scan_.recursive = item->valueint != 0;
        if ((item = cJSON_GetObjectItem(scan, "max_depth")) != nullptr) scan_.max_depth = item->valueint;
        if ((item = cJSON_GetObjectItem(scan, "max_files")) != nullptr) scan_.max_files = item->valueint;
        if ((item = cJSON_GetObjectItem(scan, "sort")) != nullptr && cJSON_IsString(item)) scan_.sort = ParseSort(item->valuestring);
        if ((item = cJSON_GetObjectItem(scan, "shuffle")) != nullptr) scan_.shuffle = item->valueint != 0;
        if ((item = cJSON_GetObjectItem(scan, "repeat")) != nullptr) scan_.repeat = item->valueint != 0;

        cJSON* exts = cJSON_GetObjectItem(scan, "extensions");
        if (exts != nullptr && cJSON_IsArray(exts)) {
            scan_.extensions.clear();
            cJSON* ext;
            cJSON_ArrayForEach(ext, exts) {
                if (cJSON_IsString(ext)) {
                    scan_.extensions.push_back(ext->valuestring);
                }
            }
        }
    }

    // Playback.
    cJSON* playback = cJSON_GetObjectItem(root, "playback");
    if (playback != nullptr) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(playback, "slide_duration")) != nullptr) playback_.slide_duration = static_cast<float>(item->valuedouble);
        if ((item = cJSON_GetObjectItem(playback, "transition_duration")) != nullptr) playback_.transition_duration = static_cast<float>(item->valuedouble);
        if ((item = cJSON_GetObjectItem(playback, "transition")) != nullptr && cJSON_IsString(item)) playback_.transition = ParseTransition(item->valuestring);
        if ((item = cJSON_GetObjectItem(playback, "ken_burns")) != nullptr) playback_.ken_burns = item->valueint != 0;
        if ((item = cJSON_GetObjectItem(playback, "ken_burns_intensity")) != nullptr) playback_.ken_burns_intensity = static_cast<float>(item->valuedouble);
        if ((item = cJSON_GetObjectItem(playback, "auto_rotate")) != nullptr) playback_.auto_rotate = item->valueint != 0;
        if ((item = cJSON_GetObjectItem(playback, "pause_on_start")) != nullptr) playback_.pause_on_start = item->valueint != 0;
    }

    // Image.
    cJSON* image = cJSON_GetObjectItem(root, "image");
    if (image != nullptr) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(image, "fit")) != nullptr && cJSON_IsString(item)) image_.fit = ParseFit(item->valuestring);
        if ((item = cJSON_GetObjectItem(image, "dither")) != nullptr && cJSON_IsString(item)) image_.dither_mode = ParseDither(item->valuestring);
        if ((item = cJSON_GetObjectItem(image, "max_decode_dimension")) != nullptr) image_.max_decode_dimension = item->valueint;
        if ((item = cJSON_GetObjectItem(image, "streaming")) != nullptr) image_.streaming = item->valueint != 0;
        if ((item = cJSON_GetObjectItem(image, "progressive_jpeg")) != nullptr) image_.progressive_jpeg = item->valueint != 0;
        if ((item = cJSON_GetObjectItem(image, "preload_next")) != nullptr) image_.preload_next = item->valueint != 0;
        if ((item = cJSON_GetObjectItem(image, "cache_size")) != nullptr) image_.cache_size = item->valueint;
        if ((item = cJSON_GetObjectItem(image, "low_memory_mode")) != nullptr) image_.low_memory_mode = item->valueint != 0;
    }

    // Network.
    cJSON* network = cJSON_GetObjectItem(root, "network");
    if (network != nullptr) {
        cJSON* item;
        cJSON* wifi_ap = cJSON_GetObjectItem(network, "wifi_ap");
        if (wifi_ap != nullptr) {
            if ((item = cJSON_GetObjectItem(wifi_ap, "enabled")) != nullptr) network_.wifi_ap.enabled = item->valueint != 0;
            if ((item = cJSON_GetObjectItem(wifi_ap, "ssid")) != nullptr && cJSON_IsString(item)) network_.wifi_ap.ssid = item->valuestring;
            if ((item = cJSON_GetObjectItem(wifi_ap, "password")) != nullptr && cJSON_IsString(item)) network_.wifi_ap.password = item->valuestring;
            if ((item = cJSON_GetObjectItem(wifi_ap, "channel")) != nullptr) network_.wifi_ap.channel = item->valueint;
        }
        cJSON* wifi_sta = cJSON_GetObjectItem(network, "wifi_sta");
        if (wifi_sta != nullptr) {
            if ((item = cJSON_GetObjectItem(wifi_sta, "enabled")) != nullptr) network_.wifi_sta.enabled = item->valueint != 0;
            if ((item = cJSON_GetObjectItem(wifi_sta, "ssid")) != nullptr && cJSON_IsString(item)) network_.wifi_sta.ssid = item->valuestring;
            if ((item = cJSON_GetObjectItem(wifi_sta, "password")) != nullptr && cJSON_IsString(item)) network_.wifi_sta.password = item->valuestring;
        }
        cJSON* web = cJSON_GetObjectItem(network, "web");
        if (web != nullptr) {
            if ((item = cJSON_GetObjectItem(web, "enabled")) != nullptr) network_.web.enabled = item->valueint != 0;
            if ((item = cJSON_GetObjectItem(web, "port")) != nullptr) network_.web.port = item->valueint;
        }
        cJSON* mqtt = cJSON_GetObjectItem(network, "mqtt");
        if (mqtt != nullptr) {
            if ((item = cJSON_GetObjectItem(mqtt, "enabled")) != nullptr) network_.mqtt.enabled = item->valueint != 0;
            if ((item = cJSON_GetObjectItem(mqtt, "host")) != nullptr && cJSON_IsString(item)) network_.mqtt.host = item->valuestring;
            if ((item = cJSON_GetObjectItem(mqtt, "port")) != nullptr) network_.mqtt.port = item->valueint;
            if ((item = cJSON_GetObjectItem(mqtt, "user")) != nullptr && cJSON_IsString(item)) network_.mqtt.user = item->valuestring;
            if ((item = cJSON_GetObjectItem(mqtt, "password")) != nullptr && cJSON_IsString(item)) network_.mqtt.password = item->valuestring;
            if ((item = cJSON_GetObjectItem(mqtt, "client_id")) != nullptr && cJSON_IsString(item)) network_.mqtt.client_id = item->valuestring;
            if ((item = cJSON_GetObjectItem(mqtt, "topic_prefix")) != nullptr && cJSON_IsString(item)) network_.mqtt.topic_prefix = item->valuestring;
            if ((item = cJSON_GetObjectItem(mqtt, "discovery")) != nullptr) network_.mqtt.discovery = item->valueint != 0;
        }
        cJSON* syslog = cJSON_GetObjectItem(network, "syslog");
        if (syslog != nullptr) {
            if ((item = cJSON_GetObjectItem(syslog, "enabled")) != nullptr) network_.syslog.enabled = item->valueint != 0;
            if ((item = cJSON_GetObjectItem(syslog, "host")) != nullptr && cJSON_IsString(item)) network_.syslog.host = item->valuestring;
            if ((item = cJSON_GetObjectItem(syslog, "port")) != nullptr) network_.syslog.port = item->valueint;
        }
    }

    // Playlists.
    cJSON* playlists = cJSON_GetObjectItem(root, "playlists");
    if (playlists != nullptr && cJSON_IsObject(playlists)) {
        cJSON* playlist;
        cJSON_ArrayForEach(playlist, playlists) {
            if (cJSON_IsString(playlist)) {
                const std::string name = playlist->valuestring;
                cJSON* paths = cJSON_GetObjectItem(playlists, name.c_str());
                if (paths != nullptr && cJSON_IsArray(paths)) {
                    PlaylistConfig config;
                    cJSON* path;
                    cJSON_ArrayForEach(path, paths) {
                        if (cJSON_IsString(path)) {
                            config.paths.push_back(path->valuestring);
                        }
                    }
                    playlists_[name] = config;
                }
            }
        }
    }

    // Active playlist.
    cJSON* active = cJSON_GetObjectItem(root, "active_playlist");
    if (active != nullptr && cJSON_IsString(active)) {
        active_playlist_ = active->valuestring;
    }

    // Logging / statistics.
    cJSON* logging = cJSON_GetObjectItem(root, "logging");
    if (logging != nullptr) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(logging, "level")) != nullptr && cJSON_IsString(item)) log_level_ = item->valuestring;
        if ((item = cJSON_GetObjectItem(logging, "circular_buffer")) != nullptr) circular_buffer_ = item->valueint;
        if ((item = cJSON_GetObjectItem(logging, "timestamps")) != nullptr) log_timestamps_ = item->valueint != 0;
    }
    cJSON* stats = cJSON_GetObjectItem(root, "statistics");
    if (stats != nullptr) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(stats, "enabled")) != nullptr) statistics_enabled_ = item->valueint != 0;
        if ((item = cJSON_GetObjectItem(stats, "persist")) != nullptr) statistics_persist_ = item->valueint != 0;
    }

    cJSON_Delete(root);

    if (validate) {
        Validate();
    }
}

void Config::Save(const std::string& path) const {
    cJSON* root = cJSON_CreateObject();

    // Hardware.
    cJSON* hw = cJSON_AddObjectToObject(root, "hardware");
    cJSON_AddNumberToObject(hw, "sd_miso", hw_.sd_miso);
    cJSON_AddNumberToObject(hw, "sd_mosi", hw_.sd_mosi);
    cJSON_AddNumberToObject(hw, "sd_sck", hw_.sd_sck);
    cJSON_AddNumberToObject(hw, "sd_cs", hw_.sd_cs);
    cJSON_AddNumberToObject(hw, "sd_freq_hz", hw_.sd_freq_hz);
    cJSON_AddBoolToObject(hw, "sdio_fallback", hw_.sdio_fallback);
    cJSON_AddNumberToObject(hw, "display_mosi", hw_.display_mosi);
    cJSON_AddNumberToObject(hw, "display_sck", hw_.display_sck);
    cJSON_AddNumberToObject(hw, "display_dc", hw_.display_dc);
    cJSON_AddNumberToObject(hw, "display_cs", hw_.display_cs);
    cJSON_AddNumberToObject(hw, "display_rst", hw_.display_rst);
    cJSON_AddNumberToObject(hw, "display_bl", hw_.display_bl);
    cJSON_AddNumberToObject(hw, "display_spi_freq_hz", hw_.display_spi_freq_hz);
    cJSON_AddNumberToObject(hw, "display_width", hw_.display_width);
    cJSON_AddNumberToObject(hw, "display_height", hw_.display_height);
    cJSON_AddNumberToObject(hw, "display_rotation", hw_.display_rotation);
    cJSON_AddBoolToObject(hw, "display_rgb565_swap", hw_.display_rgb565_swap);
    cJSON_AddNumberToObject(hw, "button_boot", hw_.button_boot);
    cJSON_AddNumberToObject(hw, "button_user", hw_.button_user);
    cJSON_AddBoolToObject(hw, "ir_enabled", hw_.ir_enabled);
    cJSON_AddNumberToObject(hw, "ir_pin", hw_.ir_pin);
    cJSON_AddNumberToObject(hw, "backlight_pwm_freq", hw_.backlight_pwm_freq);
    cJSON_AddNumberToObject(hw, "backlight_gamma", hw_.backlight_gamma);
    cJSON_AddNumberToObject(hw, "backlight_default_brightness", hw_.backlight_default_brightness);

    // Scan.
    cJSON* scan = cJSON_AddObjectToObject(root, "scan");
    cJSON_AddStringToObject(scan, "root", scan_.root.c_str());
    cJSON_AddBoolToObject(scan, "recursive", scan_.recursive);
    cJSON_AddNumberToObject(scan, "max_depth", scan_.max_depth);
    cJSON_AddNumberToObject(scan, "max_files", scan_.max_files);
    cJSON_AddStringToObject(scan, "sort", SortToString(scan_.sort).c_str());
    cJSON_AddBoolToObject(scan, "shuffle", scan_.shuffle);
    cJSON_AddBoolToObject(scan, "repeat", scan_.repeat);
    cJSON* exts = cJSON_AddArrayToObject(scan, "extensions");
    for (const auto& ext : scan_.extensions) {
        cJSON_AddItemToArray(exts, cJSON_CreateString(ext.c_str()));
    }

    // Playback.
    cJSON* playback = cJSON_AddObjectToObject(root, "playback");
    cJSON_AddNumberToObject(playback, "slide_duration", playback_.slide_duration);
    cJSON_AddNumberToObject(playback, "transition_duration", playback_.transition_duration);
    cJSON_AddStringToObject(playback, "transition", TransitionToString(playback_.transition).c_str());
    cJSON_AddBoolToObject(playback, "ken_burns", playback_.ken_burns);
    cJSON_AddNumberToObject(playback, "ken_burns_intensity", playback_.ken_burns_intensity);
    cJSON_AddBoolToObject(playback, "auto_rotate", playback_.auto_rotate);
    cJSON_AddBoolToObject(playback, "pause_on_start", playback_.pause_on_start);

    // Image.
    cJSON* image = cJSON_AddObjectToObject(root, "image");
    cJSON_AddStringToObject(image, "fit", FitToString(image_.fit).c_str());
    cJSON_AddStringToObject(image, "dither", DitherToString(image_.dither_mode).c_str());
    cJSON_AddNumberToObject(image, "max_decode_dimension", image_.max_decode_dimension);
    cJSON_AddBoolToObject(image, "streaming", image_.streaming);
    cJSON_AddBoolToObject(image, "progressive_jpeg", image_.progressive_jpeg);
    cJSON_AddBoolToObject(image, "preload_next", image_.preload_next);
    cJSON_AddNumberToObject(image, "cache_size", image_.cache_size);
    cJSON_AddBoolToObject(image, "low_memory_mode", image_.low_memory_mode);

    // Network.
    cJSON* network = cJSON_AddObjectToObject(root, "network");
    cJSON* wifi_ap = cJSON_AddObjectToObject(network, "wifi_ap");
    cJSON_AddBoolToObject(wifi_ap, "enabled", network_.wifi_ap.enabled);
    cJSON_AddStringToObject(wifi_ap, "ssid", network_.wifi_ap.ssid.c_str());
    cJSON_AddStringToObject(wifi_ap, "password", network_.wifi_ap.password.c_str());
    cJSON_AddNumberToObject(wifi_ap, "channel", network_.wifi_ap.channel);
    cJSON* wifi_sta = cJSON_AddObjectToObject(network, "wifi_sta");
    cJSON_AddBoolToObject(wifi_sta, "enabled", network_.wifi_sta.enabled);
    cJSON_AddStringToObject(wifi_sta, "ssid", network_.wifi_sta.ssid.c_str());
    cJSON_AddStringToObject(wifi_sta, "password", network_.wifi_sta.password.c_str());
    cJSON* web = cJSON_AddObjectToObject(network, "web");
    cJSON_AddBoolToObject(web, "enabled", network_.web.enabled);
    cJSON_AddNumberToObject(web, "port", network_.web.port);
    cJSON* mqtt = cJSON_AddObjectToObject(network, "mqtt");
    cJSON_AddBoolToObject(mqtt, "enabled", network_.mqtt.enabled);
    cJSON_AddStringToObject(mqtt, "host", network_.mqtt.host.c_str());
    cJSON_AddNumberToObject(mqtt, "port", network_.mqtt.port);
    cJSON_AddStringToObject(mqtt, "user", network_.mqtt.user.c_str());
    cJSON_AddStringToObject(mqtt, "password", network_.mqtt.password.c_str());
    cJSON_AddStringToObject(mqtt, "client_id", network_.mqtt.client_id.c_str());
    cJSON_AddStringToObject(mqtt, "topic_prefix", network_.mqtt.topic_prefix.c_str());
    cJSON_AddBoolToObject(mqtt, "discovery", network_.mqtt.discovery);
    cJSON* syslog = cJSON_AddObjectToObject(network, "syslog");
    cJSON_AddBoolToObject(syslog, "enabled", network_.syslog.enabled);
    cJSON_AddStringToObject(syslog, "host", network_.syslog.host.c_str());
    cJSON_AddNumberToObject(syslog, "port", network_.syslog.port);

    // Playlists.
    cJSON* playlists = cJSON_AddObjectToObject(root, "playlists");
    for (const auto& [name, config] : playlists_) {
        cJSON* paths = cJSON_AddArrayToObject(playlists, name.c_str());
        for (const auto& path : config.paths) {
            cJSON_AddItemToArray(paths, cJSON_CreateString(path.c_str()));
        }
    }

    // Active playlist.
    cJSON_AddStringToObject(root, "active_playlist", active_playlist_.c_str());

    // Logging / statistics.
    cJSON* logging = cJSON_AddObjectToObject(root, "logging");
    cJSON_AddStringToObject(logging, "level", log_level_.c_str());
    cJSON_AddNumberToObject(logging, "circular_buffer", circular_buffer_);
    cJSON_AddBoolToObject(logging, "timestamps", log_timestamps_);
    cJSON* stats = cJSON_AddObjectToObject(root, "statistics");
    cJSON_AddBoolToObject(stats, "enabled", statistics_enabled_);
    cJSON_AddBoolToObject(stats, "persist", statistics_persist_);

    const char* json_str = cJSON_Print(root);
    if (json_str != nullptr) {
        WriteFile(path, json_str);
        cJSON_free((void*)json_str);
    }
    cJSON_Delete(root);
}

bool Config::NeedsReload() const {
    struct ::stat st = {};
    if (::stat(config_path_.c_str(), &st) != 0) {
        return false;
    }
    return st.st_mtime != last_stat_.st_mtime;
}

void Config::ReloadIfChanged() {
    if (NeedsReload()) {
        ESP_LOGI(TAG, "Config file changed, reloading: %s", config_path_.c_str());
        Load(config_path_, true);
    }
}

std::string Config::ToJson() const {
    cJSON* root = cJSON_CreateObject();

    // Hardware.
    cJSON* hw = cJSON_AddObjectToObject(root, "hardware");
    cJSON_AddNumberToObject(hw, "sd_miso", hw_.sd_miso);
    cJSON_AddNumberToObject(hw, "sd_mosi", hw_.sd_mosi);
    cJSON_AddNumberToObject(hw, "sd_sck", hw_.sd_sck);
    cJSON_AddNumberToObject(hw, "sd_cs", hw_.sd_cs);
    cJSON_AddNumberToObject(hw, "sd_freq_hz", hw_.sd_freq_hz);
    cJSON_AddBoolToObject(hw, "sdio_fallback", hw_.sdio_fallback);
    cJSON_AddNumberToObject(hw, "display_mosi", hw_.display_mosi);
    cJSON_AddNumberToObject(hw, "display_sck", hw_.display_sck);
    cJSON_AddNumberToObject(hw, "display_dc", hw_.display_dc);
    cJSON_AddNumberToObject(hw, "display_cs", hw_.display_cs);
    cJSON_AddNumberToObject(hw, "display_rst", hw_.display_rst);
    cJSON_AddNumberToObject(hw, "display_bl", hw_.display_bl);
    cJSON_AddNumberToObject(hw, "display_spi_freq_hz", hw_.display_spi_freq_hz);
    cJSON_AddNumberToObject(hw, "display_width", hw_.display_width);
    cJSON_AddNumberToObject(hw, "display_height", hw_.display_height);
    cJSON_AddNumberToObject(hw, "display_rotation", hw_.display_rotation);
    cJSON_AddBoolToObject(hw, "display_rgb565_swap", hw_.display_rgb565_swap);
    cJSON_AddNumberToObject(hw, "button_boot", hw_.button_boot);
    cJSON_AddNumberToObject(hw, "button_user", hw_.button_user);
    cJSON_AddBoolToObject(hw, "ir_enabled", hw_.ir_enabled);
    cJSON_AddNumberToObject(hw, "ir_pin", hw_.ir_pin);
    cJSON_AddNumberToObject(hw, "backlight_pwm_freq", hw_.backlight_pwm_freq);
    cJSON_AddNumberToObject(hw, "backlight_gamma", hw_.backlight_gamma);
    cJSON_AddNumberToObject(hw, "backlight_default_brightness", hw_.backlight_default_brightness);

    // Scan.
    cJSON* scan = cJSON_AddObjectToObject(root, "scan");
    cJSON_AddStringToObject(scan, "root", scan_.root.c_str());
    cJSON_AddBoolToObject(scan, "recursive", scan_.recursive);
    cJSON_AddNumberToObject(scan, "max_depth", scan_.max_depth);
    cJSON_AddNumberToObject(scan, "max_files", scan_.max_files);
    cJSON_AddStringToObject(scan, "sort", SortToString(scan_.sort).c_str());
    cJSON_AddBoolToObject(scan, "shuffle", scan_.shuffle);
    cJSON_AddBoolToObject(scan, "repeat", scan_.repeat);
    cJSON* exts = cJSON_AddArrayToObject(scan, "extensions");
    for (const auto& ext : scan_.extensions) {
        cJSON_AddItemToArray(exts, cJSON_CreateString(ext.c_str()));
    }

    // Playback.
    cJSON* playback = cJSON_AddObjectToObject(root, "playback");
    cJSON_AddNumberToObject(playback, "slide_duration", playback_.slide_duration);
    cJSON_AddNumberToObject(playback, "transition_duration", playback_.transition_duration);
    cJSON_AddStringToObject(playback, "transition", TransitionToString(playback_.transition).c_str());
    cJSON_AddBoolToObject(playback, "ken_burns", playback_.ken_burns);
    cJSON_AddNumberToObject(playback, "ken_burns_intensity", playback_.ken_burns_intensity);
    cJSON_AddBoolToObject(playback, "auto_rotate", playback_.auto_rotate);
    cJSON_AddBoolToObject(playback, "pause_on_start", playback_.pause_on_start);

    // Image.
    cJSON* image = cJSON_AddObjectToObject(root, "image");
    cJSON_AddStringToObject(image, "fit", FitToString(image_.fit).c_str());
    cJSON_AddStringToObject(image, "dither", DitherToString(image_.dither_mode).c_str());
    cJSON_AddNumberToObject(image, "max_decode_dimension", image_.max_decode_dimension);
    cJSON_AddBoolToObject(image, "streaming", image_.streaming);
    cJSON_AddBoolToObject(image, "progressive_jpeg", image_.progressive_jpeg);
    cJSON_AddBoolToObject(image, "preload_next", image_.preload_next);
    cJSON_AddNumberToObject(image, "cache_size", image_.cache_size);
    cJSON_AddBoolToObject(image, "low_memory_mode", image_.low_memory_mode);

    // Network.
    cJSON* network = cJSON_AddObjectToObject(root, "network");
    cJSON* wifi_ap = cJSON_AddObjectToObject(network, "wifi_ap");
    cJSON_AddBoolToObject(wifi_ap, "enabled", network_.wifi_ap.enabled);
    cJSON_AddStringToObject(wifi_ap, "ssid", network_.wifi_ap.ssid.c_str());
    cJSON_AddStringToObject(wifi_ap, "password", network_.wifi_ap.password.c_str());
    cJSON_AddNumberToObject(wifi_ap, "channel", network_.wifi_ap.channel);
    cJSON* wifi_sta = cJSON_AddObjectToObject(network, "wifi_sta");
    cJSON_AddBoolToObject(wifi_sta, "enabled", network_.wifi_sta.enabled);
    cJSON_AddStringToObject(wifi_sta, "ssid", network_.wifi_sta.ssid.c_str());
    cJSON_AddStringToObject(wifi_sta, "password", network_.wifi_sta.password.c_str());
    cJSON* web = cJSON_AddObjectToObject(network, "web");
    cJSON_AddBoolToObject(web, "enabled", network_.web.enabled);
    cJSON_AddNumberToObject(web, "port", network_.web.port);
    cJSON* mqtt = cJSON_AddObjectToObject(network, "mqtt");
    cJSON_AddBoolToObject(mqtt, "enabled", network_.mqtt.enabled);
    cJSON_AddStringToObject(mqtt, "host", network_.mqtt.host.c_str());
    cJSON_AddNumberToObject(mqtt, "port", network_.mqtt.port);
    cJSON_AddStringToObject(mqtt, "user", network_.mqtt.user.c_str());
    cJSON_AddStringToObject(mqtt, "password", network_.mqtt.password.c_str());
    cJSON_AddStringToObject(mqtt, "client_id", network_.mqtt.client_id.c_str());
    cJSON_AddStringToObject(mqtt, "topic_prefix", network_.mqtt.topic_prefix.c_str());
    cJSON_AddBoolToObject(mqtt, "discovery", network_.mqtt.discovery);
    cJSON* syslog = cJSON_AddObjectToObject(network, "syslog");
    cJSON_AddBoolToObject(syslog, "enabled", network_.syslog.enabled);
    cJSON_AddStringToObject(syslog, "host", network_.syslog.host.c_str());
    cJSON_AddNumberToObject(syslog, "port", network_.syslog.port);

    // Playlists.
    cJSON* playlists = cJSON_AddObjectToObject(root, "playlists");
    for (const auto& [name, config] : playlists_) {
        cJSON* paths = cJSON_AddArrayToObject(playlists, name.c_str());
        for (const auto& path : config.paths) {
            cJSON_AddItemToArray(paths, cJSON_CreateString(path.c_str()));
        }
    }

    // Active playlist.
    cJSON_AddStringToObject(root, "active_playlist", active_playlist_.c_str());

    // Logging / statistics.
    cJSON* logging = cJSON_AddObjectToObject(root, "logging");
    cJSON_AddStringToObject(logging, "level", log_level_.c_str());
    cJSON_AddNumberToObject(logging, "circular_buffer", circular_buffer_);
    cJSON_AddBoolToObject(logging, "timestamps", log_timestamps_);
    cJSON* stats = cJSON_AddObjectToObject(root, "statistics");
    cJSON_AddBoolToObject(stats, "enabled", statistics_enabled_);
    cJSON_AddBoolToObject(stats, "persist", statistics_persist_);

    const char* json_str = cJSON_Print(root);
    std::string result = (json_str != nullptr) ? json_str : "";
    if (json_str != nullptr) {
        cJSON_free((void*)json_str);
    }
    cJSON_Delete(root);
    return result;
}

void Config::Validate() const {
    validation_error_.clear();

    // Validate hardware.
    if (hw_.display_width <= 0 || hw_.display_width > 4096) {
        validation_error_ = "display_width must be between 1 and 4096";
        ESP_LOGE(TAG, "%s", validation_error_.c_str());
        return;
    }
    if (hw_.display_height <= 0 || hw_.display_height > 4096) {
        validation_error_ = "display_height must be between 1 and 4096";
        ESP_LOGE(TAG, "%s", validation_error_.c_str());
        return;
    }
    if (hw_.display_rotation < 0 || hw_.display_rotation > 3) {
        validation_error_ = "display_rotation must be 0-3";
        ESP_LOGE(TAG, "%s", validation_error_.c_str());
        return;
    }
    if (hw_.backlight_pwm_freq < 100 || hw_.backlight_pwm_freq > 100000) {
        validation_error_ = "backlight_pwm_freq must be between 100 and 100000";
        ESP_LOGE(TAG, "%s", validation_error_.c_str());
        return;
    }
    if (hw_.backlight_gamma < 0.1f || hw_.backlight_gamma > 5.0f) {
        validation_error_ = "backlight_gamma must be between 0.1 and 5.0";
        ESP_LOGE(TAG, "%s", validation_error_.c_str());
        return;
    }
    if (hw_.backlight_default_brightness < 0 || hw_.backlight_default_brightness > 255) {
        validation_error_ = "backlight_default_brightness must be between 0 and 255";
        ESP_LOGE(TAG, "%s", validation_error_.c_str());
        return;
    }

    // Validate scan.
    if (scan_.max_depth < 0 || scan_.max_depth > 20) {
        validation_error_ = "max_depth must be between 0 and 20";
        return;
    }
    if (scan_.max_files < 1 || scan_.max_files > 10000) {
        validation_error_ = "max_files must be between 1 and 10000";
        return;
    }

    // Validate playback.
    if (playback_.slide_duration < 1.0f || playback_.slide_duration > 300.0f) {
        validation_error_ = "slide_duration must be between 1.0 and 300.0 seconds";
        return;
    }
    if (playback_.transition_duration < 0.0f || playback_.transition_duration > 10.0f) {
        validation_error_ = "transition_duration must be between 0.0 and 10.0 seconds";
        return;
    }
    if (playback_.ken_burns_intensity < 0.0f || playback_.ken_burns_intensity > 1.0f) {
        validation_error_ = "ken_burns_intensity must be between 0.0 and 1.0";
        return;
    }

    // Validate image.
    if (image_.max_decode_dimension < 64 || image_.max_decode_dimension > 4096) {
        validation_error_ = "max_decode_dimension must be between 64 and 4096";
        return;
    }
    if (image_.cache_size < 0 || image_.cache_size > 10) {
        validation_error_ = "cache_size must be between 0 and 10";
        return;
    }

    // Validate network.
    if (network_.web.port < 1 || network_.web.port > 65535) {
        validation_error_ = "web.port must be between 1 and 65535";
        return;
    }
    if (network_.mqtt.port < 1 || network_.mqtt.port > 65535) {
        validation_error_ = "mqtt.port must be between 1 and 65535";
        return;
    }
    if (network_.syslog.port < 1 || network_.syslog.port > 65535) {
        validation_error_ = "syslog.port must be between 1 and 65535";
        return;
    }

    // Validate logging.
    if (circular_buffer_ < 16 || circular_buffer_ > 4096) {
        validation_error_ = "circular_buffer must be between 16 and 4096";
        return;
    }
}

// ---------------------------------------------------------------------------
// NVS persistence
// ---------------------------------------------------------------------------

void Config::LoadResumePosition(int& out_index, std::string& out_playlist) const {
    nvs_handle_t handle;
    if (nvs_open(NvsNamespace().c_str(), NVS_READONLY, &handle) != ESP_OK) {
        out_index = -1;
        out_playlist = "default";
        return;
    }

    size_t length = 0;
    if (nvs_get_str(handle, ResumeKey().c_str(), nullptr, &length) == ESP_OK && length > 0) {
        std::vector<char> buffer(length);
        if (nvs_get_str(handle, ResumeKey().c_str(), buffer.data(), &length) == ESP_OK) {
            // Parse "index:playlist" format.
            const std::string value(buffer.data());
            const size_t colon = value.find(':');
            if (colon != std::string::npos) {
                out_index = std::stoi(value.substr(0, colon));
                out_playlist = value.substr(colon + 1);
            } else {
                out_index = std::stoi(value);
                out_playlist = "default";
            }
        }
    } else {
        out_index = -1;
        out_playlist = "default";
    }

    nvs_close(handle);
}

void Config::SaveResumePosition(int index, const std::string& playlist) const {
    nvs_handle_t handle;
    if (nvs_open(NvsNamespace().c_str(), NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for resume position");
        return;
    }

    const std::string value = std::to_string(index) + ":" + playlist;
    if (nvs_set_str(handle, ResumeKey().c_str(), value.c_str()) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save resume position");
    }
    nvs_commit(handle);
    nvs_close(handle);
}

void Config::LoadStats(SlideshowStats& stats) const {
    nvs_handle_t handle;
    if (nvs_open(NvsNamespace().c_str(), NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    size_t length = sizeof(SlideshowStats);
    if (nvs_get_blob(handle, StatsKey().c_str(), &stats, &length) != ESP_OK) {
        // Reset to defaults.
        stats = SlideshowStats{};
    }

    nvs_close(handle);
}

void Config::SaveStats(const SlideshowStats& stats) const {
    if (!statistics_persist_) {
        return;
    }

    nvs_handle_t handle;
    if (nvs_open(NvsNamespace().c_str(), NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for stats");
        return;
    }

    if (nvs_set_blob(handle, StatsKey().c_str(), &stats, sizeof(SlideshowStats)) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save stats");
    }
    nvs_commit(handle);
    nvs_close(handle);
}

void Config::ClearNvs() const {
    nvs_handle_t handle;
    if (nvs_open(NvsNamespace().c_str(), NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    nvs_erase_key(handle, ResumeKey().c_str());
    nvs_erase_key(handle, StatsKey().c_str());
    nvs_commit(handle);
    nvs_close(handle);
}

}  // namespace slideshow
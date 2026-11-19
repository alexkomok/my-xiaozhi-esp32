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
 * @file config.h
 * @brief Configuration management for the slideshow engine.
 *
 * This is the C++ counterpart of ``micropython/sdcard_slideshow/config.py``.
 * It provides:
 *
 * - JSON schema validation (mirroring the MicroPython defaults).
 * - NVS persistence for resume position and statistics.
 * - Hot-reload detection (file modification time).
 * - Playlist management (paths, per-playlist overrides).
 * - Typed accessors with defaults.
 *
 * The configuration is loaded from ``/sd/slideshow.json`` (or a custom path)
 * and cached in memory.  Changes to the file are detected by comparing
 * modification timestamps; the new config is applied without restarting the
 * engine.
 */

#ifndef MAIN_SDCARD_SLIDESHOW_CONFIG_H_
#define MAIN_SDCARD_SLIDESHOW_CONFIG_H_

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <sys/stat.h>
#include <memory>

#include "sdcard_slideshow.h"  // SlideshowConfig, SlideshowStats

namespace slideshow {

/**
 * @brief Exception thrown by Config when validation fails.
 */
class ConfigError : public std::exception {
public:
    explicit ConfigError(const std::string& msg) : msg_(msg) {}
    const char* what() const noexcept override { return msg_.c_str(); }

private:
    std::string msg_;
};

/**
 * @brief Core configuration structure.
 *
 * This mirrors the MicroPython @c DEFAULT_CONFIG dict.  The C++ layer parses
 * the same JSON document, so a single config file serves both the native
 * engine and the MicroPython sidecar module.
 */
class Config {
public:
    // Nested configuration structures (type definitions only).
    struct HwConfig {
        int sd_miso{12};
        int sd_mosi{13};
        int sd_sck{14};
        int sd_cs{15};
        uint32_t sd_freq_hz{20000000};
        bool sdio_fallback{true};
        int display_mosi{23};
        int display_sck{18};
        int display_dc{17};
        int display_cs{5};
        int display_rst{16};
        int display_bl{21};
        uint32_t display_spi_freq_hz{40000000};
        int display_width{480};
        int display_height{320};
        int display_rotation{0};
        bool display_rgb565_swap{false};
        int button_boot{0};
        int button_user{1};
        bool ir_enabled{false};
        int ir_pin{-1};
        int backlight_pwm_freq{1000};
        float backlight_gamma{2.2f};
        int backlight_default_brightness{128};
    };

    struct ScanConfig {
        std::string root{"/"};  // relative to the SD mount point (kSdMountPoint)
        int max_depth{10};
        // .gif is excluded by default: only the first frame is decoded and
        // animated files confuse the playlist. Opt in via config if needed.
        std::vector<std::string> extensions{".jpg", ".jpeg", ".png", ".bmp"};
        bool recursive{true};
        bool shuffle{false};
        bool repeat{true};
        int max_files{100};
        SortMode sort{SortMode::kName};
    };

    struct PlaybackConfig {
        float slide_duration{8.0f};
        float transition_duration{1.0f};
        TransitionType transition{TransitionType::kNone};
        bool ken_burns{false};
        float ken_burns_intensity{0.5f};
        bool auto_rotate{true};
        bool pause_on_start{false};
    };

    struct ImageConfig {
        int max_decode_dimension{1024};
        std::string dither{"none"};
        bool display_rgb565_swap{false};
        int display_rotation{0};
        bool streaming{false};
        bool progressive_jpeg{true};
        bool preload_next{true};
        int cache_size{3};
        bool low_memory_mode{false};
        FitMode fit{FitMode::kFit};
        DitherMode dither_mode{DitherMode::kNone};
    };

    struct NetworkConfig {
        struct Mqtt {
            bool enabled{false};
            std::string host{"mqtt.example.com"};
            int port{1883};
            std::string user{"user"};
            std::string password{"pass"};
            std::string client_id{"xiaozhi-slideshow"};
            std::string topic_prefix{"xiaozhi/slideshow"};
            bool discovery{false};
        } mqtt;

        struct Syslog {
            bool enabled{false};
            std::string host;
            int port{514};
        } syslog;

        struct WifiAp {
            bool enabled{false};
            std::string ssid{"xiaozhi-slideshow"};
            std::string password{"password"};
            int channel{1};
        } wifi_ap;

        struct WifiSta {
            bool enabled{false};
            std::string ssid;
            std::string password;
        } wifi_sta;

        struct Web {
            bool enabled{false};
            int port{80};
        } web;
    };

    struct PlaylistConfig {
        std::vector<std::string> paths;
        TransitionType transition{TransitionType::kFade};
        float slide_duration{8.0f};
    };

    Config() = default;
    ~Config() = default;

    // Non-copyable.
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    /**
     * Load configuration from a JSON file.
     *
     * @param path      Filesystem path (e.g. "/sd/slideshow.json").
     * @param validate  If true, perform strict validation; if false, only
     *                  basic parsing.
     */
    void Load(const std::string& path, bool validate = true);

    /**
     * Save the current configuration to a JSON file.
     */
    void Save(const std::string& path) const;

    /**
     * Check whether the file has been modified since the last load.
     */
    bool NeedsReload() const;

    /**
     * Reload if the file has changed.
     */
    void ReloadIfChanged();

    /**
     * Get the configuration path.
     */
    const std::string& config_path() const { return config_path_; }

    /**
     * Get the current configuration as a JSON string.
     */
    std::string ToJson() const;

    // --- Typed accessors (with defaults) ---

    // Hardware.
    int sd_miso() const { return hw_.sd_miso; }
    int sd_mosi() const { return hw_.sd_mosi; }
    int sd_sck() const { return hw_.sd_sck; }
    int sd_cs() const { return hw_.sd_cs; }
    uint32_t sd_freq_hz() const { return hw_.sd_freq_hz; }
    bool sdio_fallback() const { return hw_.sdio_fallback; }
    int display_mosi() const { return hw_.display_mosi; }
    int display_sck() const { return hw_.display_sck; }
    int display_dc() const { return hw_.display_dc; }
    int display_cs() const { return hw_.display_cs; }
    int display_rst() const { return hw_.display_rst; }
    int display_bl() const { return hw_.display_bl; }
    uint32_t display_spi_freq_hz() const { return hw_.display_spi_freq_hz; }
    int display_width() const { return hw_.display_width; }
    int display_height() const { return hw_.display_height; }
    int display_rotation() const { return hw_.display_rotation; }
    bool display_rgb565_swap() const { return hw_.display_rgb565_swap; }
    int button_boot() const { return hw_.button_boot; }
    int button_user() const { return hw_.button_user; }
    bool ir_enabled() const { return hw_.ir_enabled; }
    int ir_pin() const { return hw_.ir_pin; }
    int backlight_pwm_freq() const { return hw_.backlight_pwm_freq; }
    float backlight_gamma() const { return hw_.backlight_gamma; }
    int backlight_default_brightness() const { return hw_.backlight_default_brightness; }

    // Scan.
    const std::string& root() const { return scan_.root; }
    bool recursive() const { return scan_.recursive; }
    int max_depth() const { return scan_.max_depth; }
    int max_files() const { return scan_.max_files; }
    const std::vector<std::string>& extensions() const { return scan_.extensions; }
    SortMode sort() const { return scan_.sort; }
    bool shuffle() const { return scan_.shuffle; }
    bool repeat() const { return scan_.repeat; }

    // Playback.
    float slide_duration() const { return playback_.slide_duration; }
    float transition_duration() const { return playback_.transition_duration; }
    TransitionType transition() const { return playback_.transition; }
    bool ken_burns() const { return playback_.ken_burns; }
    float ken_burns_intensity() const { return playback_.ken_burns_intensity; }
    bool auto_rotate() const { return playback_.auto_rotate; }
    bool pause_on_start() const { return playback_.pause_on_start; }

    // Image.
    FitMode fit() const { return image_.fit; }
    DitherMode dither() const { return image_.dither_mode; }
    int max_decode_dimension() const { return image_.max_decode_dimension; }
    bool streaming() const { return image_.streaming; }
    bool progressive_jpeg() const { return image_.progressive_jpeg; }
    bool preload_next() const { return image_.preload_next; }
    int cache_size() const { return image_.cache_size; }
    bool low_memory_mode() const { return image_.low_memory_mode; }

    // Network.
    bool wifi_ap_enabled() const { return network_.wifi_ap.enabled; }
    const std::string& wifi_ap_ssid() const { return network_.wifi_ap.ssid; }
    const std::string& wifi_ap_password() const { return network_.wifi_ap.password; }
    int wifi_ap_channel() const { return network_.wifi_ap.channel; }
    bool wifi_sta_enabled() const { return network_.wifi_sta.enabled; }
    const std::string& wifi_sta_ssid() const { return network_.wifi_sta.ssid; }
    const std::string& wifi_sta_password() const { return network_.wifi_sta.password; }
    bool web_enabled() const { return network_.web.enabled; }
    int web_port() const { return network_.web.port; }
    bool mqtt_enabled() const { return network_.mqtt.enabled; }
    const std::string& mqtt_host() const { return network_.mqtt.host; }
    int mqtt_port() const { return network_.mqtt.port; }
    const std::string& mqtt_user() const { return network_.mqtt.user; }
    const std::string& mqtt_password() const { return network_.mqtt.password; }
    const std::string& mqtt_client_id() const { return network_.mqtt.client_id; }
    const std::string& mqtt_topic_prefix() const { return network_.mqtt.topic_prefix; }
    bool mqtt_discovery() const { return network_.mqtt.discovery; }
    bool syslog_enabled() const { return network_.syslog.enabled; }
    const std::string& syslog_host() const { return network_.syslog.host; }
    int syslog_port() const { return network_.syslog.port; }

    // Playlists.
    const std::string& active_playlist() const { return active_playlist_; }
    const std::map<std::string, PlaylistConfig>& playlists() const { return playlists_; }

    // Playlist setters.
    void SetActivePlaylist(const std::string& playlist) { active_playlist_ = playlist; }
    void SetTransition(TransitionType transition) { playback_.transition = transition; }
    void SetSlideDuration(float duration) { playback_.slide_duration = duration; }

    // Logging / statistics.
    const std::string& log_level() const { return log_level_; }
    int circular_buffer() const { return circular_buffer_; }
    bool log_timestamps() const { return log_timestamps_; }
    bool statistics_enabled() const { return statistics_enabled_; }
    bool statistics_persist() const { return statistics_persist_; }

    // --- NVS persistence ---

    /**
     * Load resume position (index, playlist) from NVS.
     */
    void LoadResumePosition(int& out_index, std::string& out_playlist) const;

    /**
     * Save current resume position to NVS.
     */
    void SaveResumePosition(int index, const std::string& playlist) const;

    /**
     * Load statistics from NVS.
     */
    void LoadStats(SlideshowStats& stats) const;

    /**
     * Save statistics to NVS.
     */
    void SaveStats(const SlideshowStats& stats) const;

    /**
     * Clear all NVS data for this module.
     */
    void ClearNvs() const;

    // --- Validation ---

    /**
     * Validate the current configuration.
     */
    void Validate() const;

    /**
     * Get a human-readable error message from the last validation.
     */
    const std::string& validation_error() const { return validation_error_; }

private:
    std::string config_path_;
    struct stat last_stat_;

    // Configuration data (mirrors SlideshowConfig).
    HwConfig hw_;
    ScanConfig scan_;
    PlaybackConfig playback_;
    ImageConfig image_;
    NetworkConfig network_;

    std::string active_playlist_{"default"};
    std::map<std::string, PlaylistConfig> playlists_;

    std::string log_level_{"INFO"};
    int circular_buffer_{256};
    bool log_timestamps_{true};
    bool statistics_enabled_{true};
    bool statistics_persist_{true};

    mutable std::string validation_error_;

    // --- Private helpers ---
    void ParseJson(const std::string& json_str, bool validate);
    void ApplyDefaults();
    static std::string NvsNamespace() { return "slideshow"; }
    static std::string ResumeKey() { return "resume"; }
    static std::string StatsKey() { return "stats"; }
};

}  // namespace slideshow

#endif  // MAIN_SDCARD_SLIDESHOW_CONFIG_H_

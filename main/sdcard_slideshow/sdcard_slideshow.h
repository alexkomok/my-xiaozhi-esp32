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
 * @file sdcard_slideshow.h
 * @brief Native C++ slideshow MCP tool for the ESP32-S3.
 *
 * This module provides a complete slideshow engine (scan, decode, transition,
 * playback timing) implemented natively in C++ so it can be exposed as MCP
 * tools alongside the existing `self.sdcard.*` tools.  It mirrors the
 * MicroPython module ``micropython/sdcard_slideshow/``:
 *
 * - @c Decoder  -- format detection, JPEG/BMP/PNG/GIF decoding, scaling,
 *                  RGB565 conversion with dithering, EXIF orientation.
 * - @c Config   -- JSON config with schema validation, hot reload, playlists,
 *                  NVS resume position.
 * - @c Transition -- fade, slide, wipe, zoom effects plus Ken Burns pan/zoom.
 * - @c SlideshowMCP -- orchestrates the SD card, display, engine loop and
 *                  registers every MCP tool via @c McpServer::AddTool().
 *
 * Design notes
 * ------------
 * The engine runs its timing loop in a FreeRTOS task so that no tool call or
 * other board code is blocked.  Tool calls that only read state (list images,
 * config, stats) are synchronous; state-changing calls (start/stop/next/prev)
 * push an event into the engine task's queue.  Display updates go through the
 * existing @c Display interface (ShowMediaFrame) so no new panel driver is
 * required.
 *
 * Usage from the AI client:
 *
 *     self.sdcard.slideshow.start({ "playlist": "nature", "transition": "fade" })
 *     self.sdcard.slideshow.pause({ "pause": true })
 *     self.sdcard.slideshow.next()
 */

#ifndef MAIN_SDCARD_SLIDESHOW_H_
#define MAIN_SDCARD_SLIDESHOW_H_

#include <cJSON.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// --- forward declarations ---------------------------------------------------
class Display;
class Application;
class McpServer;
namespace slideshow { class Config; }

namespace slideshow {

/**
 * Slideshow playback state.
 */
enum class SlideshowState {
    kStopped,    // Engine not running; last frame may still be on screen.
    kPlaying,    // Slides advancing according to timing.
    kPaused,     // Engine paused; current slide held.
};

/** Engine event codes for the slideshow task. */
enum class EngineEvent {
    kEventStop = 0,
    kEventPause,
    kEventNext,
    kEventPrev,
    kEventStart,
    kEventReloadConfig,
};

/**
 * Transition effect identifiers, kept in sync with transitions.py.
 */
enum class TransitionType { kNone, kFade, kSlide, kWipe, kZoom };

/**
 * Fit mode for scaling, kept in sync with config.py / decoder.py.
 */
enum class FitMode { kFit, kFill, kStretch };

/**
 * Dithering mode, kept in sync with config.py / decoder.py.
 */
enum class DitherMode { kNone, kFloydSteinberg, kBayer };

/**
 * Sort order for the file list, kept in sync with config.py.
 */
enum class SortMode { kNatural, kExif, kName, kMtime };

/**
 * @brief Convert a transition name string to a TransitionType.
 */
TransitionType ParseTransition(const std::string& str);

/**
 * @brief Convert a TransitionType to a transition name string.
 */
std::string TransitionToString(TransitionType type);

/**
 * @brief Convert a fit name string to a FitMode.
 */
FitMode ParseFit(const std::string& str);

/**
 * @brief Convert a FitMode to a fit name string.
 */
std::string FitToString(FitMode mode);

/**
 * @brief Convert a dither name string to a DitherMode.
 */
DitherMode ParseDither(const std::string& str);

/**
 * @brief Convert a DitherMode to a dither name string.
 */
std::string DitherToString(DitherMode mode);

/**
 * @brief Convert a sort name string to a SortMode.
 */
SortMode ParseSort(const std::string& str);

/**
 * @brief Convert a SortMode to a sort name string.
 */
std::string SortToString(SortMode mode);

/**
 * @brief Runtime statistics of the slideshow session.
 */
struct SlideshowStats {
    size_t total_images{0};       // Images displayed since boot.
    uint64_t display_time_ms{0};  // Cumulative time slides were shown.
    size_t error_count{0};        // Decode/render failures.
    std::string sd_health{"not_mounted"};
    uint32_t uptime_ms{0};
    std::string last_image;       // Path of the last shown image.
    size_t transitions{0};
    size_t cache_hits{0};
    size_t cache_misses{0};
};

/**
 * @brief Slideshow settings loaded from /sdcard/slideshow.json.
 *
 * This mirrors the MicroPython @c DEFAULT_CONFIG structure.  The C++ layer
 * parses the same JSON document, so a single config file serves both the native
 * engine and the MicroPython sidecar module.  Where the C++ engine cannot match
 * a MicroPython-only option, the missing value is ignored and a warning is
 * logged.
 */
struct SlideshowConfig {
    // hardware
    struct Hw {
        int sd_miso{12};
        int sd_mosi{13};
        int sd_sck{14};
        int sd_cs{15};
        uint32_t sd_freq_hz{20'000'000};
        bool sdio_fallback{true};
        int display_mosi{11};
        int display_sck{10};
        int display_dc{9};
        int display_cs{8};
        int display_rst{7};
        int display_bl{6};
        uint32_t display_spi_freq_hz{80'000'000};
        int display_width{480};
        int display_height{320};
        int display_rotation{0};
        bool display_rgb565_swap{false};
        int button_boot{0};
        int button_user{14};
        bool ir_enabled{false};
        int ir_pin{4};
        int backlight_pwm_freq{5000};
        float backlight_gamma{2.2f};
        int backlight_default_brightness{220};
    } hw;

    // scan
    struct Scan {
        std::string root{"/"};
        bool recursive{true};
        int max_depth{6};
        int max_files{2000};
        std::vector<std::string> extensions{".jpg", ".jpeg", ".png", ".bmp", ".gif"};
        SortMode sort{SortMode::kNatural};
        bool shuffle{false};
        bool repeat{true};
    } scan;

    // playback
    struct Playback {
        float slide_duration{8.0f};
        float transition_duration{0.8f};
        TransitionType transition{TransitionType::kFade};
        bool ken_burns{false};
        float ken_burns_intensity{0.08f};
        bool auto_rotate{true};
        bool pause_on_start{false};
    } playback;

    // image
    struct Image {
        FitMode fit{FitMode::kFit};
        DitherMode dither{DitherMode::kFloydSteinberg};
        int max_decode_dimension{480};
        bool streaming{true};
        bool progressive_jpeg{true};
        bool preload_next{true};
        int cache_size{2};
        bool low_memory_mode{false};
    } image;

    // network
    struct Network {
        struct Wpa { bool enabled{false}; std::string ssid; std::string password; int channel{6}; };
        Wpa wifi_ap;
        struct Sta { bool enabled{false}; std::string ssid; std::string password; };
        Sta wifi_sta;
        struct Web { bool enabled{true}; int port{80}; };
        Web web;
        struct Mqtt {
            bool enabled{false};
            std::string host;
            int port{1883};
            std::string user;
            std::string password;
            std::string client_id{"xiaozhi-slideshow"};
            std::string topic_prefix{"slideshow"};
            bool discovery{true};
        } mqtt;
        struct Syslog { bool enabled{false}; std::string host; int port{514}; };
        Syslog syslog;
    } network;

    // playlists
    std::string active_playlist{"default"};
    struct Playlist {
        std::vector<std::string> paths;
        TransitionType transition{TransitionType::kFade};
        float slide_duration{8.0f};
    };
    std::map<std::string, Playlist> playlists;

    // logging / statistics
    std::string log_level{"INFO"};
    int circular_buffer{256};
    bool log_timestamps{true};
    bool statistics_enabled{true};
    bool statistics_persist{true};
};

// Forward-declare the engine task queue type.
using EngineEventQueue = QueueHandle_t;

/**
 * @brief Top-level slideshow tool.
 *
 * This class is the C++ counterpart of the MicroPython @c SlideshowMCP class.
 * It owns the configuration, the image list, the engine task and the
 * statistics.  The board registers its instances of these methods with the
 * @c McpServer via @c AddTool() so they become callable as
 * ``self.sdcard.slideshow.*``.
 *
 * The engine is deliberately decoupled from the board: it holds pointers to a
 * @c Display and to the SD mount point and asks for a frame as RGB565 bytes,
 * letting @c ShowMediaFrame handle the panel update.  This mirrors how
 * @c DecodeJpegAndShow() works in the existing board source, so no new display
 * abstraction is introduced.
 */
class SlideshowMCP {
public:
    /**
     * Thread-safe accessors used by the tool methods.
     */
    SlideshowState state() const { return state_.load(); }
    int current_index() const { return index_.load(); }
    size_t image_count() const { return images_.size(); }
    const std::string& last_error() const { return last_error_; }
    SlideshowStats stats() const { return stats_; }

    /**
     * Construct a slideshow tool backed by the default display and config path.
     * @param display       The display used to show slides.
     * @param config_path   Path of the config file on the SD card.
     *                      Defaults to "/sdcard/slideshow.json".
     */
    explicit SlideshowMCP(Display* display, const std::string& config_path = "/sdcard/slideshow.json");

    ~SlideshowMCP();

    // Non-copyable.
    SlideshowMCP(const SlideshowMCP&) = delete;
    SlideshowMCP& operator=(const SlideshowMCP&) = delete;

    // ---- MCP tool surface (also documented in docs/sdcard_slideshow.md) ----

    /**
     * Scan the SD card and build the sorted image index.
     * JSON: {"recursive": bool, "sort": "natural|exif|name|mtime", "shuffle": bool}
     */
    static cJSON* ToolListImages(const cJSON* args);

    /**
     * Show a single image fullscreen.
     * JSON: {"path": string, "transition": "fade|slide|wipe|zoom|none"}
     */
    static cJSON* ToolShowImage(const cJSON* args);

    /**
     * Start the slideshow engine.
     * JSON: {"playlist": string, "transition": string, "slide_duration": float}
     */
    static cJSON* ToolStart(const cJSON* args);

    /**
     * Stop the slideshow engine.
     * JSON: {}
     */
    static cJSON* ToolStop(const cJSON* args);

    /**
     * Pause or resume the engine.
     * JSON: {"pause": bool}
     */
    static cJSON* ToolPause(const cJSON* args);

    /**
     * Advance to the next image.
     * JSON: {}
     */
    static cJSON* ToolNext(const cJSON* args);

    /**
     * Go to the previous image.
     * JSON: {}
     */
    static cJSON* ToolPrev(const cJSON* args);

    /**
     * Set the slide duration in seconds.
     * JSON: {"slide_duration": float}  range 1.0-300.0
     */
    static cJSON* ToolSetSlideDuration(const cJSON* args);

    /**
     * Set the transition effect.
     * JSON: {"transition": "fade|slide|wipe|zoom|none"}
     */
    static cJSON* ToolSetTransition(const cJSON* args);

    /**
     * Set the active playlist.
     * JSON: {"playlist": string}
     */
    static cJSON* ToolSetPlaylist(const cJSON* args);

    /**
     * Return the current playback status.
     * JSON: {}
     */
    static cJSON* ToolStatus(const cJSON* args);

    /**
     * Return the current image list as JSON.
     * JSON: {}
     */
    static cJSON* ToolImageList(const cJSON* args);

    /**
     * Return the current configuration (validated copy).
     * JSON: {}
     */
    static cJSON* ToolConfig(const cJSON* args);

    /**
     * Reload the configuration file without restarting the engine.
     * JSON: {}
     */
    static cJSON* ToolReloadConfig(const cJSON* args);

    /**
     * Return runtime statistics (images shown, errors, SD health, memory).
     * JSON: {}
     */
    static cJSON* ToolStats(const cJSON* args);

    /**
     * Draw the built-in test pattern on the display for hardware validation.
     * JSON: {}
     */
    static cJSON* ToolTestPattern(const cJSON* args);

    /**
     * Benchmark decode performance and memory usage.
     * JSON: {"path": string}
     */
    static cJSON* ToolBenchmark(const cJSON* args);

    /**
     * Resume the previously saved playback position.
     * JSON: {}
     */
    static cJSON* ToolResume(const cJSON* args);

    /**
     * Delete a file from the SD card.
     * JSON: {"path": string}
     */
    static cJSON* ToolDeleteFile(const cJSON* args);

    // ---- internal / helper API (exposed for the MCP dispatcher) ----------

    /**
     * Called once during board initialization.  Initializes the SD card,
     * display (if a panel is not already owned by the board), config and the
     * engine task.  Returns true on success.
     */
    bool Initialize();

    /**
     * Register every slideshow tool with the given @c McpServer instance.
     */
    void RegisterTools(McpServer* mcp_server);

    /**
     * Return the engine event queue so callers can enqueue commands from
     * arbitrary tasks (e.g. button or IR callbacks).
     */
    QueueHandle_t event_queue() const { return event_queue_; }

    /**
     * Direct event enqueue for button/IR callbacks.
     */
    void EnqueueEvent(int event_code, int32_t param = 0);

private:
    // ---- member state ----------------------------------------------------
    Display* display_;
    std::string config_path_;
    std::unique_ptr<Config> config_;          // JSON config (see below)
    std::vector<std::string> images_;         // Relative paths, sorted
    std::unique_ptr<class Decoder> decoder_;  // See decoder.h
    std::unique_ptr<class TransitionEngine> transition_engine_;
    std::atomic<SlideshowState> state_{SlideshowState::kStopped};
    std::atomic<int> index_{-1};
    std::string last_error_;
    SlideshowStats stats_;
    std::atomic<bool> shutdown_{false};
    std::vector<uint16_t> prev_frame_;
    uint64_t frame_start_ms_{0};

    // Engine: runs in its own FreeRTOS task.
    TaskHandle_t engine_task_handle_{nullptr};
    QueueHandle_t event_queue_{nullptr};
    static void EngineTask(void* arg);
    void EngineRun();

    // Image index / scanning.
    bool EnsureSdCardMounted();
    void ScanImages();
    bool IsSafeMediaPath(const std::string& path);

    // Playback.
    void ShowSlide(int index);
    bool PreloadNext(int index);

    // NVS resume position (persist index/timestamp/playlist).
    static std::string NvsResumeKey() { return "slideshow_resume"; }
    void SaveResumePosition();
    void LoadResumePosition(int& out_index, std::string& out_playlist);

    // Logging helper.
    void Log(const char* level, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

    // Singleton accessor for tool callbacks (MCP dispatcher runs on any task).
    static SlideshowMCP& GetInstance(const cJSON* args);

    // Singleton instance.
    static SlideshowMCP* instance_;
};

}  // namespace slideshow

#endif  // MAIN_SDCARD_SLIDESHOW_H_

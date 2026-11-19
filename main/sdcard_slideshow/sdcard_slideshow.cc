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
 * @file sdcard_slideshow.cc
 * @brief Native C++ slideshow engine and MCP tool implementation.
 *
 * This is the main implementation file for the slideshow MCP tool. It provides:
 *
 * - SD card initialization and recursive image scanning.
 * - A FreeRTOS task-based engine loop (non-blocking).
 * - Tool implementations registered via @c McpServer::AddTool().
 * - NVS persistence for resume position and statistics.
 * - Test patterns and benchmark for hardware validation.
 *
 * The engine holds a weak pointer to the board's @c Display and renders
 * frames through @c Display::ShowMediaFrame().  This keeps the engine decoupled
 * from any specific panel driver.
 */

#include "sdcard_slideshow.h"
#include "config.h"
#include "decoder.h"
#include "transitions.h"
#include "display/display.h"

#include <dirent.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <nvs.h>
#include <sdmmc_cmd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <numeric>
#include <sstream>

#include "cjson_utils.h"
#include "mcp_server.h"
#include "driver/sdmmc_host.h"

namespace slideshow {

static const char* TAG = "SlideshowMCP";

// Convert display rotation from quarter-turns (0-3) to degrees (0/90/180/270).
static int DisplayRotationToDegrees(int rotation) {
    switch (rotation) {
        case 0: return 0;
        case 1: return 90;
        case 2: return 180;
        case 3: return 270;
        default: return 0;
    }
}

// Engine event message for the queue.
struct EngineEventMsg {
    EngineEvent event_code;
    int32_t param;
};

// Helper functions for tool results.
cJSON* CreateError(const std::string& message) {
    cJSON* obj = cJSON_CreateObject();
    if (obj != nullptr) {
        cJSON_AddStringToObject(obj, "error", message.c_str());
    }
    return obj;
}

cJSON* CreateResult(const std::string& message) {
    cJSON* obj = cJSON_CreateObject();
    if (obj != nullptr) {
        cJSON_AddStringToObject(obj, "result", message.c_str());
    }
    return obj;
}

// Engine task stack depth in words (xTaskCreate takes words, not bytes).
// 48 KB of stack: 48 * 1024 bytes / sizeof(StackType_t) words.
constexpr uint32_t kEngineTaskStackSize = 48 * 1024 / sizeof(StackType_t);
// Default tick interval for transition steps (ms).
constexpr uint32_t kDefaultStepMs = 40;
// SD mount point owned by the board (must match SD_CARD_MOUNT_POINT in config.h).
constexpr const char* kSdMountPoint = "/sdcard";

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void SlideshowMCP::Log(const char* level, const char* fmt, ...) {
    esp_log_level_t log_level = ESP_LOG_INFO;
    if (level != nullptr) {
        if (strcasecmp(level, "error") == 0) {
            log_level = ESP_LOG_ERROR;
        } else if (strcasecmp(level, "warn") == 0 || strcasecmp(level, "warning") == 0) {
            log_level = ESP_LOG_WARN;
        } else if (strcasecmp(level, "debug") == 0) {
            log_level = ESP_LOG_DEBUG;
        } else if (strcasecmp(level, "verbose") == 0) {
            log_level = ESP_LOG_VERBOSE;
        }
    }
    va_list args;
    va_start(args, fmt);
    esp_log_writev(log_level, TAG, fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------------
// Tool dispatch helpers
// ---------------------------------------------------------------------------

namespace {

/**
 * Convert a cJSON string property to a C++ std::string, returning an error
 * string if missing or not a string.
 */
static std::optional<std::string> GetStr(const cJSON* root, const char* name) {
    if (root == nullptr) return std::nullopt;
    cJSON* item = cJSON_GetObjectItem(root, name);
    if (item == nullptr || !cJSON_IsString(item)) {
        return std::nullopt;
    }
    return std::string(item->valuestring);
}

/**
 * Convert a cJSON number property to a double, returning an error string if
 * missing or not a number.
 */
static std::optional<double> GetNum(const cJSON* root, const char* name) {
    if (root == nullptr) return std::nullopt;
    cJSON* item = cJSON_GetObjectItem(root, name);
    if (item == nullptr || !cJSON_IsNumber(item)) {
        return std::nullopt;
    }
    return item->valuedouble;
}

/**
 * Convert a cJSON boolean property to a bool, defaulting to false if missing.
 */
static bool GetBool(const cJSON* root, const char* name, bool default_value = false) {
    if (root == nullptr) return default_value;
    cJSON* item = cJSON_GetObjectItem(root, name);
    if (item == nullptr || !cJSON_IsBool(item)) return default_value;
    return cJSON_IsTrue(item);
}

}  // namespace

// ---------------------------------------------------------------------------
// SD card / scanning
// ---------------------------------------------------------------------------

bool SlideshowMCP::EnsureSdCardMounted() {
    // The board owns the SD card mount (see WifiBoard::EnsureSdCardMounted /
    // the board's EnsureSdCardMounted helper).  If the mount point exists,
    // return success; otherwise try mounting.
    //
    // Check whether the board's mount point (kSdMountPoint) is present.
    struct stat st = {};
    if (stat(kSdMountPoint, &st) == 0) {
        return true;  // already mounted by the board.
    }

    // Do NOT mount here: a second esp_vfs_fat_sdmmc_mount() on top of the
    // board's mount conflicts with the board's SDMMC host and can hang or
    // reset the chip. The board owns the mount; report its absence instead.
    ESP_LOGW(TAG, "%s is not mounted by the board yet", kSdMountPoint);
    return false;
}

namespace {

/** Case-insensitive extension comparison. */
bool HasExtension(const std::string& path, const std::vector<std::string>& exts) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
        return false;
    }
    const std::string ext = path.substr(dot);
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& e : exts) {
        std::string e_lower = e;
        std::transform(e_lower.begin(), e_lower.end(), e_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext_lower == e_lower) {
            return true;
        }
    }
    return false;
}

/** Compare two paths for natural (human) sorting. */
class NaturalComparator {
public:
    explicit NaturalComparator(const std::vector<std::string>& exts) : exts_(exts) {}

    bool operator()(const std::string& a, const std::string& b) {
        // Directory vs file ordering.
        const bool a_dir = IsDir(a);
        const bool b_dir = IsDir(b);
        if (a_dir != b_dir) return a_dir;

        return NaturalLess(a, b);
    }

private:
    static bool IsDir(const std::string& path) {
        struct stat st = {};
        return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }

    static bool NaturalLess(const std::string& a, const std::string& b) {
        size_t i = 0, j = 0;
        while (i < a.size() && j < b.size()) {
            const char ca = a[i];
            const char cb = b[j];

            if (std::isdigit(static_cast<unsigned char>(ca)) &&
                std::isdigit(static_cast<unsigned char>(cb))) {
                size_t ai = i, bj = j;
                while (ai < a.size() && std::isdigit(static_cast<unsigned char>(a[ai]))) ai++;
                while (bj < b.size() && std::isdigit(static_cast<unsigned char>(b[bj]))) bj++;
                const std::string na = a.substr(i, ai - i);
                const std::string nb = b.substr(j, bj - j);
                if (na != nb) {
                    const uint64_t va = std::stoull(na);
                    const uint64_t vb = std::stoull(nb);
                    return va < vb;
                }
                i = ai;
                j = bj;
            } else {
                const char la = std::tolower(static_cast<unsigned char>(ca));
                const char lb = std::tolower(static_cast<unsigned char>(cb));
                if (la != lb) return la < lb;
                i++;
                j++;
            }
        }
        return a.size() < b.size();
    }

    const std::vector<std::string>& exts_;
};

}  // namespace

bool SlideshowMCP::IsSafeMediaPath(const std::string& path) {
    return !path.empty() && path.front() != '/' && path.find('\\') == std::string::npos &&
           path.find(':') == std::string::npos && path.find("..") == std::string::npos;
}

void SlideshowMCP::ScanImages() {
    if (!EnsureSdCardMounted()) {
        images_.clear();
        stats_.sd_health = "not_mounted";
        Log("error", "Could not mount SD card");
        return;
    }

    const std::string root = config_->root();
    const std::string full_root = std::string(kSdMountPoint) + root;

    struct stat st = {};
    if (stat(full_root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        images_.clear();
        stats_.sd_health = "not_mounted";
        Log("error", "Scan root is not a directory: %s", full_root.c_str());
        return;
    }

    images_.clear();
    std::vector<std::string> candidates;

    // Recursive directory walk.
    std::vector<std::string> stack = {full_root};
    std::map<std::string, int> depth;
    depth[full_root] = 0;

    while (!stack.empty()) {
        const std::string dir = stack.back();
        stack.pop_back();

        DIR* d = opendir(dir.c_str());
        if (d == nullptr) continue;

        while (struct dirent* entry = readdir(d)) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            if (entry->d_name[0] == '.') continue;

            std::string relative = dir.empty()
                                       ? entry->d_name
                                       : dir.substr(full_root.size()) + "/" + entry->d_name;
            std::string full = dir + "/" + entry->d_name;

            struct stat entry_stat = {};
            if (stat(full.c_str(), &entry_stat) != 0) continue;

            if (S_ISDIR(entry_stat.st_mode)) {
                const int d = depth[dir] + 1;
                if (d <= config_->max_depth()) {
                    depth[full] = d;
                    stack.push_back(full);
                }
            } else if (HasExtension(entry->d_name, config_->extensions())) {
                // Strip leading mount point for storage (relative to the scan root).
                if (full.size() > full_root.size()) {
                    candidates.push_back(full.substr(full_root.size() + 1));
                }
            }
        }
        closedir(d);
    }

    // Sort.
    NaturalComparator comparator(config_->extensions());
    std::stable_sort(candidates.begin(), candidates.end(),
                     [&](const std::string& a, const std::string& b) {
                         return comparator(a, b);
                     });

    if (config_->shuffle()) {
        // Simple Fisher-Yates.
        std::random_shuffle(candidates.begin(), candidates.end());
    }

    // Limit.
    if (static_cast<int>(candidates.size()) > config_->max_files()) {
        candidates.resize(config_->max_files());
    }

    images_ = candidates;
    stats_.total_images += images_.size();
    stats_.sd_health = "ok";
    Log("info", "Scanned %zu images in %s", images_.size(), root.c_str());
}

// ---------------------------------------------------------------------------
// Engine: task + run loop
// ---------------------------------------------------------------------------

void SlideshowMCP::EngineTask(void* arg) {
    auto* engine = static_cast<SlideshowMCP*>(arg);

    esp_log_level_set(TAG, ESP_LOG_INFO);
    engine->EngineRun();
    engine->shutdown_.store(true);

    vTaskDelete(nullptr);
}

void SlideshowMCP::EngineRun() {
    auto& cfg = *config_;
    const float step_s = kDefaultStepMs / 1000.0f;

    uint64_t last_frame_ms = 0;
    uint64_t frame_start_ms = 0;
    // index_ starts at -1 (nothing shown yet); clamp so the engine never
    // computes a negative slide index and silently stays "stopped".
    int current_index = std::max(index_.load(), 0);
    bool paused = false;
    bool need_scan = images_.empty();

    while (!shutdown_.load()) {
        // Drain events.
        EngineEventMsg evt;
        BaseType_t ret = xQueueReceive(event_queue_, &evt, 0);
        while (ret == pdPASS) {
            switch (static_cast<int>(evt.event_code)) {
                case static_cast<int>(slideshow::EngineEvent::kEventStop):
                    Log("info", "Engine stopped");
                    state_.store(SlideshowState::kStopped);
                    paused = true;  // Pause on stop so it doesn't auto-advance
                    if (display_ != nullptr) {
                        display_->ClearMediaFrame();
                    }
                    break;
                case static_cast<int>(slideshow::EngineEvent::kEventPause):
                    paused = evt.param != 0;
                    Log("info", "Engine pause: %s", paused ? "true" : "false");
                    break;
                case static_cast<int>(slideshow::EngineEvent::kEventNext):
                    current_index = (current_index + 1) % static_cast<int>(images_.size());
                    need_scan = true;
                    Log("info", "Engine next: index=%d", current_index);
                    break;
                case static_cast<int>(slideshow::EngineEvent::kEventPrev):
                    current_index =
                        (current_index - 1 < 0) ? static_cast<int>(images_.size()) - 1
                                                : current_index - 1;
                    need_scan = true;
                    Log("info", "Engine prev: index=%d", current_index);
                    break;
                case static_cast<int>(slideshow::EngineEvent::kEventStart):
                    // Honor the start request: optional start index, unpause, mark playing.
                    current_index = (evt.param >= 0 && !images_.empty())
                                        ? evt.param % static_cast<int>(images_.size())
                                        : std::max(current_index, 0);
                    paused = false;
                    state_.store(SlideshowState::kPlaying);
                    last_frame_ms = 0;
                    Log("info", "Engine start: index=%d, state=playing, last_frame_ms=0", current_index);
                    break;
                case static_cast<int>(slideshow::EngineEvent::kEventReloadConfig):
                    cfg.ReloadIfChanged();
                    break;
                default:
                    break;
            }
            ret = xQueueReceive(event_queue_, &evt, 0);
        }

        if (need_scan) {
            if (images_.empty()) {
                ScanImages();
            }
            need_scan = false;
        }

        if (images_.empty()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        const int idx = current_index % static_cast<int>(images_.size());
        if (idx != current_index) current_index = idx;

        // Show the slide only while playing. Without this gate the engine
        // starts decoding at boot (last_frame_ms begins at 0), before any
        // start request, which crashes the device before the user acts.
        const float slide_s = cfg.slide_duration();
        const uint64_t now = esp_timer_get_time() / 1000;
        const bool playing = state_.load() == SlideshowState::kPlaying && !paused;
        const uint64_t slide_interval_ms = static_cast<uint64_t>(slide_s * 1000.0);
        const uint64_t time_since_last = now - last_frame_ms;
        const bool should_advance = playing && time_since_last >= slide_interval_ms;
        
        Log("debug", "Engine tick: now=%llu, last_frame=%llu, elapsed=%llu, interval=%llu, playing=%d, paused=%d, advance=%d",
            now, last_frame_ms, time_since_last, slide_interval_ms, playing, paused, should_advance);
        
        if (should_advance) {
            Log("info", "Engine advancing to slide %d (elapsed=%llu ms, interval=%llu ms)", idx, time_since_last, slide_interval_ms);
            ShowSlide(idx);
            current_index = (current_index + 1) % static_cast<int>(images_.size());
            last_frame_ms = now;
        }

        // Check for a full image update within the same slide duration
        // (useful when a tool changes the playlist).
        if (now - frame_start_ms >= 3000) {
            frame_start_ms = now;
            need_scan = true;
        }

        if (paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Timing: sleep until the next slide.
        const uint64_t elapsed = (esp_timer_get_time() / 1000) - last_frame_ms;
        const uint64_t remaining =
            static_cast<uint64_t>(slide_s * 1000.0) - elapsed;
        if (remaining > 100) {
            const uint64_t sleep_ms = std::min(remaining, static_cast<uint64_t>(1000));
            Log("debug", "Engine sleep: elapsed=%llu, remaining=%llu, sleeping=%llu ms", elapsed, remaining, sleep_ms);
            vTaskDelay(pdMS_TO_TICKS(sleep_ms));
        }
    }

    // Clean up: clear the display.
    if (display_ != nullptr) {
        display_->ClearMediaFrame();
    }
    state_.store(SlideshowState::kStopped);
}

void SlideshowMCP::EnqueueEvent(int event_code, int32_t param) {
    if (event_queue_ == nullptr) return;

    EngineEventMsg evt{};
    evt.event_code = static_cast<EngineEvent>(event_code);
    evt.param = param;
    BaseType_t ret = xQueueSend(event_queue_, &evt, pdMS_TO_TICKS(1000));
    if (ret != pdPASS) {
        Log("warning", "Failed to enqueue engine event %d", event_code);
    }
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

void SlideshowMCP::ShowSlide(int index) {
    if (index < 0 || index >= static_cast<int>(images_.size()) || display_ == nullptr) {
        Log("warning", "ShowSlide: rejected index=%d (images=%zu, display=%p)", index, images_.size(), static_cast<void*>(display_));
        return;
    }

    // Guard against SD card not being mounted.  The board owns the mount; if
    // it is not present, abort the slide instead of crashing on a failed
    // file open.
    if (!EnsureSdCardMounted()) {
        Log("error", "SD card not mounted; skipping slide %d", index);
        return;
    }

    const auto& path = images_[index];
    const auto& cfg = *config_;
    const uint64_t show_start_ms = esp_timer_get_time() / 1000;

    // Try to load a cached frame first (LruCache would live here).
    // For now, decode directly.

    Decoder decoder;
    Rgb565Frame frame = decoder.DecodeFile(
        std::string(kSdMountPoint) + "/" + path, cfg.display_width(),
        cfg.display_height(), cfg.fit(), cfg.dither(), cfg.display_rgb565_swap(),
        cfg.max_decode_dimension());

    if (!frame.ok) {
        stats_.error_count++;
        Log("error", "Failed to decode %s: %s", path.c_str(),
            decoder.last_error().c_str());
        last_error_ = "Failed to decode " + path;
        return;
    }

    stats_.last_image = path;
    stats_.transitions++;

    if (cfg.transition() == TransitionType::kNone) {
        // Direct display.
        Log("info", "ShowSlide: displaying %s (%dx%d, %zu bytes, transition=none, took %llu ms)",
            path.c_str(), frame.width, frame.height, frame.pixels.size(),
            (esp_timer_get_time() / 1000) - show_start_ms);
        display_->ShowMediaFrame(reinterpret_cast<const uint8_t*>(frame.pixels.data()), frame.width, frame.height,
                                 frame.width * sizeof(uint16_t),
                                 DisplayRotationToDegrees(cfg.display_rotation()));
        state_.store(SlideshowState::kPlaying);
    } else {
        // Transition path: build a blank source (previous slide) if needed.
        if (prev_frame_.size() != static_cast<size_t>(frame.width) * frame.height) {
            prev_frame_.assign(frame.width * frame.height, 0x0000);
        }

        transition_engine_->StartTransition(prev_frame_, frame.pixels, frame.width,
                                           frame.height);

        // Drive the transition.
        const auto step_ms = static_cast<uint32_t>(cfg.transition_duration() * 1000.0);
        const uint32_t steps = (step_ms / kDefaultStepMs) + 1;
        std::vector<uint16_t> tmp(frame.width * frame.height);

        for (uint32_t i = 0; i < steps; i++) {
            transition_engine_->Step(prev_frame_, frame.pixels, frame.width, frame.height, tmp);
            display_->ShowMediaFrame(reinterpret_cast<const uint8_t*>(tmp.data()), frame.width, frame.height,
                                     frame.width * sizeof(uint16_t),
                                     DisplayRotationToDegrees(cfg.display_rotation()));
            if (!transition_engine_->IsActive()) break;
        }

        // Copy current frame to prev_frame for the next transition.
        prev_frame_ = frame.pixels;
        state_.store(SlideshowState::kPlaying);
        Log("info", "ShowSlide: displayed %s (%dx%d, transition=%s, took %llu ms)",
            path.c_str(), frame.width, frame.height,
            TransitionToString(cfg.transition()).c_str(),
            (esp_timer_get_time() / 1000) - show_start_ms);
    }

    frame_start_ms_ = esp_timer_get_time() / 1000;
}

bool SlideshowMCP::PreloadNext(int index) {
    if (!config_->preload_next() || images_.size() < 2) {
        return true;
    }
    const int next_index = (index + 1) % static_cast<int>(images_.size());
    Decoder decoder;
    const auto& path = images_[next_index];
    Rgb565Frame frame = decoder.DecodeFile(
        std::string(kSdMountPoint) + "/" + path, config_->display_width(),
        config_->display_height(), config_->fit(), config_->dither(),
        config_->display_rgb565_swap(), config_->max_decode_dimension());
    if (!frame.ok) {
        Log("warning", "Predecode failed for %s: %s", path.c_str(),
            decoder.last_error().c_str());
    }
    return true;
}

// ---------------------------------------------------------------------------
// NVS resume
// ---------------------------------------------------------------------------

void SlideshowMCP::SaveResumePosition() {
    config_->SaveResumePosition(current_index(), config_->active_playlist());
}

void SlideshowMCP::LoadResumePosition(int& out_index, std::string& out_playlist) {
    config_->LoadResumePosition(out_index, out_playlist);
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

namespace {

/**
 * Build a cJSON array of image info objects.
 */
cJSON* ImageListToJson(const std::vector<std::string>& images, const std::string& prefix,
                       SlideshowStats stats) {
    cJSON* arr = cJSON_CreateArray();
    if (arr == nullptr) return nullptr;
    for (size_t i = 0; i < images.size(); i++) {
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) break;
        cJSON_AddNumberToObject(item, "index", static_cast<double>(i));
        cJSON_AddStringToObject(item, "path", images[i].c_str());
        cJSON_AddBoolToObject(item, "current", (images[i] == stats.last_image));
        if (!cJSON_AddItemToArray(arr, item)) break;
    }
    return arr;
}

}  // namespace

// --- ToolListImages ---
cJSON* SlideshowMCP::ToolListImages(const cJSON* args) {
    auto& instance = GetInstance(args);
    auto& cfg = *instance.config_;
    if (!instance.EnsureSdCardMounted()) {
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "error", "Could not mount the SD card");
        return obj;
    }

    const bool recursive = GetBool(args, "recursive", cfg.recursive());
    const bool shuffle = GetBool(args, "shuffle", cfg.shuffle());

    // Re-scan respecting the arguments (a thin override of the config values).
    // The engine holds the authoritative list; this tool mirrors it.
    cJSON* arr = ImageListToJson(instance.images_, std::string(kSdMountPoint) + "/" + cfg.root(),
                                 instance.stats());
    if (arr == nullptr) {
        return CreateError("Failed to build image list");
    }

    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "count", cJSON_GetArraySize(arr));
    cJSON_AddItemToObject(obj, "images", arr);
    cJSON_AddStringToObject(obj, "root", cfg.root().c_str());
    return obj;
}

// --- ToolShowImage ---
cJSON* SlideshowMCP::ToolShowImage(const cJSON* args) {
    auto& instance = GetInstance(args);
    const auto path = GetStr(args, "path");
    if (!path) {
        return CreateError("Missing 'path' argument");
    }
    if (!instance.EnsureSdCardMounted() || !instance.IsSafeMediaPath(*path)) {
        return CreateError("Invalid or unsafe path");
    }

    const auto trans_str = GetStr(args, "transition");
    Decoder decoder;
    const std::string full_path = std::string(kSdMountPoint) + "/" + *path;
    Rgb565Frame frame = decoder.DecodeFile(
        full_path, instance.config_->display_width(), instance.config_->display_height(),
        instance.config_->fit(), instance.config_->dither(),
        instance.config_->display_rgb565_swap(), instance.config_->max_decode_dimension());
    if (!frame.ok) {
        return CreateError("Decode failed: " + decoder.last_error());
    }

    // Transition handling: if a transition was requested, the current frame is
    // the previous one; otherwise show instantly.
    if (trans_str) {
        TransitionType trans = ParseTransition(*trans_str);
        instance.transition_engine_->SetEffect(trans);
    }

    instance.config_->display_width();
    instance.config_->display_height();

    instance.display_->ShowMediaFrame(reinterpret_cast<const uint8_t*>(frame.pixels.data()),
                                      frame.width, frame.height,
                                      frame.width * sizeof(uint16_t),
                                      DisplayRotationToDegrees(instance.config_->display_rotation()));

    return CreateResult("Image displayed on the MAX35; call self.sdcard.close_media to return");
}

// --- ToolStart ---
cJSON* SlideshowMCP::ToolStart(const cJSON* args) {
    auto& instance = GetInstance(args);

    // Apply playlist/transition/duration overrides.
    const auto playlist = GetStr(args, "playlist");
    if (playlist && instance.config_->playlists().count(*playlist)) {
        instance.config_->SetActivePlaylist(*playlist);
    }
    const auto trans_str = GetStr(args, "transition");
    if (trans_str) {
        instance.config_->SetTransition(ParseTransition(*trans_str));
    }
    const auto dur = GetNum(args, "slide_duration");
    if (dur && *dur >= 1.0 && *dur <= 300.0) {
        instance.config_->SetSlideDuration(static_cast<float>(*dur));
    }

    // Check SD card mount before starting the engine to avoid crashes.
    if (!instance.EnsureSdCardMounted()) {
        return CreateError("SD card is not mounted. The board must mount the SD card before the slideshow can start.");
    }

    instance.Log("info", "ToolStart: enqueuing kEventStart, images=%zu, root=%s, slide_duration=%.1f, transition=%s",
        instance.images_.size(), instance.config_->root().c_str(),
        instance.config_->slide_duration(), TransitionToString(instance.config_->transition()).c_str());

    instance.EnqueueEvent(static_cast<int>(slideshow::EngineEvent::kEventStart), 0);

    // Report the real engine inputs, not just "started": the engine processes
    // the event asynchronously, so include what it will play and the state it
    // is transitioning from.  This makes a bad root or empty scan visible in
    // the tool response instead of looking like a successful start.
    const char* state_str = instance.state() == SlideshowState::kPlaying
                                    ? "playing"
                                    : instance.state() == SlideshowState::kPaused ? "paused"
                                                                                  : "stopped";
    instance.Log("info", "ToolStart: state after enqueue = %s (engine processes asynchronously)", state_str);
    return CreateResult("Slideshow started: " + std::to_string(instance.images_.size()) +
                        " images from " + instance.config_->root() + " (state: " + state_str + ")");
}

// --- ToolStop ---
cJSON* SlideshowMCP::ToolStop(const cJSON* args) {
    (void)args;
    auto& instance = GetInstance(args);
    instance.EnqueueEvent(static_cast<int>(slideshow::EngineEvent::kEventStop), 0);
    instance.SaveResumePosition();
    return CreateResult("Slideshow stopped");
}

// --- ToolPause ---
cJSON* SlideshowMCP::ToolPause(const cJSON* args) {
    auto& instance = GetInstance(args);
    const bool pause = GetBool(args, "pause", true);
    instance.EnqueueEvent(static_cast<int>(slideshow::EngineEvent::kEventPause), pause ? 1 : 0);
    return CreateResult(pause ? "Slideshow paused" : "Slideshow resumed");
}

// --- ToolNext ---
cJSON* SlideshowMCP::ToolNext(const cJSON* args) {
    (void)args;
    auto& instance = GetInstance(args);
    instance.EnqueueEvent(static_cast<int>(slideshow::EngineEvent::kEventNext), 0);
    return CreateResult("Next slide");
}

// --- ToolPrev ---
cJSON* SlideshowMCP::ToolPrev(const cJSON* args) {
    (void)args;
    auto& instance = GetInstance(args);
    instance.EnqueueEvent(static_cast<int>(slideshow::EngineEvent::kEventPrev), 0);
    return CreateResult("Previous slide");
}

// --- ToolSetSlideDuration ---
cJSON* SlideshowMCP::ToolSetSlideDuration(const cJSON* args) {
    auto& instance = GetInstance(args);
    const auto dur = GetNum(args, "slide_duration");
    if (!dur || *dur < 1.0 || *dur > 300.0) {
        return CreateError("slide_duration must be between 1.0 and 300.0");
    }
    instance.config_->SetSlideDuration(static_cast<float>(*dur));
    return CreateResult("Slide duration set to " + std::to_string(*dur));
}

// --- ToolSetTransition ---
cJSON* SlideshowMCP::ToolSetTransition(const cJSON* args) {
    auto& instance = GetInstance(args);
    const auto trans_str = GetStr(args, "transition");
    if (!trans_str) {
        return CreateError("Missing 'transition' argument");
    }
    TransitionType trans = ParseTransition(*trans_str);
    instance.config_->SetTransition(trans);
    instance.transition_engine_->SetEffect(trans);
    return CreateResult("Transition set to " + std::string(TransitionToString(trans)));
}

// --- ToolSetPlaylist ---
cJSON* SlideshowMCP::ToolSetPlaylist(const cJSON* args) {
    auto& instance = GetInstance(args);
    const auto playlist = GetStr(args, "playlist");
    if (!playlist) {
        return CreateError("Missing 'playlist' argument");
    }
    if (!instance.config_->playlists().count(*playlist)) {
        return CreateError("Unknown playlist: " + *playlist);
    }
    instance.config_->SetActivePlaylist(*playlist);
    instance.EnqueueEvent(static_cast<int>(slideshow::EngineEvent::kEventStart), 0);  // trigger a rescan.
    return CreateResult("Playlist set to " + *playlist);
}

// --- ToolStatus ---
cJSON* SlideshowMCP::ToolStatus(const cJSON* args) {
    (void)args;
    auto& instance = GetInstance(args);
    auto& cfg = *instance.config_;

    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "state",
                            instance.state() == SlideshowState::kPlaying ? "playing"
                            : instance.state() == SlideshowState::kPaused    ? "paused"
                                                                             : "stopped");
    cJSON_AddNumberToObject(obj, "index", instance.current_index());
    cJSON_AddNumberToObject(obj, "count", static_cast<double>(instance.image_count()));
    cJSON_AddNumberToObject(obj, "last_index", instance.current_index());
    const std::string current_image =
        (instance.current_index() >= 0 &&
         instance.current_index() < static_cast<int>(instance.images_.size()))
            ? instance.images_[instance.current_index()]
            : std::string();
    cJSON_AddStringToObject(obj, "current_image", current_image.c_str());
    cJSON_AddNumberToObject(obj, "slide_duration",
                            static_cast<double>(cfg.slide_duration()));
    cJSON_AddStringToObject(obj, "transition",
                            TransitionToString(cfg.transition()).c_str());
    cJSON_AddStringToObject(obj, "active_playlist", cfg.active_playlist().c_str());
    cJSON_AddStringToObject(obj, "sd_health", instance.stats().sd_health.c_str());
    return obj;
}

// --- ToolImageList ---
cJSON* SlideshowMCP::ToolImageList(const cJSON* args) {
    (void)args;
    auto& instance = GetInstance(args);
    cJSON* arr = ImageListToJson(instance.images_,
                                 std::string(kSdMountPoint) + "/" + instance.config_->root(),
                                 instance.stats());
    if (arr == nullptr) return CreateError("Failed to build image list");

    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "count", cJSON_GetArraySize(arr));
    cJSON_AddItemToObject(obj, "images", arr);
    return obj;
}

// --- ToolConfig ---
cJSON* SlideshowMCP::ToolConfig(const cJSON* args) {
    (void)args;
    auto& instance = GetInstance(args);
    const std::string json_str = instance.config_->ToJson();
    cJSON* obj = cJSON_Parse(json_str.c_str());
    return obj == nullptr ? CreateError("Failed to serialize config") : obj;
}

// --- ToolReloadConfig ---
cJSON* SlideshowMCP::ToolReloadConfig(const cJSON* args) {
    (void)args;
    auto& instance = GetInstance(args);
    instance.config_->ReloadIfChanged();
    return CreateResult("Configuration reloaded");
}

// --- ToolStats ---
cJSON* SlideshowMCP::ToolStats(const cJSON* args) {
    (void)args;
    auto& instance = GetInstance(args);
    const SlideshowStats s = instance.stats();

    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "total_images", static_cast<double>(s.total_images));
    cJSON_AddNumberToObject(obj, "display_time_ms", static_cast<double>(s.display_time_ms));
    cJSON_AddNumberToObject(obj, "error_count", static_cast<double>(s.error_count));
    cJSON_AddStringToObject(obj, "sd_health", s.sd_health.c_str());
    cJSON_AddNumberToObject(obj, "uptime_ms", static_cast<double>(s.uptime_ms));
    cJSON_AddStringToObject(obj, "last_image", s.last_image.c_str());
    cJSON_AddNumberToObject(obj, "transitions", static_cast<double>(s.transitions));
    cJSON_AddNumberToObject(obj, "cache_hits", static_cast<double>(s.cache_hits));
    cJSON_AddNumberToObject(obj, "cache_misses", static_cast<double>(s.cache_misses));
    return obj;
}

// --- ToolTestPattern ---
cJSON* SlideshowMCP::ToolTestPattern(const cJSON* args) {
    (void)args;
    auto& instance = GetInstance(args);
    auto& cfg = *instance.config_;

    if (instance.display_ == nullptr) {
        return CreateError("Display not initialized");
    }

    const int w = cfg.display_width();
    const int h = cfg.display_height();

    // Build a test pattern (color bars).
    std::vector<uint16_t> pattern(static_cast<size_t>(w) * h);
    const int bar_w = w / 8;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int bar = x / bar_w;
            uint16_t color = 0;
            switch (bar) {
                case 0: color = 0xF800; break;   // red
                case 1: color = 0x07E0; break;   // green
                case 2: color = 0x001F; break;   // blue
                case 3: color = 0xFFFF; break;   // white
                case 4: color = 0xFFE0; break;   // yellow
                case 5: color = 0x7FFF; break;   // cyan
                case 6: color = 0x7F80; break;   // magenta
                case 7: color = 0x8430; break;   // gray
                default: color = 0x0000; break;
            }
            pattern[static_cast<size_t>(y) * w + x] = color;
        }
    }

    instance.display_->ShowMediaFrame(reinterpret_cast<const uint8_t*>(pattern.data()), w, h,
                                      w * sizeof(uint16_t),
                                      DisplayRotationToDegrees(cfg.display_rotation()));
    return CreateResult("Test pattern displayed");
}

// --- ToolBenchmark ---
cJSON* SlideshowMCP::ToolBenchmark(const cJSON* args) {
    auto& instance = GetInstance(args);
    const auto path = GetStr(args, "path");
    if (!path) {
        return CreateError("Missing 'path' argument");
    }
    if (!instance.EnsureSdCardMounted() || !instance.IsSafeMediaPath(*path)) {
        return CreateError("Invalid or unsafe path");
    }

    Decoder decoder;
    const std::string full_path = std::string(kSdMountPoint) + "/" + *path;

    esp_timer_handle_t timer = nullptr;
    esp_timer_create_args_t timer_args = {
        .callback = [](void*) {}, .name = "benchmark", .skip_unhandled_events = false};
    esp_timer_create(&timer_args, &timer);

    // Warm-up decode.
    Rgb565Frame frame = decoder.DecodeFile(full_path, instance.config_->display_width(),
                                           instance.config_->display_height(),
                                           instance.config_->fit(), instance.config_->dither(),
                                           instance.config_->display_rgb565_swap(),
                                           instance.config_->max_decode_dimension());
    if (!frame.ok) {
        return CreateError("Decode failed: " + decoder.last_error());
    }

    uint64_t decode_us = 0;
    for (int i = 0; i < 3; i++) {
        int64_t start = esp_timer_get_time();
        Rgb565Frame f = decoder.DecodeFile(full_path, instance.config_->display_width(),
                                           instance.config_->display_height(),
                                           instance.config_->fit(), instance.config_->dither(),
                                           instance.config_->display_rgb565_swap(),
                                           instance.config_->max_decode_dimension());
        decode_us += static_cast<uint64_t>((esp_timer_get_time() - start) / 1000);
        if (!f.ok) {
            return CreateError("Decode failed: " + decoder.last_error());
        }
    }

    const uint64_t avg_us = decode_us / 3;
    const float fps = static_cast<float>(1'000'000) / static_cast<float>(avg_us);
    const size_t decoded_bytes =
        static_cast<size_t>(frame.width) * frame.height * sizeof(uint16_t);

    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "path", path->c_str());
    cJSON_AddNumberToObject(obj, "width", frame.width);
    cJSON_AddNumberToObject(obj, "height", frame.height);
    cJSON_AddNumberToObject(obj, "decode_us", static_cast<double>(avg_us));
    cJSON_AddNumberToObject(obj, "fps", fps);
    cJSON_AddNumberToObject(obj, "decoded_bytes", static_cast<double>(decoded_bytes));
    return obj;
}

// --- ToolResume ---
cJSON* SlideshowMCP::ToolResume(const cJSON* args) {
    (void)args;
    auto& instance = GetInstance(args);
    int index = 0;
    std::string playlist;
    instance.LoadResumePosition(index, playlist);
    instance.config_->SetActivePlaylist(playlist);
    instance.EnqueueEvent(static_cast<int>(slideshow::EngineEvent::kEventStart), index);
    return CreateResult("Resumed from position " + std::to_string(index));
}

// --- ToolDeleteFile ---
cJSON* SlideshowMCP::ToolDeleteFile(const cJSON* args) {
    auto& instance = GetInstance(args);
    const auto path = GetStr(args, "path");
    if (!path || !instance.IsSafeMediaPath(*path)) {
        return CreateError("Invalid or unsafe path");
    }
    if (!instance.EnsureSdCardMounted()) {
        return CreateError("Could not mount the SD card");
    }

    const std::string full_path = std::string(kSdMountPoint) + "/" + *path;
    if (remove(full_path.c_str()) != 0) {
        return CreateError("Could not delete the file: " + std::string(strerror(errno)));
    }
    instance.EnqueueEvent(static_cast<int>(slideshow::EngineEvent::kEventStart), 0);
    return CreateResult("Deleted " + *path);
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

SlideshowMCP* SlideshowMCP::instance_ = nullptr;

SlideshowMCP::SlideshowMCP(Display* display, const std::string& config_path)
    : display_(display), config_path_(config_path) {
    instance_ = this;
    config_ = std::make_unique<Config>();
    config_->Load(config_path_, false);
    config_->Validate();

    transition_engine_ = std::make_unique<TransitionEngine>(
        config_->transition(), config_->transition_duration(),
        config_->ken_burns(), config_->ken_burns_intensity());
    transition_engine_->SetStepMs(kDefaultStepMs);

    // Events queue: capacity for pending commands.
    event_queue_ = xQueueCreate(16, sizeof(EngineEventMsg));

    last_error_.clear();
    stats_.sd_health = "not_mounted";

    // Load resume position.
    int index = -1;
    std::string playlist;
    LoadResumePosition(index, playlist);
    if (index >= 0 && !images_.empty() && index < static_cast<int>(images_.size())) {
        this->index_ = index;
    }

    // Load stats from NVS.
    if (config_->statistics_enabled()) {
        config_->LoadStats(stats_);
    }

    Log("info", "SlideshowMCP initialized on %s (config: %s)",
        display != nullptr ? "display" : "no-display", config_path.c_str());
}

SlideshowMCP::~SlideshowMCP() {
    shutdown_.store(true);
    if (engine_task_handle_ != nullptr) {
        vTaskDelete(engine_task_handle_);
        engine_task_handle_ = nullptr;
    }
    if (event_queue_ != nullptr) {
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
    }
    instance_ = nullptr;
}

bool SlideshowMCP::Initialize() {
    // Image scanning is deferred to the engine task (need_scan / images_.empty()).
    // Doing SD I/O here blocks the main task during board init and can trigger
    // the task watchdog (TG1WDT_SYS_RST) on boot.

    // Start the engine task.
    if (xTaskCreate(EngineTask, "slideshow_engine", kEngineTaskStackSize, this, 5,
                    &engine_task_handle_) != pdPASS) {
        Log("error", "Failed to create engine task");
        return false;
    }

    return true;
}

namespace {
// Converts a cJSON* tool result into a ToolResult (nullptr -> error).
ToolResult ToToolResult(cJSON* result) {
    if (result == nullptr) {
        return std::unexpected(std::string("No result"));
    }
    return ReturnValue{result};
}
}  // namespace

void SlideshowMCP::RegisterTools(McpServer* mcp_server) {
    if (mcp_server == nullptr) {
        return;
    }

    const std::string prefix = "self.sdcard.slideshow.";

    mcp_server->AddTool(prefix + "list_images",
                        "List images on the microSD card. Supports 'recursive' (default true), "
                        "'sort' (natural|exif|name|mtime) and 'shuffle' (default false).",
                        PropertyList({Property("recursive", kPropertyTypeBoolean, false),
                                      Property("sort", kPropertyTypeString),
                                      Property("shuffle", kPropertyTypeBoolean, false)}),
                        [](const PropertyList& props) -> ToolResult {
                            cJSON* args = cJSON_CreateObject();
                            if (props.has("recursive")) {
                                cJSON_AddBoolToObject(args, "recursive",
                                                      props["recursive"].value<bool>());
                            }
                            if (props.has("sort")) {
                                cJSON_AddStringToObject(args, "sort",
                                                        props["sort"].value<std::string>().c_str());
                            }
                            if (props.has("shuffle")) {
                                cJSON_AddBoolToObject(args, "shuffle",
                                                      props["shuffle"].value<bool>());
                            }
                            auto result = ToolListImages(args);
                            cJSON_Delete(args);
                            return ToToolResult(result);
                        });

    mcp_server->AddTool(prefix + "show_image",
                        "Display an image from the SD card fullscreen. Supports 'path' and "
                        "'transition' (fade|slide|wipe|zoom|none).",
                        PropertyList({Property("path", kPropertyTypeString),
                                      Property("transition", kPropertyTypeString)}),
                        [](const PropertyList& props) -> ToolResult {
                            cJSON* args = cJSON_CreateObject();
                            if (props.has("path")) {
                                cJSON_AddStringToObject(args, "path",
                                                        props["path"].value<std::string>().c_str());
                            }
                            if (props.has("transition")) {
                                cJSON_AddStringToObject(args, "transition",
                                                        props["transition"].value<std::string>().c_str());
                            }
                            auto result = ToolShowImage(args);
                            cJSON_Delete(args);
                            return ToToolResult(result);
                        });

    mcp_server->AddTool(prefix + "start",
                        "Start the slideshow engine. Supports 'playlist', 'transition' and "
                        "'slide_duration' (1-300).",
                        PropertyList({Property("playlist", kPropertyTypeString),
                                      Property("transition", kPropertyTypeString),
                                      Property("slide_duration", kPropertyTypeInteger, 8)}),
                        [](const PropertyList& props) -> ToolResult {
                            cJSON* args = cJSON_CreateObject();
                            if (props.has("playlist")) {
                                cJSON_AddStringToObject(args, "playlist",
                                                        props["playlist"].value<std::string>().c_str());
                            }
                            if (props.has("transition")) {
                                cJSON_AddStringToObject(args, "transition",
                                                        props["transition"].value<std::string>().c_str());
                            }
                            if (props.has("slide_duration")) {
                                cJSON_AddNumberToObject(args, "slide_duration",
                                                        props["slide_duration"].value<int>());
                            }
                            auto result = ToolStart(args);
                            cJSON_Delete(args);
                            return ToToolResult(result);
                        });

    mcp_server->AddTool(prefix + "stop",
                        "Stop the slideshow engine.", PropertyList(),
                        [](const PropertyList&) -> ToolResult {
                            return ToolStop(nullptr);
                        });

#if 0  // Reduced tool surface (device tool limit). Re-enable after base verification.
    mcp_server->AddTool(prefix + "pause",
                        "Pause or resume the slideshow engine. Supports 'pause' (default true).",
                        PropertyList({Property("pause", kPropertyTypeBoolean, true)}),
                        [](const PropertyList& props) -> ToolResult {
                            cJSON* args = cJSON_CreateObject();
                            cJSON_AddBoolToObject(args, "pause",
                                                  props["pause"].value<bool>());
                            auto result = ToolPause(args);
                            cJSON_Delete(args);
                            return ToToolResult(result);
                        });

#endif

    mcp_server->AddTool(prefix + "next",
                        "Advance to the next image.", PropertyList(),
                        [](const PropertyList&) -> ToolResult {
                            return ToolNext(nullptr);
                        });

    mcp_server->AddTool(prefix + "prev",
                        "Go to the previous image.", PropertyList(),
                        [](const PropertyList&) -> ToolResult {
                            return ToolPrev(nullptr);
                        });

#if 0
    mcp_server->AddTool(prefix + "set_slide_duration",
                        "Set the slide duration in seconds (1-300).",
                        PropertyList({Property("slide_duration", kPropertyTypeInteger, 8)}),
                        [](const PropertyList& props) -> ToolResult {
                            cJSON* args = cJSON_CreateObject();
                            cJSON_AddNumberToObject(args, "slide_duration",
                                                    props["slide_duration"].value<int>());
                            auto result = ToolSetSlideDuration(args);
                            cJSON_Delete(args);
                            return ToToolResult(result);
                        });

    mcp_server->AddTool(prefix + "set_transition",
                        "Set the transition effect (fade|slide|wipe|zoom|none).",
                        PropertyList({Property("transition", kPropertyTypeString)}),
                        [](const PropertyList& props) -> ToolResult {
                            cJSON* args = cJSON_CreateObject();
                            cJSON_AddStringToObject(args, "transition",
                                                    props["transition"].value<std::string>().c_str());
                            auto result = ToolSetTransition(args);
                            cJSON_Delete(args);
                            return ToToolResult(result);
                        });

    mcp_server->AddTool(prefix + "set_playlist",
                        "Set the active playlist.", PropertyList({Property("playlist", kPropertyTypeString)}),
                        [](const PropertyList& props) -> ToolResult {
                            cJSON* args = cJSON_CreateObject();
                            cJSON_AddStringToObject(args, "playlist",
                                                    props["playlist"].value<std::string>().c_str());
                            auto result = ToolSetPlaylist(args);
                            cJSON_Delete(args);
                            return ToToolResult(result);
                        });

    mcp_server->AddTool(prefix + "status",
                        "Return the current playback status (state, index, image, etc.).",
                        PropertyList(), [](const PropertyList&) -> ToolResult {
                            return ToolStatus(nullptr);
                        });

    mcp_server->AddTool(prefix + "image_list",
                        "Return the current image list as JSON.", PropertyList(),
                        [](const PropertyList&) -> ToolResult {
                            return ToolImageList(nullptr);
                        });

    mcp_server->AddTool(prefix + "config",
                        "Return the current configuration (validated copy) as JSON.", PropertyList(),
                        [](const PropertyList&) -> ToolResult {
                            return ToolConfig(nullptr);
                        });

    mcp_server->AddTool(prefix + "reload_config",
                        "Reload the configuration file without restarting the engine.", PropertyList(),
                        [](const PropertyList&) -> ToolResult {
                            return ToolReloadConfig(nullptr);
                        });

    mcp_server->AddTool(prefix + "stats",
                        "Return runtime statistics (images shown, errors, SD health, memory).",
                        PropertyList(), [](const PropertyList&) -> ToolResult {
                            return ToolStats(nullptr);
                        });

    mcp_server->AddTool(prefix + "test_pattern",
                        "Draw the built-in test pattern for hardware validation.", PropertyList(),
                        [](const PropertyList&) -> ToolResult {
                            return ToolTestPattern(nullptr);
                        });

    mcp_server->AddTool(prefix + "benchmark",
                        "Benchmark decode performance. Supports 'path'.",
                        PropertyList({Property("path", kPropertyTypeString)}),
                        [](const PropertyList& props) -> ToolResult {
                            cJSON* args = cJSON_CreateObject();
                            if (props.has("path")) {
                                cJSON_AddStringToObject(args, "path",
                                                        props["path"].value<std::string>().c_str());
                            }
                            auto result = ToolBenchmark(args);
                            cJSON_Delete(args);
                            return ToToolResult(result);
                        });

    mcp_server->AddTool(prefix + "resume",
                        "Resume the previously saved playback position.", PropertyList(),
                        [](const PropertyList&) -> ToolResult {
                            return ToolResume(nullptr);
                        });

    mcp_server->AddTool(prefix + "delete_file",
                        "Delete a file from the SD card. Supports 'path'.",
                        PropertyList({Property("path", kPropertyTypeString)}),
                        [](const PropertyList& props) -> ToolResult {
                            cJSON* args = cJSON_CreateObject();
                            if (props.has("path")) {
                                cJSON_AddStringToObject(args, "path",
                                                        props["path"].value<std::string>().c_str());
                            }
                            auto result = ToolDeleteFile(args);
                            cJSON_Delete(args);
                            return ToToolResult(result);
                        });

#endif

    Log("info", "Registered %zu slideshow tools", 6);
}

SlideshowMCP& SlideshowMCP::GetInstance(const cJSON* args) {
    (void)args;
    if (instance_ == nullptr) {
        // Default construction would require a Display pointer.  For tool calls
        // that happen before Initialize(), this is an error path.
        return *instance_;  // Caller must handle nullptr -> fallback.
    }
    return *instance_;
}

}  // namespace slideshow
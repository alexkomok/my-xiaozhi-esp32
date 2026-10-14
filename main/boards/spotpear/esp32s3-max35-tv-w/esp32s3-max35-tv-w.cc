#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "esp32_camera.h"
#include "mcp_server.h"
#include "wifi_board.h"

#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <jpeglib.h>
#include <sdmmc_cmd.h>
#include <sys/stat.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <csetjmp>
#include <new>
#include <string>

#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <driver/sdmmc_host.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

typedef struct {
    int cmd;
    const void* data;
    size_t data_bytes;
    unsigned int delay_ms;
} st7796_lcd_init_cmd_t;

typedef struct {
    const st7796_lcd_init_cmd_t* init_cmds;
    uint16_t init_cmds_size;
} st7796_vendor_config_t;

namespace {

constexpr size_t kMaxMjpegScanBytes = 8 * 1024 * 1024;
constexpr size_t kMaxMjpegPlaybackBytes = 128 * 1024 * 1024;
constexpr size_t kMaxMjpegFrameBytes = 512 * 1024;

struct JpegErrorManager {
    jpeg_error_mgr base;
    jmp_buf jump_buffer;
};

struct JpegDecodeContext {
    jpeg_decompress_struct decoder;
    JpegErrorManager error;
    uint8_t* rgb565;
    bool decoder_created;
};

void JpegErrorExit(j_common_ptr common) {
    auto* error = reinterpret_cast<JpegErrorManager*>(common->err);
    longjmp(error->jump_buffer, 1);
}

// Extracts the next FFD8..FFD9 JPEG frame from a raw MJPEG stream; false at end, limit or error.
bool ReadNextMjpegFrame(FILE* file, std::string& frame, size_t& bytes_remaining) {
    std::array<uint8_t, 4096> buffer{};
    int previous_byte = -1;
    bool in_frame = false;
    frame.clear();

    while (bytes_remaining > 0) {
        size_t bytes_to_read = std::min(buffer.size(), bytes_remaining);
        size_t bytes_read = fread(buffer.data(), 1, bytes_to_read, file);
        if (bytes_read == 0) {
            return false;
        }
        bytes_remaining -= bytes_read;

        for (size_t i = 0; i < bytes_read; i++) {
            int current_byte = buffer[i];
            if (!in_frame) {
                if (previous_byte == 0xff && current_byte == 0xd8) {
                    frame.assign(1, static_cast<char>(0xff));
                    frame.push_back(static_cast<char>(0xd8));
                    in_frame = true;
                }
            } else {
                frame.push_back(static_cast<char>(current_byte));
                if (frame.size() > kMaxMjpegFrameBytes) {
                    frame.clear();
                    return false;
                }
                if (previous_byte == 0xff && current_byte == 0xd9) {
                    return true;
                }
            }
            previous_byte = current_byte;
        }
    }
    return false;
}

// Decodes a JPEG (baseline or progressive) downscaled to fit 512 px and shows it fullscreen.
bool DecodeJpegAndShow(Display* display, const uint8_t* data, size_t size,
                       const std::atomic<bool>* stop_requested = nullptr) {
    constexpr size_t kMaxJpegInputBytes = 8 * 1024 * 1024;
    constexpr size_t kMaxDecodedBytes = 2 * 1024 * 1024;
    constexpr size_t kMaxDisplayDimension = 512;
    if (data == nullptr || size == 0 || size > kMaxJpegInputBytes) {
        ESP_LOGE("MAX35_SD", "JPEG input size is invalid: %u bytes", static_cast<unsigned>(size));
        return false;
    }

    std::unique_ptr<JpegDecodeContext> context(new (std::nothrow) JpegDecodeContext{});
    if (context == nullptr) {
        return false;
    }

    context->decoder.err = jpeg_std_error(&context->error.base);
    context->error.base.error_exit = JpegErrorExit;
    if (setjmp(context->error.jump_buffer) != 0) {
        if (context->decoder_created) {
            jpeg_destroy_decompress(&context->decoder);
        }
        heap_caps_free(context->rgb565);
        ESP_LOGE("MAX35_SD", "libjpeg-turbo rejected or failed to decode the JPEG");
        return false;
    }

    jpeg_create_decompress(&context->decoder);
    context->decoder_created = true;
    jpeg_mem_src(&context->decoder, data, static_cast<unsigned long>(size));
    if (jpeg_read_header(&context->decoder, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&context->decoder);
        return false;
    }

    context->decoder.out_color_space = JCS_RGB;
    bool output_fits = false;
    const unsigned int scale_denominators[] = {1, 2, 4, 8};
    for (unsigned int denominator : scale_denominators) {
        context->decoder.scale_num = 1;
        context->decoder.scale_denom = denominator;
        jpeg_calc_output_dimensions(&context->decoder);
        size_t output_size = static_cast<size_t>(context->decoder.output_width) *
                             context->decoder.output_height * sizeof(uint16_t);
        if (context->decoder.output_width > 0 && context->decoder.output_height > 0 &&
            std::max(context->decoder.output_width, context->decoder.output_height) <=
                kMaxDisplayDimension &&
            output_size <= kMaxDecodedBytes) {
            output_fits = true;
            break;
        }
    }
    if (!output_fits || !jpeg_start_decompress(&context->decoder) ||
        context->decoder.output_components != 3) {
        jpeg_destroy_decompress(&context->decoder);
        context->decoder_created = false;
        return false;
    }

    const size_t width = context->decoder.output_width;
    const size_t height = context->decoder.output_height;
    const size_t output_size = width * height * sizeof(uint16_t);
    context->rgb565 = static_cast<uint8_t*>(
        heap_caps_malloc(output_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (context->rgb565 == nullptr) {
        jpeg_destroy_decompress(&context->decoder);
        context->decoder_created = false;
        return false;
    }

    JSAMPARRAY row = (*context->decoder.mem->alloc_sarray)(
        reinterpret_cast<j_common_ptr>(&context->decoder), JPOOL_IMAGE,
        context->decoder.output_width * context->decoder.output_components, 1);
    auto* destination = reinterpret_cast<uint16_t*>(context->rgb565);
    while (context->decoder.output_scanline < context->decoder.output_height) {
        if ((stop_requested != nullptr && stop_requested->load()) ||
            jpeg_read_scanlines(&context->decoder, row, 1) != 1) {
            jpeg_destroy_decompress(&context->decoder);
            context->decoder_created = false;
            heap_caps_free(context->rgb565);
            return false;
        }
        size_t y = context->decoder.output_scanline - 1;
        for (size_t x = 0; x < width; x++) {
            const uint8_t* pixel = row[0] + x * 3;
            destination[y * width + x] = ((pixel[0] >> 3) << 11) |
                                         ((pixel[1] >> 2) << 5) | (pixel[2] >> 3);
        }
    }

    bool decoded = jpeg_finish_decompress(&context->decoder);
    jpeg_destroy_decompress(&context->decoder);
    context->decoder_created = false;
    if (!decoded) {
        heap_caps_free(context->rgb565);
        return false;
    }
    bool shown = stop_requested == nullptr || !stop_requested->load()
                     ? display->ShowMediaFrame(context->rgb565, width, height,
                                               width * sizeof(uint16_t))
                     : false;
    heap_caps_free(context->rgb565);
    return shown;
}

}  // namespace

static const uint8_t kColmod[] = {0x05};
static const uint8_t kF0Unlock1[] = {0xC3};
static const uint8_t kF0Unlock2[] = {0x96};
static const uint8_t kB4[] = {0x01};
static const uint8_t kB7[] = {0xC6};
static const uint8_t kC0[] = {0x80, 0x45};
static const uint8_t kC1[] = {0x13};
static const uint8_t kC2[] = {0xA7};
static const uint8_t kC5[] = {0x0A};
static const uint8_t kE8[] = {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33};
static const uint8_t kE0[] = {0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30,
                              0x33, 0x47, 0x17, 0x13, 0x13, 0x2B, 0x31};
static const uint8_t kE1[] = {0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F,
                              0x33, 0x47, 0x38, 0x15, 0x16, 0x2C, 0x32};
static const uint8_t kF0Lock1[] = {0x3C};
static const uint8_t kF0Lock2[] = {0x69};

static const st7796_lcd_init_cmd_t kSt7796InitCommands[] = {
    {0x11, nullptr, 0, 120},
    {0x3A, kColmod, sizeof(kColmod), 0},
    {0xF0, kF0Unlock1, sizeof(kF0Unlock1), 0},
    {0xF0, kF0Unlock2, sizeof(kF0Unlock2), 0},
    {0xB4, kB4, sizeof(kB4), 0},
    {0xB7, kB7, sizeof(kB7), 0},
    {0xC0, kC0, sizeof(kC0), 0},
    {0xC1, kC1, sizeof(kC1), 0},
    {0xC2, kC2, sizeof(kC2), 0},
    {0xC5, kC5, sizeof(kC5), 0},
    {0xE8, kE8, sizeof(kE8), 0},
    {0xE0, kE0, sizeof(kE0), 0},
    {0xE1, kE1, sizeof(kE1), 0},
    {0xF0, kF0Lock1, sizeof(kF0Lock1), 0},
    {0xF0, kF0Lock2, sizeof(kF0Lock2), 120},
    {0x21, nullptr, 0, 0},
    {0x29, nullptr, 0, 0},
};

class SpotpearMax35TvBoard : public WifiBoard {
private:
    Button boot_button_;
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    LcdDisplay* display_ = nullptr;
    Esp32Camera* camera_ = nullptr;
    sdmmc_card_t* sd_card_ = nullptr;
    bool sd_card_mounted_ = false;
    std::atomic<bool> video_playing_{false};
    std::atomic<bool> video_stop_requested_{false};
    std::atomic<bool> image_shown_{false};

    struct VideoPlaybackRequest {
        SpotpearMax35TvBoard* board;
        std::string path;
        bool loop;
    };

    void RunVideoPlayback(const std::string& path, bool loop) {
        const std::string full_path = std::string(SD_CARD_MOUNT_POINT) + "/" + path;
        FILE* file = fopen(full_path.c_str(), "rb");
        if (file == nullptr) {
            ESP_LOGE("MAX35_SD", "Could not open video file: %s", path.c_str());
            return;
        }

        auto display = GetDisplay();
        size_t bytes_remaining = kMaxMjpegPlaybackBytes;
        while (video_playing_.load() && !video_stop_requested_.load()) {
            std::string frame;
            if (!ReadNextMjpegFrame(file, frame, bytes_remaining)) {
                if (loop && !video_stop_requested_.load() && fseek(file, 0, SEEK_SET) == 0) {
                    bytes_remaining = kMaxMjpegPlaybackBytes;
                    continue;
                }
                break;
            }

            if (!DecodeJpegAndShow(display, reinterpret_cast<const uint8_t*>(frame.data()),
                                   frame.size(), &video_stop_requested_)) {
                if (!video_stop_requested_.load()) {
                    ESP_LOGE("MAX35_SD", "Could not display decoded frame from %s", path.c_str());
                }
                break;
            }
        }

        fclose(file);
        display->ClearMediaFrame();
        video_playing_.store(false);
    }

    static void VideoPlaybackTask(void* arg) {
        std::unique_ptr<VideoPlaybackRequest> request(static_cast<VideoPlaybackRequest*>(arg));
        request->board->RunVideoPlayback(request->path, request->loop);
        request->board->video_playing_.store(false);
        vTaskDelete(nullptr);
    }

    bool StartVideoPlayback(const std::string& path, bool loop) {
        auto request = std::make_unique<VideoPlaybackRequest>(VideoPlaybackRequest{this, path, loop});
        bool expected = false;
        if (!video_playing_.compare_exchange_strong(expected, true)) {
            return false;
        }
        video_stop_requested_.store(false);

        VideoPlaybackRequest* task_request = request.release();
        if (xTaskCreate(VideoPlaybackTask, "sd_video", 8192, task_request, 3, nullptr) != pdPASS) {
            delete task_request;
            video_playing_.store(false);
            return false;
        }
        return true;
    }

    bool EnsureSdCardMounted() {
        if (sd_card_mounted_) {
            return true;
        }

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;

        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.clk = SD_CARD_CLK_PIN;
        slot.cmd = SD_CARD_CMD_PIN;
        slot.d0 = SD_CARD_D0_PIN;
        slot.width = SD_CARD_BUS_WIDTH;
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        esp_vfs_fat_mount_config_t mount_config = VFS_FAT_MOUNT_DEFAULT_CONFIG();
        mount_config.max_files = 8;
        mount_config.allocation_unit_size = 16 * 1024;

        esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_CARD_MOUNT_POINT, &host, &slot, &mount_config,
                                                &sd_card_);
        if (err != ESP_OK) {
            ESP_LOGE("MAX35_SD", "Failed to mount SD card: %s", esp_err_to_name(err));
            return false;
        }

        sd_card_mounted_ = true;
        sdmmc_card_print_info(stdout, sd_card_);
        return true;
    }

    static bool IsSafeMediaPath(const std::string& path) {
        return !path.empty() && path.front() != '/' && path.find('\\') == std::string::npos &&
               path.find(':') == std::string::npos && path.find("..") == std::string::npos;
    }

    static std::string LowercaseExtension(const std::string& path) {
        size_t dot = path.find_last_of('.');
        if (dot == std::string::npos) {
            return {};
        }
        std::string extension = path.substr(dot);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return extension;
    }

    static const char* MediaKind(const std::string& path) {
        const std::string extension = LowercaseExtension(path);
        if (extension == ".jpg" || extension == ".jpeg" || extension == ".png") {
            return "image";
        }
        if (extension == ".mjpeg" || extension == ".mjpg" || extension == ".mjp") {
            return "video";
        }
        if (extension == ".mp3") {
            return "audio";
        }
        return nullptr;
    }

    // only_kind == nullptr lists every media kind.
    static void AppendMediaFiles(const std::string& relative_dir, int depth, size_t& count,
                                 cJSON* files, const char* only_kind = nullptr) {
        if (depth > 4 || count >= 100) {
            return;
        }

        std::string directory = SD_CARD_MOUNT_POINT;
        if (!relative_dir.empty()) {
            directory += "/" + relative_dir;
        }
        DIR* dir = opendir(directory.c_str());
        if (dir == nullptr) {
            return;
        }

        while (count < 100) {
            dirent* entry = readdir(dir);
            if (entry == nullptr) {
                break;
            }
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
                entry->d_name[0] == '.') {
                continue;
            }

            std::string relative_path = relative_dir.empty()
                                            ? entry->d_name
                                            : relative_dir + "/" + entry->d_name;
            std::string full_path = std::string(SD_CARD_MOUNT_POINT) + "/" + relative_path;
            struct stat info = {};
            if (stat(full_path.c_str(), &info) != 0) {
                continue;
            }
            if (S_ISDIR(info.st_mode)) {
                AppendMediaFiles(relative_path, depth + 1, count, files, only_kind);
                continue;
            }

            const char* kind = MediaKind(relative_path);
            if (kind == nullptr || (only_kind != nullptr && strcmp(kind, only_kind) != 0)) {
                continue;
            }

            cJSON* item = cJSON_CreateObject();
            if (item == nullptr) {
                break;
            }
            cJSON_AddStringToObject(item, "path", relative_path.c_str());
            cJSON_AddStringToObject(item, "kind", kind);
            cJSON_AddNumberToObject(item, "size_bytes", static_cast<double>(info.st_size));
            if (!cJSON_AddItemToArray(files, item)) {
                cJSON_Delete(item);
                break;
            }
            count++;
        }
        closedir(dir);
    }

    // Lists media files as JSON; only_kind is "image", "video", "audio" or nullptr for all.
    ToolResult ListMedia(const char* only_kind) {
        if (!EnsureSdCardMounted()) {
            return std::unexpected("Could not mount the SD card");
        }

        CJsonUniquePtr result(cJSON_CreateObject());
        CJsonUniquePtr files(cJSON_CreateArray());
        if (result == nullptr || files == nullptr) {
            return std::unexpected("Failed to allocate the media list");
        }
        size_t count = 0;
        AppendMediaFiles("", 0, count, files.get(), only_kind);
        cJSON_AddNumberToObject(result.get(), "count", static_cast<double>(count));
        cJSON_AddItemToObject(result.get(), "files", files.release());
        return ReturnValue{result.release()};
    }

    // Returns true when something was on screen and has been dismissed.
    bool CloseMedia() {
        bool closed = false;
        if (video_playing_.load()) {
            video_stop_requested_.store(true);
            closed = true;
        }
        if (image_shown_.exchange(false)) {
            display_->ClearMediaFrame();
            closed = true;
        }
        return closed;
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool(
            "self.sdcard.list_media",
            "List all image, MJPEG video, and MP3 audio files on the MAX35 microSD card. Paths are "
            "relative to the card root and can be passed to the corresponding media tools.",
            PropertyList(), [this](const PropertyList&) -> ToolResult { return ListMedia(nullptr); });
        mcp_server.AddTool(
            "self.sdcard.list_images",
            "List only the JPG/JPEG/PNG pictures on the microSD card, including camera photos.",
            PropertyList(), [this](const PropertyList&) -> ToolResult { return ListMedia("image"); });
        mcp_server.AddTool(
            "self.sdcard.list_videos", "List only the MJPEG videos on the microSD card.",
            PropertyList(), [this](const PropertyList&) -> ToolResult { return ListMedia("video"); });
        mcp_server.AddTool(
            "self.sdcard.list_music", "List only the MP3 music files on the microSD card.",
            PropertyList(), [this](const PropertyList&) -> ToolResult { return ListMedia("audio"); });
        mcp_server.AddTool(
            "self.sdcard.close_media",
            "Close the fullscreen picture or stop the video and return to the chat screen.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue { return CloseMedia(); });
        mcp_server.AddTool(
            "self.sdcard.save_camera_photo",
            "Take a photo with the camera and save it as a JPEG in the photos folder on the "
            "microSD card. Returns the saved path.",
            PropertyList(), [this](const PropertyList&) -> ToolResult {
                if (!EnsureSdCardMounted()) {
                    return std::unexpected("Could not mount the SD card");
                }
                const std::string directory = std::string(SD_CARD_MOUNT_POINT) + "/photos";
                if (mkdir(directory.c_str(), 0775) != 0 && errno != EEXIST) {
                    return std::unexpected("Could not create the photos folder");
                }

                std::string relative_path;
                struct stat info = {};
                for (int index = 1; index < 10000; index++) {
                    char name[32];
                    snprintf(name, sizeof(name), "photos/IMG_%04d.jpg", index);
                    const std::string full = std::string(SD_CARD_MOUNT_POINT) + "/" + name;
                    if (stat(full.c_str(), &info) != 0) {
                        relative_path = name;
                        break;
                    }
                }
                if (relative_path.empty()) {
                    return std::unexpected("The photos folder is full");
                }
                if (!camera_->Capture()) {
                    return std::unexpected("Camera capture failed");
                }
                if (!camera_->SaveJpeg((std::string(SD_CARD_MOUNT_POINT) + "/" + relative_path).c_str())) {
                    return std::unexpected("Could not encode or write the photo");
                }
                return ReturnValue{relative_path};
            });

        Property image_path("path", kPropertyTypeString);
        image_path.SetMaxLength(128);
        mcp_server.AddTool(
            "self.sdcard.read_image",
            "Read an image from the SD card and return it to the AI for visual analysis. "
            "Supports JPEG and PNG files listed by self.sdcard.list_media.",
            PropertyList({image_path}), [this](const PropertyList& properties) -> ToolResult {
                const std::string path = properties["path"].value<std::string>();
                const std::string extension = LowercaseExtension(path);
                if (!IsSafeMediaPath(path) ||
                    (extension != ".jpg" && extension != ".jpeg" && extension != ".png")) {
                    return std::unexpected("Path must name a JPG, JPEG, or PNG file on the SD card");
                }
                if (!EnsureSdCardMounted()) {
                    return std::unexpected("Could not mount the SD card");
                }

                const std::string full_path = std::string(SD_CARD_MOUNT_POINT) + "/" + path;
                FILE* file = fopen(full_path.c_str(), "rb");
                if (file == nullptr) {
                    return std::unexpected("Could not open the image file");
                }
                if (fseek(file, 0, SEEK_END) != 0) {
                    fclose(file);
                    return std::unexpected("Could not determine image size");
                }
                long file_size = ftell(file);
                if (file_size <= 0 || file_size > 8 * 1024 * 1024 ||
                    fseek(file, 0, SEEK_SET) != 0) {
                    fclose(file);
                    return std::unexpected("Image must be between 1 byte and 8 MB");
                }
                std::string data(static_cast<size_t>(file_size), '\0');
                size_t bytes_read = fread(data.data(), 1, data.size(), file);
                fclose(file);
                if (bytes_read != data.size()) {
                    return std::unexpected("Failed to read the complete image");
                }

                const char* mime_type = extension == ".png" ? "image/png" : "image/jpeg";
                return ReturnValue{new ImageContent(mime_type, data)};
            });

        Property display_image_path("path", kPropertyTypeString);
        display_image_path.SetMaxLength(128);
        mcp_server.AddTool(
            "self.sdcard.show_image",
            "Display a JPEG or PNG image fullscreen on the MAX35. Use a path returned by "
            "self.sdcard.list_media. This is for on-device viewing; use read_image for AI analysis.",
            PropertyList({display_image_path}), [this](const PropertyList& properties) -> ToolResult {
                const std::string path = properties["path"].value<std::string>();
                const std::string extension = LowercaseExtension(path);
                if (!IsSafeMediaPath(path) ||
                    (extension != ".jpg" && extension != ".jpeg" && extension != ".png")) {
                    return std::unexpected("Path must name a JPG, JPEG, or PNG file on the SD card");
                }
                if (!EnsureSdCardMounted()) {
                    return std::unexpected("Could not mount the SD card");
                }

                const std::string full_path = std::string(SD_CARD_MOUNT_POINT) + "/" + path;
                FILE* file = fopen(full_path.c_str(), "rb");
                if (file == nullptr || fseek(file, 0, SEEK_END) != 0) {
                    if (file != nullptr) {
                        fclose(file);
                    }
                    return std::unexpected("Could not open the image file");
                }
                long file_size = ftell(file);
                if (file_size <= 0 || file_size > 8 * 1024 * 1024 ||
                    fseek(file, 0, SEEK_SET) != 0) {
                    fclose(file);
                    return std::unexpected("Image must be between 1 byte and 8 MB");
                }
                std::string data(static_cast<size_t>(file_size), '\0');
                size_t bytes_read = fread(data.data(), 1, data.size(), file);
                fclose(file);
                if (bytes_read != data.size()) {
                    return std::unexpected("Failed to read the complete image");
                }
                const auto* image_data = reinterpret_cast<const uint8_t*>(data.data());
                if (extension == ".png") {
                    if (data.size() < 24 ||
                        memcmp(image_data, "\x89PNG\r\n\x1a\n", 8) != 0) {
                        return std::unexpected("PNG signature or header is invalid");
                    }
                    const uint32_t width = (static_cast<uint32_t>(image_data[16]) << 24) |
                                           (static_cast<uint32_t>(image_data[17]) << 16) |
                                           (static_cast<uint32_t>(image_data[18]) << 8) |
                                           image_data[19];
                    const uint32_t height = (static_cast<uint32_t>(image_data[20]) << 24) |
                                            (static_cast<uint32_t>(image_data[21]) << 16) |
                                            (static_cast<uint32_t>(image_data[22]) << 8) |
                                            image_data[23];
                    if (width == 0 || height == 0) {
                        return std::unexpected("PNG dimensions are invalid");
                    }
                    if (width > 1024 * 1024 / height) {
                        char error[128];
                        snprintf(error, sizeof(error),
                                 "PNG is %lux%lu (%lu pixels); fullscreen limit is 1 megapixel",
                                 static_cast<unsigned long>(width),
                                 static_cast<unsigned long>(height),
                                 static_cast<unsigned long>(width * height));
                        return std::unexpected(error);
                    }
                }
                bool displayed = extension == ".png"
                                     ? display_->ShowPngImage(image_data, data.size())
                                     : DecodeJpegAndShow(display_, image_data, data.size());
                if (!displayed) {
                    return std::unexpected(
                        extension == ".png"
                            ? "PNG decoding failed; retry with a standard non-animated PNG"
                            : "JPEG display failed; progressive JPEG encoding may not be supported");
                }
                image_shown_.store(true);
                return ReturnValue{std::string("Image displayed on the MAX35; press the boot button "
                                               "or call self.sdcard.close_media to return to chat")};
            });

        Property video_path("path", kPropertyTypeString);
        video_path.SetMaxLength(128);
        Property video_frame("frame_index", kPropertyTypeInteger, 0, 0, 3000);
        mcp_server.AddTool(
            "self.sdcard.read_video_frame",
            "Return one JPEG frame from an MJPEG video on the SD card for visual analysis. "
            "Use frame_index 0 for the first frame; call again with later indices to inspect "
            "other moments. Supports MJPEG, MJP, and MJPG files.",
            PropertyList({video_path, video_frame}),
            [this](const PropertyList& properties) -> ToolResult {
                const std::string path = properties["path"].value<std::string>();
                const std::string extension = LowercaseExtension(path);
                if (!IsSafeMediaPath(path) ||
                    (extension != ".mjpeg" && extension != ".mjpg" && extension != ".mjp")) {
                    return std::unexpected("Path must name an MJPEG, MJP, or MJPG file on the SD card");
                }
                if (!EnsureSdCardMounted()) {
                    return std::unexpected("Could not mount the SD card");
                }

                const std::string full_path = std::string(SD_CARD_MOUNT_POINT) + "/" + path;
                FILE* file = fopen(full_path.c_str(), "rb");
                if (file == nullptr) {
                    return std::unexpected("Could not open the video file");
                }
                size_t target_frame = static_cast<size_t>(properties["frame_index"].value<int>());
                size_t current_frame = 0;
                size_t bytes_scanned = 0;
                constexpr size_t kMaxBytesScanned = 8 * 1024 * 1024;
                constexpr size_t kMaxFrameBytes = 512 * 1024;
                std::array<uint8_t, 4096> buffer{};
                int previous_byte = -1;
                bool in_frame = false;
                std::string frame;
                while (bytes_scanned < kMaxBytesScanned) {
                    size_t bytes_to_read =
                        std::min(buffer.size(), kMaxBytesScanned - bytes_scanned);
                    size_t bytes_read = fread(buffer.data(), 1, bytes_to_read, file);
                    if (bytes_read == 0) {
                        break;
                    }
                    bytes_scanned += bytes_read;
                    for (size_t i = 0; i < bytes_read; i++) {
                        int current_byte = buffer[i];
                        if (!in_frame) {
                            if (previous_byte == 0xff && current_byte == 0xd8) {
                                frame.assign(1, static_cast<char>(0xff));
                                frame.push_back(static_cast<char>(0xd8));
                                in_frame = true;
                            }
                        } else {
                            frame.push_back(static_cast<char>(current_byte));
                            if (frame.size() > kMaxFrameBytes) {
                                fclose(file);
                                return std::unexpected("Video frame exceeds the 512 KB limit");
                            }
                            if (previous_byte == 0xff && current_byte == 0xd9) {
                                if (current_frame == target_frame) {
                                    fclose(file);
                                    return ReturnValue{new ImageContent("image/jpeg", frame)};
                                }
                                current_frame++;
                                frame.clear();
                                in_frame = false;
                            }
                        }
                        previous_byte = current_byte;
                    }
                }
                fclose(file);
                return std::unexpected(bytes_scanned >= kMaxBytesScanned
                                           ? "No JPEG frame found in the first 8 MB; use a raw MJPEG file"
                                           : "Requested JPEG frame was not found in the video");
            });

        Property playback_path("path", kPropertyTypeString);
        playback_path.SetMaxLength(128);
        Property playback_loop("loop", kPropertyTypeBoolean, false);
        mcp_server.AddTool(
            "self.sdcard.play_video",
            "Play a raw MJPEG video fullscreen on the MAX35. Supports .mjpeg, .mjpg, and .mjp "
            "files listed by self.sdcard.list_media. The optional loop flag repeats playback.",
            PropertyList({playback_path, playback_loop}),
            [this](const PropertyList& properties) -> ToolResult {
                const std::string path = properties["path"].value<std::string>();
                const std::string extension = LowercaseExtension(path);
                if (!IsSafeMediaPath(path) ||
                    (extension != ".mjpeg" && extension != ".mjpg" && extension != ".mjp")) {
                    return std::unexpected("Path must name a raw MJPEG, MJP, or MJPG file");
                }
                if (!EnsureSdCardMounted()) {
                    return std::unexpected("Could not mount the SD card");
                }
                if (!StartVideoPlayback(path, properties["loop"].value<bool>())) {
                    return std::unexpected("Video playback is already active or task creation failed");
                }
                return ReturnValue{std::string("Video playback started")};
            });

        mcp_server.AddTool(
            "self.sdcard.stop_video", "Stop SD-card video playback and return to the chat screen.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                if (!video_playing_.load()) {
                    return false;
                }
                video_stop_requested_.store(true);
                return true;
            });

    }

    void InitializeI2c() {
        i2c_master_bus_config_t config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = 1},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&config, &codec_i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t config = {};
        config.mosi_io_num = DISPLAY_MOSI_PIN;
        config.miso_io_num = GPIO_NUM_NC;
        config.sclk_io_num = DISPLAY_CLK_PIN;
        config.quadwp_io_num = GPIO_NUM_NC;
        config.quadhd_io_num = GPIO_NUM_NC;
        config.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &config, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        st7796_vendor_config_t vendor_config = {
            .init_cmds = kSt7796InitCommands,
            .init_cmds_size = sizeof(kSt7796InitCommands) / sizeof(kSt7796InitCommands[0]),
        };
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));

        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeCamera() {
        camera_config_t config = {
            .pin_pwdn = CAMERA_PIN_PWDN,
            .pin_reset = CAMERA_PIN_RESET,
            .pin_xclk = CAMERA_PIN_XCLK,
            .pin_sccb_sda = -1,
            .pin_sccb_scl = -1,
            .pin_d7 = CAMERA_PIN_D7,
            .pin_d6 = CAMERA_PIN_D6,
            .pin_d5 = CAMERA_PIN_D5,
            .pin_d4 = CAMERA_PIN_D4,
            .pin_d3 = CAMERA_PIN_D3,
            .pin_d2 = CAMERA_PIN_D2,
            .pin_d1 = CAMERA_PIN_D1,
            .pin_d0 = CAMERA_PIN_D0,
            .pin_vsync = CAMERA_PIN_VSYNC,
            .pin_href = CAMERA_PIN_HREF,
            .pin_pclk = CAMERA_PIN_PCLK,
            .xclk_freq_hz = 20000000,
            .ledc_timer = LEDC_TIMER_0,
            .ledc_channel = LEDC_CHANNEL_0,
            .pixel_format = PIXFORMAT_YUV422,
            .frame_size = FRAMESIZE_QVGA,
            .jpeg_quality = 12,
            .fb_count = 2,
            .fb_location = CAMERA_FB_IN_PSRAM,
            .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
            .sccb_i2c_port = I2C_NUM_0,
        };
        camera_ = new Esp32Camera(config, true);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            if (CloseMedia()) {
                return;
            }
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    SpotpearMax35TvBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeCamera();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            codec_i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    Display* GetDisplay() override { return display_; }

    Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    Camera* GetCamera() override { return camera_; }
};

DECLARE_BOARD(SpotpearMax35TvBoard);
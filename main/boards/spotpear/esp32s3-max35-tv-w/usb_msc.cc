// USB Mass Storage Class (MSC) device implementation for the MAX35 board.
//
// Exposes the microSD card to a host PC over USB OTG using TinyUSB MSC.
// The mount point can be switched between the application (ESP32) and the
// USB host, but only one side can access the card at a time.
//
// See usb_msc.h for the public API.
//
// This file is only compiled when CONFIG_USE_USB_MSC is set (see
// main/CMakeLists.txt), so no #if guard is needed here.

#include "usb_msc.h"

#include <esp_check.h>
#include <esp_log.h>
#include <class/msc/msc.h>
#include <tinyusb.h>
#include <tinyusb_default_config.h>
#include <tinyusb_msc.h>

namespace usb_msc {

namespace {

static const char* TAG = "usb_msc";

// USB MSC Descriptors
// VID/PID: 0x303A/0x4002 (Espressif VID, custom PID for MAX35)
// Endpoints: EP 0x01 OUT / 0x81 IN, Interface 0

#define EPNUM_MSC 1
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum { ITF_NUM_MSC = 0, ITF_NUM_TOTAL };

enum {
    EDPT_CTRL_OUT = 0x00,
    EDPT_CTRL_IN = 0x80,
    EDPT_MSC_OUT = 0x01,
    EDPT_MSC_IN = 0x81,
};

static tusb_desc_device_t descriptor_config = {.bLength = sizeof(descriptor_config),
                                               .bDescriptorType = TUSB_DESC_DEVICE,
                                               .bcdUSB = 0x0200,
                                               .bDeviceClass = TUSB_CLASS_MISC,
                                               .bDeviceSubClass = MISC_SUBCLASS_COMMON,
                                               .bDeviceProtocol = MISC_PROTOCOL_IAD,
                                               .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
                                               .idVendor = 0x303A,   // Espressif VID
                                               .idProduct = 0x4002,  // MAX35 PID
                                               .bcdDevice = 0x100,
                                               .iManufacturer = 0x01,
                                               .iProduct = 0x02,
                                               .iSerialNumber = 0x03,
                                               .bNumConfigurations = 0x01};

static uint8_t const msc_fs_configuration_desc[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

static char const* string_desc_arr[] = {
    (const char[]){0x09, 0x04},  // 0: supported language is English (0x0409)
    "SpotPear",                  // 1: Manufacturer
    "ESP32S3-MAX35-TV-W",        // 2: Product
    "123456",                    // 3: Serial
    "MAX35 MSC",                 // 4: MSC interface
};

}  // namespace

UsbMsc& UsbMsc::GetInstance() {
    static UsbMsc instance;
    return instance;
}

namespace {

// Storage handle for the TinyUSB MSC device.
static tinyusb_msc_storage_handle_t s_storage_handle = nullptr;

// Mount point mapping helper.
static tinyusb_msc_mount_point_t MountPointToTinyusb(MountPoint point) {
    return (point == MountPoint::kUsb) ? TINYUSB_MSC_STORAGE_MOUNT_USB
                                       : TINYUSB_MSC_STORAGE_MOUNT_APP;
}

static MountPoint TinyubToMountPoint(tinyusb_msc_mount_point_t point) {
    return (point == TINYUSB_MSC_STORAGE_MOUNT_USB) ? MountPoint::kUsb : MountPoint::kApp;
}

// Callback invoked when the mount point changes (e.g. host connects/disconnects).
static void StorageMountChangedCb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t* event,
                                  void* arg) {
    auto* self = static_cast<UsbMsc*>(arg);
    if (self == nullptr) {
        return;
    }

    switch (event->id) {
        case TINYUSB_MSC_EVENT_MOUNT_COMPLETE: {
            MountPoint point = TinyubToMountPoint(event->mount_point);
            self->SetCurrentMountPoint(point);
            ESP_LOGI(TAG, "Storage mount complete: %s",
                     (point == MountPoint::kApp) ? "application" : "USB host");
            break;
        }
        case TINYUSB_MSC_EVENT_MOUNT_FAILED:
        case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
            ESP_LOGE(TAG, "Storage mount failed or format required");
            break;
        default:
            break;
    }
}

}  // namespace

esp_err_t UsbMsc::Initialize(sdmmc_card_t* card) {
    if (card == nullptr) {
        last_error_ = "SD card is not mounted";
        ESP_LOGE(TAG, "%s", last_error_);
        return ESP_ERR_INVALID_ARG;
    }
    if (initialized_) {
        last_error_ = "";
        return ESP_OK;
    }

    // Non-const base path buffer (TinyUSB expects char*).
    static char kBasePath[] = "/sdcard";

    tinyusb_msc_storage_config_t storage_cfg = {};
    storage_cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP;  // Start with card mounted to app
    storage_cfg.medium.card = card;
    storage_cfg.fat_fs.base_path = kBasePath;
    storage_cfg.fat_fs.config.format_if_mount_failed = false;
    storage_cfg.fat_fs.config.max_files = 5;
    storage_cfg.fat_fs.config.allocation_unit_size = 0;
    storage_cfg.fat_fs.config.disk_status_check_enable = false;
    storage_cfg.fat_fs.config.use_one_fat = false;
    storage_cfg.fat_fs.do_not_format = true;
    storage_cfg.fat_fs.format_flags = 0;

    esp_err_t err = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_storage_handle);
    if (err != ESP_OK) {
        last_error_ = esp_err_to_name(err);
        ESP_LOGE(TAG, "Failed to create USB MSC storage: %s", last_error_);
        return err;
    }

    // Set callback for mount point changes
    err = tinyusb_msc_set_storage_callback(StorageMountChangedCb, this);
    if (err != ESP_OK) {
        last_error_ = esp_err_to_name(err);
        ESP_LOGE(TAG, "Failed to set USB MSC callback: %s", last_error_);
        return err;
    }

    // Install TinyUSB device driver with MSC descriptors
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &descriptor_config;
    tusb_cfg.descriptor.full_speed_config = msc_fs_configuration_desc;
    tusb_cfg.descriptor.string = string_desc_arr;
    tusb_cfg.descriptor.string_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);

    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        last_error_ = esp_err_to_name(err);
        ESP_LOGE(TAG, "Failed to install TinyUSB driver: %s", last_error_);
        return err;
    }

    initialized_ = true;
    last_error_ = "";
    ESP_LOGI(TAG, "USB MSC initialized; SD card mounted to application");
    return ESP_OK;
}

esp_err_t UsbMsc::SetMountPoint(MountPoint point) {
    if (!initialized_) {
        last_error_ = "USB MSC is not initialized";
        return ESP_ERR_INVALID_STATE;
    }

    tinyusb_msc_mount_point_t tinyusb_point = MountPointToTinyusb(point);
    esp_err_t err = tinyusb_msc_set_storage_mount_point(s_storage_handle, tinyusb_point);
    if (err != ESP_OK) {
        last_error_ = esp_err_to_name(err);
        ESP_LOGE(TAG, "Failed to set USB MSC mount point: %s", last_error_);
        return err;
    }

    current_mount_ = point;
    last_error_ = "";
    ESP_LOGI(TAG, "USB MSC mount point set to %s",
             (point == MountPoint::kUsb) ? "USB host" : "application");
    return ESP_OK;
}

void UsbMsc::SetCurrentMountPoint(MountPoint point) { current_mount_ = point; }

esp_err_t UsbMsc::GetMountPoint(MountPoint* point) const {
    if (!initialized_ || point == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *point = current_mount_;
    return ESP_OK;
}

bool UsbMsc::IsExposedToHost() const { return current_mount_ == MountPoint::kUsb; }

}  // namespace usb_msc

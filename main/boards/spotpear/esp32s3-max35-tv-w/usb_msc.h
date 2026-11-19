// USB Mass Storage Class (MSC) device for the SpotPear ESP32S3-MAX35-TV-W board.
//
// Exposes the microSD card (mounted at /sdcard) to a host PC over USB OTG.
// The host sees the card as a removable USB drive; the application can still
// read/write the card when the host is not connected (mount point is switched
// between APP and USB by the TinyUSB MSC storage layer).
//
// Usage:
//   #include "usb_msc.h"
//   UsbMsc::GetInstance().Initialize(sd_card);  // sd_card must be mounted
//   // Later, to expose to host:
//   UsbMsc::GetInstance().SetMountPoint(usb_msc::MountPoint::kUsb);
//   // To return to application:
//   UsbMsc::GetInstance().SetMountPoint(usb_msc::MountPoint::kApp);

#ifndef MAIN_BOARDS_SPOTPEAR_ESP32S3_MAX35_TV_W_USB_MSC_H_
#define MAIN_BOARDS_SPOTPEAR_ESP32S3_MAX35_TV_W_USB_MSC_H_

#include <esp_err.h>
#include <sdmmc_cmd.h>
#include <stdbool.h>
#include <stdint.h>

namespace usb_msc {

/**
 * @brief Controls which side (application or USB host) owns the SD card.
 *
 * The TinyUSB MSC storage layer allows only one side to access the card at a
 * time.  Switching the mount point unmounts the FAT filesystem from one side
 * and mounts it on the other.
 */
enum class MountPoint {
    kApp = 0,  ///< Application (ESP32) owns the card; host cannot access it.
    kUsb = 1,  ///< USB host owns the card; application cannot access it.
};

/**
 * @brief USB Mass Storage device wrapper for the MAX35 microSD card.
 *
 * This class initialises the TinyUSB MSC device stack and exposes the SD card
 * as a USB mass-storage device.  It is a singleton because the TinyUSB device
 * stack can only be installed once.
 */
class UsbMsc {
public:
    /// Returns the singleton instance.
    static UsbMsc& GetInstance();

    /**
     * @brief Initialise the USB MSC device stack with the given SD card.
     *
     * Must be called after the SD card has been mounted by the board (the
     * board's EnsureSdCardMounted() must have succeeded).  The card is
     * initially mounted to the application; call SetMountPoint(kUsb) to
     * expose it to a host PC.
     *
     * @param card  Pointer to the already-initialised sdmmc_card_t.
     * @return ESP_OK on success, or an error code.
     */
    esp_err_t Initialize(sdmmc_card_t* card);

    /**
     * @brief Switch the mount point between application and USB host.
     *
     * @param point  The desired mount point.
     * @return ESP_OK on success, or an error code.
     */
    esp_err_t SetMountPoint(MountPoint point);

    /**
     * @brief Get the current mount point.
     *
     * @param[out] point  Set to the current mount point.
     * @return ESP_OK on success, or an error code.
     */
    esp_err_t GetMountPoint(MountPoint* point) const;

    /**
     * @brief Check whether the USB MSC device is currently exposed to a host.
     *
     * @return true if the mount point is kUsb, false otherwise.
     */
    bool IsExposedToHost() const;

    /**
     * @brief Set the current mount point (used by the TinyUSB MSC callback).
     *
     * @param point  The current mount point.
     */
    void SetCurrentMountPoint(MountPoint point);

    /**
     * @brief Check whether the USB MSC device stack has been initialised.
     *
     * @return true if Initialize() has been called successfully.
     */
    bool IsInitialized() const { return initialized_; }

    /**
     * @brief Get the last error message (for logging / MCP tool responses).
     *
     * @return A human-readable error string, or nullptr if no error.
     */
    const char* GetLastError() const { return last_error_; }

private:
    UsbMsc() = default;
    ~UsbMsc() = default;
    UsbMsc(const UsbMsc&) = delete;
    UsbMsc& operator=(const UsbMsc&) = delete;

    bool initialized_ = false;
    MountPoint current_mount_ = MountPoint::kApp;
    const char* last_error_ = nullptr;
};

}  // namespace usb_msc

#endif  // MAIN_BOARDS_SPOTPEAR_ESP32S3_MAX35_TV_W_USB_MSC_H_
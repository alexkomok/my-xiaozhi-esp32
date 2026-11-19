# SpotPear ESP32S3-MAX35-TV-W

ESP32-S3-R8 board with 16 MB flash, 8 MB PSRAM, a 480x320 ST7796S display,
OV5640 camera (DVP, YUV422), ES7210/ES8311 audio with device-side AEC, a microSD
slot, and an NS4150B speaker amplifier. The firmware is XiaoZhi, a voice assistant
that the cloud AI controls through MCP tools; this board uses the WeChat chat style
and Russian locale.

## Build and deploy

Requires ESP-IDF v6.1 (minimum v6.0.1). Source the environment first:

```sh
# Windows (cmd)
call "C:\Espressif\v6.1\esp-idf\export.bat"
# Linux/macOS
source /path/to/esp-idf/export.sh
```

Build the firmware from the repository root:

```sh
python scripts/build.py spotpear/esp32s3-max35-tv-w --name esp32s3-max35-tv-w --language ru-RU
```

Flash to the board (replace `COM5` with your port; on Linux/macOS use e.g. `/dev/ttyUSB0`):

```sh
idf.py -p COM5 flash
```

Verify the boot and watch the console:

```sh
idf.py -p COM5 monitor
```

The build produces `build/xiaozhi.bin` (~3 MB) plus bootloader, partition table, OTA
data, and assets. The flash step writes and hash-verifies every image. A successful
build and boot is not hardware validation — audio, display, camera, and SD card still
need physical testing.

## Behavior specific to this board

- The camera preview is rotated 90 degrees clockwise to match the sensor mounting.
- Photos and images displayed via `self.sdcard.show_image` are rotated 90 degrees clockwise
  so the AI always sees them in the board's landscape orientation.
- The microSD card is mounted lazily at `/sdcard` on first use (SDMMC slot 1, 1-bit:
  CLK GPIO43, CMD GPIO44, D0 GPIO21). The pin map was derived from the factory firmware.
- A short press of the boot button closes a fullscreen picture or video; otherwise it
  toggles chat as usual.
- **USB Mass Storage (MSC)**: When enabled (`CONFIG_USE_USB_MSC=y`), the microSD card can
  be exposed to a host PC over USB OTG as a removable USB drive. The mount point is
  switched at runtime via MCP tools (`self.usbmsc.expose`, `self.usbmsc.retract`,
  `self.usbmsc.status`). Only one side (application or USB host) can access the card
  at a time. The SD card and TinyUSB stack are initialized on demand, when an SD card
  or USB MSC tool is first used; they are not initialized during boot.

### USB Mass Storage workflow

1. Insert a FAT/exFAT microSD card supported by the ESP-IDF SDMMC/VFS stack.
2. Call `self.usbmsc.expose` to mount the card, initialize TinyUSB if needed, and
   switch ownership to the host USB port.
3. Safely eject the drive from the host before calling `self.usbmsc.retract`.
4. Call `self.usbmsc.retract` to return ownership to the firmware. The
   `self.usbmsc.status` tool reports whether the card is currently exposed.

The firmware does not access the card concurrently while it is owned by the USB host.
Use `self.usbmsc.retract` before application-side SD card tools resume card access.
Mounting or TinyUSB initialization failures are returned as MCP tool errors instead of
halting the firmware.

## Supported SD card media

| Kind  | Formats | Notes |
|-------|---------|-------|
| Image | JPG, JPEG (including progressive), PNG | Shown fullscreen. PNG is limited to 1 megapixel; JPEG is downscaled to fit 512 px. |
| Video | Raw MJPEG: `.mjpeg`, `.mjpg`, `.mjp` | Video only, no audio. |
| Music | MP3 | Listed only; local playback is not implemented. |

## MCP tools

| Tool | Purpose |
|------|---------|
| `self.sdcard.list_media` | List every image, video and MP3 file (up to 100). |
| `self.sdcard.list_images` | List only JPG/JPEG/PNG files, including camera photos. |
| `self.sdcard.list_videos` | List only MJPEG videos. |
| `self.sdcard.list_music` | List only MP3 files. |
| `self.sdcard.delete_file` | Permanently delete a file using its relative SD card path. |
| `self.sdcard.read_image` | Return an image to the AI for visual analysis (up to 8 MB). |
| `self.sdcard.read_video_frame` | Return one frame of an MJPEG video to the AI (`frame_index`). |
| `self.sdcard.show_image` | Show a JPEG or PNG fullscreen on the display (rotated 90° clockwise). |
| `self.sdcard.play_video` | Play an MJPEG video fullscreen; optional `loop`. |
| `self.sdcard.stop_video` | Stop video playback. |
| `self.sdcard.close_media` | Close the picture or stop the video and return to chat. |
| `self.sdcard.save_camera_photo` | Take a photo and save it as `photos/IMG_NNNN.jpg` (sensor orientation, not rotated). |
| `self.usbmsc.expose` | Expose the microSD card to a host PC over USB OTG (requires `CONFIG_USE_USB_MSC=y`). |
| `self.usbmsc.retract` | Return the microSD card to application control (requires `CONFIG_USE_USB_MSC=y`). |
| `self.usbmsc.status` | Get the current USB MSC status (requires `CONFIG_USE_USB_MSC=y`). |
| `self.screen.scroll` | Scroll the chat history up or down (common tool, WeChat style only). |

## Notable functions

- `DecodeJpegAndShow` (board `.cc`): decodes a JPEG in memory with libjpeg-turbo at the
  smallest 1/1 to 1/8 scale that fits, converts it to RGB565 and shows it.
- `ReadNextMjpegFrame` (board `.cc`): reads the next JPEG frame from a raw MJPEG file
  using a buffered scan with size limits.
- `Display::ShowMediaFrame`, `ShowPngImage`, `ClearMediaFrame`: fullscreen RGB565 overlay
  above the chat screen; PNG is decoded with LVGL's LodePNG decoder.
- `Display::ScrollChat`: scrolls the chat history for the AI-driven scroll tool.
- `Esp32Camera::SaveJpeg`: encodes the last captured frame as JPEG and writes it to a file.

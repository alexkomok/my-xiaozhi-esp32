# SpotPear ESP32S3-MAX35-TV-W

ESP32-S3-R8 board with 16 MB flash, 8 MB PSRAM, a 480x320 ST7796S display,
OV5640 camera (DVP, YUV422), ES7210/ES8311 audio with device-side AEC, a microSD
slot, and an NS4150B speaker amplifier. The firmware is XiaoZhi, a voice assistant
that the cloud AI controls through MCP tools; this board uses the WeChat chat style
and Russian locale.

```sh
python scripts/build.py spotpear/esp32s3-max35-tv-w --name esp32s3-max35-tv-w --language ru-RU
```

## Behavior specific to this board

- The camera preview is rotated 90 degrees clockwise to match the sensor mounting.
- The microSD card is mounted lazily at `/sdcard` on first use (SDMMC slot 1, 1-bit:
  CLK GPIO43, CMD GPIO44, D0 GPIO21). The pin map was derived from the factory firmware.
- A short press of the boot button closes a fullscreen picture or video; otherwise it
  toggles chat as usual.

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
| `self.sdcard.show_image` | Show a JPEG or PNG fullscreen on the display. |
| `self.sdcard.play_video` | Play an MJPEG video fullscreen; optional `loop`. |
| `self.sdcard.stop_video` | Stop video playback. |
| `self.sdcard.close_media` | Close the picture or stop the video and return to chat. |
| `self.sdcard.save_camera_photo` | Take a photo and save it as `photos/IMG_NNNN.jpg` (sensor orientation, not rotated). |
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

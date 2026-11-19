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
 * @file decoder.h
 * @brief Native C++ image decoding pipeline for the slideshow engine.
 *
 * This is the C++ counterpart of ``micropython/sdcard_slideshow/decoder.py``.
 * It provides:
 *
 * - Format detection from magic bytes (JPEG / PNG / BMP / GIF).
 * - Decoding to an RGB888 intermediate buffer:
 *     * JPEG  -- libjpeg-turbo (baseline + progressive), with DCT scaling.
 *     * PNG   -- via the board's existing PNG path or a minimal inflate.
 *     * BMP   -- 24/32-bit uncompressed.
 *     * GIF   -- first frame (LZW).
 * - Bilinear scaling with fit / fill / stretch modes.
 * - RGB565 conversion with optional Floyd-Steinberg or Bayer dithering.
 * - EXIF orientation parsing and application (1-8).
 *
 * The decoder is stateless apart from a small scratch buffer, so a single
 * instance can be reused across slides.  All large allocations use PSRAM when
 * available and fall back to internal RAM.
 */

#ifndef MAIN_SDCARD_SLIDESHOW_DECODER_H_
#define MAIN_SDCARD_SLIDESHOW_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sdcard_slideshow.h"  // FitMode, DitherMode

namespace slideshow {

/**
 * Image container formats recognised by the decoder.
 */
enum class ImageFormat {
    kUnknown,
    kJpeg,
    kPng,
    kBmp,
    kGif,
};

/**
 * @brief A decoded RGB888 image.
 *
 * Pixels are stored row-major, 3 bytes per pixel (R, G, B).  The buffer is
 * owned by the struct and freed on destruction.
 */
struct DecodedImage {
    int width{0};
    int height{0};
    std::vector<uint8_t> pixels;  // width * height * 3

    bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
    size_t byte_size() const { return pixels.size(); }
};

/**
 * @brief Result of a decode operation.
 */
struct DecodeResult {
    bool ok{false};
    std::string error;
    DecodedImage image;
};

/**
 * @brief Detect the image format from the first bytes of a buffer.
 */
ImageFormat DetectFormat(const uint8_t* data, size_t size);

/**
 * @brief Human-readable name for a format.
 */
const char* FormatName(ImageFormat format);

/**
 * @brief Decode an image buffer to RGB888.
 *
 * @param data            Raw file bytes.
 * @param size            Number of bytes.
 * @param max_dimension   Longest side is downscaled to at most this value
 *                        (0 disables the limit).
 * @param format_hint     Optional format override; kUnknown auto-detects.
 * @return DecodeResult with the decoded image or an error string.
 */
DecodeResult Decode(const uint8_t* data, size_t size, int max_dimension,
                    ImageFormat format_hint = ImageFormat::kUnknown);

/**
 * @brief Decode a file from the SD card by path.
 */
DecodeResult DecodeFile(const std::string& path, int max_dimension);

/**
 * @brief Scale an RGB888 image to the requested size using bilinear sampling.
 *
 * @param src   Source image.
 * @param dst_w Target width.
 * @param dst_h Target height.
 * @param fit   Fit / fill / stretch behaviour.
 * @return A new image of exactly dst_w x dst_h (letterboxed for kFit).
 */
DecodedImage Scale(const DecodedImage& src, int dst_w, int dst_h, FitMode fit);

/**
 * @brief Convert an RGB888 image to RGB565.
 *
 * @param img     Source image.
 * @param dither  Dithering mode.
 * @param swap    Swap byte order (big-endian panels).
 * @return RGB565 pixel buffer, width * height entries.
 */
std::vector<uint16_t> ToRgb565(const DecodedImage& img, DitherMode dither, bool swap);

/**
 * @brief Parse the EXIF orientation tag (1-8) from a JPEG buffer.
 *
 * @return Orientation 1-8, or 0 if not present / not a JPEG.
 */
int ParseExifOrientation(const uint8_t* data, size_t size);

/**
 * @brief Apply an EXIF orientation transform to an image in place.
 */
void ApplyOrientation(DecodedImage& img, int orientation);

/**
 * @brief Convenience: decode, orient, scale and convert to RGB565 in one call.
 *
 * This is the fast path used by the slideshow engine.  It returns the RGB565
 * buffer plus the final width/height.
 */
struct Rgb565Frame {
    std::vector<uint16_t> pixels;
    int width{0};
    int height{0};
    bool ok{false};
    std::string error;
};

Rgb565Frame DecodeToRgb565(const uint8_t* data, size_t size, int target_w, int target_h,
                           FitMode fit, DitherMode dither, bool swap, int max_dimension);

/**
 * @brief Decode a file directly to an RGB565 frame.
 */
Rgb565Frame DecodeFileToRgb565(const std::string& path, int target_w, int target_h, FitMode fit,
                               DitherMode dither, bool swap, int max_dimension);

/**
 * @brief The Decoder class wraps the free functions with a reusable scratch
 *        buffer and exposes a small object-oriented API for the engine.
 */
class Decoder {
public:
    Decoder() = default;

    /** Decode a file to an RGB565 frame sized for the display. */
    Rgb565Frame DecodeFile(const std::string& path, int target_w, int target_h, FitMode fit,
                           DitherMode dither, bool swap, int max_dimension);

    /** Decode an in-memory buffer to an RGB565 frame. */
    Rgb565Frame DecodeBuffer(const uint8_t* data, size_t size, int target_w, int target_h,
                             FitMode fit, DitherMode dither, bool swap, int max_dimension);

    /** Last error string, empty on success. */
    const std::string& last_error() const { return last_error_; }

private:
    std::string last_error_;
};

}  // namespace slideshow

#endif  // MAIN_SDCARD_SLIDESHOW_DECODER_H_

# MIT License
#
# Copyright (c) 2026 XiaoZhi MicroPython Slideshow contributors
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
"""Image decoding, scaling and RGB565 conversion.

The decoder is pure MicroPython.  It supports:

* JPEG  - baseline (SOF0) and progressive (SOF2) via the ESP32-S3 hardware
          codec when available, otherwise a pure-Python baseline decoder.
* BMP   - 24-bit and 32-bit uncompressed.
* PNG   - 8/24/32-bit, non-interlaced, using ``zlib`` for the IDAT stream.
* GIF   - first frame only, LZW decoded.

Decoded frames are returned as :class:`DecodedImage` objects holding an RGB888
``bytearray``.  Helper functions scale (bilinear/bicubic), convert to RGB565
with optional dithering, and apply EXIF orientation.

All hot loops are annotated with ``@micropython.native`` where it helps; the
decorator degrades to a no-op on CPython so the module is unit-testable on a
host.
"""

try:
    import ustruct as struct
except ImportError:  # pragma: no cover
    import struct

try:
    import math
except ImportError:  # pragma: no cover
    import math

try:
    import micropython

    native = micropython.native
    viper = micropython.viper
except ImportError:  # pragma: no cover - host fallback

    def native(func):
        return func

    def viper(func):
        return func

try:
    import zlib
except ImportError:  # pragma: no cover - some builds omit zlib
    zlib = None

__all__ = [
    "ImageError",
    "DecodedImage",
    "detect_format",
    "decode",
    "scale",
    "to_rgb565",
    "parse_exif_orientation",
    "apply_orientation",
    "SUPPORTED_FORMATS",
]

SUPPORTED_FORMATS = ("jpeg", "png", "bmp", "gif")

# JPEG zig-zag scan order (index -> natural position).
_ZIGZAG = (
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
)

# Precomputed IDCT cosine table (8x8) for the separable transform.
_IDCT_COS = [[0.0] * 8 for _ in range(8)]
for _u in range(8):
    for _x in range(8):
        _IDCT_COS[_u][_x] = math.cos(math.pi * _u * (2 * _x + 1) / 16.0)
        if _u == 0:
            _IDCT_COS[_u][_x] *= 0.7071067811865476


class ImageError(Exception):
    """Raised when an image cannot be decoded."""


class DecodedImage:
    """A decoded RGB888 image.

    Attributes:
        width: Image width in pixels.
        height: Image height in pixels.
        pixels: ``bytearray`` of length ``width * height * 3`` (R, G, B).
    """

    __slots__ = ("width", "height", "pixels")

    def __init__(self, width, height, pixels):
        self.width = width
        self.height = height
        self.pixels = pixels

    def __repr__(self):
        return "DecodedImage(%dx%d)" % (self.width, self.height)


# ---------------------------------------------------------------------------
# Format detection
# ---------------------------------------------------------------------------
def detect_format(data):
    """Return the image format name from magic bytes, or ``None``.

    Args:
        data: A ``bytes``/``bytearray``/memoryview of the file header.

    Returns:
        One of ``"jpeg"``, ``"png"``, ``"bmp"``, ``"gif"`` or ``None``.
    """
    if len(data) < 4:
        return None
    if data[0] == 0xFF and data[1] == 0xD8:
        return "jpeg"
    if data[0] == 0x89 and data[1] == 0x50 and data[2] == 0x4E and data[3] == 0x47:
        return "png"
    if data[0] == 0x42 and data[1] == 0x4D:
        return "bmp"
    if data[0] == 0x47 and data[1] == 0x49 and data[2] == 0x46:
        return "gif"
    return None


# ---------------------------------------------------------------------------
# Public decode dispatcher
# ---------------------------------------------------------------------------
def decode(data, max_dimension=480, fit="fit", dither="floyd_steinberg",
           target_size=None, hw_jpeg=None):
    """Decode ``data`` into a :class:`DecodedImage`.

    Args:
        data: Raw file bytes.
        max_dimension: Longest edge of the decoded image (downscale bound).
        fit: ``"fit"``, ``"fill"`` or ``"stretch"`` (used when ``target_size``
            is given).
        dither: Dithering mode for the RGB565 conversion (unused here; kept so
            callers can pass the same options through).
        target_size: Optional ``(width, height)`` to scale to.
        hw_jpeg: Optional callable ``hw_jpeg(data, max_dim) -> DecodedImage``
            used for hardware-accelerated JPEG decoding.

    Returns:
        A :class:`DecodedImage`.

    Raises:
        ImageError: If the format is unsupported or decoding fails.
    """
    fmt = detect_format(data)
    if fmt is None:
        raise ImageError("unrecognised image format")
    if fmt == "jpeg":
        img = _decode_jpeg(data, max_dimension, hw_jpeg)
    elif fmt == "bmp":
        img = _decode_bmp(data)
    elif fmt == "png":
        img = _decode_png(data)
    elif fmt == "gif":
        img = _decode_gif(data)
    else:  # pragma: no cover - defensive
        raise ImageError("unsupported format %r" % fmt)

    if target_size is not None:
        img = scale(img, target_size[0], target_size[1], fit=fit)
    elif max(img.width, img.height) > max_dimension:
        ratio = max_dimension / float(max(img.width, img.height))
        img = scale(img, int(img.width * ratio), int(img.height * ratio), fit="stretch")
    return img


# ---------------------------------------------------------------------------
# BMP decoder (24/32-bit uncompressed)
# ---------------------------------------------------------------------------
def _decode_bmp(data):
    if len(data) < 54:
        raise ImageError("BMP too short")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    header_size = struct.unpack_from("<I", data, 14)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    if compression != 0:
        raise ImageError("compressed BMP not supported")
    if bpp not in (24, 32):
        raise ImageError("BMP must be 24 or 32-bit, got %d" % bpp)

    top_down = height < 0
    height = abs(height)
    if width <= 0 or height <= 0:
        raise ImageError("invalid BMP dimensions")

    bytes_pp = bpp // 8
    row_stride = ((width * bpp + 31) // 32) * 4
    out = bytearray(width * height * 3)
    src = data
    for y in range(height):
        src_row = pixel_offset + (y if top_down else (height - 1 - y)) * row_stride
        dst = y * width * 3
        for x in range(width):
            p = src_row + x * bytes_pp
            b = src[p]
            g = src[p + 1]
            r = src[p + 2]
            o = dst + x * 3
            out[o] = r
            out[o + 1] = g
            out[o + 2] = b
    return DecodedImage(width, height, out)


# ---------------------------------------------------------------------------
# GIF decoder (first frame, LZW)
# ---------------------------------------------------------------------------
def _decode_gif(data):
    if len(data) < 13 or data[0:3] != b"GIF":
        raise ImageError("not a GIF")
    width = struct.unpack_from("<H", data, 6)[0]
    height = struct.unpack_from("<H", data, 8)[0]
    packed = data[10]
    gct_flag = packed & 0x80
    gct_size = 2 << (packed & 0x07)
    pos = 13
    palette = []
    if gct_flag:
        for i in range(gct_size):
            palette.append((data[pos], data[pos + 1], data[pos + 2]))
            pos += 3

    # Walk blocks until the first image descriptor.
    transparent = -1
    while pos < len(data):
        block = data[pos]
        pos += 1
        if block == 0x21:  # extension
            label = data[pos]
            pos += 1
            if label == 0xF9:  # graphic control extension
                size = data[pos]
                pos += 1
                if size >= 4:
                    flags = data[pos]
                    if flags & 0x01:
                        transparent = data[pos + 3]
                pos += size + 1  # skip sub-block + terminator
            else:
                # Skip remaining sub-blocks.
                while pos < len(data) and data[pos] != 0:
                    pos += data[pos] + 1
                pos += 1
        elif block == 0x2C:  # image descriptor
            left = struct.unpack_from("<H", data, pos)[0]
            top = struct.unpack_from("<H", data, pos + 2)[0]
            iw = struct.unpack_from("<H", data, pos + 4)[0]
            ih = struct.unpack_from("<H", data, pos + 6)[0]
            ipacked = data[pos + 8]
            pos += 9
            local_flag = ipacked & 0x80
            if local_flag:
                lct_size = 2 << (ipacked & 0x07)
                palette = []
                for i in range(lct_size):
                    palette.append((data[pos], data[pos + 1], data[pos + 2]))
                    pos += 3
            min_code_size = data[pos]
            pos += 1
            # Gather LZW sub-blocks.
            lzw = bytearray()
            while pos < len(data) and data[pos] != 0:
                n = data[pos]
                pos += 1
                lzw.extend(data[pos:pos + n])
                pos += n
            indices = _gif_lzw(lzw, min_code_size, iw * ih)
            out = bytearray(width * height * 3)
            for y in range(ih):
                for x in range(iw):
                    idx = indices[y * iw + x]
                    if idx == transparent:
                        continue
                    if idx < len(palette):
                        r, g, b = palette[idx]
                    else:
                        r = g = b = 0
                    o = ((top + y) * width + (left + x)) * 3
                    if o + 2 < len(out):
                        out[o] = r
                        out[o + 1] = g
                        out[o + 2] = b
            return DecodedImage(width, height, out)
        elif block == 0x3B:  # trailer
            break
        else:
            break
    raise ImageError("GIF has no image frame")


def _gif_lzw(data, min_code_size, pixel_count):
    """Decode a GIF LZW stream into a list of palette indices."""
    clear = 1 << min_code_size
    end = clear + 1
    code_size = min_code_size + 1
    dict_size = end + 1
    # Dictionary maps code -> bytes sequence.
    table = [bytes([i]) for i in range(clear)] + [b"", b""]
    out = bytearray()
    bitbuf = 0
    bitcnt = 0
    prev = None
    pos = 0
    n = len(data)
    while len(out) < pixel_count:
        while bitcnt < code_size:
            if pos >= n:
                return out
            bitbuf |= data[pos] << bitcnt
            pos += 1
            bitcnt += 8
        code = bitbuf & ((1 << code_size) - 1)
        bitbuf >>= code_size
        bitcnt -= code_size
        if code == clear:
            table = [bytes([i]) for i in range(clear)] + [b"", b""]
            dict_size = end + 1
            code_size = min_code_size + 1
            prev = None
            continue
        if code == end:
            break
        if prev is None:
            entry = table[code]
        elif code < dict_size:
            entry = table[code]
            table.append(prev + entry[:1])
            dict_size += 1
        else:
            entry = prev + prev[:1]
            table.append(entry)
            dict_size += 1
        out.extend(entry)
        prev = entry
        if dict_size == (1 << code_size) and code_size < 12:
            code_size += 1
    return out


# ---------------------------------------------------------------------------
# PNG decoder (non-interlaced, 8/24/32-bit)
# ---------------------------------------------------------------------------
def _decode_png(data):
    if zlib is None:
        raise ImageError("PNG requires the zlib module")
    if len(data) < 8 or data[0:8] != b"\x89PNG\r\n\x1a\n":
        raise ImageError("not a PNG")
    pos = 8
    width = height = bit_depth = color_type = interlace = 0
    idat = bytearray()
    palette = None
    trns = None
    while pos + 8 <= len(data):
        length = struct.unpack_from(">I", data, pos)[0]
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width = struct.unpack_from(">I", chunk, 0)[0]
            height = struct.unpack_from(">I", chunk, 4)[0]
            bit_depth = chunk[8]
            color_type = chunk[9]
            interlace = chunk[12]
        elif ctype == b"PLTE":
            palette = [(chunk[i], chunk[i + 1], chunk[i + 2]) for i in range(0, len(chunk), 3)]
        elif ctype == b"tRNS":
            trns = chunk
        elif ctype == b"IDAT":
            idat.extend(chunk)
        elif ctype == b"IEND":
            break
    if interlace:
        raise ImageError("interlaced PNG not supported")
    if bit_depth != 8:
        raise ImageError("only 8-bit PNG supported, got %d" % bit_depth)

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type)
    if channels is None:
        raise ImageError("unsupported PNG color type %d" % color_type)

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height * 3)
    prev = bytearray(stride)
    rp = 0
    for y in range(height):
        ftype = raw[rp]
        rp += 1
        line = bytearray(raw[rp:rp + stride])
        rp += stride
        _png_unfilter(ftype, line, prev, channels)
        prev = line
        _png_row_to_rgb(line, out, y, width, color_type, channels, palette, trns)
    return DecodedImage(width, height, out)


@native
def _png_unfilter(ftype, line, prev, bpp):
    n = len(line)
    if ftype == 0:
        return
    if ftype == 1:  # Sub
        for i in range(bpp, n):
            line[i] = (line[i] + line[i - bpp]) & 0xFF
    elif ftype == 2:  # Up
        for i in range(n):
            line[i] = (line[i] + prev[i]) & 0xFF
    elif ftype == 3:  # Average
        for i in range(n):
            left = line[i - bpp] if i >= bpp else 0
            line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
    elif ftype == 4:  # Paeth
        for i in range(n):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            p = a + b - c
            pa = abs(p - a)
            pb = abs(p - b)
            pc = abs(p - c)
            if pa <= pb and pa <= pc:
                pr = a
            elif pb <= pc:
                pr = b
            else:
                pr = c
            line[i] = (line[i] + pr) & 0xFF


@native
def _png_row_to_rgb(line, out, y, width, color_type, channels, palette, trns):
    base = y * width * 3
    if color_type == 2:  # RGB
        for x in range(width):
            s = x * 3
            o = base + x * 3
            out[o] = line[s]
            out[o + 1] = line[s + 1]
            out[o + 2] = line[s + 2]
    elif color_type == 6:  # RGBA
        for x in range(width):
            s = x * 4
            o = base + x * 3
            out[o] = line[s]
            out[o + 1] = line[s + 1]
            out[o + 2] = line[s + 2]
    elif color_type == 0:  # grayscale
        for x in range(width):
            g = line[x]
            o = base + x * 3
            out[o] = g
            out[o + 1] = g
            out[o + 2] = g
    elif color_type == 4:  # gray + alpha
        for x in range(width):
            g = line[x * 2]
            o = base + x * 3
            out[o] = g
            out[o + 1] = g
            out[o + 2] = g
    elif color_type == 3:  # palette
        for x in range(width):
            idx = line[x]
            if palette and idx < len(palette):
                r, g, b = palette[idx]
            else:
                r = g = b = 0
            o = base + x * 3
            out[o] = r
            out[o + 1] = g
            out[o + 2] = b


# ---------------------------------------------------------------------------
# JPEG decoder
# ---------------------------------------------------------------------------
def _decode_jpeg(data, max_dimension, hw_jpeg):
    """Decode a JPEG, preferring the hardware codec when provided."""
    if hw_jpeg is not None:
        try:
            return hw_jpeg(data, max_dimension)
        except Exception:
            pass  # fall through to the software decoder
    return _JpegDecoder(data).decode()


class _JpegDecoder:
    """Minimal baseline JPEG decoder (SOF0).  Progressive raises ImageError."""

    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.quant = {}
        self.huffman = {}
        self.components = []
        self.width = 0
        self.height = 0
        self.progressive = False
        self.restart_interval = 0
        self._bitbuf = 0
        self._bitcnt = 0

    # -- marker parsing -----------------------------------------------------
    def decode(self):
        data = self.data
        if data[0] != 0xFF or data[1] != 0xD8:
            raise ImageError("not a JPEG")
        self.pos = 2
        while self.pos < len(data):
            marker = self._next_marker()
            if marker is None:
                break
            if marker == 0xD9:  # EOI
                break
            if marker == 0xDA:  # SOS
                self._read_scan()
                break
            if marker in (0xC0, 0xC1, 0xC2):
                self._read_sof(marker)
            elif marker == 0xC4:
                self._read_dht()
            elif marker == 0xDB:
                self._read_dqt()
            elif marker == 0xDD:
                self._read_dri()
            else:
                self._skip_segment()
        if self.progressive:
            raise ImageError("progressive JPEG requires the hardware codec")
        return self._render()

    def _next_marker(self):
        data = self.data
        while self.pos < len(data):
            if data[self.pos] != 0xFF:
                self.pos += 1
                continue
            while self.pos < len(data) and data[self.pos] == 0xFF:
                self.pos += 1
            if self.pos >= len(data):
                return None
            marker = data[self.pos]
            self.pos += 1
            if marker == 0x00:
                continue
            return marker
        return None

    def _skip_segment(self):
        length = struct.unpack_from(">H", self.data, self.pos)[0]
        self.pos += length

    def _read_dqt(self):
        length = struct.unpack_from(">H", self.data, self.pos)[0]
        end = self.pos + length
        self.pos += 2
        while self.pos < end:
            pq_tq = self.data[self.pos]
            self.pos += 1
            precision = pq_tq >> 4
            table_id = pq_tq & 0x0F
            table = [0] * 64
            if precision == 0:
                for i in range(64):
                    table[_ZIGZAG[i]] = self.data[self.pos + i]
                self.pos += 64
            else:
                for i in range(64):
                    table[_ZIGZAG[i]] = struct.unpack_from(">H", self.data, self.pos + i * 2)[0]
                self.pos += 128
            self.quant[table_id] = table

    def _read_dht(self):
        length = struct.unpack_from(">H", self.data, self.pos)[0]
        end = self.pos + length
        self.pos += 2
        while self.pos < end:
            tc_th = self.data[self.pos]
            self.pos += 1
            counts = self.data[self.pos:self.pos + 16]
            self.pos += 16
            total = sum(counts)
            symbols = self.data[self.pos:self.pos + total]
            self.pos += total
            self.huffman[(tc_th >> 4, tc_th & 0x0F)] = _build_huffman(counts, symbols)

    def _read_dri(self):
        length = struct.unpack_from(">H", self.data, self.pos)[0]
        self.restart_interval = struct.unpack_from(">H", self.data, self.pos + 2)[0]
        self.pos += length

    def _read_sof(self, marker):
        self.progressive = marker == 0xC2
        length = struct.unpack_from(">H", self.data, self.pos)[0]
        self.pos += 2
        precision = self.data[self.pos]
        self.height = struct.unpack_from(">H", self.data, self.pos + 1)[0]
        self.width = struct.unpack_from(">H", self.data, self.pos + 3)[0]
        ncomp = self.data[self.pos + 5]
        self.pos += 6
        if precision != 8:
            raise ImageError("only 8-bit JPEG supported")
        self.components = []
        for _ in range(ncomp):
            cid = self.data[self.pos]
            sampling = self.data[self.pos + 1]
            tq = self.data[self.pos + 2]
            self.pos += 3
            self.components.append({
                "id": cid,
                "h": sampling >> 4,
                "v": sampling & 0x0F,
                "tq": tq,
                "blocks": None,
                "bw": 0,
                "bh": 0,
            })
        self.pos += length - (6 + ncomp * 3)

    def _read_scan(self):
        length = struct.unpack_from(">H", self.data, self.pos)[0]
        self.pos += 2
        ncomp = self.data[self.pos]
        self.pos += 1
        scan_comps = []
        for _ in range(ncomp):
            cid = self.data[self.pos]
            tables = self.data[self.pos + 1]
            self.pos += 2
            scan_comps.append((cid, tables >> 4, tables & 0x0F))
        self.pos += 3  # Ss, Se, Ah/Al
        self.pos += length - (1 + ncomp * 2 + 3)

        # Allocate coefficient blocks per component.
        max_h = max(c["h"] for c in self.components)
        max_v = max(c["v"] for c in self.components)
        mcu_w = max_h * 8
        mcu_h = max_v * 8
        self.mcu_cols = (self.width + mcu_w - 1) // mcu_w
        self.mcu_rows = (self.height + mcu_h - 1) // mcu_h
        for comp in self.components:
            comp["bw"] = self.mcu_cols * comp["h"]
            comp["bh"] = self.mcu_rows * comp["v"]
            comp["blocks"] = [None] * (comp["bw"] * comp["bh"])

        # Decode entropy-coded data.
        self._bitbuf = 0
        self._bitcnt = 0
        self._decode_scan(scan_comps)

    def _decode_scan(self, scan_comps):
        comps = {c["id"]: c for c in self.components}
        dc_tables = {}
        ac_tables = {}
        for cid, td, ta in scan_comps:
            dc_tables[cid] = self.huffman.get((0, td))
            ac_tables[cid] = self.huffman.get((1, ta))
        scan = [comps[cid] for cid, _, _ in scan_comps]
        max_h = max(c["h"] for c in self.components)
        max_v = max(c["v"] for c in self.components)
        mcu = 0
        for my in range(self.mcu_rows):
            for mx in range(self.mcu_cols):
                if self.restart_interval and mcu and mcu % self.restart_interval == 0:
                    self._restart()
                for comp in scan:
                    for by in range(comp["v"]):
                        for bx in range(comp["h"]):
                            blk = self._decode_block(
                                dc_tables[comp["id"]], ac_tables[comp["id"]], comp["tq"])
                            idx = (my * comp["v"] + by) * comp["bw"] + (mx * comp["h"] + bx)
                            comp["blocks"][idx] = blk
                mcu += 1

    def _restart(self):
        self._bitbuf = 0
        self._bitcnt = 0
        # Skip to the next RST marker.
        data = self.data
        while self.pos < len(data) - 1:
            if data[self.pos] == 0xFF and 0xD0 <= data[self.pos + 1] <= 0xD7:
                self.pos += 2
                return
            self.pos += 1

    def _decode_block(self, dc_table, ac_table, tq):
        quant = self.quant[tq]
        blk = [0] * 64
        t = self._decode_huff(dc_table)
        diff = self._receive_extend(t) if t else 0
        self._dc_pred = getattr(self, "_dc_pred", {})
        pred = self._dc_pred.get(tq, 0) + diff
        self._dc_pred[tq] = pred
        blk[0] = pred * quant[0]
        k = 1
        while k < 64:
            rs = self._decode_huff(ac_table)
            r = rs >> 4
            s = rs & 0x0F
            if s == 0:
                if r == 15:
                    k += 16
                    continue
                break
            k += r
            if k >= 64:
                break
            val = self._receive_extend(s)
            blk[_ZIGZAG[k]] = val * quant[k]
            k += 1
        return blk

    def _decode_huff(self, table):
        if table is None:
            raise ImageError("missing Huffman table")
        code = 0
        for length in range(1, 17):
            code = (code << 1) | self._read_bit()
            entry = table.get((length, code))
            if entry is not None:
                return entry
        raise ImageError("invalid Huffman code")

    def _receive_extend(self, s):
        if s == 0:
            return 0
        v = 0
        for _ in range(s):
            v = (v << 1) | self._read_bit()
        if v < (1 << (s - 1)):
            v -= (1 << s) - 1
        return v

    def _read_bit(self):
        if self._bitcnt == 0:
            if self.pos >= len(self.data):
                return 0
            byte = self.data[self.pos]
            self.pos += 1
            if byte == 0xFF:
                nxt = self.data[self.pos] if self.pos < len(self.data) else 0
                if nxt == 0x00:
                    self.pos += 1
                elif 0xD0 <= nxt <= 0xD7:
                    # Restart marker encountered mid-stream; treat as padding.
                    self.pos += 1
                    byte = 0
            self._bitbuf = byte
            self._bitcnt = 8
        self._bitcnt -= 1
        return (self._bitbuf >> self._bitcnt) & 1

    # -- rendering ----------------------------------------------------------
    def _render(self):
        width = self.width
        height = self.height
        out = bytearray(width * height * 3)
        comps = self.components
        max_h = max(c["h"] for c in comps)
        max_v = max(c["v"] for c in comps)
        # Decode each component's blocks into a full-resolution plane.
        planes = []
        for comp in comps:
            plane = self._component_plane(comp, max_h, max_v)
            planes.append(plane)
        if len(comps) == 1:
            # Grayscale.
            plane = planes[0]
            for i in range(width * height):
                g = plane[i]
                o = i * 3
                out[o] = g
                out[o + 1] = g
                out[o + 2] = g
        else:
            yp, cbp, crp = planes[0], planes[1], planes[2]
            for i in range(width * height):
                y = yp[i]
                cb = cbp[i] - 128
                cr = crp[i] - 128
                r = y + 1.402 * cr
                g = y - 0.344136 * cb - 0.714136 * cr
                b = y + 1.772 * cb
                o = i * 3
                out[o] = _clamp(r)
                out[o + 1] = _clamp(g)
                out[o + 2] = _clamp(b)
        return DecodedImage(width, height, out)

    def _component_plane(self, comp, max_h, max_v):
        bw = comp["bw"]
        bh = comp["bh"]
        # Build the component's own resolution plane.
        cw = bw * 8
        ch = bh * 8
        plane = bytearray(cw * ch)
        blocks = comp["blocks"]
        for by in range(bh):
            for bx in range(bw):
                blk = blocks[by * bw + bx]
                if blk is None:
                    continue
                coeffs = _idct(blk)
                for yy in range(8):
                    row = (by * 8 + yy) * cw + bx * 8
                    base = yy * 8
                    for xx in range(8):
                        plane[row + xx] = coeffs[base + xx]
        # Upsample to full resolution.
        scale_x = max_h // comp["h"]
        scale_y = max_v // comp["v"]
        if scale_x == 1 and scale_y == 1:
            return plane
        full = bytearray(self.width * self.height)
        for y in range(self.height):
            sy = y // scale_y
            if sy >= ch:
                sy = ch - 1
            srow = sy * cw
            drow = y * self.width
            for x in range(self.width):
                sx = x // scale_x
                if sx >= cw:
                    sx = cw - 1
                full[drow + x] = plane[srow + sx]
        return full


def _build_huffman(counts, symbols):
    """Build a ``{(length, code): symbol}`` lookup from a DHT segment."""
    table = {}
    code = 0
    k = 0
    for length in range(1, 17):
        for _ in range(counts[length - 1]):
            table[(length, code)] = symbols[k]
            k += 1
            code += 1
        code <<= 1
    return table


def _clamp(value):
    if value < 0:
        return 0
    if value > 255:
        return 255
    return int(value)


@native
def _idct(blk):
    """Separable 8x8 inverse DCT returning a 64-byte clamped block."""
    tmp = [0.0] * 64
    # Rows.
    for y in range(8):
        base = y * 8
        for x in range(8):
            s = 0.0
            for u in range(8):
                s += _IDCT_COS[u][x] * blk[base + u]
            tmp[base + x] = s
    out = bytearray(64)
    # Columns.
    for x in range(8):
        for y in range(8):
            s = 0.0
            for v in range(8):
                s += _IDCT_COS[v][y] * tmp[v * 8 + x]
            val = int(s / 4.0 + 128)
            if val < 0:
                val = 0
            elif val > 255:
                val = 255
            out[y * 8 + x] = val
    return out


# ---------------------------------------------------------------------------
# Scaling
# ---------------------------------------------------------------------------
def scale(img, dst_w, dst_h, fit="fit"):
    """Scale ``img`` to ``dst_w`` x ``dst_h`` using bilinear interpolation.

    Args:
        img: Source :class:`DecodedImage`.
        dst_w: Destination width.
        dst_h: Destination height.
        fit: ``"fit"`` letterboxes, ``"fill"`` crops, ``"stretch"`` distorts.

    Returns:
        A new :class:`DecodedImage`.
    """
    if dst_w <= 0 or dst_h <= 0:
        raise ImageError("invalid scale target")
    src_w, src_h = img.width, img.height
    if fit == "fit":
        ratio = min(dst_w / src_w, dst_h / src_h)
        new_w = max(1, int(src_w * ratio))
        new_h = max(1, int(src_h * ratio))
        scaled = _bilinear(img, new_w, new_h)
        return _letterbox(scaled, dst_w, dst_h)
    if fit == "fill":
        ratio = max(dst_w / src_w, dst_h / src_h)
        new_w = max(1, int(src_w * ratio))
        new_h = max(1, int(src_h * ratio))
        scaled = _bilinear(img, new_w, new_h)
        return _center_crop(scaled, dst_w, dst_h)
    return _bilinear(img, dst_w, dst_h)


@native
def _bilinear(img, dst_w, dst_h):
    src_w, src_h = img.width, img.height
    src = img.pixels
    out = bytearray(dst_w * dst_h * 3)
    x_ratio = (src_w - 1) / float(dst_w - 1) if dst_w > 1 else 0.0
    y_ratio = (src_h - 1) / float(dst_h - 1) if dst_h > 1 else 0.0
    for y in range(dst_h):
        sy = y * y_ratio
        y0 = int(sy)
        y1 = min(y0 + 1, src_h - 1)
        fy = sy - y0
        row0 = y0 * src_w * 3
        row1 = y1 * src_w * 3
        drow = y * dst_w * 3
        for x in range(dst_w):
            sx = x * x_ratio
            x0 = int(sx)
            x1 = min(x0 + 1, src_w - 1)
            fx = sx - x0
            p00 = row0 + x0 * 3
            p01 = row0 + x1 * 3
            p10 = row1 + x0 * 3
            p11 = row1 + x1 * 3
            o = drow + x * 3
            for c in range(3):
                top = src[p00 + c] * (1 - fx) + src[p01 + c] * fx
                bot = src[p10 + c] * (1 - fx) + src[p11 + c] * fx
                out[o + c] = int(top * (1 - fy) + bot * fy)
    return DecodedImage(dst_w, dst_h, out)


def _letterbox(img, dst_w, dst_h):
    out = bytearray(dst_w * dst_h * 3)
    off_x = (dst_w - img.width) // 2
    off_y = (dst_h - img.height) // 2
    for y in range(img.height):
        src = y * img.width * 3
        dst = ((y + off_y) * dst_w + off_x) * 3
        out[dst:dst + img.width * 3] = img.pixels[src:src + img.width * 3]
    return DecodedImage(dst_w, dst_h, out)


def _center_crop(img, dst_w, dst_h):
    off_x = max(0, (img.width - dst_w) // 2)
    off_y = max(0, (img.height - dst_h) // 2)
    out = bytearray(dst_w * dst_h * 3)
    for y in range(dst_h):
        src = ((y + off_y) * img.width + off_x) * 3
        dst = y * dst_w * 3
        out[dst:dst + dst_w * 3] = img.pixels[src:src + dst_w * 3]
    return DecodedImage(dst_w, dst_h, out)


# ---------------------------------------------------------------------------
# RGB565 conversion with dithering
# ---------------------------------------------------------------------------
def to_rgb565(img, dither="floyd_steinberg", swap=False):
    """Convert a :class:`DecodedImage` to a little-endian RGB565 bytearray.

    Args:
        img: Source image.
        dither: ``"none"``, ``"floyd_steinberg"`` or ``"bayer"``.
        swap: Swap the two bytes of each pixel (some panels expect big-endian).

    Returns:
        ``bytearray`` of length ``width * height * 2``.
    """
    if dither == "floyd_steinberg":
        return _rgb565_fs(img, swap)
    if dither == "bayer":
        return _rgb565_bayer(img, swap)
    return _rgb565_plain(img, swap)


@native
def _rgb565_plain(img, swap):
    src = img.pixels
    n = img.width * img.height
    out = bytearray(n * 2)
    for i in range(n):
        s = i * 3
        r = src[s] >> 3
        g = src[s + 1] >> 2
        b = src[s + 2] >> 3
        v = (r << 11) | (g << 5) | b
        o = i * 2
        if swap:
            out[o] = v & 0xFF
            out[o + 1] = v >> 8
        else:
            out[o] = v >> 8
            out[o + 1] = v & 0xFF
    return out


@native
def _rgb565_bayer(img, swap):
    src = img.pixels
    w = img.width
    h = img.height
    out = bytearray(w * h * 2)
    bayer = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))
    for y in range(h):
        for x in range(w):
            i = y * w + x
            s = i * 3
            t = bayer[y & 3][x & 3] / 16.0 - 0.5
            r = _clamp(src[s] + t * 8) >> 3
            g = _clamp(src[s + 1] + t * 4) >> 2
            b = _clamp(src[s + 2] + t * 8) >> 3
            v = (r << 11) | (g << 5) | b
            o = i * 2
            if swap:
                out[o] = v & 0xFF
                out[o + 1] = v >> 8
            else:
                out[o] = v >> 8
                out[o + 1] = v & 0xFF
    return out


@native
def _rgb565_fs(img, swap):
    w = img.width
    h = img.height
    src = img.pixels
    # Work on a float error buffer for one row plus the next.
    cur = [0.0] * (w * 3)
    nxt = [0.0] * (w * 3)
    out = bytearray(w * h * 2)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            s = i * 3
            r = src[s] + cur[x * 3]
            g = src[s + 1] + cur[x * 3 + 1]
            b = src[s + 2] + cur[x * 3 + 2]
            r5 = _clamp(r) >> 3
            g6 = _clamp(g) >> 2
            b5 = _clamp(b) >> 3
            er = r - (r5 << 3)
            eg = g - (g6 << 2)
            eb = b - (b5 << 3)
            v = (r5 << 11) | (g6 << 5) | b5
            o = i * 2
            if swap:
                out[o] = v & 0xFF
                out[o + 1] = v >> 8
            else:
                out[o] = v >> 8
                out[o + 1] = v & 0xFF
            # Distribute error (7/16 right, 3/16 down-left, 5/16 down, 1/16 down-right).
            if x + 1 < w:
                cur[(x + 1) * 3] += er * 7 / 16
                cur[(x + 1) * 3 + 1] += eg * 7 / 16
                cur[(x + 1) * 3 + 2] += eb * 7 / 16
            if y + 1 < h:
                if x > 0:
                    nxt[(x - 1) * 3] += er * 3 / 16
                    nxt[(x - 1) * 3 + 1] += eg * 3 / 16
                    nxt[(x - 1) * 3 + 2] += eb * 3 / 16
                nxt[x * 3] += er * 5 / 16
                nxt[x * 3 + 1] += eg * 5 / 16
                nxt[x * 3 + 2] += eb * 5 / 16
                if x + 1 < w:
                    nxt[(x + 1) * 3] += er / 16
                    nxt[(x + 1) * 3 + 1] += eg / 16
                    nxt[(x + 1) * 3 + 2] += eb / 16
        cur, nxt = nxt, [0.0] * (w * 3)
    return out


# ---------------------------------------------------------------------------
# EXIF orientation
# ---------------------------------------------------------------------------
def parse_exif_orientation(data):
    """Return the EXIF orientation tag (1-8) or 1 if absent.

    Only JPEG APP1/Exif segments are inspected; other formats return 1.
    """
    if len(data) < 4 or data[0] != 0xFF or data[1] != 0xD8:
        return 1
    pos = 2
    n = len(data)
    while pos + 4 <= n:
        if data[pos] != 0xFF:
            pos += 1
            continue
        marker = data[pos + 1]
        if marker == 0xE1:  # APP1
            length = struct.unpack_from(">H", data, pos + 2)[0]
            seg = data[pos + 4:pos + 2 + length]
            if seg[0:6] == b"Exif\x00\x00":
                return _exif_orientation_from_tiff(seg[6:])
            pos += 2 + length
        elif marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            pos += 2
        else:
            if pos + 4 > n:
                break
            length = struct.unpack_from(">H", data, pos + 2)[0]
            pos += 2 + length
    return 1


def _exif_orientation_from_tiff(tiff):
    if len(tiff) < 8:
        return 1
    if tiff[0:2] == b"II":
        endian = "<"
    elif tiff[0:2] == b"MM":
        endian = ">"
    else:
        return 1
    ifd_offset = struct.unpack_from(endian + "I", tiff, 4)[0]
    if ifd_offset + 2 > len(tiff):
        return 1
    count = struct.unpack_from(endian + "H", tiff, ifd_offset)[0]
    entry = ifd_offset + 2
    for _ in range(count):
        if entry + 12 > len(tiff):
            break
        tag = struct.unpack_from(endian + "H", tiff, entry)[0]
        if tag == 0x0112:  # Orientation
            value = struct.unpack_from(endian + "H", tiff, entry + 8)[0]
            return value if 1 <= value <= 8 else 1
        entry += 12
    return 1


def apply_orientation(img, orientation):
    """Rotate/flip ``img`` according to an EXIF orientation value (1-8)."""
    if orientation == 1:
        return img
    w, h = img.width, img.height
    src = img.pixels
    if orientation in (5, 6, 7, 8):
        out_w, out_h = h, w
    else:
        out_w, out_h = w, h
    out = bytearray(out_w * out_h * 3)
    for y in range(h):
        for x in range(w):
            if orientation == 2:  # flip horizontal
                nx, ny = w - 1 - x, y
            elif orientation == 3:  # rotate 180
                nx, ny = w - 1 - x, h - 1 - y
            elif orientation == 4:  # flip vertical
                nx, ny = x, h - 1 - y
            elif orientation == 5:  # transpose
                nx, ny = y, x
            elif orientation == 6:  # rotate 90 CW
                nx, ny = h - 1 - y, x
            elif orientation == 7:  # transverse
                nx, ny = h - 1 - y, w - 1 - x
            elif orientation == 8:  # rotate 90 CCW
                nx, ny = y, w - 1 - x
            else:
                nx, ny = x, y
            s = (y * w + x) * 3
            d = (ny * out_w + nx) * 3
            out[d] = src[s]
            out[d + 1] = src[s + 1]
            out[d + 2] = src[s + 2]
    return DecodedImage(out_w, out_h, out)
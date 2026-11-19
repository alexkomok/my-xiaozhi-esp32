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
"""Transition effects for the slideshow.

Each transition implements the :class:`Transition` interface, taking a source
frame and an alpha and returning a destination frame of identical size.  All
methods write into the caller-supplied destination bytearray so that no
per-frame allocation is required.  The effect of a single frame is
``src * (1 - alpha) + dst * alpha`` for alpha in ``[0.0, 1.0]``.

A Ken Burns camera motion is applied on top: a slow pan/zoom over the
``slide_duration`` of the image.
"""

try:
    import math
except ImportError:
    import math as math  # fallback for CPython test harness

__all__ = ["Transition", "NoneTransition", "FadeTransition", "SlideTransition",
           "WipeTransition", "ZoomTransition", "KenBurns", "blend"]


def blend(a, b, t, depth=2):
    """Blend two integer values ``a`` and ``b`` with interpolation ``t``.

    For 16-bit RGB565 this accumulates a running delta to avoid a multiply per
    pixel, which speeds up the hot loop on the ESP32.
    """
    if depth == 2:
        return (a & 0xFFFF) + (((b & 0xFFFF) - (a & 0xFFFF)) * t >> depth)
    r0 = a >> 16
    g0 = (a >> 8) & 0xFF
    b0 = a & 0xFF
    r1 = b >> 16
    g1 = (b >> 8) & 0xFF
    b1 = b & 0xFF
    v = ((r0 + (r1 - r0) * t) << 16) | ((g0 + (g1 - g0) * t) << 8) | (b0 + (b1 - b0) * t)
    return v & 0xFFFFFF


class Transition:
    """Base class for frame transitions.

    Subclasses implement ``step(src, dst, alpha, w, h, ...)``, which computes one
    frame of the transition.  ``alpha`` ranges from ``0.0`` (source fully visible)
    to ``1.0`` (destination fully visible).
    """

    NAME = "base"

    def step(self, src, dst, alpha, w, h, x0, y0, x1, y1, **kwargs):
        raise NotImplementedError

    def reset(self, w, h):
        """Reset internal state; called before each transition."""


class NoneTransition(Transition):
    """Instant switch with no effect."""

    NAME = "none"

    def step(self, src, dst, alpha, w, h, x0, y0, x1, y1, **kwargs):
        # Nothing to blend; the caller handles copying when alpha == 0.
        pass

    def reset(self, w, h):
        pass


class FadeTransition(Transition):
    """Cross-dissolve between the two frames."""

    NAME = "fade"

    def step(self, src, dst, alpha, w, h, x0, y0, x1, y1, **kwargs):
        t = int(alpha * 255)
        one_minus = 255 - t
        n = w * h
        for i in range(n):
            a = src[i]
            b = dst[i]
            r0 = a >> 8
            g0 = a & 0xFF
            r1 = b >> 8
            g1 = b & 0xFF
            ra = (r0 * one_minus + r1 * t) >> 8
            ga = (g0 * one_minus + g1 * t) >> 8
            o = i << 1
            dst[o] = ra
            dst[o + 1] = ga


class SlideTransition(Transition):
    """Slide the destination over the source from a given direction."""

    NAME = "slide"

    def __init__(self):
        self._offset = 0

    def step(self, src, dst, alpha, w, h, x0, y0, x1, y1, direction="right", **kwargs):
        t = alpha if alpha > 0.0 else 0.001
        n = w * h
        # Determine a slide width that grows with alpha (clamped to screen).
        sw = max(1, int(w * t))
        # Move the source so that it exits from the chosen direction.
        if direction == "right":
            srow = (y0 * w + max(0, sw)) * 3
            cpx = 0
        elif direction == "left":
            srow = (y0 * w + max(0, w - sw)) * 3
            cpx = max(0, w - sw)
        elif direction == "down":
            srow = (max(0, h - sw) * w + x0) * 3
            cpx = x0
        else:  # up
            srow = (y0 * w + x0) * 3
            cpx = x0
        for y in range(y0, y1):
            dr = y * w * 3
            sr = srow + (y - y0) * w * 3
            for x in range(x0, x1):
                if direction in ("right", "left") and x < cpx:
                    continue
                if direction in ("down", "up") and y < (y0 + (sw if direction == "down" else 0)):
                    continue
                s = sr + x * 3
                d = dr + x * 3
                dst[d] = src[s]
                dst[d + 1] = src[s + 1]
                dst[d + 2] = src[s + 2]


class WipeTransition(Transition):
    """Linear wipe from a start region to an end region."""

    NAME = "wipe"

    def step(self, src, dst, alpha, w, h, x0, y0, x1, y1, direction="left", **kwargs):
        t = alpha if alpha > 0.0 else 0.001
        n = w * h
        if direction == "left":
            limit = x0 + int((x1 - x0) * t)
            for y in range(y0, y1):
                sr = y * w * 3
                dr = y * w * 3
                for x in range(x0, limit):
                    s = sr + x * 3
                    d = dr + x * 3
                    dst[d] = src[s]
                    dst[d + 1] = src[s + 1]
                    dst[d + 2] = src[s + 2]
        elif direction == "right":
            limit = x1 - int((x1 - x0) * t)
            for y in range(y0, y1):
                sr = y * w * 3
                dr = y * w * 3
                for x in range(limit, x1):
                    s = sr + x * 3
                    d = dr + x * 3
                    dst[d] = src[s]
                    dst[d + 1] = src[s + 1]
                    dst[d + 2] = src[s + 2]
        elif direction == "up":
            limit = y1 - int((y1 - y0) * t)
            for y in range(y0, limit):
                sr = y * w * 3
                dr = y * w * 3
                for x in range(x0, x1):
                    s = sr + x * 3
                    d = dr + x * 3
                    dst[d] = src[s]
                    dst[d + 1] = src[s + 1]
                    dst[d + 2] = src[s + 2]
        else:  # down
            limit = y0 + int((y1 - y0) * t)
            for y in range(limit, y1):
                sr = y * w * 3
                dr = y * w * 3
                for x in range(x0, x1):
                    s = sr + x * 3
                    d = dr + x * 3
                    dst[d] = src[s]
                    dst[d + 1] = src[s + 1]
                    dst[d + 2] = src[s + 2]


class ZoomTransition(Transition):
    """Zoom the destination into view from a scaled source."""

    NAME = "zoom"

    def __init__(self):
        self._scale = 1.0
        self._cx = 0
        self._cy = 0

    def step(self, src, dst, alpha, w, h, x0, y0, x1, y1, **kwargs):
        t = alpha if alpha > 0.0 else 0.001
        self._scale = 1.0 - 0.30 * t  # zoom from 0.7 up to 1.0
        self._cx = int(w * 0.5)
        self._cy = int(h * 0.5)
        # Fill background with the current frame (src) first, then zoom the dst
        # window into the centre as alpha grows.
        src.fill(0)
        n = w * h * 3
        dst[0:n] = src[0:n]
        sw = int(w * self._scale)
        sh = int(h * self._scale)
        x0s = (w - sw) // 2
        y0s = (h - sh) // 2
        for yy in range(sh):
            srcy = y0s + yy
            dsty = y0 + (h - sh) // 2 + yy
            if dsty >= h:
                break
            srow = srcy * w * 3
            drow = dsty * w * 3
            for xx in range(sw):
                sx = x0s + xx
                dx = x0 + (w - sw) // 2 + xx
                if dx >= w:
                    break
                s = srow + sx * 3
                d = drow + dx * 3
                dst[d] = src[s]
                dst[d + 1] = src[s + 1]
                dst[d + 2] = src[s + 2]

    def reset(self, w, h):
        self._scale = 1.0
        self._cx = int(w * 0.5)
        self._cy = int(h * 0.5)


class KenBurns:
    """Slow pan-and-zoom for a static image over ``duration`` seconds.

    Attributes:
        duration: Seconds for one pan/zoom cycle (``playback.ken_burns_intensity``
            is mapped to ``1.0`` (slow, 1 cycle per slide) .. ``0`` (no motion)).
    """

    def __init__(self, duration=30.0):
        self.duration = duration
        self._t0 = None
        self._phase = 0.0

    def reset(self, slide_seconds):
        self.duration = max(1.0, slide_seconds) if self.duration <= 0 else self.duration
        self._phase = 0.0

    def set_alpha(self, alpha):
        """Return the Ken Burns pan/zoom alpha at time ``alpha`` (``0.0..1.0``).

        This drives the blend amount: as the camera moves, the source frame
        shrinks and the destination frame grows.
        """
        if self.duration <= 0:
            return 0.0
        self._phase += 1.0 / 60.0
        if self._phase >= self.duration:
            self._phase = 0.0
        phase = math.sin((self._phase / self.duration) * math.pi * 2)
        return max(0.0, min(1.0, 0.5 + 0.5 * phase))
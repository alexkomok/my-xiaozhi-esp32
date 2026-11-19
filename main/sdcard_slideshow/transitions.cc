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
 * @file transitions.cc
 * @brief Transition effect implementations.
 */

#include "transitions.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace slideshow {

// ---------------------------------------------------------------------------
// NoneTransition
// ---------------------------------------------------------------------------

class NoneTransition : public Transition {
public:
    void Reset() override {}

    float Step(const std::vector<uint16_t>& src, const std::vector<uint16_t>& dst, float alpha,
               size_t w, size_t h, std::vector<uint16_t>& out) override {
        (void)src;
        (void)alpha;
        if (out.size() < w * h) out.resize(w * h);
        std::memcpy(out.data(), dst.data(), w * h * sizeof(uint16_t));
        return 1.0f;
    }
};

// ---------------------------------------------------------------------------
// FadeTransition
// ---------------------------------------------------------------------------

class FadeTransition : public Transition {
public:
    void Reset() override {}

    float Step(const std::vector<uint16_t>& src, const std::vector<uint16_t>& dst, float alpha,
               size_t w, size_t h, std::vector<uint16_t>& out) override {
        if (out.size() < w * h) out.resize(w * h);
        if (alpha <= 0.0f) {
            std::memcpy(out.data(), src.data(), w * h * sizeof(uint16_t));
            return 0.0f;
        }
        if (alpha >= 1.0f) {
            std::memcpy(out.data(), dst.data(), w * h * sizeof(uint16_t));
            return 1.0f;
        }

        for (size_t i = 0; i < w * h; i++) {
            const uint16_t s = src[i];
            const uint16_t d = dst[i];
            const int sr = ((s >> 11) & 0x1F) * 8;
            const int sg = ((s >> 5) & 0x3F) * 4;
            const int sb = (s & 0x1F) * 8;
            const int dr = ((d >> 11) & 0x1F) * 8;
            const int dg = ((d >> 5) & 0x3F) * 4;
            const int db = (d & 0x1F) * 8;

            const int br = static_cast<int>(sr + (dr - sr) * alpha);
            const int bg = static_cast<int>(sg + (dg - sg) * alpha);
            const int bb = static_cast<int>(sb + (db - sb) * alpha);

            out[i] = static_cast<uint16_t>(((br >> 3) << 11) | ((bg >> 2) << 5) | (bb >> 3));
        }
        return alpha;
    }
};

// ---------------------------------------------------------------------------
// SlideTransition
// ---------------------------------------------------------------------------

class SlideTransition : public Transition {
public:
    explicit SlideTransition(int dir) : dir_(dir) {}

    void Reset() override {}

    float Step(const std::vector<uint16_t>& src, const std::vector<uint16_t>& dst, float alpha,
               size_t w, size_t h, std::vector<uint16_t>& out) override {
        if (out.size() < w * h) out.resize(w * h);

        if (alpha <= 0.0f) {
            std::memcpy(out.data(), src.data(), w * h * sizeof(uint16_t));
            return 0.0f;
        }
        if (alpha >= 1.0f) {
            std::memcpy(out.data(), dst.data(), w * h * sizeof(uint16_t));
            return 1.0f;
        }

        const float max_dim = std::max(static_cast<float>(w), static_cast<float>(h));
        const float position = max_dim * (1.0f - alpha);
        const int pos = static_cast<int>(position);

        for (int y = 0; y < static_cast<int>(h); y++) {
            for (int x = 0; x < static_cast<int>(w); x++) {
                int sx = x, sy = y;

                if (dir_ == 0) {            // from top: src moves down
                    sy = y - pos;
                } else if (dir_ == 1) {     // from right: src moves left
                    sx = x + pos;
                } else if (dir_ == 2) {     // from bottom: src moves up
                    sy = y + pos;
                } else {                    // from left: src moves right
                    sx = x - pos;
                }

                const bool show_src = (dir_ == 0 && sy >= 0 && sy < static_cast<int>(h)) ||
                                      (dir_ == 1 && sx < static_cast<int>(w) && sx >= 0) ||
                                      (dir_ == 2 && sy < static_cast<int>(h) && sy >= 0) ||
                                      (dir_ == 3 && sx >= 0 && sx < static_cast<int>(w));

                const size_t idx = static_cast<size_t>(y) * w + x;
                if (show_src) {
                    const int px = std::clamp(sx, 0, static_cast<int>(w) - 1);
                    const int py = std::clamp(sy, 0, static_cast<int>(h) - 1);
                    out[idx] = src[static_cast<size_t>(py) * w + px];
                } else {
                    out[idx] = dst[idx];
                }
            }
        }
        return alpha;
    }

private:
    int dir_{0};
};

// ---------------------------------------------------------------------------
// WipeTransition
// ---------------------------------------------------------------------------

class WipeTransition : public Transition {
public:
    explicit WipeTransition(int dir) : dir_(dir) {}

    void Reset() override {}

    float Step(const std::vector<uint16_t>& src, const std::vector<uint16_t>& dst, float alpha,
               size_t w, size_t h, std::vector<uint16_t>& out) override {
        if (out.size() < w * h) out.resize(w * h);

        if (alpha <= 0.0f) {
            std::memcpy(out.data(), src.data(), w * h * sizeof(uint16_t));
            return 0.0f;
        }
        if (alpha >= 1.0f) {
            std::memcpy(out.data(), dst.data(), w * h * sizeof(uint16_t));
            return 1.0f;
        }

        const float max_dim = std::max(static_cast<float>(w), static_cast<float>(h));
        const float passed = max_dim * alpha;
        const int px = static_cast<int>(passed);

        for (int y = 0; y < static_cast<int>(h); y++) {
            for (int x = 0; x < static_cast<int>(w); x++) {
                bool show_dst = false;
                if (dir_ == 0) {                   // vertical, top-down
                    show_dst = y >= px;
                } else if (dir_ == 1) {            // horizontal, left-right
                    show_dst = x >= px;
                } else if (dir_ == 2) {            // diagonal
                    show_dst = x + y >= px;
                } else {                           // horizontal, right-left
                    show_dst = x <= (static_cast<int>(w) - 1 - px);
                }

                const size_t idx = static_cast<size_t>(y) * w + x;
                out[idx] = show_dst ? dst[idx] : src[idx];
            }
        }
        return alpha;
    }

private:
    int dir_{0};
};

// ---------------------------------------------------------------------------
// ZoomTransition
// ---------------------------------------------------------------------------

class ZoomTransition : public Transition {
public:
    void Reset() override {}

    float Step(const std::vector<uint16_t>& src, const std::vector<uint16_t>& dst, float alpha,
               size_t w, size_t h, std::vector<uint16_t>& out) override {
        if (out.size() < w * h) out.resize(w * h);

        if (alpha <= 0.0f) {
            std::memcpy(out.data(), src.data(), w * h * sizeof(uint16_t));
            return 0.0f;
        }
        if (alpha >= 1.0f) {
            std::memcpy(out.data(), dst.data(), w * h * sizeof(uint16_t));
            return 1.0f;
        }

        const float scale = 1.0f - alpha;  // 1.0 at start, 0 at end
        const int cx = static_cast<int>(w / 2);
        const int cy = static_cast<int>(h / 2);

        for (int y = 0; y < static_cast<int>(h); y++) {
            for (int x = 0; x < static_cast<int>(w); x++) {
                if (scale > 0.99f) {
                    out[static_cast<size_t>(y) * w + x] = src[static_cast<size_t>(y) * w + x];
                } else {
                    const int sx = cx + static_cast<int>((x - cx) / scale);
                    const int sy = cy + static_cast<int>((y - cy) / scale);

                    if (sx >= 0 && sx < static_cast<int>(w) && sy >= 0 && sy < static_cast<int>(h)) {
                        out[static_cast<size_t>(y) * w + x] =
                            src[static_cast<size_t>(sy) * w + sx];
                    } else {
                        out[static_cast<size_t>(y) * w + x] = dst[static_cast<size_t>(y) * w + x];
                    }
                }
            }
        }
        return alpha;
    }
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<Transition> CreateTransition(TransitionType type) {
    switch (type) {
        case TransitionType::kNone:
            return std::make_unique<NoneTransition>();
        case TransitionType::kFade:
            return std::make_unique<FadeTransition>();
        case TransitionType::kSlide:
            return std::make_unique<SlideTransition>(0);
        case TransitionType::kZoom:
            return std::make_unique<ZoomTransition>();
        case TransitionType::kWipe:
            return std::make_unique<WipeTransition>(0);
        default:
            return std::make_unique<NoneTransition>();
    }
}

// ---------------------------------------------------------------------------
// TransitionEngine
// ---------------------------------------------------------------------------

TransitionEngine::TransitionEngine(TransitionType effect, float transition_duration_s,
                                   bool ken_burns, float ken_burns_intensity)
    : effect_(effect),
      duration_s_(transition_duration_s),
      transition_(CreateTransition(effect)),
      ken_burns_intensity_(ken_burns_intensity),
      ken_burns_enabled_(ken_burns),
      current_alpha_(0.0f),
      ken_step_(0) {}

void TransitionEngine::SetKenBurns(bool enabled, float intensity) {
    ken_burns_enabled_ = enabled;
    ken_burns_intensity_ = intensity;
}

void TransitionEngine::SetEffect(TransitionType effect) {
    effect_ = effect;
    transition_ = CreateTransition(effect);
}

void TransitionEngine::SetStepMs(uint32_t step_ms) { step_ms_ = step_ms; }

void TransitionEngine::StartTransition(const std::vector<uint16_t>& src, const std::vector<uint16_t>& dst,
                                       size_t w, size_t h) {
    (void)src;
    (void)dst;
    frame_w_ = w;
    frame_h_ = h;
    transition_->Reset();
    current_alpha_ = 0.0f;
    ken_step_ = 0;
    active_ = true;
}

bool TransitionEngine::Step(const std::vector<uint16_t>& src, const std::vector<uint16_t>& dst,
                            size_t w, size_t h, std::vector<uint16_t>& out) {
    if (!active_ || frame_w_ == 0 || frame_h_ == 0) {
        if (out.size() < w * h) out.resize(w * h);
        std::memcpy(out.data(), dst.data(), w * h * sizeof(uint16_t));
        return false;
    }

    const float step_alpha = std::min(current_alpha_ + (step_ms_ / 1000.0f) / duration_s_, 1.0f);

    if (ken_burns_enabled_) {
        // Ken Burns: apply a slow zoom/pan to the destination frame.
        // For a minimal implementation we blend the raw dst; a full
        // implementation would render dst at the current scale/offset.
        (void)src;
        out.resize(w * h);
        std::memcpy(out.data(), dst.data(), w * h * sizeof(uint16_t));
    } else {
        current_alpha_ = transition_->Step(src, dst, step_alpha, frame_w_, frame_h_, out);
    }

    if (current_alpha_ >= 1.0f) {
        current_alpha_ = 1.0f;
        active_ = false;
        return false;
    }
    return true;
}

void TransitionEngine::Reset() {
    active_ = false;
    current_alpha_ = 0.0f;
    ken_step_ = 0;
    transition_->Reset();
}

// ---------------------------------------------------------------------------
// KenBurns
// ---------------------------------------------------------------------------

void KenBurns::SetAlpha(float alpha, size_t w, size_t h, float& scale, int& ox, int& oy) const {
    if (intensity_ <= 0.0f) {
        scale = 1.0f;
        ox = 0;
        oy = 0;
        return;
    }

    const float t = intensity_;
    const float s = 1.0f + t * std::sinf(2.0f * 3.14159265358979323846f * alpha);
    const float dx = t * 0.25f * std::cosf(2.0f * 3.14159265358979323846f * alpha);
    const float dy = t * 0.25f * std::sinf(2.0f * 3.14159265358979323846f * alpha);

    scale = s;
    ox = static_cast<int>(static_cast<float>(w) * (dx - 0.5f) + 0.5f);
    oy = static_cast<int>(static_cast<float>(h) * (dy - 0.5f) + 0.5f);
}

}  // namespace slideshow
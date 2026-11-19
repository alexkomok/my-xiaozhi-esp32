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
 * @file transitions.h
 * @brief Visual transition effects for the slideshow engine.
 *
 * This is the C++ counterpart of ``micropython/sdcard_slideshow/transitions.py``.
 * It supports:
 *
 * - ``kNone``    -- instant switch.
 * - ``kFade``    -- cross-dissolve between two RGB565 frames.
 * - ``kSlide``   -- slide the incoming image from one of four directions.
 * - ``kZoom``    -- zoom-in from the center (Ken Burns style).
 * - ``kWipe``    -- wipe the incoming image across the screen.
 *
 * Transition steps produce a destination frame given a source frame and the
 * target frame, so transitions can be applied on top of the existing
 * ``ShowMediaFrame`` path without duplicating display code.
 */

#ifndef MAIN_SDCARD_SLIDESHOW_TRANSITIONS_H_
#define MAIN_SDCARD_SLIDESHOW_TRANSITIONS_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sdcard_slideshow.h"  // TransitionType

namespace slideshow {

/**
 * @brief Base class for transitions.
 *
 * A transition blends a ``src`` frame into a ``dst`` frame according to an
 * @c alpha value in the range [0.0, 1.0].  @c alpha == 0 produces the source,
 * @c alpha == 1 produces the destination.
 */
class Transition {
public:
    virtual ~Transition() = default;

    /**
     * Step the transition forward.
     *
     * @param src     Source frame (RGB565, width x height).
     * @param dst     Destination frame (RGB565, width x height).
     * @param alpha   Current alpha [0.0, 1.0]; this call advances it.
     * @param w       Frame width.
     * @param h       Frame height.
     * @param out     Output buffer (w x h), written by this call.
     * @return The next alpha value, or 1.0 when the transition is complete.
     */
    virtual float Step(const std::vector<uint16_t>& src, const std::vector<uint16_t>& dst,
                       float alpha, size_t w, size_t h, std::vector<uint16_t>& out) = 0;

    /** Reset the transition state so the next run starts at alpha 0. */
    virtual void Reset() = 0;
};

/** Transition effect identifiers, kept in sync with sdcard_slideshow.h. */
using TransitionType = slideshow::TransitionType;

/**
 * @brief Create a transition object from the effect identifier.
 */
std::unique_ptr<Transition> CreateTransition(TransitionType type);

/**
 * @brief A group of transitions that animates a sequence of slides.
 *
 * The @c TransitionEngine drives a single transition per slide change,
 * stepping it every @c step_ms milliseconds.  It supports Ken Burns pan/zoom
 * and a configurable step interval, and it reports the current alpha so the
 * caller can know when the transition has finished.
 */
class TransitionEngine {
public:
    TransitionEngine(TransitionType effect = TransitionType::kFade, float transition_duration_s = 0.8f,
                     bool ken_burns = false, float ken_burns_intensity = 0.08f);

    /** Configure Ken Burns. @c intensity controls pan/zoom strength [0, 1]. */
    void SetKenBurns(bool enabled, float intensity);

    /** Set the transition effect. */
    void SetEffect(TransitionType effect);

    /** Set the step interval in milliseconds. */
    void SetStepMs(uint32_t step_ms);

    /** Advance the active transition by one step. Returns true if still active. */
    bool Step(const std::vector<uint16_t>& src, const std::vector<uint16_t>& dst, size_t w, size_t h,
              std::vector<uint16_t>& out);

    /** Set a fresh transition from src to dst. */
    void StartTransition(const std::vector<uint16_t>& src, const std::vector<uint16_t>& dst, size_t w,
                         size_t h);

    /** @return True while a transition is in progress. */
    bool IsActive() const { return active_ && current_alpha_ < 1.0f; }

    /** @return Current alpha in [0, 1]. */
    float Alpha() const { return current_alpha_; }

    /** @return Current step ms. */
    uint32_t StepMs() const { return step_ms_; }

    /** Reset any in-flight transition. */
    void Reset();

private:
    TransitionType effect_;
    float duration_s_;
    std::unique_ptr<Transition> transition_;
    float ken_burns_intensity_;
    bool ken_burns_enabled_;
    std::vector<float> ken_burns_scale_;   // scale at each step
    std::vector<int> ken_burns_x_;
    std::vector<int> ken_burns_y_;
    uint32_t step_ms_{40};
    float current_alpha_{0.0f};
    int ken_step_{0};
    int ken_total_steps_{0};
    bool active_{false};
    size_t frame_w_{0};
    size_t frame_h_{0};
};

/**
 * Ken Burns animation helpers.  @c SetAlpha() computes the pan/zoom offset and
 * scale for the given frame alpha so the caller can render the current slide
 * with a smooth drift.
 */
class KenBurns {
public:
    KenBurns() : KenBurns(0.08f) {}

    explicit KenBurns(float intensity)
        : intensity_(intensity), phase_x_(0.0f), phase_y_(0.0f) {}

    /** Set the frame alpha [0, 1]; returns the scale and origin offset. */
    void SetAlpha(float alpha, size_t w, size_t h, float& scale, int& ox, int& oy) const;

private:
    float intensity_;
    float phase_x_;
    float phase_y_;
};

}  // namespace slideshow

#endif  // MAIN_SDCARD_SLIDESHOW_TRANSITIONS_H_

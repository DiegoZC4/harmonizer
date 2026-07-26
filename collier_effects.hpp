#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace harmonizer {

static constexpr size_t kFreezeLayerCount = 3;

struct CollierEffectSettings {
    std::array<bool, kFreezeLayerCount> freezeHold{};
    std::array<float, kFreezeLayerCount> freezeLevel{0.5f, 0.5f, 0.5f};
    std::array<float, kFreezeLayerCount> freezeTranspose{};
    std::array<uint32_t, kFreezeLayerCount> freezeClearGeneration{};
    float freezeTone = 1.0f;
    float chorusMix = 0.0f;
    float reverbMix = 0.0f;
};

struct CollierEffectState {
    std::array<bool, kFreezeLayerCount> freezeActive{};
};

namespace detail {

static constexpr float kFxPi = 3.14159265358979323846f;

inline float clamp(float value, float low, float high) {
    return std::max(low, std::min(value, high));
}

class CombFilter {
public:
    CombFilter() = default;
    explicit CombFilter(size_t length) : buffer_(std::max<size_t>(length, 1), 0.0f) {}

    float process(float input, float feedback, float damping) {
        const float output = buffer_[position_];
        filterStore_ = output * (1.0f - damping) + filterStore_ * damping;
        buffer_[position_] = input + filterStore_ * feedback;
        position_ = (position_ + 1) % buffer_.size();
        return output;
    }

    void clear() {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        position_ = 0;
        filterStore_ = 0.0f;
    }

private:
    std::vector<float> buffer_{1, 0.0f};
    size_t position_ = 0;
    float filterStore_ = 0.0f;
};

class AllpassFilter {
public:
    AllpassFilter() = default;
    explicit AllpassFilter(size_t length) : buffer_(std::max<size_t>(length, 1), 0.0f) {}

    float process(float input) {
        const float delayed = buffer_[position_];
        const float output = delayed - input;
        buffer_[position_] = input + delayed * 0.5f;
        position_ = (position_ + 1) % buffer_.size();
        return output;
    }

    void clear() {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        position_ = 0;
    }

private:
    std::vector<float> buffer_{1, 0.0f};
    size_t position_ = 0;
};

// A compact Freeverb-style Schroeder network. Freeze mode uses unity comb
// feedback with no damping, so its stored tail remains bounded and does not
// need a regenerated fundamental.
class StereoDiffusionReverb {
public:
    explicit StereoDiffusionReverb(float sampleRate) {
        static constexpr std::array<int, 8> kCombLengths{
            1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617,
        };
        static constexpr std::array<int, 4> kAllpassLengths{556, 441, 341, 225};
        const float scale = sampleRate / 44100.0f;
        for (size_t index = 0; index < combLeft_.size(); ++index) {
            const size_t left = static_cast<size_t>(std::lround(kCombLengths[index] * scale));
            const size_t right = static_cast<size_t>(std::lround((kCombLengths[index] + 23) * scale));
            combLeft_[index] = CombFilter(left);
            combRight_[index] = CombFilter(right);
        }
        for (size_t index = 0; index < allpassLeft_.size(); ++index) {
            const size_t left = static_cast<size_t>(std::lround(kAllpassLengths[index] * scale));
            const size_t right = static_cast<size_t>(std::lround((kAllpassLengths[index] + 23) * scale));
            allpassLeft_[index] = AllpassFilter(left);
            allpassRight_[index] = AllpassFilter(right);
        }
    }

    void process(float inputLeft, float inputRight, float feedback, float damping,
                 float inputGain, float& outputLeft, float& outputRight) {
        const float leftInput = (inputLeft * 0.88f + inputRight * 0.12f) * inputGain;
        const float rightInput = (inputRight * 0.88f + inputLeft * 0.12f) * inputGain;
        outputLeft = 0.0f;
        outputRight = 0.0f;
        for (size_t index = 0; index < combLeft_.size(); ++index) {
            outputLeft += combLeft_[index].process(leftInput, feedback, damping);
            outputRight += combRight_[index].process(rightInput, feedback, damping);
        }
        outputLeft /= static_cast<float>(combLeft_.size());
        outputRight /= static_cast<float>(combRight_.size());
        for (size_t index = 0; index < allpassLeft_.size(); ++index) {
            outputLeft = allpassLeft_[index].process(outputLeft);
            outputRight = allpassRight_[index].process(outputRight);
        }
    }

    void clear() {
        for (CombFilter& filter : combLeft_) filter.clear();
        for (CombFilter& filter : combRight_) filter.clear();
        for (AllpassFilter& filter : allpassLeft_) filter.clear();
        for (AllpassFilter& filter : allpassRight_) filter.clear();
    }

private:
    std::array<CombFilter, 8> combLeft_{};
    std::array<CombFilter, 8> combRight_{};
    std::array<AllpassFilter, 4> allpassLeft_{};
    std::array<AllpassFilter, 4> allpassRight_{};
};

class StereoPitchDelay {
public:
    explicit StereoPitchDelay(float sampleRate)
        : bufferLeft_(ringSize(sampleRate), 0.0f),
          bufferRight_(bufferLeft_.size(), 0.0f),
          mask_(bufferLeft_.size() - 1),
          windowSamples_(std::max(256.0f, sampleRate * 0.046f)),
          minimumDelay_(std::max(16.0f, sampleRate * 0.0015f)) {}

    void process(float inputLeft, float inputRight, float semitones,
                 float& outputLeft, float& outputRight) {
        bufferLeft_[writePosition_ & mask_] = inputLeft;
        bufferRight_[writePosition_ & mask_] = inputRight;

        const float clampedSemitones = clamp(semitones, -24.0f, 24.0f);
        if (std::fabs(clampedSemitones - lastSemitones_) >= 0.0001f) {
            lastSemitones_ = clampedSemitones;
            pitchRatio_ = std::pow(2.0f, clampedSemitones / 12.0f);
        }
        const float ratio = pitchRatio_;
        phase_ += (1.0f - ratio) / windowSamples_;
        phase_ -= std::floor(phase_);
        const float secondPhase = phase_ < 0.5f ? phase_ + 0.5f : phase_ - 0.5f;
        const float firstWindow = std::sin(kFxPi * phase_);
        const float firstWeight = firstWindow * firstWindow;
        const float secondWeight = 1.0f - firstWeight;

        const float firstDelay = minimumDelay_ + phase_ * windowSamples_;
        const float secondDelay = minimumDelay_ + secondPhase * windowSamples_;
        const float shiftedLeft = read(bufferLeft_, firstDelay) * firstWeight +
                                  read(bufferLeft_, secondDelay) * secondWeight;
        const float shiftedRight = read(bufferRight_, firstDelay) * firstWeight +
                                   read(bufferRight_, secondDelay) * secondWeight;

        const float targetMix = std::fabs(semitones) >= 0.005f ? 1.0f : 0.0f;
        shiftMix_ += (targetMix - shiftMix_) * 0.004f;
        outputLeft = inputLeft + (shiftedLeft - inputLeft) * shiftMix_;
        outputRight = inputRight + (shiftedRight - inputRight) * shiftMix_;
        ++writePosition_;
    }

    void clear() {
        std::fill(bufferLeft_.begin(), bufferLeft_.end(), 0.0f);
        std::fill(bufferRight_.begin(), bufferRight_.end(), 0.0f);
        writePosition_ = 0;
        phase_ = 0.25f;
        shiftMix_ = 0.0f;
        lastSemitones_ = 0.0f;
        pitchRatio_ = 1.0f;
    }

private:
    static size_t ringSize(float sampleRate) {
        size_t wanted = static_cast<size_t>(std::ceil(sampleRate * 0.12f));
        size_t size = 1;
        while (size < wanted) size <<= 1;
        return size;
    }

    float read(const std::vector<float>& buffer, float delay) const {
        const float readPosition = static_cast<float>(writePosition_) - delay;
        const int64_t base = static_cast<int64_t>(std::floor(readPosition));
        const float fraction = readPosition - static_cast<float>(base);
        const float first = buffer[static_cast<size_t>(base) & mask_];
        const float second = buffer[static_cast<size_t>(base + 1) & mask_];
        return first + (second - first) * fraction;
    }

    std::vector<float> bufferLeft_;
    std::vector<float> bufferRight_;
    size_t mask_ = 0;
    uint64_t writePosition_ = 0;
    float windowSamples_ = 2048.0f;
    float minimumDelay_ = 64.0f;
    float phase_ = 0.25f;
    float shiftMix_ = 0.0f;
    float lastSemitones_ = 0.0f;
    float pitchRatio_ = 1.0f;
};

class StereoChorus {
public:
    explicit StereoChorus(float sampleRate)
        : sampleRate_(sampleRate),
          bufferLeft_(ringSize(sampleRate), 0.0f),
          bufferRight_(bufferLeft_.size(), 0.0f),
          mask_(bufferLeft_.size() - 1) {}

    void process(float inputLeft, float inputRight, float mix,
                 float& outputLeft, float& outputRight) {
        bufferLeft_[writePosition_ & mask_] = inputLeft;
        bufferRight_[writePosition_ & mask_] = inputRight;
        const float amount = clamp(mix, 0.0f, 1.0f);
        if (amount <= 0.0001f) {
            outputLeft = inputLeft;
            outputRight = inputRight;
            advance();
            return;
        }
        const float leftDelay = sampleRate_ *
            (0.014f + 0.0035f * std::sin(phase_));
        const float rightDelay = sampleRate_ *
            (0.014f + 0.0035f * std::sin(phase_ + kFxPi * 0.5f));
        const float wetLeft = read(bufferLeft_, leftDelay);
        const float wetRight = read(bufferRight_, rightDelay);
        outputLeft = inputLeft + (wetLeft - inputLeft) * amount;
        outputRight = inputRight + (wetRight - inputRight) * amount;
        advance();
    }

private:
    void advance() {
        phase_ += 2.0f * kFxPi * 0.31f / sampleRate_;
        if (phase_ >= 2.0f * kFxPi) phase_ -= 2.0f * kFxPi;
        ++writePosition_;
    }
    static size_t ringSize(float sampleRate) {
        size_t wanted = static_cast<size_t>(std::ceil(sampleRate * 0.03f));
        size_t size = 1;
        while (size < wanted) size <<= 1;
        return size;
    }

    float read(const std::vector<float>& buffer, float delay) const {
        const float readPosition = static_cast<float>(writePosition_) - delay;
        const int64_t base = static_cast<int64_t>(std::floor(readPosition));
        const float fraction = readPosition - static_cast<float>(base);
        const float first = buffer[static_cast<size_t>(base) & mask_];
        const float second = buffer[static_cast<size_t>(base + 1) & mask_];
        return first + (second - first) * fraction;
    }

    float sampleRate_ = 44100.0f;
    std::vector<float> bufferLeft_;
    std::vector<float> bufferRight_;
    size_t mask_ = 0;
    uint64_t writePosition_ = 0;
    float phase_ = 0.0f;
};

class FreezeLayer {
public:
    explicit FreezeLayer(float sampleRate)
        : sampleRate_(sampleRate), reverb_(sampleRate), pitchShifter_(sampleRate) {}

    void process(float inputLeft, float inputRight, bool requestedHold,
                 uint32_t clearGeneration, float level, float transpose,
                 float tone, float& outputLeft, float& outputRight) {
        if (clearGeneration != seenClearGeneration_) {
            seenClearGeneration_ = clearGeneration;
            resetPending_ = true;
        }
        if (requestedHold) {
            if (!held_) held_ = true;
            resetPending_ = false;
        } else if (held_) {
            resetPending_ = true;
        }

        const float targetEnvelope = held_ && !resetPending_ ? 1.0f : 0.0f;
        const float envelopeSeconds = targetEnvelope > envelope_ ? 0.015f : 0.030f;
        const float envelopeCoefficient =
            1.0f - std::exp(-1.0f / std::max(1.0f, sampleRate_ * envelopeSeconds));
        envelope_ += (targetEnvelope - envelope_) * envelopeCoefficient;
        if (envelope_ < 0.0001f) envelope_ = 0.0f;

        float reverbLeft = 0.0f;
        float reverbRight = 0.0f;
        if (held_) {
            reverb_.process(0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                            reverbLeft, reverbRight);
        } else {
            reverb_.process(inputLeft, inputRight, 0.84f, 0.24f, 0.045f,
                            reverbLeft, reverbRight);
        }

        if (resetPending_ && envelope_ == 0.0f) {
            reverb_.clear();
            pitchShifter_.clear();
            toneLeft_ = 0.0f;
            toneRight_ = 0.0f;
            held_ = false;
            resetPending_ = false;
            reverb_.process(inputLeft, inputRight, 0.84f, 0.24f, 0.045f,
                            reverbLeft, reverbRight);
        }

        if (!held_ && envelope_ == 0.0f) {
            outputLeft = 0.0f;
            outputRight = 0.0f;
            return;
        }

        float shiftedLeft = 0.0f;
        float shiftedRight = 0.0f;
        pitchShifter_.process(reverbLeft, reverbRight, transpose,
                              shiftedLeft, shiftedRight);

        const float toneAmount = clamp(tone, 0.0f, 1.0f);
        if (toneAmount < 0.999f) {
            if (std::fabs(toneAmount - previousTone_) >= 0.0001f) {
                const float minimumCutoff = 420.0f;
                const float maximumCutoff = 18000.0f;
                const float cutoff = minimumCutoff *
                    std::pow(maximumCutoff / minimumCutoff, toneAmount);
                tonePole_ = std::exp(-2.0f * kFxPi * cutoff / sampleRate_);
                previousTone_ = toneAmount;
            }
            toneLeft_ = shiftedLeft * (1.0f - tonePole_) + toneLeft_ * tonePole_;
            toneRight_ = shiftedRight * (1.0f - tonePole_) + toneRight_ * tonePole_;
            shiftedLeft = toneLeft_;
            shiftedRight = toneRight_;
        } else {
            toneLeft_ = shiftedLeft;
            toneRight_ = shiftedRight;
        }

        const float gain = clamp(level, 0.0f, 1.25f) * envelope_;
        outputLeft = shiftedLeft * gain;
        outputRight = shiftedRight * gain;
    }

    bool active() const { return held_ || envelope_ > 0.0001f; }

private:
    float sampleRate_ = 44100.0f;
    StereoDiffusionReverb reverb_;
    StereoPitchDelay pitchShifter_;
    bool held_ = false;
    bool resetPending_ = false;
    uint32_t seenClearGeneration_ = 0;
    float envelope_ = 0.0f;
    float toneLeft_ = 0.0f;
    float toneRight_ = 0.0f;
    float previousTone_ = -1.0f;
    float tonePole_ = 0.0f;
};

inline float transparentCeiling(float value) {
    const float magnitude = std::fabs(value);
    if (magnitude <= 0.98f) return value;
    const float limited = 0.98f + 0.02f * std::tanh((magnitude - 0.98f) / 0.02f);
    return std::copysign(limited, value);
}

} // namespace detail

class CollierEffects {
public:
    explicit CollierEffects(float sampleRate)
        : chorus_(sampleRate), mainReverb_(sampleRate),
          freezeLayers_{detail::FreezeLayer(sampleRate),
                        detail::FreezeLayer(sampleRate),
                        detail::FreezeLayer(sampleRate)} {}

    CollierEffectState process(float inputLeft, float inputRight,
                               const CollierEffectSettings& settings,
                               float& outputLeft, float& outputRight) {
        float chorusLeft = 0.0f;
        float chorusRight = 0.0f;
        chorus_.process(inputLeft, inputRight, settings.chorusMix,
                        chorusLeft, chorusRight);

        const float reverbMix = detail::clamp(settings.reverbMix, 0.0f, 1.0f);
        float liveLeft = chorusLeft;
        float liveRight = chorusRight;
        float reverbLeft = 0.0f;
        float reverbRight = 0.0f;
        mainReverb_.process(chorusLeft, chorusRight, 0.88f, 0.28f, 0.035f,
                            reverbLeft, reverbRight);
        if (reverbMix > 0.0001f) {
            liveLeft += (reverbLeft - liveLeft) * reverbMix;
            liveRight += (reverbRight - liveRight) * reverbMix;
        }

        outputLeft = liveLeft;
        outputRight = liveRight;
        CollierEffectState state;
        for (size_t layer = 0; layer < freezeLayers_.size(); ++layer) {
            float freezeLeft = 0.0f;
            float freezeRight = 0.0f;
            freezeLayers_[layer].process(
                liveLeft, liveRight,
                settings.freezeHold[layer],
                settings.freezeClearGeneration[layer],
                settings.freezeLevel[layer],
                settings.freezeTranspose[layer],
                settings.freezeTone,
                freezeLeft, freezeRight);
            outputLeft += freezeLeft;
            outputRight += freezeRight;
            state.freezeActive[layer] = freezeLayers_[layer].active();
        }

        outputLeft = detail::transparentCeiling(outputLeft);
        outputRight = detail::transparentCeiling(outputRight);
        return state;
    }

private:
    detail::StereoChorus chorus_;
    detail::StereoDiffusionReverb mainReverb_;
    std::array<detail::FreezeLayer, kFreezeLayerCount> freezeLayers_;
};

} // namespace harmonizer

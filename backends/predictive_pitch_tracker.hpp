#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Causal alpha-beta tracker for source-F0 control. Measurements are timestamped
// at the center of the detector window; estimateAt() projects that state to the
// source-audio block a shifter is currently consuming.
class PredictivePitchTracker {
public:
    struct UpdateResult {
        bool onset = false;
        bool voiced = false;
        float confidence = 0.0f;
    };

    explicit PredictivePitchTracker(float sampleRate,
                                    float positionGain = kDefaultPositionGain,
                                    float velocityGain = kDefaultVelocityGain,
                                    float velocityResidualLimit =
                                        kDefaultVelocityResidualLimit)
        : sampleRate_(sampleRate),
          positionGain_(positionGain),
          velocityGain_(velocityGain),
          velocityResidualLimit_(velocityResidualLimit) {}

    UpdateResult update(float measuredMidi,
                        bool valid,
                        bool energetic,
                        bool stable,
                        uint64_t measurementSample) {
        UpdateResult result;
        const bool resumed = !wasEnergetic_ || missingHops_ > kResetMissingHops;
        wasEnergetic_ = energetic;

        if (!valid || !energetic || measuredMidi <= 0.0f) {
            missingHops_++;
            slopeSemitonesPerSecond_ *= energetic ? 0.65f : 0.0f;
            stableMeasurement_ = false;
            if (!energetic || missingHops_ > kVoicedHoldHops) voiced_ = false;
            result.voiced = voiced_;
            result.confidence = confidence();
            return result;
        }

        if (!initialized_ || resumed) {
            reset(measuredMidi, measurementSample);
            result.onset = true;
        } else {
            const float dt = std::max(
                1.0f / sampleRate_,
                (float)(measurementSample - stateSample_) / sampleRate_);
            const float predicted = stateMidi_ + slopeSemitonesPerSecond_ * dt;
            const float residual = measuredMidi - predicted;

            if (std::fabs(residual) >= kJumpResetSemitones) {
                reset(measuredMidi, measurementSample);
                result.onset = true;
            } else {
                stateMidi_ = predicted + positionGain_ * residual;
                const float velocityResidual = std::clamp(
                    residual, -velocityResidualLimit_, velocityResidualLimit_);
                slopeSemitonesPerSecond_ += velocityGain_ * velocityResidual / dt;
                slopeSemitonesPerSecond_ = std::clamp(
                    slopeSemitonesPerSecond_, -kMaxSlope, kMaxSlope);
                stateSample_ = measurementSample;
                coherentFrames_ = std::min(coherentFrames_ + 1, 8);
            }
        }

        missingHops_ = 0;
        voiced_ = true;
        stableMeasurement_ = stable;
        result.voiced = true;
        result.confidence = confidence();
        return result;
    }

    float estimateAt(uint64_t targetSample) const {
        if (!initialized_) return -1.0f;
        const int64_t deltaSamples = (int64_t)targetSample - (int64_t)stateSample_;
        const float horizon = std::clamp(
            (float)deltaSamples / sampleRate_, -kMaxProjectionSec, kMaxProjectionSec);
        return stateMidi_ + slopeSemitonesPerSecond_ * horizon;
    }

    bool voiced() const { return voiced_; }
    bool initialized() const { return initialized_; }
    float slope() const { return slopeSemitonesPerSecond_; }

    float confidence() const {
        if (!voiced_) return 0.0f;
        if (stableMeasurement_ || coherentFrames_ >= 3) return 1.0f;
        if (coherentFrames_ == 2) return 0.75f;
        return 0.35f;
    }

private:
    static constexpr float kDefaultPositionGain = 0.60f;
    static constexpr float kDefaultVelocityGain = 0.30f;
    // A single plausible-but-wrong F0 frame must not become a large projected
    // slope impulse. Position still follows the measurement; only the velocity
    // innovation is made robust.
    static constexpr float kDefaultVelocityResidualLimit = 0.12f;
    static constexpr float kJumpResetSemitones = 1.25f;
    static constexpr float kMaxSlope = 24.0f;
    static constexpr float kMaxProjectionSec = 0.055f;
    static constexpr int kVoicedHoldHops = 2;
    static constexpr int kResetMissingHops = 3;

    float sampleRate_ = 44100.0f;
    float positionGain_ = kDefaultPositionGain;
    float velocityGain_ = kDefaultVelocityGain;
    float velocityResidualLimit_ = kDefaultVelocityResidualLimit;
    bool initialized_ = false;
    bool voiced_ = false;
    bool wasEnergetic_ = false;
    bool stableMeasurement_ = false;
    float stateMidi_ = -1.0f;
    float slopeSemitonesPerSecond_ = 0.0f;
    uint64_t stateSample_ = 0;
    int coherentFrames_ = 0;
    int missingHops_ = 0;

    void reset(float measuredMidi, uint64_t measurementSample) {
        initialized_ = true;
        voiced_ = true;
        stableMeasurement_ = false;
        stateMidi_ = measuredMidi;
        slopeSemitonesPerSecond_ = 0.0f;
        stateSample_ = measurementSample;
        coherentFrames_ = 1;
        missingHops_ = 0;
    }
};

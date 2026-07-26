#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

// Keeps a lock-free input/output bridge near a fixed occupancy when the two
// PortAudio streams are driven by independent hardware clocks. The returned
// source step is input frames consumed per output frame.
class OutputBridgeClockController {
public:
    OutputBridgeClockController(double sampleRate, double targetFrames)
        : sampleRate_(sampleRate), targetFrames_(targetFrames) {}

    void reset(double initialFillFrames = 0.0) {
        initialized_ = false;
        filteredFillFrames_ = initialFillFrames;
        integralCorrection_ = 0.0;
        sourceStep_ = 1.0;
    }

    double update(double fillFrames, std::size_t outputFrames) {
        const double dt = std::max(
            1.0 / sampleRate_, static_cast<double>(outputFrames) / sampleRate_);
        if (!initialized_) {
            filteredFillFrames_ = fillFrames;
            initialized_ = true;
        } else {
            const double alpha = 1.0 - std::exp(-dt / kFillFilterSec);
            filteredFillFrames_ += alpha * (fillFrames - filteredFillFrames_);
        }

        const double errorFrames = filteredFillFrames_ - targetFrames_;
        integralCorrection_ *= std::exp(-dt / kIntegralLeakSec);
        integralCorrection_ = std::clamp(
            integralCorrection_ + kIntegralGain * errorFrames * dt,
            -kIntegralLimit, kIntegralLimit);
        const double correction = std::clamp(
            kProportionalGain * errorFrames + integralCorrection_,
            -kMaximumCorrection, kMaximumCorrection);
        sourceStep_ = 1.0 + correction;
        return sourceStep_;
    }

    double filteredFillFrames() const { return filteredFillFrames_; }
    double targetFrames() const { return targetFrames_; }
    double sourceStep() const { return sourceStep_; }
    double correctionPpm() const { return (sourceStep_ - 1.0) * 1.0e6; }

private:
    static constexpr double kFillFilterSec = 0.05;
    static constexpr double kProportionalGain = 2.0e-6;
    static constexpr double kIntegralGain = 2.0e-8;
    static constexpr double kIntegralLeakSec = 120.0;
    static constexpr double kIntegralLimit = 0.0003;
    static constexpr double kMaximumCorrection = 0.0010;

    double sampleRate_ = 44100.0;
    double targetFrames_ = 256.0;
    bool initialized_ = false;
    double filteredFillFrames_ = 0.0;
    double integralCorrection_ = 0.0;
    double sourceStep_ = 1.0;
};

// Four-point forward Lagrange interpolation. At integral positions this
// returns x0 exactly, so a perfectly matched bridge remains bit-transparent.
inline float interpolateBridgeSample(float x0, float x1, float x2, float x3,
                                     float fraction) {
    const float t = fraction;
    const float l0 = -((t - 1.0f) * (t - 2.0f) * (t - 3.0f)) / 6.0f;
    const float l1 =  (t * (t - 2.0f) * (t - 3.0f)) / 2.0f;
    const float l2 = -(t * (t - 1.0f) * (t - 3.0f)) / 2.0f;
    const float l3 =  (t * (t - 1.0f) * (t - 2.0f)) / 6.0f;
    return x0 * l0 + x1 * l1 + x2 * l2 + x3 * l3;
}

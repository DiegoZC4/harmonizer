#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "collier_effects.hpp"

namespace {

constexpr float kSampleRate = 44100.0f;
constexpr float kPi = 3.14159265358979323846f;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

float voiceSample(size_t sample) {
    const float phase = 2.0f * kPi * 261.625565f * static_cast<float>(sample) / kSampleRate;
    return 0.20f * std::sin(phase) + 0.08f * std::sin(2.0f * phase) +
           0.04f * std::sin(3.0f * phase);
}

} // namespace

int main() {
    {
        harmonizer::CollierEffects effects(kSampleRate);
        harmonizer::CollierEffectSettings settings;
        float maximumError = 0.0f;
        for (size_t sample = 0; sample < 12000; ++sample) {
            const float inputLeft = 0.30f * std::sin(sample * 0.017f);
            const float inputRight = 0.25f * std::cos(sample * 0.013f);
            float outputLeft = 0.0f;
            float outputRight = 0.0f;
            effects.process(inputLeft, inputRight, settings, outputLeft, outputRight);
            maximumError = std::max(maximumError, std::fabs(outputLeft - inputLeft));
            maximumError = std::max(maximumError, std::fabs(outputRight - inputRight));
        }
        require(maximumError == 0.0f, "disabled effects must be sample-exact pass-through");
    }

    {
        harmonizer::CollierEffects effects(kSampleRate);
        harmonizer::CollierEffectSettings settings;
        settings.chorusMix = 0.65f;
        settings.reverbMix = 0.45f;
        double changedEnergy = 0.0;
        double tailEnergy = 0.0;
        for (size_t sample = 0; sample < static_cast<size_t>(1.0f * kSampleRate); ++sample) {
            const float input = sample < static_cast<size_t>(0.35f * kSampleRate)
                ? voiceSample(sample)
                : 0.0f;
            float outputLeft = 0.0f;
            float outputRight = 0.0f;
            effects.process(input, input, settings, outputLeft, outputRight);
            changedEnergy += std::fabs(outputLeft - input) + std::fabs(outputRight - input);
            if (sample > static_cast<size_t>(0.55f * kSampleRate)) {
                tailEnergy += outputLeft * outputLeft + outputRight * outputRight;
            }
        }
        require(changedEnergy > 1.0, "chorus/reverb controls must change the live signal");
        require(tailEnergy > 1e-5, "musical reverb must produce an audible tail");
    }

    harmonizer::CollierEffects effects(kSampleRate);
    harmonizer::CollierEffectSettings settings;
    settings.freezeLevel[0] = 1.0f;
    const size_t holdSample = static_cast<size_t>(1.2f * kSampleRate);
    const size_t voiceEnd = static_cast<size_t>(1.3f * kSampleRate);
    const size_t totalSamples = static_cast<size_t>(3.2f * kSampleRate);
    double heldSquareSum = 0.0;
    size_t heldCount = 0;
    harmonizer::CollierEffectState state;

    for (size_t sample = 0; sample < totalSamples; ++sample) {
        if (sample == holdSample) settings.freezeHold[0] = true;
        const float input = sample < voiceEnd ? voiceSample(sample) : 0.0f;
        float outputLeft = 0.0f;
        float outputRight = 0.0f;
        state = effects.process(input, input, settings, outputLeft, outputRight);
        if (sample >= static_cast<size_t>(2.0f * kSampleRate)) {
            heldSquareSum += 0.5 * (outputLeft * outputLeft + outputRight * outputRight);
            ++heldCount;
        }
    }
    const double heldRms = std::sqrt(heldSquareSum / static_cast<double>(heldCount));
    require(state.freezeActive[0], "latched freeze must report active");
    require(heldRms > 0.0005, "latched freeze must sustain after input becomes silent");

    settings.freezeHold[0] = false;
    settings.freezeClearGeneration[0]++;
    double clearedSquareSum = 0.0;
    for (size_t sample = 0; sample < static_cast<size_t>(0.5f * kSampleRate); ++sample) {
        float outputLeft = 0.0f;
        float outputRight = 0.0f;
        state = effects.process(0.0f, 0.0f, settings, outputLeft, outputRight);
        if (sample > static_cast<size_t>(0.35f * kSampleRate)) {
            clearedSquareSum += outputLeft * outputLeft + outputRight * outputRight;
        }
    }
    require(!state.freezeActive[0], "clear must deactivate the freeze layer");
    require(clearedSquareSum < 1e-8, "clear must remove the previous frozen tail");

    std::cout << "collier effects: PASS (held RMS " << heldRms << ")\n";
    return 0;
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "backends/predictive_pitch_tracker.hpp"

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kSampleRate = 44100.0f;
constexpr uint64_t kHop = 512;
constexpr uint64_t kWindowHalf = 1024;
constexpr uint64_t kEarlyBlockHalf = 64;

enum class FixtureKind {
    Vibrato,
    Noise,
    Outlier,
    Glissando,
    NoteStep,
    Dropout,
};

struct Fixture {
    const char* name;
    FixtureKind kind;
    float parameterA;
    float parameterB;
};

struct ErrorMetric {
    double sumSq = 0.0;
    double peak = 0.0;
    int count = 0;

    void add(double error) {
        sumSq += error * error;
        peak = std::max(peak, std::fabs(error));
        count++;
    }

    double rms() const {
        return count > 0 ? std::sqrt(sumSq / count) : 0.0;
    }
};

struct MatrixResult {
    ErrorMetric vibrato;
    ErrorMetric noise;
    ErrorMetric outlier;
    ErrorMetric glissando;
    ErrorMetric noteStep;
    ErrorMetric dropout;

    double score() const {
        return vibrato.rms() / 0.16 +
               noise.rms() / 0.10 +
               outlier.peak / 0.35 +
               glissando.rms() / 0.08 +
               noteStep.rms() / 0.12 +
               dropout.rms() / 0.18;
    }
};

float truthAt(const Fixture& fixture, double seconds) {
    switch (fixture.kind) {
    case FixtureKind::Vibrato:
    case FixtureKind::Dropout:
        return 60.0f + fixture.parameterB *
            std::sin(2.0 * kPi * fixture.parameterA * seconds);
    case FixtureKind::Glissando:
        return 55.0f + fixture.parameterA * (float)seconds;
    case FixtureKind::NoteStep:
        return seconds < 2.5 ? 60.0f : 64.0f;
    case FixtureKind::Noise:
    case FixtureKind::Outlier:
        return 60.0f;
    }
    return 60.0f;
}

float deterministicNoise(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return ((state >> 8) * (1.0f / 16777215.0f)) * 2.0f - 1.0f;
}

ErrorMetric runFixture(const Fixture& fixture,
                       float positionGain,
                       float velocityGain,
                       float velocityResidualLimit) {
    PredictivePitchTracker tracker(
        kSampleRate, positionGain, velocityGain, velocityResidualLimit);
    ErrorMetric metric;
    uint32_t noiseState = 0x61c88647u;
    int hopIndex = 0;

    const uint64_t durationSamples = (uint64_t)(6.0f * kSampleRate);
    for (uint64_t end = kWindowHalf; end <= durationSamples; end += kHop) {
        const uint64_t measurementSample = end - kWindowHalf;
        const uint64_t targetSample = end > kEarlyBlockHalf
            ? end - kEarlyBlockHalf
            : 0;
        const double measurementTime = (double)measurementSample / kSampleRate;
        const double targetTime = (double)targetSample / kSampleRate;
        float measured = truthAt(fixture, measurementTime);
        bool valid = true;

        if (fixture.kind == FixtureKind::Noise) {
            measured += fixture.parameterA * deterministicNoise(noiseState);
        } else if (fixture.kind == FixtureKind::Outlier && hopIndex == 190) {
            measured += fixture.parameterA;
        } else if (fixture.kind == FixtureKind::Dropout) {
            const int dropoutPhase = hopIndex % 43;
            valid = dropoutPhase != 20 && dropoutPhase != 21;
        }

        const bool stable = hopIndex >= 3;
        const auto update = tracker.update(
            valid ? measured : -1.0f, valid, true, stable, measurementSample);
        if (targetTime >= 1.0 && update.voiced) {
            metric.add(tracker.estimateAt(targetSample) -
                       truthAt(fixture, targetTime));
        }
        hopIndex++;
    }
    return metric;
}

MatrixResult evaluateMatrix(float positionGain,
                            float velocityGain,
                            float velocityResidualLimit) {
    static constexpr std::array<Fixture, 13> fixtures = {{
        {"vibrato-2hz-0.25", FixtureKind::Vibrato, 2.0f, 0.25f},
        {"vibrato-2hz-0.50", FixtureKind::Vibrato, 2.0f, 0.50f},
        {"vibrato-4hz-0.25", FixtureKind::Vibrato, 4.0f, 0.25f},
        {"vibrato-4hz-0.50", FixtureKind::Vibrato, 4.0f, 0.50f},
        {"vibrato-6hz-0.25", FixtureKind::Vibrato, 6.0f, 0.25f},
        {"vibrato-6hz-0.50", FixtureKind::Vibrato, 6.0f, 0.50f},
        {"noise-0.15", FixtureKind::Noise, 0.15f, 0.0f},
        {"outlier-0.50", FixtureKind::Outlier, 0.50f, 0.0f},
        {"gliss-up-2", FixtureKind::Glissando, 2.0f, 0.0f},
        {"gliss-down-2", FixtureKind::Glissando, -2.0f, 0.0f},
        {"note-step", FixtureKind::NoteStep, 0.0f, 0.0f},
        {"dropout-2hz-0.25", FixtureKind::Dropout, 2.0f, 0.25f},
        {"dropout-6hz-0.25", FixtureKind::Dropout, 6.0f, 0.25f},
    }};

    MatrixResult result;
    for (const Fixture& fixture : fixtures) {
        ErrorMetric metric = runFixture(
            fixture, positionGain, velocityGain, velocityResidualLimit);
        switch (fixture.kind) {
        case FixtureKind::Vibrato:
            result.vibrato.sumSq += metric.sumSq;
            result.vibrato.peak = std::max(result.vibrato.peak, metric.peak);
            result.vibrato.count += metric.count;
            break;
        case FixtureKind::Noise: result.noise = metric; break;
        case FixtureKind::Outlier: result.outlier = metric; break;
        case FixtureKind::Glissando:
            result.glissando.sumSq += metric.sumSq;
            result.glissando.peak = std::max(result.glissando.peak, metric.peak);
            result.glissando.count += metric.count;
            break;
        case FixtureKind::NoteStep: result.noteStep = metric; break;
        case FixtureKind::Dropout:
            result.dropout.sumSq += metric.sumSq;
            result.dropout.peak = std::max(result.dropout.peak, metric.peak);
            result.dropout.count += metric.count;
            break;
        }
    }
    return result;
}

void printMatrixRow(float positionGain,
                    float velocityGain,
                    float velocityResidualLimit,
                    const MatrixResult& result,
                    const char* suffix = "") {
    std::cout << std::fixed << std::setprecision(3)
              << "gains " << positionGain << "/" << velocityGain
              << " clip " << velocityResidualLimit
              << ": vib " << result.vibrato.rms()
              << ", noise " << result.noise.rms()
              << ", outlier peak " << result.outlier.peak
              << ", gliss " << result.glissando.rms()
              << ", step " << result.noteStep.rms()
              << ", dropout " << result.dropout.rms()
              << ", score " << result.score() << suffix << "\n";
}

bool testGainMatrix() {
    static constexpr std::array<float, 6> positionGains = {
        0.35f, 0.45f, 0.55f, 0.60f, 0.65f, 0.75f,
    };
    static constexpr std::array<float, 7> velocityGains = {
        0.05f, 0.08f, 0.12f, 0.16f, 0.20f, 0.24f, 0.30f,
    };
    static constexpr std::array<float, 6> residualLimits = {
        0.08f, 0.12f, 0.18f, 0.25f, 0.35f, 1000000.0f,
    };

    double bestScore = std::numeric_limits<double>::infinity();
    float bestPosition = 0.0f;
    float bestVelocity = 0.0f;
    float bestResidualLimit = 0.0f;
    MatrixResult bestResult;
    for (float position : positionGains) {
        for (float velocity : velocityGains) {
            for (float residualLimit : residualLimits) {
                MatrixResult result = evaluateMatrix(
                    position, velocity, residualLimit);
                if (result.score() < bestScore) {
                    bestScore = result.score();
                    bestPosition = position;
                    bestVelocity = velocity;
                    bestResidualLimit = residualLimit;
                    bestResult = result;
                }
            }
        }
    }

    const MatrixResult legacy = evaluateMatrix(0.60f, 0.30f, 1000000.0f);
    const MatrixResult production = evaluateMatrix(0.60f, 0.30f, 0.12f);
    printMatrixRow(0.60f, 0.30f, 1000000.0f, legacy, " (legacy)");
    printMatrixRow(0.60f, 0.30f, 0.12f, production, " (production)");
    printMatrixRow(bestPosition, bestVelocity, bestResidualLimit,
                   bestResult, " (matrix best)");

    const bool fixtureGates =
        production.vibrato.rms() <= 0.24 &&
        production.noise.rms() <= 0.16 &&
        production.outlier.peak <= 0.55 &&
        production.glissando.rms() <= 0.12 &&
        production.noteStep.rms() <= 0.30 &&
        production.dropout.rms() <= 0.24;
    const bool nearBest = production.score() <= bestScore * 1.12;
    std::cout << "tracker fixture matrix: "
              << (fixtureGates && nearBest ? "PASS" : "FAIL")
              << " (production/best " << production.score() / bestScore << ")\n";
    return fixtureGates && nearBest;
}

bool testProjection() {
    PredictivePitchTracker tracker(kSampleRate);
    double predictiveErrorSq = 0.0;
    double reactiveErrorSq = 0.0;
    int count = 0;

    for (uint64_t end = kHop; end <= (uint64_t)(5.0f * kSampleRate); end += kHop) {
        uint64_t measurementSample = end > kWindowHalf ? end - kWindowHalf : 0;
        float measured = 60.0f + 0.5f * std::sin(
            2.0 * kPi * 2.0 * (double)measurementSample / kSampleRate);
        tracker.update(measured, true, true, end > 3 * kHop, measurementSample);

        uint64_t targetSample = end - kEarlyBlockHalf;
        if (targetSample < (uint64_t)kSampleRate) continue;
        float truth = 60.0f + 0.5f * std::sin(
            2.0 * kPi * 2.0 * (double)targetSample / kSampleRate);
        float predicted = tracker.estimateAt(targetSample);
        const double predictiveError = predicted - truth;
        const double reactiveError = measured - truth;
        predictiveErrorSq += predictiveError * predictiveError;
        reactiveErrorSq += reactiveError * reactiveError;
        count++;
    }

    const double predictiveRms = std::sqrt(predictiveErrorSq / count);
    const double reactiveRms = std::sqrt(reactiveErrorSq / count);
    std::cout << "tracker projection: reactive " << predictiveRms * 0.0 + reactiveRms
              << " st RMS -> predictive " << predictiveRms << " st RMS\n";
    return predictiveRms < 0.055 && predictiveRms < reactiveRms * 0.65;
}

bool testOnsetAndReset() {
    PredictivePitchTracker tracker(kSampleRate);
    const auto first = tracker.update(60.0f, true, true, false, 1000);
    const auto second = tracker.update(60.1f, true, true, false, 1512);
    const auto jump = tracker.update(64.0f, true, true, false, 2024);
    const auto silence = tracker.update(-1.0f, false, false, false, 2536);

    const bool pass = first.onset && first.voiced && first.confidence < 0.5f &&
                      !second.onset && second.confidence >= 0.7f &&
                      jump.onset &&
                      std::fabs(tracker.estimateAt(2024) - 64.0f) < 0.01f &&
                      !silence.voiced;
    std::cout << "tracker onset/reset: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass;
}
}  // namespace

int main() {
    const bool pass = testProjection() && testOnsetAndReset() && testGainMatrix();
    std::cout << "parallel pitch tracker: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "backends/output_bridge_controller.hpp"

namespace {
constexpr double kSampleRate = 44100.0;
constexpr std::size_t kCallbackFrames = 64;
constexpr double kTargetFrames = 384.0;

bool testClockDrift(double producerPpm) {
    OutputBridgeClockController controller(kSampleRate, kTargetFrames);
    double fill = 352.0;
    double minFill = fill;
    double maxFill = fill;
    const double producerStep = 1.0 + producerPpm * 1.0e-6;
    const int callbacks = static_cast<int>(600.0 * kSampleRate / kCallbackFrames);

    for (int callback = 0; callback < callbacks; ++callback) {
        const double sourceStep = controller.update(fill, kCallbackFrames);
        fill += kCallbackFrames * (producerStep - sourceStep);
        if (callback > callbacks / 10) {
            minFill = std::min(minFill, fill);
            maxFill = std::max(maxFill, fill);
        }
    }

    const double rateError = controller.correctionPpm() - producerPpm;
    const bool pass = fill > 348.0 && fill < 420.0 &&
                      minFill > 320.0 && maxFill < 450.0 &&
                      std::fabs(rateError) < 2.0;
    std::cout << "bridge drift " << (producerPpm >= 0.0 ? "+" : "")
              << producerPpm << " ppm: fill " << fill
              << " frames, correction " << controller.correctionPpm()
              << " ppm, range " << minFill << ".." << maxFill
              << " -> " << (pass ? "PASS" : "FAIL") << "\n";
    return pass;
}

bool testInterpolation() {
    bool pass = true;
    for (float value : {-1.0f, -0.25f, 0.0f, 0.75f, 1.0f}) {
        pass = pass && interpolateBridgeSample(value, 0.2f, -0.4f, 0.8f, 0.0f) == value;
    }
    const float midpoint = interpolateBridgeSample(0.0f, 1.0f, 8.0f, 27.0f, 0.5f);
    pass = pass && std::fabs(midpoint - 0.125f) < 1.0e-6f;
    std::cout << "bridge interpolation: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass;
}
}

int main() {
    const bool pass = testClockDrift(100.0) &&
                      testClockDrift(-100.0) &&
                      testInterpolation();
    std::cout << "output bridge controller: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}

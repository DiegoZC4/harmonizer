#pragma once

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <vector>

#include <rubberband/RubberBandStretcher.h>

// Included by harmonizer_web.cpp after PitchDetector, SharedDisplay, and
// Capture are defined. The browser is only a controller; this native engine
// owns every sample of the live audio path.

class RubberBandR2Shifter {
    static constexpr size_t kBlockSize = 128;
    static constexpr size_t kFifoSize = 16384;
    static constexpr size_t kFifoMask = kFifoSize - 1;
    static_assert((kFifoSize & kFifoMask) == 0,
                  "R2 output FIFO size must be a power of two");

    RubberBand::RubberBandStretcher stretcher;
    std::array<float, kFifoSize> fifo{};
    std::array<float, kBlockSize * 4> retrieveBuffer{};
    uint64_t fifoWrite = 0;
    uint64_t fifoRead = 0;
    size_t startDelay = 0;

public:
    RubberBandR2Shifter()
        : stretcher(kSampleRate, 1,
              RubberBand::RubberBandStretcher::OptionProcessRealTime |
              RubberBand::RubberBandStretcher::OptionEngineFaster |
              RubberBand::RubberBandStretcher::OptionWindowShort |
              RubberBand::RubberBandStretcher::OptionFormantPreserved |
              RubberBand::RubberBandStretcher::OptionPitchHighConsistency |
              RubberBand::RubberBandStretcher::OptionTransientsSmooth |
              RubberBand::RubberBandStretcher::OptionDetectorSoft |
              RubberBand::RubberBandStretcher::OptionThreadingNever)
    {
        stretcher.setTimeRatio(1.0);
        stretcher.setPitchScale(1.0);
        stretcher.setMaxProcessSize(kBlockSize);
        startDelay = stretcher.getStartDelay();
    }

    size_t getBlockSize() const { return kBlockSize; }
    size_t getStartDelay() const { return startDelay; }
    void setPitchScale(double scale) { stretcher.setPitchScale(scale); }

    void shift(const float *const *input, float *const *output) {
        stretcher.process(input, kBlockSize, false);

        int available = stretcher.available();
        while (available > 0) {
            size_t request = std::min((size_t)available, retrieveBuffer.size());
            float *channels[] = { retrieveBuffer.data() };
            size_t retrieved = stretcher.retrieve(channels, request);
            for (size_t i = 0; i < retrieved; i++) {
                if (fifoWrite - fifoRead >= kFifoSize) fifoRead++;
                fifo[fifoWrite++ & kFifoMask] = retrieveBuffer[i];
            }
            available = stretcher.available();
        }

        for (size_t i = 0; i < kBlockSize; i++) {
            output[0][i] = fifoRead < fifoWrite
                ? fifo[fifoRead++ & kFifoMask]
                : 0.0f;
        }
    }
};

struct Voice {
    RubberBandR2Shifter shifter;

    std::atomic<bool>     gateOn{false};
    std::atomic<int>      midiNote{-1};
    std::atomic<uint64_t> stamp{0};

    float envelope     = 0.0f;
    float pitchRatio   = 1.0f;
    float panL         = 0.707f;
    float panR         = 0.707f;
    bool  sustained    = false;
    float attackCoeff  = 0.0f;
    float releaseCoeff = 0.0f;

    Voice() = default;

    void init() {
        attackCoeff  = 1.0f - std::exp(-1.0f / (kAttackSec * kSampleRate));
        releaseCoeff = 1.0f - std::exp(-1.0f / (kReleaseSec * kSampleRate));
        shifter.setPitchScale(1.0);
    }

    bool isAudible() const {
        return gateOn.load(std::memory_order_relaxed) || envelope > 0.001f;
    }

    float tickEnvelope() {
        float target = gateOn.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        float coeff = target > envelope ? attackCoeff : releaseCoeff;
        envelope += (target - envelope) * coeff;
        if (envelope < 0.0001f) envelope = 0.0f;
        return envelope;
    }

    void updatePan() {
        int note = midiNote.load(std::memory_order_relaxed);
        if (note < 0) return;
        float spread = clampf((float)(note - 48) / 24.0f, 0.0f, 1.0f);
        float side = note % 2 == 0 ? -1.0f : 1.0f;
        float theta = (side * spread + 1.0f) * 0.25f * kPi;
        panL = std::cos(theta);
        panR = std::sin(theta);
    }
};

struct AudioEngine {
    static constexpr size_t kOutputBridgeFrames = 8192;
    static constexpr size_t kOutputBridgeMask = kOutputBridgeFrames - 1;
    static constexpr size_t kOutputBridgePrimeFrames = 384;
    static_assert((kOutputBridgeFrames & kOutputBridgeMask) == 0,
                  "output bridge size must be a power of two");

    PitchDetector detector;
    Voice         voices[kMaxVoices];
    SharedDisplay display;

    size_t blockSize          = 0;
    size_t bufPos             = 0;
    size_t shifterDelaySamples = 0;
    size_t dryDelayPos        = 0;
    float monitorGainLinear   = 1.0f;
    std::vector<float> inputBuf;
    std::vector<float> outputL;
    std::vector<float> outputR;
    std::vector<float> shiftBuf;
    std::vector<float> voicingBuf;
    std::vector<float> dryDelay;
    std::vector<float> outputBridgeL;
    std::vector<float> outputBridgeR;
    std::atomic<uint64_t> outputBridgeWrite{0};
    std::atomic<uint64_t> outputBridgeRead{0};
    std::atomic<bool> outputBridgePrimed{false};

    float pitchBuf[kPitchHopSize] = {};
    int   pitchPos       = 0;
    float detectedF0     = -1.0f;
    float detectedMidi   = -1.0f;
    float prevMidi       = -1.0f;
    float recentPitchRms = 0.0f;
    bool  pitchStable    = false;
    bool  pitchVoiced    = false;
    int   pitchHoldFrames = 0;

    static constexpr int   kPitchHistLen = 9;
    static constexpr int   kCorrectionControlHistoryLen = 16;
    static constexpr int   kCorrectionLagHops = 0;
    static constexpr float kFlutterDerivativeCompensation = 0.0f;
    static constexpr float kFlutterEnergyCompensation = 0.0f;
    static constexpr float kFlutterEnergyMeanAlpha = 0.05f;
    static constexpr float kFlutterCompensationRange = 0.75f;
    static constexpr float kFlutterCompensationMax = 0.20f;
    static constexpr int   kMinPitchValidFrames = 3;
    static constexpr int   kPitchReleaseFrames = 10;
    static constexpr float kPitchSmoothingAlpha = 0.30f;
    static constexpr float kPitchMaxStepSemitones = 2.0f;
    static constexpr float kPitchSnapSemitones = 1.5f;
    static constexpr float kPitchGateReleaseRatio = 0.55f;
    float pitchHist[kPitchHistLen] = {};
    int   pitchHistIdx = 0;
    float smoothedMidi = -1.0f;
    float fastCorrectionMidi = -1.0f;
    float correctionMidi = -1.0f;
    int   correctionHoldFrames = 0;
    float correctionControlHistory[kCorrectionControlHistoryLen] = {};
    int   correctionControlHistoryIdx = 0;
    float previousCorrectionDelta = 0.0f;
    float correctionDeltaEnergyMean = 0.0f;
    bool  correctionFlutterReady = false;

    float voicingEnv          = 0.0f;
    float voicingAttackCoeff  = 0.0f;
    float voicingReleaseCoeff = 0.0f;
    int   voicedGraceBlocks   = 0;

    std::mutex         midiMutex;
    bool               sustainOn = false;
    uint64_t           noteCounter = 0;
    std::atomic<float> pitchBend{0.0f};

    std::atomic<uint64_t> sampleClock{0};
    std::atomic<uint64_t> testToneStartClock{0};
    std::atomic<uint64_t> testToneEndClock{0};
    float                 testTonePhase = 0.0f;
    Capture capture;

    void init() {
        for (int voice = 0; voice < kMaxVoices; voice++) voices[voice].init();

        blockSize = voices[0].shifter.getBlockSize();
        shifterDelaySamples = voices[0].shifter.getStartDelay();
        inputBuf.assign(blockSize, 0.0f);
        outputL.assign(blockSize, 0.0f);
        outputR.assign(blockSize, 0.0f);
        shiftBuf.assign(blockSize, 0.0f);
        voicingBuf.assign(blockSize, 0.0f);
        dryDelay.assign(shifterDelaySamples, 0.0f);
        outputBridgeL.assign(kOutputBridgeFrames, 0.0f);
        outputBridgeR.assign(kOutputBridgeFrames, 0.0f);

        voicingAttackCoeff = 1.0f - std::exp(-1.0f / (kVoicingAttackSec * kSampleRate));
        voicingReleaseCoeff = 1.0f - std::exp(-1.0f / (kVoicingReleaseSec * kSampleRate));

        std::cerr << "Rubber Band R2 Short: block " << blockSize
                  << " samples, start delay " << shifterDelaySamples
                  << " samples, DSP path " << dspLatencyMs() << " ms\n";
    }

    double dspLatencyMs() const {
        return 1000.0 *
            (double)(blockSize + shifterDelaySamples + kOutputBridgePrimeFrames) /
            kSampleRate;
    }

    void processBlock() {
        float gateRms = display.voicedGateRms.load(std::memory_order_relaxed);
        bool voicedInput = pitchVoiced && detectedMidi > 0.0f &&
                           recentPitchRms >= gateRms * kPitchGateReleaseRatio;
        if (voicedInput) voicedGraceBlocks = kVoicingGraceHops;
        else if (voicedGraceBlocks > 0) voicedGraceBlocks--;
        bool voicingOn = voicedInput || voicedGraceBlocks > 0;

        float voicingTarget = voicingOn ? 1.0f : 0.0f;
        for (size_t sample = 0; sample < blockSize; sample++) {
            float coeff = voicingTarget > voicingEnv
                ? voicingAttackCoeff
                : voicingReleaseCoeff;
            voicingEnv += (voicingTarget - voicingEnv) * coeff;
            voicingBuf[sample] = voicingEnv;
        }
        if (voicingEnv < 0.0001f) voicingEnv = 0.0f;

        float wetGain = 0.0f;
        float dryGain = 0.0f;
        wetDryFromBalance(display.wetDryBalance.load(std::memory_order_relaxed),
                          wetGain, dryGain);

        // The backend reports its algorithmic start delay. Delay the dry path
        // by the same amount whenever wet audio is present, otherwise the
        // Blend control creates comb filtering instead of a coherent mix.
        for (size_t sample = 0; sample < blockSize; sample++) {
            float delayedDry = inputBuf[sample];
            if (!dryDelay.empty()) {
                delayedDry = dryDelay[dryDelayPos];
                dryDelay[dryDelayPos] = inputBuf[sample];
                dryDelayPos = (dryDelayPos + 1) % dryDelay.size();
            }
            if (wetGain <= 0.0001f) delayedDry = inputBuf[sample];
            outputL[sample] = delayedDry * dryGain;
            outputR[sample] = delayedDry * dryGain;
        }

        int activeVoices = 0;
        for (int voice = 0; voice < kMaxVoices; voice++) {
            int note = voices[voice].midiNote.load(std::memory_order_relaxed);
            if (note > 0 && voices[voice].isAudible()) activeVoices++;
        }
        float perVoiceWet = activeVoices > 0
            ? wetGain / std::sqrt((float)activeVoices)
            : 0.0f;
        float bend = pitchBend.load(std::memory_order_relaxed);
        const float* inputChannels[] = { inputBuf.data() };
        float* outputChannels[] = { shiftBuf.data() };

        for (int voice = 0; voice < kMaxVoices; voice++) {
            Voice& current = voices[voice];
            int note = current.midiNote.load(std::memory_order_relaxed);
            if (note <= 0 || !current.isAudible()) continue;

            // correctionMidi tracks the source F0 independently of the slow
            // display contour. The target is the exact held MIDI pitch.
            if (correctionMidi > 0.0f && pitchVoiced) {
                float targetMidi = display.glideTargetMidi(
                    note, sampleClock.load(std::memory_order_relaxed)) + bend;
                float correctionControl = correctionMidi;
                if (detectedMidi > 0.0f) {
                    float correctionDelta = correctionMidi - detectedMidi;

                    // Optional backend-specific flutter correction during slow
                    // vibrato. This stays local to a stable sung note and is
                    // clamped so note changes cannot create correction spikes.
                    if (std::fabs(correctionDelta) <= kFlutterCompensationRange &&
                        std::fabs(previousCorrectionDelta) <= kFlutterCompensationRange &&
                        correctionFlutterReady) {
                        float flutterCompensation = kFlutterDerivativeCompensation *
                            (correctionDelta * correctionDelta -
                             previousCorrectionDelta * previousCorrectionDelta);
                        flutterCompensation += kFlutterEnergyCompensation *
                            (correctionDelta * correctionDelta -
                             correctionDeltaEnergyMean);
                        correctionControl += clampf(flutterCompensation,
                                                     -kFlutterCompensationMax,
                                                     kFlutterCompensationMax);
                    }
                }
                current.pitchRatio = clampf(
                    noteToFreq(targetMidi) / noteToFreq(correctionControl),
                    0.25f, 4.0f);
            }
            current.shifter.setPitchScale(current.pitchRatio);
            current.shifter.shift(inputChannels, outputChannels);
            current.updatePan();

            for (size_t sample = 0; sample < blockSize; sample++) {
                float envelope = current.tickEnvelope();
                float shifted = shiftBuf[sample] * envelope * voicingBuf[sample] * perVoiceWet;
                outputL[sample] += shifted * current.panL;
                outputR[sample] += shifted * current.panR;
            }
        }
        if (correctionMidi > 0.0f && detectedMidi > 0.0f) {
            float correctionDelta = correctionMidi - detectedMidi;
            if (std::fabs(correctionDelta) <= kFlutterCompensationRange) {
                previousCorrectionDelta = correctionDelta;
                float correctionEnergy = correctionDelta * correctionDelta;
                if (correctionFlutterReady) {
                    correctionDeltaEnergyMean += kFlutterEnergyMeanAlpha *
                        (correctionEnergy - correctionDeltaEnergyMean);
                } else {
                    correctionDeltaEnergyMean = correctionEnergy;
                    correctionFlutterReady = true;
                }
            } else {
                previousCorrectionDelta = 0.0f;
                correctionDeltaEnergyMean = 0.0f;
                correctionFlutterReady = false;
            }
        } else {
            previousCorrectionDelta = 0.0f;
            correctionDeltaEnergyMean = 0.0f;
            correctionFlutterReady = false;
        }

        for (size_t sample = 0; sample < blockSize; sample++) {
            outputL[sample] = std::tanh(outputL[sample]);
            outputR[sample] = std::tanh(outputR[sample]);
        }
    }
};

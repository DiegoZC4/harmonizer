#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <vector>

#include <rubberband/RubberBandLiveShifter.h>

// Included by harmonizer_web.cpp after PitchDetector, SharedDisplay, and
// Capture are defined. The browser is only a controller; this native engine
// owns every sample of the live audio path.

struct Voice {
    RubberBand::RubberBandLiveShifter shifter;

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

    Voice()
        : shifter(kSampleRate, 1,
                  RubberBand::RubberBandLiveShifter::OptionFormantPreserved |
                  RubberBand::RubberBandLiveShifter::OptionWindowShort)
    {}

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
    std::vector<float> articulationBuf;
    std::vector<float> midiEnvelopeBuf;
    std::vector<float> alignedInputBuf;
    std::vector<float> highBandBuf;
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
    static constexpr int   kCorrectionLagHops = 1;
    static constexpr float kFlutterDerivativeCompensation = 5.3f;
    static constexpr float kFlutterEnergyCompensation = -1.6f;
    static constexpr float kFlutterEnergyMeanAlpha = 0.05f;
    static constexpr float kFlutterCompensationRange = 0.75f;
    static constexpr float kFlutterCompensationMax = 0.20f;
    static constexpr int   kMinPitchValidFrames = 3;
    static constexpr int   kPitchReleaseFrames = 10;
    static constexpr float kPitchSmoothingAlpha = 0.30f;
    static constexpr float kPitchMaxStepSemitones = 2.0f;
    static constexpr float kPitchSnapSemitones = 1.5f;
    static constexpr float kPitchGateReleaseRatio = 0.55f;
    static constexpr float kUnvoicedGateRatio = 0.20f;
    static constexpr float kUnvoicedGateFloorRms = 0.0005f;
    static constexpr float kHeldPitchForgetSec = 0.80f;
    static constexpr float kArticulationCrossfadeSec = 0.006f;
    static constexpr float kSibilanceLowpassHz = 3000.0f;
    static constexpr float kSibilanceHighBandRatio = 0.40f;
    static constexpr float kSibilanceStrongHighBandRatio = 0.65f;
    static constexpr float kSibilanceCrossingsPer8ms = 16.0f;
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
    float articulationEnv     = 0.0f;
    float articulationCoeff   = 0.0f;
    float sibilanceLowpass    = 0.0f;
    float heldSourceMidi      = -1.0f;
    size_t heldPitchSilenceSamples = 0;

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
        articulationBuf.assign(blockSize, 0.0f);
        midiEnvelopeBuf.assign(blockSize, 0.0f);
        alignedInputBuf.assign(blockSize, 0.0f);
        highBandBuf.assign(blockSize, 0.0f);
        dryDelay.assign(shifterDelaySamples, 0.0f);
        outputBridgeL.assign(kOutputBridgeFrames, 0.0f);
        outputBridgeR.assign(kOutputBridgeFrames, 0.0f);

        voicingAttackCoeff = 1.0f - std::exp(-1.0f / (kVoicingAttackSec * kSampleRate));
        voicingReleaseCoeff = 1.0f - std::exp(-1.0f / (kVoicingReleaseSec * kSampleRate));
        articulationCoeff = 1.0f - std::exp(
            -1.0f / (kArticulationCrossfadeSec * kSampleRate));

        std::cerr << "Rubber Band LiveShifter: block " << blockSize
                  << " samples, start delay " << shifterDelaySamples
                  << " samples, DSP path " << dspLatencyMs() << " ms\n";
    }

    double dspLatencyMs() const {
        return 1000.0 *
            (double)(blockSize + shifterDelaySamples + kOutputBridgePrimeFrames) /
            kSampleRate;
    }

    bool detectSibilance(float& score) {
        float peak = 0.0f;
        float totalEnergy = 0.0f;
        float highBandEnergy = 0.0f;
        float low = sibilanceLowpass;
        const float lowpassCoeff = 1.0f - std::exp(
            -2.0f * kPi * kSibilanceLowpassHz / kSampleRate);

        for (size_t sample = 0; sample < blockSize; sample++) {
            float input = inputBuf[sample];
            low += lowpassCoeff * (input - low);
            float high = input - low;
            highBandBuf[sample] = high;
            peak = std::max(peak, std::fabs(input));
            totalEnergy += input * input;
            highBandEnergy += high * high;
        }
        sibilanceLowpass = low;

        const float crossingThreshold = std::max(0.0005f, peak * 0.08f);
        int previousSign = 0;
        int crossings = 0;
        for (float high : highBandBuf) {
            int sign = high > crossingThreshold ? 1
                     : high < -crossingThreshold ? -1
                     : 0;
            if (sign != 0) {
                if (previousSign != 0 && sign != previousSign) crossings++;
                previousSign = sign;
            }
        }

        float highBandRatio = highBandEnergy / std::max(totalEnergy, 1.0e-12f);
        float crossingsPer8ms = crossings *
            (0.008f * kSampleRate / std::max(1.0f, (float)blockSize));
        score = std::min(highBandRatio / kSibilanceHighBandRatio,
                         crossingsPer8ms / kSibilanceCrossingsPer8ms);
        score = clampf(score, 0.0f, 4.0f);

        bool highFrequencyVariation =
            highBandRatio >= kSibilanceHighBandRatio &&
            crossingsPer8ms >= kSibilanceCrossingsPer8ms;
        return highFrequencyVariation &&
            (!pitchStable || highBandRatio >= kSibilanceStrongHighBandRatio);
    }

    void processBlock() {
        float gateRms = display.voicedGateRms.load(std::memory_order_relaxed);
        bool voicedInput = pitchVoiced && detectedMidi > 0.0f &&
                           recentPitchRms >= gateRms * kPitchGateReleaseRatio;
        float unvoicedGateRms = std::max(kUnvoicedGateFloorRms,
                                         gateRms * kUnvoicedGateRatio);
        bool energeticInput = recentPitchRms >= unvoicedGateRms;
        float sibilanceScore = 0.0f;
        bool sibilantInput = energeticInput && detectSibilance(sibilanceScore);

        // A fricative has no F0 to detect. Latch only a stable voiced estimate,
        // then keep that source pitch through energetic unvoiced articulation.
        // This preserves the last transposition ratio without inventing a
        // pitch from sibilant noise. A real pause closes and eventually clears
        // the latch so background noise cannot revive an old sung note.
        if (pitchStable && correctionMidi > 0.0f && voicedInput) {
            heldSourceMidi = correctionMidi;
            heldPitchSilenceSamples = 0;
        } else if (energeticInput) {
            heldPitchSilenceSamples = 0;
        } else if (heldSourceMidi > 0.0f) {
            heldPitchSilenceSamples += blockSize;
            if (heldPitchSilenceSamples >=
                (size_t)(kHeldPitchForgetSec * kSampleRate)) {
                heldSourceMidi = -1.0f;
                heldPitchSilenceSamples = 0;
            }
        }

        bool detectorUnvoiced = !pitchStable && energeticInput &&
                                heldSourceMidi > 0.0f;
        bool unvoicedInput = sibilantInput || detectorUnvoiced;
        bool tcBypass = display.unvoicedMode.load(std::memory_order_relaxed) ==
                        static_cast<int>(UnvoicedMode::TcBypass);
        display.unvoicedActive.store(unvoicedInput, std::memory_order_relaxed);
        display.sibilanceScore.store(sibilanceScore, std::memory_order_relaxed);
        if (voicedInput || unvoicedInput) voicedGraceBlocks = kVoicingGraceHops;
        else if (voicedGraceBlocks > 0) voicedGraceBlocks--;
        bool voicingOn = voicedInput || unvoicedInput || voicedGraceBlocks > 0;

        float voicingTarget = voicingOn ? 1.0f : 0.0f;
        for (size_t sample = 0; sample < blockSize; sample++) {
            float coeff = voicingTarget > voicingEnv
                ? voicingAttackCoeff
                : voicingReleaseCoeff;
            voicingEnv += (voicingTarget - voicingEnv) * coeff;
            voicingBuf[sample] = voicingEnv;

            float articulationTarget = tcBypass && unvoicedInput ? 1.0f : 0.0f;
            articulationEnv +=
                (articulationTarget - articulationEnv) * articulationCoeff;
            articulationBuf[sample] = articulationEnv;
        }
        if (voicingEnv < 0.0001f) voicingEnv = 0.0f;

        float wetGain = 0.0f;
        float dryGain = 0.0f;
        wetDryFromBalance(display.wetDryBalance.load(std::memory_order_relaxed),
                          wetGain, dryGain);

        // Rubber Band reports its algorithmic start delay. Delay the dry path
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
            alignedInputBuf[sample] = delayedDry;
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
        std::fill(midiEnvelopeBuf.begin(), midiEnvelopeBuf.end(), 0.0f);

        for (int voice = 0; voice < kMaxVoices; voice++) {
            Voice& current = voices[voice];
            int note = current.midiNote.load(std::memory_order_relaxed);
            if (note <= 0 || !current.isAudible()) continue;

            // correctionMidi tracks the source F0 independently of the slow
            // display contour. The target is the exact held MIDI pitch.
            bool liveCorrection = correctionMidi > 0.0f && pitchVoiced &&
                                  !unvoicedInput;
            float sourceMidi = liveCorrection ? correctionMidi : heldSourceMidi;
            if (sourceMidi > 0.0f) {
                float targetMidi = display.glideTargetMidi(
                    note, sampleClock.load(std::memory_order_relaxed)) + bend;
                float correctionControl = sourceMidi;
                if (liveCorrection && detectedMidi > 0.0f) {
                    float correctionDelta = correctionMidi - detectedMidi;

                    // Counter LiveShifter's doubled-rate flutter during slow
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
                midiEnvelopeBuf[sample] = std::max(midiEnvelopeBuf[sample], envelope);
                float shifted = shiftBuf[sample] * envelope * voicingBuf[sample] *
                                (1.0f - articulationBuf[sample]) * perVoiceWet;
                outputL[sample] += shifted * current.panL;
                outputR[sample] += shifted * current.panR;
            }
        }

        if (activeVoices > 0 && wetGain > 0.0001f) {
            for (size_t sample = 0; sample < blockSize; sample++) {
                float articulation = alignedInputBuf[sample] * wetGain *
                    voicingBuf[sample] * midiEnvelopeBuf[sample] *
                    articulationBuf[sample];
                outputL[sample] += articulation;
                outputR[sample] += articulation;
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

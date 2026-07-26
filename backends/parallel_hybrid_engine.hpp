#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include <rubberband/RubberBandLiveShifter.h>
#include <rubberband/RubberBandStretcher.h>

#include "predictive_pitch_tracker.hpp"

// Included by harmonizer_web.cpp after PitchDetector, SharedDisplay, and
// Capture are defined. The browser is only a controller; this native engine
// owns every sample of the live audio path.

class RubberBandR2EarlyShifter {
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
    RubberBandR2EarlyShifter()
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
    RubberBand::RubberBandLiveShifter qualityShifter;
    RubberBandR2EarlyShifter earlyShifter;

    std::atomic<bool>     gateOn{false};
    std::atomic<int>      midiNote{-1};
    std::atomic<uint64_t> stamp{0};
    std::atomic<uint64_t> handoffEventSample{0};

    float qualityEnvelope   = 0.0f;
    float earlyEnvelope     = 0.0f;
    float envelope          = 0.0f;
    float qualityPitchRatio = 1.0f;
    float earlyPitchRatio   = 1.0f;
    float panL         = 0.707f;
    float panR         = 0.707f;
    bool  sustained    = false;
    float attackCoeff  = 0.0f;
    float releaseCoeff = 0.0f;

    Voice()
        : qualityShifter(kSampleRate, 1,
                  RubberBand::RubberBandLiveShifter::OptionFormantPreserved |
                  RubberBand::RubberBandLiveShifter::OptionWindowShort)
    {}

    void init() {
        attackCoeff  = 1.0f - std::exp(-1.0f / (kAttackSec * kSampleRate));
        releaseCoeff = 1.0f - std::exp(-1.0f / (kReleaseSec * kSampleRate));
        qualityShifter.setPitchScale(1.0);
        earlyShifter.setPitchScale(1.0);
    }

    bool isAudible() const {
        return gateOn.load(std::memory_order_relaxed) ||
               qualityEnvelope > 0.001f || earlyEnvelope > 0.001f;
    }

    float tickEnvelope(float& pathEnvelope) {
        float target = gateOn.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        float coeff = target > pathEnvelope ? attackCoeff : releaseCoeff;
        pathEnvelope += (target - pathEnvelope) * coeff;
        if (pathEnvelope < 0.0001f) pathEnvelope = 0.0f;
        envelope = std::max(qualityEnvelope, earlyEnvelope);
        return pathEnvelope;
    }

    float tickQualityEnvelope() { return tickEnvelope(qualityEnvelope); }
    float tickEarlyEnvelope() { return tickEnvelope(earlyEnvelope); }

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
    static constexpr size_t kEarlyBlockSize = 128;
    static constexpr size_t kQualityBlockSize = 512;
    static constexpr size_t kOutputBridgeFrames = 8192;
    static constexpr size_t kOutputBridgeMask = kOutputBridgeFrames - 1;
    static constexpr size_t kOutputBridgePrimeFrames = 384;
    static_assert((kOutputBridgeFrames & kOutputBridgeMask) == 0,
                  "output bridge size must be a power of two");

    PitchDetector detector;
    PredictivePitchTracker pitchPredictor{kSampleRate};
    Voice         voices[kMaxVoices];
    SharedDisplay display;

    size_t blockSize = kEarlyBlockSize;
    size_t bufPos = 0;
    size_t qualityInputPos = 0;
    size_t qualityOutputPos = 0;
    size_t qualityStartDelaySamples = 0;
    size_t earlyStartDelaySamples = 0;
    size_t dryDelaySamples = 0;
    size_t dryDelayPos = 0;
    float monitorGainLinear = 1.0f;
    std::vector<float> inputBuf;
    std::vector<float> outputL;
    std::vector<float> outputR;
    std::vector<float> earlyShiftBuf;
    std::vector<float> earlyVoicingBuf;
    std::vector<float> earlyArticulationBuf;
    std::vector<float> earlyMidiEnvelopeBuf;
    std::vector<float> earlyConfidenceBuf;
    std::vector<float> alignedInputBuf;
    std::vector<float> qualityInputBuf;
    std::vector<float> qualityMixL;
    std::vector<float> qualityMixR;
    static constexpr int kQualityWorkerCount = 4;
    std::array<std::vector<float>, kQualityWorkerCount> qualityWorkerShiftBuf;
    std::array<std::vector<float>, kQualityWorkerCount> qualityWorkerMixL;
    std::array<std::vector<float>, kQualityWorkerCount> qualityWorkerMixR;
    std::vector<float> qualityVoicingBuf;
    std::vector<float> qualityArticulationBuf;
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
    static constexpr float kHandoffFadeSec = 0.030f;
    static constexpr float kConfidenceSmoothingSec = 0.005f;
    float pitchHist[kPitchHistLen] = {};
    int   pitchHistIdx = 0;
    float smoothedMidi = -1.0f;
    float fastCorrectionMidi = -1.0f;
    float rawQualityCorrectionMidi = -1.0f;
    float correctionMidi = -1.0f;
    int   correctionHoldFrames = 0;
    float correctionControlHistory[kCorrectionControlHistoryLen] = {};
    uint64_t correctionControlSampleHistory[kCorrectionControlHistoryLen] = {};
    int   correctionControlHistoryIdx = 0;
    uint64_t latestRawPitchSample = 0;
    uint64_t earlyControlSample = 0;
    uint64_t qualityControlSample = 0;
    float latestRawPitchMidi = -1.0f;
    float previousCorrectionDelta = 0.0f;
    float correctionDeltaEnergyMean = 0.0f;
    bool  correctionFlutterReady = false;
    bool  predictorVoiced = false;
    float predictorConfidence = 0.0f;
    uint64_t lastPitchOnsetSample = 0;
    static constexpr int kDefaultEarlyPitchOffsetSamples = -1184;
    float flutterCompensationAmount = 0.0f;
    int earlyPitchOffsetSamples = kDefaultEarlyPitchOffsetSamples;
    bool persistentEarlyPath = false;

    float earlyVoicingEnv     = 0.0f;
    float qualityVoicingEnv   = 0.0f;
    float voicingAttackCoeff  = 0.0f;
    float voicingReleaseCoeff = 0.0f;
    int   voicedGraceBlocks   = 0;
    float earlyArticulationEnv = 0.0f;
    float qualityArticulationEnv = 0.0f;
    float articulationCoeff   = 0.0f;
    float earlyConfidenceEnv  = 0.0f;
    float confidenceCoeff     = 0.0f;
    float sibilanceLowpass    = 0.0f;
    float heldSourceMidi      = -1.0f;
    size_t heldPitchSilenceSamples = 0;
    bool currentEarlyVoicingOn = false;
    bool currentQualityVoicingOn = false;
    bool currentUnvoicedInput = false;
    bool currentTcBypass = true;

    std::mutex         midiMutex;
    bool               sustainOn = false;
    uint64_t           noteCounter = 0;
    std::atomic<float> pitchBend{0.0f};

    std::array<std::thread, kQualityWorkerCount - 1> qualityWorkers;
    std::mutex qualityWorkerMutex;
    std::condition_variable qualityWorkerWake;
    std::condition_variable qualityWorkerDone;
    bool qualityWorkersStopping = false;
    uint64_t qualityJobGeneration = 0;
    int qualityWorkersPending = 0;
    float qualityJobPerVoiceWet = 0.0f;
    float qualityJobBend = 0.0f;
    float qualityJobEarlyWeight = 0.0f;
    uint64_t qualityJobMixStartSample = 0;

    std::atomic<uint64_t> sampleClock{0};
    std::atomic<uint64_t> testToneStartClock{0};
    std::atomic<uint64_t> testToneEndClock{0};
    float                 testTonePhase = 0.0f;
    Capture capture;

    void qualityWorkerLoop(int workerIndex) {
        configureAudioWorkerThread(3.0);
        uint64_t completedGeneration = 0;
        while (true) {
            std::unique_lock<std::mutex> lock(qualityWorkerMutex);
            qualityWorkerWake.wait(lock, [&] {
                return qualityWorkersStopping ||
                       qualityJobGeneration != completedGeneration;
            });
            if (qualityWorkersStopping) return;
            completedGeneration = qualityJobGeneration;
            lock.unlock();

            processQualityVoiceGroup(workerIndex);

            lock.lock();
            qualityWorkersPending--;
            if (qualityWorkersPending == 0) qualityWorkerDone.notify_one();
        }
    }

    void startQualityWorkers() {
        qualityWorkersStopping = false;
        qualityJobGeneration = 0;
        qualityWorkersPending = 0;
        for (int worker = 1; worker < kQualityWorkerCount; ++worker) {
            qualityWorkers[worker - 1] =
                std::thread([this, worker] { qualityWorkerLoop(worker); });
        }
    }

    void stopQualityWorkers() {
        {
            std::lock_guard<std::mutex> lock(qualityWorkerMutex);
            qualityWorkersStopping = true;
        }
        qualityWorkerWake.notify_all();
        for (auto& worker : qualityWorkers) {
            if (worker.joinable()) worker.join();
        }
    }

    ~AudioEngine() { stopQualityWorkers(); }

    void init() {
        for (int voice = 0; voice < kMaxVoices; voice++) voices[voice].init();

        blockSize = voices[0].earlyShifter.getBlockSize();
        qualityStartDelaySamples = voices[0].qualityShifter.getStartDelay();
        earlyStartDelaySamples = voices[0].earlyShifter.getStartDelay();
        dryDelaySamples = qualityStartDelaySamples + kQualityBlockSize - blockSize;
        inputBuf.assign(blockSize, 0.0f);
        outputL.assign(blockSize, 0.0f);
        outputR.assign(blockSize, 0.0f);
        earlyShiftBuf.assign(blockSize, 0.0f);
        earlyVoicingBuf.assign(blockSize, 0.0f);
        earlyArticulationBuf.assign(blockSize, 0.0f);
        earlyMidiEnvelopeBuf.assign(blockSize, 0.0f);
        earlyConfidenceBuf.assign(blockSize, 0.0f);
        alignedInputBuf.assign(blockSize, 0.0f);
        qualityInputBuf.assign(kQualityBlockSize, 0.0f);
        qualityMixL.assign(kQualityBlockSize, 0.0f);
        qualityMixR.assign(kQualityBlockSize, 0.0f);
        for (int worker = 0; worker < kQualityWorkerCount; ++worker) {
            qualityWorkerShiftBuf[worker].assign(kQualityBlockSize, 0.0f);
            qualityWorkerMixL[worker].assign(kQualityBlockSize, 0.0f);
            qualityWorkerMixR[worker].assign(kQualityBlockSize, 0.0f);
        }
        qualityVoicingBuf.assign(kQualityBlockSize, 0.0f);
        qualityArticulationBuf.assign(kQualityBlockSize, 0.0f);
        highBandBuf.assign(kQualityBlockSize, 0.0f);
        dryDelay.assign(dryDelaySamples, 0.0f);
        outputBridgeL.assign(kOutputBridgeFrames, 0.0f);
        outputBridgeR.assign(kOutputBridgeFrames, 0.0f);

        voicingAttackCoeff = 1.0f - std::exp(-1.0f / (kVoicingAttackSec * kSampleRate));
        voicingReleaseCoeff = 1.0f - std::exp(-1.0f / (kVoicingReleaseSec * kSampleRate));
        articulationCoeff = 1.0f - std::exp(
            -1.0f / (kArticulationCrossfadeSec * kSampleRate));
        confidenceCoeff = 1.0f - std::exp(
            -1.0f / (kConfidenceSmoothingSec * kSampleRate));
        startQualityWorkers();

        display.earlyPathLatencyMs.store((float)earlyLatencyMs(), std::memory_order_relaxed);
        display.qualityPathLatencyMs.store((float)qualityLatencyMs(), std::memory_order_relaxed);
        std::cerr << "Parallel R2 + Live512: DSP path " << dspLatencyMs()
                  << " ms; early " << earlyLatencyMs()
                  << " ms, quality " << qualityLatencyMs() << " ms\n";
    }

    double dspLatencyMs() const {
        return display.parallelEarlyBlend.load(std::memory_order_relaxed) > 0.0001f
            ? earlyLatencyMs()
            : qualityLatencyMs();
    }

    double earlyLatencyMs() const {
        return 1000.0 *
            (double)(blockSize + earlyStartDelaySamples + kOutputBridgePrimeFrames) /
            kSampleRate;
    }

    double qualityLatencyMs() const {
        return 1000.0 *
            (double)(kQualityBlockSize + qualityStartDelaySamples +
                     kOutputBridgePrimeFrames) /
            kSampleRate;
    }

    void updatePitchControl(float frequency,
                            bool stable,
                            float gateRms,
                            uint64_t detectorEndSample) {
        const bool energetic =
            recentPitchRms >= gateRms * kPitchGateReleaseRatio;
        const bool valid = frequency > 0.0f;
        const uint64_t halfWindow = (uint64_t)kPitchWinSize / 2;
        const uint64_t measurementSample = detectorEndSample > halfWindow
            ? detectorEndSample - halfWindow
            : 0;
        latestRawPitchSample = measurementSample;
        latestRawPitchMidi = valid && energetic
            ? freqToMidi(frequency)
            : -1.0f;
        PredictivePitchTracker::UpdateResult update = pitchPredictor.update(
            valid ? freqToMidi(frequency) : -1.0f,
            valid,
            energetic,
            stable,
            measurementSample);

        predictorVoiced = update.voiced;
        predictorConfidence = update.confidence;
        if (update.onset) lastPitchOnsetSample = detectorEndSample;

        const uint64_t earlyHalf = blockSize / 2;
        const uint64_t qualityHalf = kQualityBlockSize / 2;
        const uint64_t earlyTarget = detectorEndSample > earlyHalf
            ? detectorEndSample - earlyHalf
            : 0;
        const uint64_t qualityTarget = detectorEndSample > qualityHalf
            ? detectorEndSample - qualityHalf
            : 0;
        fastCorrectionMidi = predictorVoiced
            ? pitchPredictor.estimateAt(earlyTarget)
            : -1.0f;
        float predictedQualityMidi = predictorVoiced
            ? pitchPredictor.estimateAt(qualityTarget)
            : -1.0f;

        // LiveShifter applies control to audio already buffered internally.
        // Its tested optimum remains the previous raw detector hop; projecting
        // to the incoming block doubles the residual vibrato. Keep that
        // timeline alignment while the low-latency R2 path uses prediction.
        if (valid && energetic) {
            rawQualityCorrectionMidi = freqToMidi(frequency);
            correctionHoldFrames = kPitchReleaseFrames;
        } else if (correctionHoldFrames > 0) {
            correctionHoldFrames--;
        } else {
            rawQualityCorrectionMidi = -1.0f;
        }
        correctionControlHistory[correctionControlHistoryIdx] =
            rawQualityCorrectionMidi;
        correctionControlSampleHistory[correctionControlHistoryIdx] =
            measurementSample;
        correctionControlHistoryIdx =
            (correctionControlHistoryIdx + 1) % kCorrectionControlHistoryLen;
        constexpr int controlLag =
            kCorrectionLagHops < kCorrectionControlHistoryLen
                ? kCorrectionLagHops
                : kCorrectionControlHistoryLen - 1;
        int delayedIndex =
            (correctionControlHistoryIdx - 1 - controlLag +
             kCorrectionControlHistoryLen) % kCorrectionControlHistoryLen;
        float delayedCorrection = correctionControlHistory[delayedIndex];
        correctionMidi = delayedCorrection > 0.0f
            ? delayedCorrection
            : rawQualityCorrectionMidi;
        qualityControlSample = delayedCorrection > 0.0f
            ? correctionControlSampleHistory[delayedIndex]
            : measurementSample;

        display.predictedEarlyMidi.store(fastCorrectionMidi, std::memory_order_relaxed);
        display.predictedQualityMidi.store(predictedQualityMidi, std::memory_order_relaxed);
        display.correctionMidi.store(correctionMidi, std::memory_order_relaxed);
        display.pitchSlope.store(pitchPredictor.slope(), std::memory_order_relaxed);
        display.predictorConfidence.store(predictorConfidence, std::memory_order_relaxed);
        display.predictorVoiced.store(predictorVoiced, std::memory_order_relaxed);
    }

    void onVoiceAssigned(int voice) {
        if (voice < 0 || voice >= kMaxVoices) return;
        voices[voice].handoffEventSample.store(
            sampleClock.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    }

    void configurePitchExperiments(float flutterAmount,
                                   int earlyOffsetSamples,
                                   bool keepEarlyPath) {
        flutterCompensationAmount = clampf(flutterAmount, 0.0f, 1.0f);
        earlyPitchOffsetSamples = std::clamp(
            earlyOffsetSamples, -kPitchWinSize, kPitchWinSize);
        persistentEarlyPath = keepEarlyPath;
    }

    float projectedPitchForBlock(size_t blockSamples,
                                 uint64_t& targetSample) const {
        uint64_t endSample = sampleClock.load(std::memory_order_relaxed);
        uint64_t halfBlock = blockSamples / 2;
        int64_t signedTargetSample =
            (int64_t)endSample - (int64_t)halfBlock +
            (int64_t)earlyPitchOffsetSamples;
        targetSample = signedTargetSample > 0
            ? (uint64_t)signedTargetSample
            : 0;
        if (!predictorVoiced || !pitchPredictor.initialized()) return -1.0f;
        return pitchPredictor.estimateAt(
            targetSample);
    }

    float handoffEnvelope(const Voice& voice, uint64_t mixSample) const {
        if (persistentEarlyPath) return 1.0f;
        uint64_t eventSample = std::max(
            voice.handoffEventSample.load(std::memory_order_relaxed),
            lastPitchOnsetSample);
        uint64_t qualityArrival = eventSample + kQualityBlockSize +
                                  qualityStartDelaySamples;
        if (mixSample <= qualityArrival) return 1.0f;

        const uint64_t fadeSamples =
            std::max<uint64_t>(1, (uint64_t)(kHandoffFadeSec * kSampleRate));
        uint64_t elapsed = mixSample - qualityArrival;
        if (elapsed >= fadeSamples) return 0.0f;
        float progress = (float)elapsed / (float)fadeSamples;
        float smooth = progress * progress * (3.0f - 2.0f * progress);
        return 1.0f - smooth;
    }

    bool detectSibilance(float& score) {
        float peak = 0.0f;
        float totalEnergy = 0.0f;
        float highBandEnergy = 0.0f;
        float low = sibilanceLowpass;
        const float lowpassCoeff = 1.0f - std::exp(
            -2.0f * kPi * kSibilanceLowpassHz / kSampleRate);

        for (size_t sample = 0; sample < kQualityBlockSize; sample++) {
            float input = qualityInputBuf[sample];
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
            (0.008f * kSampleRate / (float)kQualityBlockSize);
        score = std::min(highBandRatio / kSibilanceHighBandRatio,
                         crossingsPer8ms / kSibilanceCrossingsPer8ms);
        score = clampf(score, 0.0f, 4.0f);

        bool highFrequencyVariation =
            highBandRatio >= kSibilanceHighBandRatio &&
            crossingsPer8ms >= kSibilanceCrossingsPer8ms;
        return highFrequencyVariation &&
            (!pitchStable || highBandRatio >= kSibilanceStrongHighBandRatio);
    }

    void updateVoicingState() {
        float gateRms = display.voicedGateRms.load(std::memory_order_relaxed);
        bool voicedInput = pitchVoiced && detectedMidi > 0.0f &&
                           recentPitchRms >= gateRms * kPitchGateReleaseRatio;
        bool predictedInput = predictorVoiced && fastCorrectionMidi > 0.0f &&
                              recentPitchRms >= gateRms * kPitchGateReleaseRatio;
        bool qualityReady = predictedInput && predictorConfidence >= 0.70f;
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
            heldPitchSilenceSamples += kQualityBlockSize;
            if (heldPitchSilenceSamples >=
                (size_t)(kHeldPitchForgetSec * kSampleRate)) {
                heldSourceMidi = -1.0f;
                heldPitchSilenceSamples = 0;
            }
        }

        bool detectorUnvoiced = !pitchStable && !predictorVoiced && energeticInput &&
                                heldSourceMidi > 0.0f;
        currentUnvoicedInput = sibilantInput || detectorUnvoiced;
        currentTcBypass = display.unvoicedMode.load(std::memory_order_relaxed) ==
                          static_cast<int>(UnvoicedMode::TcBypass);
        display.unvoicedActive.store(currentUnvoicedInput, std::memory_order_relaxed);
        display.sibilanceScore.store(sibilanceScore, std::memory_order_relaxed);
        if (voicedInput || qualityReady || currentUnvoicedInput)
            voicedGraceBlocks = kVoicingGraceHops;
        else if (voicedGraceBlocks > 0) voicedGraceBlocks--;
        currentEarlyVoicingOn = predictedInput || currentUnvoicedInput ||
                                voicedGraceBlocks > 0;
        currentQualityVoicingOn = voicedInput || qualityReady ||
                                  currentUnvoicedInput || voicedGraceBlocks > 0;
    }

    void fillPathEnvelopes(size_t samples,
                           bool pathVoicingOn,
                           float& voicingEnv,
                           std::vector<float>& voicing,
                           float& articulationEnv,
                           std::vector<float>& articulation) {
        float voicingTarget = pathVoicingOn ? 1.0f : 0.0f;
        float articulationTarget = currentTcBypass && currentUnvoicedInput
            ? 1.0f
            : 0.0f;
        for (size_t sample = 0; sample < samples; sample++) {
            float coeff = voicingTarget > voicingEnv
                ? voicingAttackCoeff
                : voicingReleaseCoeff;
            voicingEnv += (voicingTarget - voicingEnv) * coeff;
            voicing[sample] = voicingEnv;
            articulationEnv += (articulationTarget - articulationEnv) * articulationCoeff;
            articulation[sample] = articulationEnv;
        }
        if (voicingEnv < 0.0001f) voicingEnv = 0.0f;
    }

    int activeVoiceCount() const {
        int activeVoices = 0;
        for (int voice = 0; voice < kMaxVoices; voice++) {
            int note = voices[voice].midiNote.load(std::memory_order_relaxed);
            if (note > 0 && voices[voice].isAudible()) activeVoices++;
        }
        return activeVoices;
    }

    void updateFlutterState() {
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
                return;
            }
        }
        previousCorrectionDelta = 0.0f;
        correctionDeltaEnergyMean = 0.0f;
        correctionFlutterReady = false;
    }

    void processQualityVoiceGroup(int workerIndex) {
        auto& shiftBuffer = qualityWorkerShiftBuf[workerIndex];
        auto& mixLeft = qualityWorkerMixL[workerIndex];
        auto& mixRight = qualityWorkerMixR[workerIndex];
        std::fill(mixLeft.begin(), mixLeft.end(), 0.0f);
        std::fill(mixRight.begin(), mixRight.end(), 0.0f);

        const float* inputChannels[] = { qualityInputBuf.data() };
        float* outputChannels[] = { shiftBuffer.data() };
        for (int voice = workerIndex; voice < kMaxVoices;
             voice += kQualityWorkerCount) {
            Voice& current = voices[voice];
            int note = current.midiNote.load(std::memory_order_relaxed);
            if (note <= 0 || !current.isAudible()) continue;

            bool liveCorrection = correctionMidi > 0.0f && pitchVoiced &&
                                  !currentUnvoicedInput;
            float sourceMidi = liveCorrection ? correctionMidi : heldSourceMidi;
            if (sourceMidi > 0.0f) {
                float targetMidi = display.glideTargetMidi(
                    note, qualityJobMixStartSample) + qualityJobBend;
                float correctionControl = sourceMidi;
                if (liveCorrection && detectedMidi > 0.0f &&
                    flutterCompensationAmount > 0.0f) {
                    float correctionDelta = correctionMidi - detectedMidi;
                    if (std::fabs(correctionDelta) <= kFlutterCompensationRange &&
                        std::fabs(previousCorrectionDelta) <= kFlutterCompensationRange &&
                        correctionFlutterReady) {
                        float flutterCompensation = kFlutterDerivativeCompensation *
                            (correctionDelta * correctionDelta -
                             previousCorrectionDelta * previousCorrectionDelta);
                        flutterCompensation += kFlutterEnergyCompensation *
                            (correctionDelta * correctionDelta -
                             correctionDeltaEnergyMean);
                        correctionControl += flutterCompensationAmount *
                            clampf(flutterCompensation,
                                   -kFlutterCompensationMax,
                                   kFlutterCompensationMax);
                    }
                }
                current.qualityPitchRatio = clampf(
                    noteToFreq(targetMidi) / noteToFreq(correctionControl),
                    0.25f, 4.0f);
            }
            current.qualityShifter.setPitchScale(current.qualityPitchRatio);
            current.qualityShifter.shift(inputChannels, outputChannels);
            current.updatePan();

            for (size_t sample = 0; sample < kQualityBlockSize; sample++) {
                float envelope = current.tickQualityEnvelope();
                float handoff = handoffEnvelope(
                    current, qualityJobMixStartSample + sample);
                float qualityPathGain =
                    1.0f - qualityJobEarlyWeight * handoff;
                float shifted = shiftBuffer[sample] * envelope *
                                qualityVoicingBuf[sample] *
                                (1.0f - qualityArticulationBuf[sample]) *
                                qualityJobPerVoiceWet * qualityPathGain;
                mixLeft[sample] += shifted * current.panL;
                mixRight[sample] += shifted * current.panR;
            }
        }
    }

    void processQualityBlock(float wetGain, float requestedEarlyWeight) {
        fillPathEnvelopes(kQualityBlockSize, currentQualityVoicingOn,
                          qualityVoicingEnv, qualityVoicingBuf,
                          qualityArticulationEnv, qualityArticulationBuf);
        std::fill(qualityMixL.begin(), qualityMixL.end(), 0.0f);
        std::fill(qualityMixR.begin(), qualityMixR.end(), 0.0f);

        int activeVoices = activeVoiceCount();
        if (activeVoices <= 0) {
            updateFlutterState();
            return;
        }

        {
            std::lock_guard<std::mutex> lock(qualityWorkerMutex);
            qualityJobPerVoiceWet = wetGain / std::sqrt((float)activeVoices);
            qualityJobBend = pitchBend.load(std::memory_order_relaxed);
            qualityJobEarlyWeight = requestedEarlyWeight;
            qualityJobMixStartSample =
                sampleClock.load(std::memory_order_relaxed) + kEarlyBlockSize;
            qualityWorkersPending = kQualityWorkerCount - 1;
            qualityJobGeneration++;
        }
        qualityWorkerWake.notify_all();
        processQualityVoiceGroup(0);

        {
            std::unique_lock<std::mutex> lock(qualityWorkerMutex);
            qualityWorkerDone.wait(lock, [&] {
                return qualityWorkersPending == 0;
            });
        }

        for (int worker = 0; worker < kQualityWorkerCount; ++worker) {
            for (size_t sample = 0; sample < kQualityBlockSize; ++sample) {
                qualityMixL[sample] += qualityWorkerMixL[worker][sample];
                qualityMixR[sample] += qualityWorkerMixR[worker][sample];
            }
        }
        updateFlutterState();
    }

    int processEarlyBlock(float wetGain, float requestedEarlyWeight) {
        fastCorrectionMidi = projectedPitchForBlock(blockSize, earlyControlSample);
        display.predictedEarlyMidi.store(fastCorrectionMidi, std::memory_order_relaxed);

        fillPathEnvelopes(blockSize, currentEarlyVoicingOn,
                          earlyVoicingEnv, earlyVoicingBuf,
                          earlyArticulationEnv, earlyArticulationBuf);
        std::fill(earlyMidiEnvelopeBuf.begin(), earlyMidiEnvelopeBuf.end(), 0.0f);
        for (size_t sample = 0; sample < blockSize; sample++) {
            earlyConfidenceEnv +=
                (predictorConfidence - earlyConfidenceEnv) * confidenceCoeff;
            earlyConfidenceBuf[sample] = earlyConfidenceEnv;
        }

        int activeVoices = activeVoiceCount();
        float perVoiceWet = activeVoices > 0
            ? wetGain / std::sqrt((float)activeVoices)
            : 0.0f;
        float bend = pitchBend.load(std::memory_order_relaxed);
        uint64_t mixStartSample = sampleClock.load(std::memory_order_relaxed);
        float handoffActivity = 0.0f;
        const float* inputChannels[] = { inputBuf.data() };
        float* outputChannels[] = { earlyShiftBuf.data() };

        for (int voice = 0; voice < kMaxVoices; voice++) {
            Voice& current = voices[voice];
            int note = current.midiNote.load(std::memory_order_relaxed);
            if (note <= 0 || !current.isAudible()) continue;

            bool liveCorrection = fastCorrectionMidi > 0.0f && predictorVoiced &&
                                  !currentUnvoicedInput;
            float sourceMidi = liveCorrection ? fastCorrectionMidi : heldSourceMidi;
            if (sourceMidi > 0.0f) {
                float targetMidi = display.glideTargetMidi(note, mixStartSample) + bend;
                current.earlyPitchRatio = clampf(
                    noteToFreq(targetMidi) / noteToFreq(sourceMidi),
                    0.25f, 4.0f);
            }
            current.earlyShifter.setPitchScale(current.earlyPitchRatio);
            current.earlyShifter.shift(inputChannels, outputChannels);
            current.updatePan();

            for (size_t sample = 0; sample < blockSize; sample++) {
                float envelope = current.tickEarlyEnvelope();
                earlyMidiEnvelopeBuf[sample] =
                    std::max(earlyMidiEnvelopeBuf[sample], envelope);
                float handoff = handoffEnvelope(current, mixStartSample + sample);
                handoffActivity = std::max(handoffActivity, handoff);
                float provisionalGain = currentUnvoicedInput
                    ? 1.0f
                    : earlyConfidenceBuf[sample];
                float earlyPathGain = requestedEarlyWeight * handoff *
                                      provisionalGain;
                float shifted = earlyShiftBuf[sample] * envelope *
                                earlyVoicingBuf[sample] *
                                (1.0f - earlyArticulationBuf[sample]) *
                                perVoiceWet * earlyPathGain;
                outputL[sample] += shifted * current.panL;
                outputR[sample] += shifted * current.panR;
            }
        }
        display.parallelHandoff.store(
            requestedEarlyWeight * handoffActivity,
            std::memory_order_relaxed);
        return activeVoices;
    }

    void processBlock() {
        for (size_t sample = 0; sample < blockSize; sample++) {
            qualityInputBuf[qualityInputPos + sample] = inputBuf[sample];
        }
        qualityInputPos += blockSize;
        bool qualityBlockReady = qualityInputPos == kQualityBlockSize;
        if (qualityBlockReady) updateVoicingState();

        float wetGain = 0.0f;
        float dryGain = 0.0f;
        wetDryFromBalance(display.wetDryBalance.load(std::memory_order_relaxed),
                          wetGain, dryGain);
        float earlyWeight = clampf(
            display.parallelEarlyBlend.load(std::memory_order_relaxed),
            0.0f, 1.0f);

        // Dry monitoring and TC-style consonant bypass stay aligned with the
        // slower quality arrival. The R2 path is deliberately not delayed: its
        // earlier onset is the perceptual anchor this backend is testing.
        for (size_t sample = 0; sample < blockSize; sample++) {
            float delayedDry = inputBuf[sample];
            if (!dryDelay.empty()) {
                delayedDry = dryDelay[dryDelayPos];
                dryDelay[dryDelayPos] = inputBuf[sample];
                dryDelayPos = (dryDelayPos + 1) % dryDelay.size();
            }
            if (wetGain <= 0.0001f) delayedDry = inputBuf[sample];
            alignedInputBuf[sample] = delayedDry;
            outputL[sample] = delayedDry * dryGain +
                              qualityMixL[qualityOutputPos + sample];
            outputR[sample] = delayedDry * dryGain +
                              qualityMixR[qualityOutputPos + sample];
        }

        int activeVoices = processEarlyBlock(wetGain, earlyWeight);
        const uint64_t telemetrySample =
            sampleClock.load(std::memory_order_relaxed);
        if (telemetrySample % kPitchHopSize == 0) {
            display.appendPitchTelemetry(
                telemetrySample,
                latestRawPitchSample,
                latestRawPitchMidi,
                earlyControlSample,
                fastCorrectionMidi,
                qualityControlSample,
                correctionMidi,
                display.parallelHandoff.load(std::memory_order_relaxed),
                pitchBend.load(std::memory_order_relaxed));
        }
        if (activeVoices > 0 && wetGain > 0.0001f) {
            for (size_t sample = 0; sample < blockSize; sample++) {
                float articulation = alignedInputBuf[sample] * wetGain *
                    earlyVoicingBuf[sample] * earlyMidiEnvelopeBuf[sample] *
                    earlyArticulationBuf[sample];
                outputL[sample] += articulation;
                outputR[sample] += articulation;
            }
        }

        if (qualityBlockReady) {
            processQualityBlock(wetGain, earlyWeight);
            qualityInputPos = 0;
            qualityOutputPos = 0;
        } else {
            qualityOutputPos += blockSize;
        }

        for (size_t sample = 0; sample < blockSize; sample++) {
            outputL[sample] = std::tanh(outputL[sample]);
            outputR[sample] = std::tanh(outputR[sample]);
        }
    }
};

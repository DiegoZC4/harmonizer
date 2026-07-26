/*
 * harmonizer_web.cpp — Native Rubber Band Harmonizer with Browser GUI
 *
 * The browser serves controls and visualization only. PortAudio, aubio pitch
 * tracking, MIDI voice allocation, and formant-preserving Rubber Band pitch
 * shifting all run in this native process.
 *
 * brew install portaudio aubio rubberband
 *
 * Build:
 *   clang++ -O3 -std=c++17 harmonizer_web.cpp \
 *     -I/opt/homebrew/include -L/opt/homebrew/lib \
 *     -lportaudio -laubio -lrubberband -lpthread \
 *     -framework CoreAudio -framework CoreFoundation \
 *     -o harmonizer_web
 *
 * Browser GUI:
 *   http://127.0.0.1:8794/
 *   SSE stream for pitch/MIDI state, HTTP endpoints for sliders.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <portaudio.h>
#include <aubio/aubio.h>

#include "backends/output_bridge_controller.hpp"
#include "collier_effects.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pa_mac_core.h>
#include <pthread.h>
#endif

// ═══════════════════════════════════════════════════════════════════════════
//  Configuration
// ═══════════════════════════════════════════════════════════════════════════

static constexpr int   kSampleRate      = 44100;
static constexpr int   kFramesPerBuffer = 64;
static constexpr int   kMaxVoices       = 16;
static constexpr float kPi              = 3.14159265358979323846f;
static constexpr double kOutputBridgeTargetFrames = 384.0;

// Pitch detection
static constexpr int   kPitchWinSize    = 2048;
static constexpr int   kPitchHopSize    = 512;
// Accept a one-semitone guard band below A1 so a clear but slightly flat A1 is
// not rejected by an exact 55.000 Hz boundary.
static constexpr int   kMinDetectedMidi = 32;
static constexpr int   kMaxDetectedMidi = 84;    // C6
// YINFFT can report A1-B1 one octave high, so probe the strict low lane for
// primary candidates through C#3. Only A1-B1 low-lane results are accepted.
static constexpr int   kLowPitchProbeCeilingMidi = 49;
static constexpr float kLowPitchMinConfidence = 0.85f;

// Voice
static constexpr float kAttackSec       = 0.005f;
static constexpr float kReleaseSec      = 0.080f;

// Voicing gate for the wet harmony as a whole. Two Rubber Band blocks bridge
// brief detector dropouts before the wet path fades.
static constexpr int   kVoicingGraceHops  = 2;
static constexpr float kVoicingAttackSec  = 0.015f;
static constexpr float kVoicingReleaseSec = 0.120f;

static constexpr float kBendRange       = 2.0f;

// Constant-amplitude wet/dry crossfade.
static constexpr float kDefaultWetDryBalance = 1.0f; // 0 = dry, 1 = wet
static constexpr float kDefaultMonitorGainDb = 18.0f;
static constexpr float kDefaultVoicedGateRms = 0.0100f;
static constexpr float kDefaultStableSemitoneWindow = 1.0f;
static constexpr float kDefaultParallelEarlyBlend = 0.25f;
static constexpr float kDefaultGlideAmount = 0.0f;
static constexpr float kDefaultGlideTimeMs = 280.0f;
static constexpr float kDefaultChorusMix = 0.0f;
static constexpr float kDefaultReverbMix = 0.0f;
static constexpr float kDefaultFreezeLevel = 0.5f;
static constexpr float kDefaultFreezeTranspose = 0.0f;
static constexpr float kDefaultFreezeTone = 1.0f;

enum class UnvoicedMode : int {
    TcBypass = 0,
    HoldRatio = 1,
};
static constexpr UnvoicedMode kDefaultUnvoicedMode = UnvoicedMode::TcBypass;

static constexpr int   kDefaultWebPort  = 8794;
static constexpr const char* kWebIndexPath = "web/index.html";
static constexpr const char* kAudioInputPreferencePath = ".harmonizer_input_device";
static constexpr const char* kAudioOutputPreferencePath = ".harmonizer_output_device";

#if defined(_WIN32)
using SocketHandle = SOCKET;
static constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

static bool initializeSockets() {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

static void shutdownSockets() { WSACleanup(); }
static void closeSocket(SocketHandle socket) { closesocket(socket); }
static void shutdownSocket(SocketHandle socket) { ::shutdown(socket, SD_BOTH); }
static std::string socketErrorText() { return std::to_string(WSAGetLastError()); }
#else
using SocketHandle = int;
static constexpr SocketHandle kInvalidSocket = -1;

static bool initializeSockets() { return true; }
static void shutdownSockets() {}
static void closeSocket(SocketHandle socket) { ::close(socket); }
static void shutdownSocket(SocketHandle socket) { ::shutdown(socket, SHUT_RDWR); }
static std::string socketErrorText() { return std::strerror(errno); }
#endif

static std::atomic<bool> gRunning{true};
static std::atomic<bool> gAudioReady{false};
static std::atomic<bool> gMidiReady{false};
struct AudioEngine;

struct AudioCallbackContext {
    AudioEngine* engine = nullptr;
    std::atomic<bool> enabled{false};

    AudioCallbackContext(AudioEngine* audioEngine, bool isEnabled)
        : engine(audioEngine), enabled(isEnabled) {}
};

static std::mutex gAudioMutex;
static bool gPortAudioInitialized = false;
static PaStream* gAudioInputStream = nullptr;
static PaStream* gAudioOutputStream = nullptr;
static std::unique_ptr<AudioCallbackContext> gAudioInputContext;
static std::unique_ptr<AudioCallbackContext> gAudioOutputContext;
static std::atomic<bool> gOutputBridgeRebasePending{false};
static PaDeviceIndex gAudioInputDevice = paNoDevice;
static PaDeviceIndex gAudioOutputDevice = paNoDevice;
static std::atomic<float> gAudioInputLatencyMs{0.0f};
static std::atomic<float> gAudioOutputLatencyMs{0.0f};

struct OutputBridgeRuntime {
    OutputBridgeClockController clock{
        static_cast<double>(kSampleRate), kOutputBridgeTargetFrames};
    double readPosition = 0.0;
    float fadeGain = 0.0f;
    float lastLeft = 0.0f;
    float lastRight = 0.0f;
    bool positionReady = false;

    void reset(uint64_t read = 0) {
        clock.reset();
        readPosition = static_cast<double>(read);
        fadeGain = 0.0f;
        lastLeft = 0.0f;
        lastRight = 0.0f;
        positionReady = false;
    }
};

static OutputBridgeRuntime gOutputBridgeRuntime;

struct AudioDevice {
    PaDeviceIndex index = paNoDevice;
    std::string name;
};

static std::vector<AudioDevice> gAudioInputDevices;
static std::vector<AudioDevice> gAudioOutputDevices;
static std::string gAudioError;
static std::string gAudioRouteStatus = "starting";
static std::string gAudioRouteStage;
static float gAudioRouteElapsedMs = 0.0f;
static bool gAudioRouteKeptPrevious = false;
static int gAudioPortAudioError = paNoError;
static long gAudioHostError = 0;
static std::string gAudioHostErrorText;
static std::string gAudioInputName = "unknown";
static std::string gAudioOutputName = "unknown";
static std::string gMidiError;

static std::string portAudioDeviceName(PaDeviceIndex device) {
    if (device == paNoDevice) return "none";
    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    return (info && info->name) ? info->name : "unknown";
}

// ── Utilities ──────────────────────────────────────────────────────────────

static inline float noteToFreq(float n) {
    return 440.0f * std::pow(2.0f, (n - 69.0f) / 12.0f);
}
static inline float freqToMidi(float f) {
    return (f > 0.0f) ? 69.0f + 12.0f * std::log2(f / 440.0f) : -1.0f;
}
static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline void wetDryFromBalance(float balance, float& wet, float& dry) {
    balance = clampf(balance, 0.0f, 1.0f);
    wet = balance;
    dry = 1.0f - balance;
}
static inline float elapsedMilliseconds(
    std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

struct AudioRouteFailure {
    std::string stage;
    std::string message;
    PaError portAudioError = paNoError;
    long hostError = 0;
    std::string hostErrorText;
};

static std::string audioHostErrorCode(long error) {
    const uint32_t code = static_cast<uint32_t>(error);
    std::array<char, 4> fourcc{
        static_cast<char>((code >> 24) & 0xff),
        static_cast<char>((code >> 16) & 0xff),
        static_cast<char>((code >> 8) & 0xff),
        static_cast<char>(code & 0xff),
    };
    const bool printable = std::all_of(fourcc.begin(), fourcc.end(), [](char c) {
        return std::isprint(static_cast<unsigned char>(c)) != 0;
    });
    std::ostringstream out;
    if (printable) out << "'" << std::string(fourcc.begin(), fourcc.end()) << "' ";
    out << "(" << error << ", 0x" << std::hex << std::uppercase << code << ")";
    return out.str();
}

static AudioRouteFailure captureAudioRouteFailure(
    PaError error, const std::string& stage, const std::string& deviceName) {
    AudioRouteFailure failure;
    failure.stage = stage;
    failure.portAudioError = error;
    const bool hostErrorRelevant =
        error == paInternalError || error == paUnanticipatedHostError ||
        error == paDeviceUnavailable || error == paSampleFormatNotSupported;
    if (hostErrorRelevant) {
        const PaHostErrorInfo* host = Pa_GetLastHostErrorInfo();
        if (host) {
            failure.hostError = host->errorCode;
            failure.hostErrorText = host->errorText ? host->errorText : "";
        }
    }

    const bool input = stage.find("input") != std::string::npos;
    const bool starting = stage.find("start") != std::string::npos;
    std::ostringstream message;
    message << (starting ? "Could not start " : "Could not open ")
            << (input ? "input" : "output") << " \"" << deviceName << "\": "
            << Pa_GetErrorText(error) << " (PortAudio " << error << ")";
    if (failure.hostError != 0 || !failure.hostErrorText.empty()) {
        message << "; host " << audioHostErrorCode(failure.hostError);
        if (!failure.hostErrorText.empty()) message << " " << failure.hostErrorText;
    }
    failure.message = message.str();
    return failure;
}

static void setAudioRouteFailureLocked(const AudioRouteFailure& failure,
                                       float elapsedMs, bool keptPrevious) {
    gAudioRouteStatus = "error";
    gAudioRouteStage = failure.stage;
    gAudioRouteElapsedMs = elapsedMs;
    gAudioRouteKeptPrevious = keptPrevious;
    gAudioPortAudioError = failure.portAudioError;
    gAudioHostError = failure.hostError;
    gAudioHostErrorText = failure.hostErrorText;
    std::ostringstream error;
    error << failure.message << " after " << std::fixed << std::setprecision(0)
          << elapsedMs << " ms";
    if (failure.stage.find("start") != std::string::npos &&
        static_cast<uint32_t>(failure.hostError) == 0x77686174U) {
        error << ". CoreAudio could not start the hardware; reconnect it or test it in Audio MIDI Setup";
    }
    if (keptPrevious) {
        error << ". Previous route kept: " << gAudioInputName
              << " -> " << gAudioOutputName;
    }
    gAudioError = error.str();
}

static void setAudioRouteSuccessLocked(float elapsedMs) {
    gAudioRouteStatus = "ready";
    gAudioRouteStage.clear();
    gAudioRouteElapsedMs = elapsedMs;
    gAudioRouteKeptPrevious = false;
    gAudioPortAudioError = paNoError;
    gAudioHostError = 0;
    gAudioHostErrorText.clear();
    gAudioError.clear();
}
static inline void updateAtomicMax(std::atomic<float>& target, float value) {
    float current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {}
}
static inline void updateAtomicMin(std::atomic<float>& target, float value) {
    float current = target.load(std::memory_order_relaxed);
    while ((current < 0.0f || value < current) &&
           !target.compare_exchange_weak(current, value,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {}
}
static inline void updateAtomicMax(std::atomic<uint32_t>& target, uint32_t value) {
    uint32_t current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {}
}
static inline void configureAudioWorkerThread(double computationMs) {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0) return;
    const double absolutePerMillisecond =
        1.0e6 * static_cast<double>(timebase.denom) /
        static_cast<double>(timebase.numer);
    constexpr double periodMs =
        1000.0 * static_cast<double>(kPitchHopSize) / kSampleRate;
    thread_time_constraint_policy_data_t policy{};
    policy.period = static_cast<uint32_t>(periodMs * absolutePerMillisecond);
    policy.computation = static_cast<uint32_t>(
        std::min(computationMs, periodMs * 0.8) * absolutePerMillisecond);
    policy.constraint = policy.period;
    policy.preemptible = TRUE;
    thread_policy_set(pthread_mach_thread_np(pthread_self()),
                      THREAD_TIME_CONSTRAINT_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy),
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#elif defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#else
    (void)computationMs;
#endif
}
static const char* unvoicedModeKey(int mode) {
    return mode == static_cast<int>(UnvoicedMode::HoldRatio)
        ? "hold-ratio"
        : "tc-bypass";
}
static int unvoicedModeFromKey(const std::string& key) {
    return key == "hold-ratio"
        ? static_cast<int>(UnvoicedMode::HoldRatio)
        : static_cast<int>(UnvoicedMode::TcBypass);
}
static inline float foldHighPitchCandidate(float hz, float anchorMidi) {
    if (hz <= 0.0f) return -1.0f;

    // Without an anchor, trust the detector: folding toward a guessed
    // register octave-locks voices singing above it. The median filter
    // establishes a real anchor within a few frames.
    if (anchorMidi < 0.0f) return hz;

    float targetMidi = anchorMidi;
    float bestHz = hz;
    float bestDistance = std::fabs(freqToMidi(hz) - targetMidi);
    for (float candidate = hz * 0.5f;
         candidate >= noteToFreq(kMinDetectedMidi);
         candidate *= 0.5f) {
        float distance = std::fabs(freqToMidi(candidate) - targetMidi);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestHz = candidate;
        }
    }
    return bestHz;
}
// ── Pitch Detector (aubio) ─────────────────────────────────────────────────

struct PitchDetector {
    aubio_pitch_t* au     = nullptr;
    aubio_pitch_t* lowAu  = nullptr;
    fvec_t*        in     = nullptr;
    fvec_t*        out    = nullptr;
    fvec_t*        lowOut = nullptr;
    int            silenceFlushHops = 0;
    int            lowLaneWarmHops = 0;
    float          lastAcceptedHz = -1.0f;

    PitchDetector() {
        au = new_aubio_pitch("yinfft", kPitchWinSize, kPitchHopSize, kSampleRate);
        lowAu = new_aubio_pitch("yin", kPitchWinSize, kPitchHopSize, kSampleRate);
        aubio_pitch_set_unit(au, "Hz");
        aubio_pitch_set_unit(lowAu, "Hz");
        aubio_pitch_set_silence(au, -50.0f);
        aubio_pitch_set_silence(lowAu, -50.0f);
        // 0.40 measured best on the vocadito fixtures (make test-pitch):
        // looser values let yinfft return persistent sub-octave readings.
        aubio_pitch_set_tolerance(au, 0.40f);
        // The low lane is only accepted at high confidence, so keep YIN's
        // stricter threshold for clear fundamentals from A1 through B1.
        aubio_pitch_set_tolerance(lowAu, 0.15f);
        in  = new_fvec(kPitchHopSize);
        out = new_fvec(1);
        lowOut = new_fvec(1);
    }
    ~PitchDetector() {
        if (au) del_aubio_pitch(au);
        if (lowAu) del_aubio_pitch(lowAu);
        if (in) del_fvec(in);
        if (out) del_fvec(out);
        if (lowOut) del_fvec(lowOut);
    }
    float detect(const float* samples, float gateRms) {
        float rms = 0.0f;
        for (int i = 0; i < kPitchHopSize; i++) { in->data[i] = samples[i]; rms += samples[i]*samples[i]; }
        const bool energetic = std::sqrt(rms / kPitchHopSize) >= gateRms;
        if (!energetic) {
            // Flush one complete analysis window, then stop spending several
            // milliseconds per hop on an indefinitely silent input. The
            // detector remains filled with silence until the gate reopens.
            constexpr int kWindowHops = kPitchWinSize / kPitchHopSize;
            if (silenceFlushHops < kWindowHops) {
                aubio_pitch_do(au, in, out);
                silenceFlushHops++;
            }
            lowLaneWarmHops = 0;
            return -1.0f;
        }

        silenceFlushHops = 0;
        aubio_pitch_do(au, in, out);
        float f = fvec_get_sample(out, 0);

        // YINFFT's 2048-sample lane bottoms out near C2 even for a clean A1.
        // Time-domain YIN resolves that register accurately, but is more prone
        // to sub-octave errors higher up. Only hand off when both detectors are
        // near the floor and the low estimate itself is unambiguously clear.
        bool primaryNearFloor =
            f <= 0.0f || f < noteToFreq(kLowPitchProbeCeilingMidi);
        if (primaryNearFloor) {
            aubio_pitch_do(lowAu, in, lowOut);
            constexpr int kWindowHops = kPitchWinSize / kPitchHopSize;
            lowLaneWarmHops = std::min(lowLaneWarmHops + 1, kWindowHops);
            float lowF = fvec_get_sample(lowOut, 0);
            float lowConfidence = aubio_pitch_get_confidence(lowAu);
            if (lowLaneWarmHops < kWindowHops) return -1.0f;
            bool clearLowPitch = lowLaneWarmHops >= kWindowHops &&
                                 lowF >= noteToFreq(kMinDetectedMidi) &&
                                 lowF < noteToFreq(36) &&
                                 lowConfidence >= kLowPitchMinConfidence;
            if (clearLowPitch) f = lowF;
        } else {
            lowLaneWarmHops = 0;
        }

        // No upper range check here: octave-up errors are folded back into
        // range by the caller (foldHighPitchCandidate), which needs to see them.
        if (f <= 0.0f || f < noteToFreq(kMinDetectedMidi)) return -1.0f;
        lastAcceptedHz = f;
        return f;
    }
};

// ── Shared Display State (lock-free, audio/MIDI → browser) ─────────────────

struct PitchTelemetrySample {
    std::atomic<uint64_t> sequence{0};
    std::atomic<uint64_t> dspSample{0};
    std::atomic<uint64_t> rawPitchSample{0};
    std::atomic<uint64_t> earlyControlSample{0};
    std::atomic<uint64_t> qualityControlSample{0};
    std::atomic<float> rawMidi{-1.0f};
    std::atomic<float> earlyControlMidi{-1.0f};
    std::atomic<float> qualityControlMidi{-1.0f};
    std::atomic<float> earlyPathWeight{0.0f};
    std::atomic<float> pitchBend{0.0f};
    std::atomic<uint64_t> noteBitsLo{0};
    std::atomic<uint64_t> noteBitsHi{0};
};

struct SharedDisplay {
    static constexpr size_t kPitchTelemetryCapacity = 128;

    SharedDisplay() {
        for (int note = 0; note < 128; ++note) {
            glideFromMidi[note].store(static_cast<float>(note), std::memory_order_relaxed);
            glideStartSample[note].store(0, std::memory_order_relaxed);
        }
        for (size_t layer = 0; layer < harmonizer::kFreezeLayerCount; ++layer) {
            freezeHold[layer].store(false, std::memory_order_relaxed);
            freezeActive[layer].store(false, std::memory_order_relaxed);
            freezeLevel[layer].store(kDefaultFreezeLevel, std::memory_order_relaxed);
            freezeTranspose[layer].store(kDefaultFreezeTranspose, std::memory_order_relaxed);
            freezeClearGeneration[layer].store(0, std::memory_order_relaxed);
        }
    }

    std::atomic<float>    detectedMidi{-1.0f};
    std::atomic<float>    rawMidi{-1.0f};
    std::atomic<float>    correctionMidi{-1.0f};
    std::atomic<float>    predictedEarlyMidi{-1.0f};
    std::atomic<float>    predictedQualityMidi{-1.0f};
    std::atomic<float>    pitchSlope{0.0f};
    std::atomic<float>    predictorConfidence{0.0f};
    std::atomic<bool>     predictorVoiced{false};
    std::atomic<float>    parallelHandoff{0.0f};
    std::atomic<float>    pitchRms{0.0f};
    std::atomic<bool>     pitchStable{false};
    std::atomic<bool>     pitchVoiced{false};
    std::atomic<float>    wetDryBalance{kDefaultWetDryBalance};
    std::atomic<float>    monitorGainDb{kDefaultMonitorGainDb};
    std::atomic<float>    voicedGateRms{kDefaultVoicedGateRms};
    std::atomic<float>    stableSemitoneWindow{kDefaultStableSemitoneWindow};
    std::atomic<float>    parallelEarlyBlend{kDefaultParallelEarlyBlend};
    std::atomic<float>    glideAmount{kDefaultGlideAmount};
    std::atomic<float>    glideTimeMs{kDefaultGlideTimeMs};
    std::atomic<float>    chorusMix{kDefaultChorusMix};
    std::atomic<float>    reverbMix{kDefaultReverbMix};
    std::atomic<float>    freezeTone{kDefaultFreezeTone};
    std::array<std::atomic<bool>, harmonizer::kFreezeLayerCount> freezeHold{};
    std::array<std::atomic<bool>, harmonizer::kFreezeLayerCount> freezeActive{};
    std::array<std::atomic<float>, harmonizer::kFreezeLayerCount> freezeLevel;
    std::array<std::atomic<float>, harmonizer::kFreezeLayerCount> freezeTranspose;
    std::array<std::atomic<uint32_t>, harmonizer::kFreezeLayerCount> freezeClearGeneration{};
    std::array<std::atomic<float>, 128> glideFromMidi;
    std::array<std::atomic<uint64_t>, 128> glideStartSample;
    std::atomic<int> mostRecentGlideNote{-1};
    std::atomic<float>    earlyPathLatencyMs{-1.0f};
    std::atomic<float>    qualityPathLatencyMs{-1.0f};
    std::atomic<int>      unvoicedMode{static_cast<int>(kDefaultUnvoicedMode)};
    std::atomic<bool>     unvoicedActive{false};
    std::atomic<float>    sibilanceScore{0.0f};
    std::atomic<float>    inputPeak{0.0f};
    std::atomic<float>    outputPeak{0.0f};
    std::atomic<float>    bridgeFillFrames{0.0f};
    std::atomic<float>    bridgeFilteredFillFrames{0.0f};
    std::atomic<float>    bridgeMinFrames{-1.0f};
    std::atomic<float>    bridgeMaxFrames{0.0f};
    std::atomic<float>    bridgeRatePpm{0.0f};
    std::atomic<float>    inputCallbackLastMs{0.0f};
    std::atomic<float>    inputCallbackMaxMs{0.0f};
    std::atomic<float>    outputCallbackLastMs{0.0f};
    std::atomic<float>    outputCallbackMaxMs{0.0f};
    std::atomic<float>    dspBlockLastMs{0.0f};
    std::atomic<float>    dspBlockMaxMs{0.0f};
    std::atomic<float>    pitchDetectorLastMs{0.0f};
    std::atomic<float>    pitchDetectorMaxMs{0.0f};
    std::atomic<float>    dspWorkerBatchMaxMs{0.0f};
    std::atomic<uint32_t> dspQueueFrames{0};
    std::atomic<uint32_t> dspQueueMaxFrames{0};
    std::atomic<uint64_t> callbackFrames{0};
    std::atomic<uint64_t> inputOverflows{0};
    std::atomic<uint64_t> outputUnderflows{0};
    std::atomic<uint64_t> noteBitsLo{0};   // notes 0–63
    std::atomic<uint64_t> noteBitsHi{0};   // notes 64–127
    std::array<PitchTelemetrySample, kPitchTelemetryCapacity> pitchTelemetry{};
    std::atomic<uint64_t> pitchTelemetryCount{0};

    void appendPitchTelemetry(uint64_t dspSample,
                              uint64_t rawPitchSample,
                              float rawMidi,
                              uint64_t earlyControlSample,
                              float earlyControlMidi,
                              uint64_t qualityControlSample,
                              float qualityControlMidi,
                              float earlyPathWeight,
                              float bend) {
        const uint64_t sequence =
            pitchTelemetryCount.fetch_add(1, std::memory_order_relaxed) + 1;
        PitchTelemetrySample& slot =
            pitchTelemetry[(sequence - 1) % kPitchTelemetryCapacity];
        slot.dspSample.store(dspSample, std::memory_order_relaxed);
        slot.rawPitchSample.store(rawPitchSample, std::memory_order_relaxed);
        slot.earlyControlSample.store(earlyControlSample, std::memory_order_relaxed);
        slot.qualityControlSample.store(qualityControlSample, std::memory_order_relaxed);
        slot.rawMidi.store(rawMidi, std::memory_order_relaxed);
        slot.earlyControlMidi.store(earlyControlMidi, std::memory_order_relaxed);
        slot.qualityControlMidi.store(qualityControlMidi, std::memory_order_relaxed);
        slot.earlyPathWeight.store(earlyPathWeight, std::memory_order_relaxed);
        slot.pitchBend.store(bend, std::memory_order_relaxed);
        slot.noteBitsLo.store(noteBitsLo.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
        slot.noteBitsHi.store(noteBitsHi.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
        slot.sequence.store(sequence, std::memory_order_release);
    }

    void noteOn(int n) {
        if (n < 64) noteBitsLo.fetch_or(1ULL << n,      std::memory_order_relaxed);
        else        noteBitsHi.fetch_or(1ULL << (n-64), std::memory_order_relaxed);
    }
    void noteOff(int n) {
        if (n < 64) noteBitsLo.fetch_and(~(1ULL << n),      std::memory_order_relaxed);
        else        noteBitsHi.fetch_and(~(1ULL << (n-64)), std::memory_order_relaxed);
    }
    bool isNoteOn(int n) const {
        if (n < 64) return (noteBitsLo.load(std::memory_order_relaxed) >> n) & 1;
        return (noteBitsHi.load(std::memory_order_relaxed) >> (n-64)) & 1;
    }

    void beginGlide(int note, uint64_t sample) {
        if (note < 0 || note > 127) return;
        const int previous = mostRecentGlideNote.exchange(note, std::memory_order_relaxed);
        glideFromMidi[note].store(
            previous >= 0 ? static_cast<float>(previous) : static_cast<float>(note),
            std::memory_order_relaxed);
        glideStartSample[note].store(sample, std::memory_order_relaxed);
    }

    float glideTargetMidi(int note, uint64_t sample) const {
        if (note < 0 || note > 127) return static_cast<float>(note);
        const float amount = clampf(glideAmount.load(std::memory_order_relaxed), 0.0f, 1.0f);
        if (amount <= 0.0001f) return static_cast<float>(note);
        const float durationMs = clampf(
            glideTimeMs.load(std::memory_order_relaxed), 10.0f, 2000.0f);
        const uint64_t durationSamples = static_cast<uint64_t>(
            std::max(1.0f, durationMs * 0.001f * kSampleRate));
        const uint64_t start = glideStartSample[note].load(std::memory_order_relaxed);
        if (sample >= start + durationSamples) return static_cast<float>(note);
        const float linear = clampf(
            static_cast<float>(sample > start ? sample - start : 0) /
                static_cast<float>(durationSamples),
            0.0f, 1.0f);
        const float progress = linear * linear * (3.0f - 2.0f * linear);
        const float from = glideFromMidi[note].load(std::memory_order_relaxed);
        const float glideStart = static_cast<float>(note) +
            (from - static_cast<float>(note)) * amount;
        return glideStart + (static_cast<float>(note) - glideStart) * progress;
    }
};

// ── Diagnostic Capture ─────────────────────────────────────────────────────
// Records the raw mic, the processed output, per-frame pitch state, and
// timestamped MIDI events into captures/cap_<stamp>/ so a bad-sounding take
// can be replayed and diagnosed offline (pitch_analyzer reads mic.wav
// directly). Toggled from the GUI via /api/capture.

struct Capture {
    static constexpr int kMaxSeconds = 120;

    std::atomic<bool>   active{false};
    std::mutex          controlMutex;   // serializes start/stop (HTTP threads)
    uint64_t            startClock = 0;

    std::vector<float>  mic;    // mono input
    std::vector<float>  outL;   // processed output
    std::vector<float>  outR;
    std::atomic<size_t> audioLen{0};

    struct FrameRow {
        uint64_t sample;
        float rms, rawHz, foldedHz, medianHz, smoothedMidi, correctionMidi;
        float predictedEarlyMidi, predictedQualityMidi, pitchSlope;
        float predictorConfidence, parallelHandoff;
        uint8_t stable, voiced, predictorVoiced;
    };
    std::vector<FrameRow> frames;
    std::atomic<size_t>   frameLen{0};

    struct MidiRow { uint64_t sample; uint8_t on; uint8_t note; };
    std::vector<MidiRow> midi;
    std::atomic<size_t>  midiLen{0};

    struct EffectRow { uint64_t sample; uint8_t slot; uint8_t held; uint8_t clear; };
    std::vector<EffectRow> effects;
    std::atomic<size_t> effectLen{0};
    std::mutex effectMutex;

    // Audio thread only. The release store on the length publishes the
    // buffer writes to the HTTP thread that serializes after stop.
    void appendAudio(float in, float l, float r) {
        size_t n = audioLen.load(std::memory_order_relaxed);
        if (n >= mic.size()) return;
        mic[n] = in; outL[n] = l; outR[n] = r;
        audioLen.store(n + 1, std::memory_order_release);
    }
    void appendFrame(const FrameRow& row) {
        size_t n = frameLen.load(std::memory_order_relaxed);
        if (n >= frames.size()) return;
        frames[n] = row;
        frameLen.store(n + 1, std::memory_order_release);
    }
    // MIDI handler threads, serialized by AudioEngine::midiMutex.
    void appendMidi(uint64_t sample, bool on, int note) {
        size_t n = midiLen.load(std::memory_order_relaxed);
        if (n >= midi.size()) return;
        midi[n] = { sample, (uint8_t)(on ? 1 : 0), (uint8_t)note };
        midiLen.store(n + 1, std::memory_order_release);
    }
    void appendEffect(uint64_t sample, int slot, bool held, bool clear) {
        std::lock_guard<std::mutex> lock(effectMutex);
        size_t n = effectLen.load(std::memory_order_relaxed);
        if (n >= effects.size()) return;
        effects[n] = { sample, (uint8_t)slot, (uint8_t)(held ? 1 : 0),
                       (uint8_t)(clear ? 1 : 0) };
        effectLen.store(n + 1, std::memory_order_release);
    }
};

// The server and capture plumbing stay in this translation unit; the native
// DSP backend is isolated here so the browser remains only a GUI. Backend-lab
// wrappers override these macros without changing the production build.
#ifndef HARMONIZER_ENGINE_HEADER
#define HARMONIZER_ENGINE_HEADER "harmonizer_rubberband_engine.hpp"
#endif
#ifndef HARMONIZER_DSP_BACKEND
#define HARMONIZER_DSP_BACKEND "rubberband-formant-preserved"
#endif
#ifndef HARMONIZER_DSP_TITLE
#define HARMONIZER_DSP_TITLE "Rubber Band LiveShifter, formant preserved"
#endif
#ifndef HARMONIZER_BACKEND_KEY
#define HARMONIZER_BACKEND_KEY "live512"
#endif
#include HARMONIZER_ENGINE_HEADER

static harmonizer::CollierEffects gCollierEffects(static_cast<float>(kSampleRate));

static inline void processCollierEffects(SharedDisplay& display,
                                         float inputLeft, float inputRight,
                                         float& outputLeft, float& outputRight) {
    harmonizer::CollierEffectSettings settings;
    for (size_t layer = 0; layer < harmonizer::kFreezeLayerCount; ++layer) {
        settings.freezeHold[layer] =
            display.freezeHold[layer].load(std::memory_order_relaxed);
        settings.freezeLevel[layer] =
            display.freezeLevel[layer].load(std::memory_order_relaxed);
        settings.freezeTranspose[layer] =
            display.freezeTranspose[layer].load(std::memory_order_relaxed);
        settings.freezeClearGeneration[layer] =
            display.freezeClearGeneration[layer].load(std::memory_order_relaxed);
    }
    settings.freezeTone = display.freezeTone.load(std::memory_order_relaxed);
    settings.chorusMix = display.chorusMix.load(std::memory_order_relaxed);
    settings.reverbMix = display.reverbMix.load(std::memory_order_relaxed);
    const harmonizer::CollierEffectState state =
        gCollierEffects.process(inputLeft, inputRight, settings, outputLeft, outputRight);
    for (size_t layer = 0; layer < harmonizer::kFreezeLayerCount; ++layer) {
        display.freezeActive[layer].store(state.freezeActive[layer],
                                          std::memory_order_relaxed);
    }
}

struct BackendSpec {
    const char* key;
    const char* name;
    const char* executable;
};

static constexpr BackendSpec kBackendSpecs[] = {
    {"live512", "Rubber Band Live 512", "harmonizer_web"},
    {"parallel", "Parallel R2 + Live 512", "harmonizer_web_parallel"},
    {"live128", "Rubber Band Live 128", "harmonizer_web_rubberband_live128"},
    {"r2", "Rubber Band R2 Short", "harmonizer_web_rubberband_r2"},
    {"signalsmith", "Signalsmith Low Latency", "harmonizer_web_signalsmith"},
};

static std::filesystem::path gExecutablePath;
static std::mutex gBackendMutex;
static std::string gRequestedBackend;
static std::string gServerInstance;

// ── Per-sample engine step (shared by live callback and --render) ──────────

static inline void processSample(AudioEngine* eng, float s, float& outL, float& outR) {
    {
        // Feed pitch detector
        eng->pitchBuf[eng->pitchPos++] = s;
        if (eng->pitchPos >= kPitchHopSize) {
            float sumSq = 0.0f;
            for (int j = 0; j < kPitchHopSize; j++) sumSq += eng->pitchBuf[j] * eng->pitchBuf[j];
            eng->recentPitchRms = std::sqrt(sumSq / kPitchHopSize);

            float gateRms = eng->display.voicedGateRms.load(std::memory_order_relaxed);
            float detectorGateRms = eng->pitchVoiced ? gateRms * AudioEngine::kPitchGateReleaseRatio : gateRms;
            const auto detectorStart = std::chrono::steady_clock::now();
            float rawFreq = eng->detector.detect(eng->pitchBuf, detectorGateRms);
            const float detectorMs = elapsedMilliseconds(detectorStart);
            eng->display.pitchDetectorLastMs.store(detectorMs, std::memory_order_relaxed);
            updateAtomicMax(eng->display.pitchDetectorMaxMs, detectorMs);
            float freq = foldHighPitchCandidate(rawFreq, eng->smoothedMidi);
            if (freq > 0.0f &&
                (freq < noteToFreq(kMinDetectedMidi) ||
                 freq > noteToFreq(kMaxDetectedMidi))) {
                freq = -1.0f;
            }
            float midi = freq > 0.0f ? freqToMidi(freq) : -1.0f;
            eng->display.rawMidi.store(midi, std::memory_order_relaxed);

            // Median filter suppresses speckles before the slower contour smoother.
            eng->pitchHist[eng->pitchHistIdx] = freq;
            eng->pitchHistIdx = (eng->pitchHistIdx + 1) % AudioEngine::kPitchHistLen;

            // Copy history, sort, take median (ignore -1 invalids)
            float sorted[AudioEngine::kPitchHistLen];
            int   validN = 0;
            for (int k = 0; k < AudioEngine::kPitchHistLen; k++)
                if (eng->pitchHist[k] > 0.0f) sorted[validN++] = eng->pitchHist[k];

            float medianFreq = -1.0f;
            if (validN > 0) {
                // insertion sort on the small array
                for (int a = 1; a < validN; a++) {
                    float v = sorted[a]; int b = a - 1;
                    while (b >= 0 && sorted[b] > v) { sorted[b+1] = sorted[b]; b--; }
                    sorted[b+1] = v;
                }
                medianFreq = sorted[validN / 2];
            }
            float medianMidi = freqToMidi(medianFreq);
            float stableWindow = eng->display.stableSemitoneWindow.load(std::memory_order_relaxed);

#ifndef HARMONIZER_CUSTOM_PITCH_CONTROL
            // The long history above is intentionally calm enough to draw and
            // gate. The inverse shift instead uses the raw detector estimate
            // from the previous 512-sample hop. That 11.6 ms alignment matches
            // LiveShifter's buffered analysis without passing slow vibrato.
            if (freq > 0.0f &&
                eng->recentPitchRms >= gateRms * AudioEngine::kPitchGateReleaseRatio) {
                eng->fastCorrectionMidi = freqToMidi(freq);
                eng->correctionHoldFrames = AudioEngine::kPitchReleaseFrames;
            } else if (eng->correctionHoldFrames > 0) {
                eng->correctionHoldFrames--;
            } else {
                eng->fastCorrectionMidi = -1.0f;
            }

            eng->correctionControlHistory[eng->correctionControlHistoryIdx] =
                eng->fastCorrectionMidi;
            eng->correctionControlHistoryIdx =
                (eng->correctionControlHistoryIdx + 1) %
                AudioEngine::kCorrectionControlHistoryLen;
            constexpr int kControlLag =
                AudioEngine::kCorrectionLagHops < AudioEngine::kCorrectionControlHistoryLen
                ? AudioEngine::kCorrectionLagHops
                : AudioEngine::kCorrectionControlHistoryLen - 1;
            int delayedIndex =
                (eng->correctionControlHistoryIdx - 1 - kControlLag +
                 AudioEngine::kCorrectionControlHistoryLen) %
                AudioEngine::kCorrectionControlHistoryLen;
            float delayedCorrection = eng->correctionControlHistory[delayedIndex];
            eng->correctionMidi = delayedCorrection > 0.0f
                ? delayedCorrection
                : eng->fastCorrectionMidi;
            eng->display.correctionMidi.store(eng->correctionMidi, std::memory_order_relaxed);
#endif

            // Stable = a majority of recent valid readings agree with the
            // median. Judging the history against itself (instead of requiring
            // the current frame to be valid and near the median) lets single
            // dropped or glitched frames pass without breaking the contour.
            int agreeN = 0;
            if (medianMidi > 0.0f) {
                for (int k = 0; k < validN; k++)
                    if (std::fabs(freqToMidi(sorted[k]) - medianMidi) < stableWindow) agreeN++;
            }
            // The RMS check keeps stale history from holding "stable" into
            // silence after a phrase ends.
            bool stable = (validN >= AudioEngine::kMinPitchValidFrames &&
                           medianMidi > 0.0f &&
                           2 * agreeN > validN &&
                           eng->recentPitchRms >= gateRms * AudioEngine::kPitchGateReleaseRatio);
            bool holdVoiced = eng->pitchVoiced &&
                              eng->smoothedMidi > 0.0f &&
                              eng->pitchHoldFrames > 0 &&
                              eng->recentPitchRms >= gateRms * AudioEngine::kPitchGateReleaseRatio;

            if (stable) {
                if (eng->smoothedMidi < 0.0f ||
                    std::fabs(medianMidi - eng->smoothedMidi) > AudioEngine::kPitchSnapSemitones) {
                    // Note change (or fresh latch): the median already agrees
                    // on the new pitch, so snap rather than glide through the gap.
                    eng->smoothedMidi = medianMidi;
                } else {
                    float delta = clampf(medianMidi - eng->smoothedMidi,
                                         -AudioEngine::kPitchMaxStepSemitones,
                                         AudioEngine::kPitchMaxStepSemitones);
                    float targetMidi = eng->smoothedMidi + delta;
                    eng->smoothedMidi += (targetMidi - eng->smoothedMidi) *
                                         AudioEngine::kPitchSmoothingAlpha;
                }
                eng->detectedMidi = eng->smoothedMidi;
                eng->detectedF0   = noteToFreq(eng->smoothedMidi);
                eng->pitchStable  = true;
                eng->pitchVoiced  = true;
                eng->pitchHoldFrames = AudioEngine::kPitchReleaseFrames;
            } else if (holdVoiced) {
                eng->detectedMidi = eng->smoothedMidi;
                eng->detectedF0   = noteToFreq(eng->smoothedMidi);
                eng->pitchStable  = false;
                eng->pitchVoiced  = true;
                eng->pitchHoldFrames--;
            } else {
                eng->detectedMidi = -1.0f;
                eng->detectedF0  = -1.0f;
                eng->pitchStable = false;
                eng->pitchVoiced = false;
                eng->pitchHoldFrames = 0;
                eng->smoothedMidi = -1.0f;
            }
            eng->prevMidi = eng->detectedMidi;
            eng->display.detectedMidi.store(eng->detectedMidi, std::memory_order_relaxed);
            eng->display.pitchRms.store(eng->recentPitchRms, std::memory_order_relaxed);
            eng->display.pitchStable.store(eng->pitchStable, std::memory_order_relaxed);
            eng->display.pitchVoiced.store(eng->pitchVoiced, std::memory_order_relaxed);

#ifdef HARMONIZER_CUSTOM_PITCH_CONTROL
            eng->updatePitchControl(
                freq,
                stable,
                gateRms,
                eng->sampleClock.load(std::memory_order_relaxed) + 1);
#endif

            if (eng->capture.active.load(std::memory_order_acquire)) {
                Capture::FrameRow row;
                row.sample       = eng->sampleClock.load(std::memory_order_relaxed);
                row.rms          = eng->recentPitchRms;
                row.rawHz        = rawFreq;
                row.foldedHz     = freq;
                row.medianHz     = medianFreq;
                row.smoothedMidi = eng->detectedMidi;
                row.correctionMidi = eng->correctionMidi;
                row.predictedEarlyMidi = eng->display.predictedEarlyMidi.load(std::memory_order_relaxed);
                row.predictedQualityMidi = eng->display.predictedQualityMidi.load(std::memory_order_relaxed);
                row.pitchSlope = eng->display.pitchSlope.load(std::memory_order_relaxed);
                row.predictorConfidence = eng->display.predictorConfidence.load(std::memory_order_relaxed);
                row.parallelHandoff = eng->display.parallelHandoff.load(std::memory_order_relaxed);
                row.stable       = eng->pitchStable ? 1 : 0;
                row.voiced       = eng->pitchVoiced ? 1 : 0;
                row.predictorVoiced = eng->display.predictorVoiced.load(std::memory_order_relaxed) ? 1 : 0;
                eng->capture.appendFrame(row);
            }
            eng->pitchPos = 0;
        }

        // Accumulate the next native shifter block while returning the
        // preceding processed block to PortAudio.
        if (eng->bufPos == 0) {
            float gainDb = eng->display.monitorGainDb.load(std::memory_order_relaxed);
            eng->monitorGainLinear = std::pow(10.0f, gainDb / 20.0f);
        }
        eng->inputBuf[eng->bufPos] = clampf(s * eng->monitorGainLinear, -1.0f, 1.0f);
        outL = eng->outputL[eng->bufPos];
        outR = eng->outputR[eng->bufPos];
        processCollierEffects(eng->display, outL, outR, outL, outR);

        uint64_t clock = eng->sampleClock.load(std::memory_order_relaxed);
        uint64_t toneStart = eng->testToneStartClock.load(std::memory_order_relaxed);
        uint64_t toneEnd = eng->testToneEndClock.load(std::memory_order_relaxed);
        if (clock >= toneStart && clock < toneEnd) {
            constexpr float kTonePeak = 0.10f;
            constexpr uint64_t kToneFadeSamples = kSampleRate / 100;
            uint64_t elapsed = clock - toneStart;
            uint64_t remaining = toneEnd - clock;
            float fade = std::min(1.0f,
                std::min((float)elapsed / kToneFadeSamples,
                         (float)remaining / kToneFadeSamples));
            float tone = std::sin(eng->testTonePhase) * kTonePeak * fade;
            outL = clampf(outL + tone, -1.0f, 1.0f);
            outR = clampf(outR + tone, -1.0f, 1.0f);
            eng->testTonePhase += 2.0f * kPi * 440.0f / kSampleRate;
            if (eng->testTonePhase >= 2.0f * kPi)
                eng->testTonePhase -= 2.0f * kPi;
        }

        eng->sampleClock.fetch_add(1, std::memory_order_relaxed);
        if (eng->capture.active.load(std::memory_order_acquire))
            eng->capture.appendAudio(s, outL, outR);

        if (++eng->bufPos >= eng->blockSize) {
            const auto blockStart = std::chrono::steady_clock::now();
            eng->processBlock();
            const float blockMs = elapsedMilliseconds(blockStart);
            eng->display.dspBlockLastMs.store(blockMs, std::memory_order_relaxed);
            updateAtomicMax(eng->display.dspBlockMaxMs, blockMs);
            eng->bufPos = 0;
        }
    }
}

// ── PortAudio Callbacks ────────────────────────────────────────────────────
// Input and output use separate native streams so a USB mic and the Mac's
// headphone device do not have to coexist inside one fragile AUHAL unit.

class DspWorkerRuntime {
    static constexpr size_t kInputRingFrames = 8192;
    static constexpr size_t kInputRingMask = kInputRingFrames - 1;
    static constexpr size_t kWorkerBatchFrames = 64;
    static_assert((kInputRingFrames & kInputRingMask) == 0,
                  "DSP input ring size must be a power of two");

    std::array<float, kInputRingFrames> inputRing_{};
    std::atomic<uint64_t> inputWrite_{0};
    std::atomic<uint64_t> inputRead_{0};
    std::atomic<bool> running_{false};
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    std::thread thread_;
    AudioEngine* engine_ = nullptr;

    bool processBatch() {
        uint64_t read = inputRead_.load(std::memory_order_relaxed);
        const uint64_t write = inputWrite_.load(std::memory_order_acquire);
        if (read >= write || !engine_) return false;

        const auto batchStart = std::chrono::steady_clock::now();
        const size_t count = static_cast<size_t>(
            std::min<uint64_t>(write - read, kWorkerBatchFrames));
        uint64_t bridgeWrite =
            engine_->outputBridgeWrite.load(std::memory_order_relaxed);
        uint64_t bridgeRead =
            engine_->outputBridgeRead.load(std::memory_order_acquire);
        bool bridgeOverflow = false;

        for (size_t sample = 0; sample < count; ++sample) {
            float outL = 0.0f;
            float outR = 0.0f;
            processSample(engine_, inputRing_[(size_t)read & kInputRingMask],
                          outL, outR);
            read++;
            if (bridgeWrite - bridgeRead < AudioEngine::kOutputBridgeFrames) {
                const size_t slot =
                    (size_t)bridgeWrite & AudioEngine::kOutputBridgeMask;
                engine_->outputBridgeL[slot] = outL;
                engine_->outputBridgeR[slot] = outR;
                bridgeWrite++;
            } else {
                bridgeOverflow = true;
            }
        }

        engine_->outputBridgeWrite.store(bridgeWrite, std::memory_order_release);
        inputRead_.store(read, std::memory_order_release);
        if (bridgeOverflow) {
            engine_->display.inputOverflows.fetch_add(1, std::memory_order_relaxed);
        }

        const uint32_t queued = static_cast<uint32_t>(
            inputWrite_.load(std::memory_order_acquire) - read);
        engine_->display.dspQueueFrames.store(queued, std::memory_order_relaxed);
        updateAtomicMax(engine_->display.dspQueueMaxFrames, queued);
        updateAtomicMax(engine_->display.dspWorkerBatchMaxMs,
                        elapsedMilliseconds(batchStart));
        return true;
    }

    void run() {
        configureAudioWorkerThread(5.0);
        while (running_.load(std::memory_order_acquire)) {
            if (processBatch()) continue;
            std::unique_lock<std::mutex> lock(wakeMutex_);
            wake_.wait(lock, [this] {
                return !running_.load(std::memory_order_acquire) ||
                       inputRead_.load(std::memory_order_relaxed) <
                           inputWrite_.load(std::memory_order_acquire);
            });
        }
    }

public:
    ~DspWorkerRuntime() { stop(); }

    void start(AudioEngine* engine) {
        stop();
        engine_ = engine;
        inputRead_.store(0, std::memory_order_relaxed);
        inputWrite_.store(0, std::memory_order_relaxed);
        engine_->display.dspQueueFrames.store(0, std::memory_order_relaxed);
        engine_->display.dspQueueMaxFrames.store(0, std::memory_order_relaxed);
        engine_->display.dspWorkerBatchMaxMs.store(0.0f, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        wake_.notify_one();
        if (thread_.joinable()) thread_.join();
        engine_ = nullptr;
    }

    bool push(const float* samples, size_t count) {
        if (!running_.load(std::memory_order_acquire)) return true;
        uint64_t write = inputWrite_.load(std::memory_order_relaxed);
        const uint64_t read = inputRead_.load(std::memory_order_acquire);
        bool overflow = false;
        for (size_t sample = 0; sample < count; ++sample) {
            if (write - read >= kInputRingFrames) {
                overflow = true;
                continue;
            }
            inputRing_[(size_t)write & kInputRingMask] = samples[sample];
            write++;
        }
        inputWrite_.store(write, std::memory_order_release);
        if (engine_) {
            const uint32_t queued = static_cast<uint32_t>(write - read);
            engine_->display.dspQueueFrames.store(queued, std::memory_order_relaxed);
            updateAtomicMax(engine_->display.dspQueueMaxFrames, queued);
        }
        wake_.notify_one();
        return overflow;
    }
};

static DspWorkerRuntime gDspWorker;

static inline float bridgeInterpolatedSample(const std::vector<float>& channel,
                                             size_t mask,
                                             double position) {
    const uint64_t base = static_cast<uint64_t>(position);
    const float fraction = static_cast<float>(position - static_cast<double>(base));
    return interpolateBridgeSample(
        channel[(size_t)base & mask],
        channel[(size_t)(base + 1) & mask],
        channel[(size_t)(base + 2) & mask],
        channel[(size_t)(base + 3) & mask],
        fraction);
}

static int audioInputCallback(const void* input, void*, unsigned long frames,
                              const PaStreamCallbackTimeInfo*,
                              PaStreamCallbackFlags statusFlags, void* ud)
{
    const auto callbackStart = std::chrono::steady_clock::now();
    auto* context = static_cast<AudioCallbackContext*>(ud);
    if (!context || !context->enabled.load(std::memory_order_acquire))
        return paContinue;
    auto* eng = context->engine;
    const float* mic = static_cast<const float*>(input);
    if (statusFlags & paInputOverflow)
        eng->display.inputOverflows.fetch_add(1, std::memory_order_relaxed);
    if (!mic) {
        const float callbackMs = elapsedMilliseconds(callbackStart);
        eng->display.inputCallbackLastMs.store(callbackMs, std::memory_order_relaxed);
        updateAtomicMax(eng->display.inputCallbackMaxMs, callbackMs);
        return paContinue;
    }

    float inputPeak = 0.0f;
    for (unsigned long i = 0; i < frames; i++) {
        inputPeak = std::max(inputPeak, std::fabs(mic[i]));
    }
    if (gDspWorker.push(mic, frames)) {
        eng->display.inputOverflows.fetch_add(1, std::memory_order_relaxed);
    }

    float previousInput = eng->display.inputPeak.load(std::memory_order_relaxed);
    eng->display.inputPeak.store(std::max(inputPeak, previousInput * 0.995f),
                                 std::memory_order_relaxed);
    const float callbackMs = elapsedMilliseconds(callbackStart);
    eng->display.inputCallbackLastMs.store(callbackMs, std::memory_order_relaxed);
    updateAtomicMax(eng->display.inputCallbackMaxMs, callbackMs);
    return paContinue;
}

static int audioOutputCallback(const void*, void* output, unsigned long frames,
                               const PaStreamCallbackTimeInfo*,
                               PaStreamCallbackFlags statusFlags, void* ud)
{
    const auto callbackStart = std::chrono::steady_clock::now();
    float* stereo = static_cast<float*>(output);
    auto* context = static_cast<AudioCallbackContext*>(ud);
    if (!context || !context->enabled.load(std::memory_order_acquire)) {
        if (stereo) std::fill(stereo, stereo + 2 * frames, 0.0f);
        return paContinue;
    }
    auto* eng = context->engine;
    if (gOutputBridgeRebasePending.exchange(false, std::memory_order_acq_rel)) {
        const uint64_t currentWrite =
            eng->outputBridgeWrite.load(std::memory_order_acquire);
        const uint64_t retained = std::min<uint64_t>(
            currentWrite,
            static_cast<uint64_t>(std::llround(kOutputBridgeTargetFrames)));
        const uint64_t currentRead = currentWrite - retained;
        eng->outputBridgeRead.store(currentRead, std::memory_order_release);
        eng->outputBridgePrimed.store(false, std::memory_order_relaxed);
        gOutputBridgeRuntime.reset(currentRead);
        eng->display.bridgeFillFrames.store(
            static_cast<float>(retained), std::memory_order_relaxed);
        eng->display.bridgeFilteredFillFrames.store(
            static_cast<float>(retained), std::memory_order_relaxed);
        eng->display.bridgeMinFrames.store(-1.0f, std::memory_order_relaxed);
        eng->display.bridgeMaxFrames.store(
            static_cast<float>(retained), std::memory_order_relaxed);
        eng->display.bridgeRatePpm.store(0.0f, std::memory_order_relaxed);
    }
    bool streamUnderflow = (statusFlags & paOutputUnderflow) != 0;
    uint64_t read = eng->outputBridgeRead.load(std::memory_order_relaxed);
    uint64_t write = eng->outputBridgeWrite.load(std::memory_order_acquire);
    bool primed = eng->outputBridgePrimed.load(std::memory_order_relaxed);
    constexpr uint64_t kInterpolationLookahead = 4;
    constexpr float kBridgeFadeStep =
        1.0f / std::max(1.0f, 0.003f * static_cast<float>(kSampleRate));
    if (!primed && write - read >=
        AudioEngine::kOutputBridgePrimeFrames + kInterpolationLookahead) {
        primed = true;
        gOutputBridgeRuntime.readPosition = static_cast<double>(read);
        gOutputBridgeRuntime.positionReady = true;
        gOutputBridgeRuntime.fadeGain = 0.0f;
        gOutputBridgeRuntime.clock.reset(static_cast<double>(write - read));
    }

    if (!gOutputBridgeRuntime.positionReady) {
        gOutputBridgeRuntime.readPosition = static_cast<double>(read);
    }
    double readPosition = gOutputBridgeRuntime.readPosition;
    double sourceStep = 1.0;
    if (primed) {
        sourceStep = gOutputBridgeRuntime.clock.update(
            std::max(0.0, static_cast<double>(write) - readPosition), frames);
    }

    float outputPeak = 0.0f;
    for (unsigned long i = 0; i < frames; i++) {
        float outL = 0.0f;
        float outR = 0.0f;
        const uint64_t base = static_cast<uint64_t>(readPosition);
        if (primed && base + kInterpolationLookahead <= write) {
            outL = bridgeInterpolatedSample(
                eng->outputBridgeL, AudioEngine::kOutputBridgeMask, readPosition);
            outR = bridgeInterpolatedSample(
                eng->outputBridgeR, AudioEngine::kOutputBridgeMask, readPosition);
            readPosition += sourceStep;
            gOutputBridgeRuntime.fadeGain = std::min(
                1.0f, gOutputBridgeRuntime.fadeGain + kBridgeFadeStep);
            outL = clampf(outL * gOutputBridgeRuntime.fadeGain, -1.0f, 1.0f);
            outR = clampf(outR * gOutputBridgeRuntime.fadeGain, -1.0f, 1.0f);
            gOutputBridgeRuntime.lastLeft = outL;
            gOutputBridgeRuntime.lastRight = outR;
        } else if (primed) {
            primed = false;
            streamUnderflow = true;
            gOutputBridgeRuntime.clock.reset();
        }

        if (!primed && gOutputBridgeRuntime.fadeGain > 0.0f) {
            gOutputBridgeRuntime.fadeGain = std::max(
                0.0f, gOutputBridgeRuntime.fadeGain - kBridgeFadeStep);
            outL = gOutputBridgeRuntime.lastLeft * gOutputBridgeRuntime.fadeGain;
            outR = gOutputBridgeRuntime.lastRight * gOutputBridgeRuntime.fadeGain;
        }
        stereo[2*i] = outL;
        stereo[2*i + 1] = outR;
        outputPeak = std::max(outputPeak, std::max(std::fabs(outL), std::fabs(outR)));
    }
    gOutputBridgeRuntime.readPosition = readPosition;
    read = static_cast<uint64_t>(readPosition);
    eng->outputBridgeRead.store(read, std::memory_order_release);
    eng->outputBridgePrimed.store(primed, std::memory_order_relaxed);
    if (streamUnderflow)
        eng->display.outputUnderflows.fetch_add(1, std::memory_order_relaxed);

    const float bridgeFill = static_cast<float>(
        std::max(0.0, static_cast<double>(write) - readPosition));
    eng->display.bridgeFillFrames.store(bridgeFill, std::memory_order_relaxed);
    eng->display.bridgeFilteredFillFrames.store(
        primed ? static_cast<float>(gOutputBridgeRuntime.clock.filteredFillFrames())
               : bridgeFill,
        std::memory_order_relaxed);
    if (primed) {
        updateAtomicMin(eng->display.bridgeMinFrames, bridgeFill);
        updateAtomicMax(eng->display.bridgeMaxFrames, bridgeFill);
    }
    eng->display.bridgeRatePpm.store(
        primed ? static_cast<float>(gOutputBridgeRuntime.clock.correctionPpm()) : 0.0f,
        std::memory_order_relaxed);

    // A short peak hold keeps the 1.45 ms callback blocks visible to the
    // 20 Hz browser state stream without adding locks to either audio thread.
    float previousOutput = eng->display.outputPeak.load(std::memory_order_relaxed);
    eng->display.outputPeak.store(std::max(outputPeak, previousOutput * 0.995f),
                                  std::memory_order_relaxed);
    eng->display.callbackFrames.fetch_add(frames, std::memory_order_relaxed);
    const float callbackMs = elapsedMilliseconds(callbackStart);
    eng->display.outputCallbackLastMs.store(callbackMs, std::memory_order_relaxed);
    updateAtomicMax(eng->display.outputCallbackMaxMs, callbackMs);
    return paContinue;
}

static void closeAudioStreamLocked() {
    gAudioReady.store(false, std::memory_order_relaxed);
    if (gAudioInputContext)
        gAudioInputContext->enabled.store(false, std::memory_order_release);
    if (gAudioOutputContext)
        gAudioOutputContext->enabled.store(false, std::memory_order_release);
    if (gAudioInputStream && Pa_IsStreamActive(gAudioInputStream) == 1)
        Pa_StopStream(gAudioInputStream);
    gDspWorker.stop();
    if (gAudioOutputStream && Pa_IsStreamActive(gAudioOutputStream) == 1)
        Pa_StopStream(gAudioOutputStream);
    if (gAudioInputStream) Pa_CloseStream(gAudioInputStream);
    if (gAudioOutputStream) Pa_CloseStream(gAudioOutputStream);
    gAudioInputStream = nullptr;
    gAudioOutputStream = nullptr;
    gAudioInputContext.reset();
    gAudioOutputContext.reset();
    gAudioInputLatencyMs.store(0.0f, std::memory_order_relaxed);
    gAudioOutputLatencyMs.store(0.0f, std::memory_order_relaxed);
}

static void enumerateAudioOutputsLocked() {
    gAudioOutputDevices.clear();
    int count = Pa_GetDeviceCount();
    if (count < 0) return;
    for (PaDeviceIndex index = 0; index < count; index++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
        if (!info || info->maxOutputChannels < 2) continue;
        gAudioOutputDevices.push_back({index, info->name ? info->name : "unknown"});
    }
}

static void enumerateAudioInputsLocked() {
    gAudioInputDevices.clear();
    int count = Pa_GetDeviceCount();
    if (count < 0) return;
    for (PaDeviceIndex index = 0; index < count; index++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
        if (!info || info->maxInputChannels < 1) continue;
        gAudioInputDevices.push_back({index, info->name ? info->name : "unknown"});
    }
}

static PaDeviceIndex preferredAudioInputLocked(PaDeviceIndex fallback) {
    std::ifstream input(kAudioInputPreferencePath);
    std::string preferredName;
    std::getline(input, preferredName);
    if (!preferredName.empty() && preferredName.back() == '\r') preferredName.pop_back();
    for (const auto& device : gAudioInputDevices) {
        if (device.name == preferredName) return device.index;
    }
    return fallback;
}

static PaDeviceIndex preferredAudioOutputLocked(PaDeviceIndex fallback) {
    std::ifstream input(kAudioOutputPreferencePath);
    std::string preferredName;
    std::getline(input, preferredName);
    if (!preferredName.empty() && preferredName.back() == '\r') preferredName.pop_back();
    for (const auto& device : gAudioOutputDevices) {
        if (device.name == preferredName) return device.index;
    }
    return fallback;
}

static void persistAudioOutputLocked(PaDeviceIndex outputDevice) {
    for (const auto& device : gAudioOutputDevices) {
        if (device.index != outputDevice) continue;
        std::ofstream output(kAudioOutputPreferencePath, std::ios::trunc);
        if (output) output << device.name << '\n';
        return;
    }
}

static void persistAudioInputLocked(PaDeviceIndex inputDevice) {
    for (const auto& device : gAudioInputDevices) {
        if (device.index != inputDevice) continue;
        std::ofstream output(kAudioInputPreferencePath, std::ios::trunc);
        if (output) output << device.name << '\n';
        return;
    }
}

struct AudioStreamParameters {
    PaStreamParameters value{};
#if defined(__APPLE__)
    PaMacCoreStreamInfo mac{};
#endif
};

static bool prepareAudioStreamParametersLocked(PaDeviceIndex device, bool input,
                                               AudioStreamParameters& prepared) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    if (!info) return false;
    if (input && info->maxInputChannels < 1) return false;
    if (!input && info->maxOutputChannels < 2) return false;
    prepared.value.device = device;
    prepared.value.channelCount = input ? 1 : 2;
    prepared.value.sampleFormat = paFloat32;
    prepared.value.suggestedLatency = input
        ? info->defaultLowInputLatency : info->defaultLowOutputLatency;
#if defined(__APPLE__)
    PaMacCore_SetupStreamInfo(&prepared.mac, paMacCorePlayNice);
    prepared.value.hostApiSpecificStreamInfo = &prepared.mac;
#endif
    return true;
}

static bool audioDeviceAvailableLocked(PaDeviceIndex device, bool input) {
    const auto& devices = input ? gAudioInputDevices : gAudioOutputDevices;
    return std::any_of(devices.begin(), devices.end(), [device](const AudioDevice& item) {
        return item.index == device;
    });
}

static AudioRouteFailure audioSelectionFailure(const std::string& stage,
                                                const std::string& message) {
    AudioRouteFailure failure;
    failure.stage = stage;
    failure.message = message;
    failure.portAudioError = paInvalidDevice;
    return failure;
}

static void resetAudioPipelineLocked(AudioEngine* engine) {
    gOutputBridgeRebasePending.store(false, std::memory_order_relaxed);
    engine->outputBridgeRead.store(0, std::memory_order_relaxed);
    engine->outputBridgeWrite.store(0, std::memory_order_relaxed);
    engine->outputBridgePrimed.store(false, std::memory_order_relaxed);
    gOutputBridgeRuntime.reset();
    engine->display.bridgeFillFrames.store(0.0f, std::memory_order_relaxed);
    engine->display.bridgeFilteredFillFrames.store(0.0f, std::memory_order_relaxed);
    engine->display.bridgeMinFrames.store(-1.0f, std::memory_order_relaxed);
    engine->display.bridgeMaxFrames.store(0.0f, std::memory_order_relaxed);
    engine->display.bridgeRatePpm.store(0.0f, std::memory_order_relaxed);
    engine->display.inputCallbackLastMs.store(0.0f, std::memory_order_relaxed);
    engine->display.inputCallbackMaxMs.store(0.0f, std::memory_order_relaxed);
    engine->display.outputCallbackLastMs.store(0.0f, std::memory_order_relaxed);
    engine->display.outputCallbackMaxMs.store(0.0f, std::memory_order_relaxed);
    engine->display.dspBlockLastMs.store(0.0f, std::memory_order_relaxed);
    engine->display.dspBlockMaxMs.store(0.0f, std::memory_order_relaxed);
    engine->display.pitchDetectorLastMs.store(0.0f, std::memory_order_relaxed);
    engine->display.pitchDetectorMaxMs.store(0.0f, std::memory_order_relaxed);
}

static void rebaseOutputBridgeLocked(AudioEngine* engine) {
    (void)engine;
    gOutputBridgeRebasePending.store(true, std::memory_order_release);
}

static bool openAudioRouteLocked(AudioEngine* engine, PaDeviceIndex inputDevice,
                                 PaDeviceIndex outputDevice) {
    const auto started = std::chrono::steady_clock::now();
    if (!gPortAudioInitialized) {
        setAudioRouteFailureLocked(
            audioSelectionFailure("initialize", "PortAudio is not initialized"),
            elapsedMilliseconds(started), false);
        return false;
    }

    enumerateAudioInputsLocked();
    enumerateAudioOutputsLocked();
    if (!audioDeviceAvailableLocked(inputDevice, true)) {
        setAudioRouteFailureLocked(
            audioSelectionFailure("select-input", "Selected audio input is unavailable"),
            elapsedMilliseconds(started), false);
        return false;
    }
    if (!audioDeviceAvailableLocked(outputDevice, false)) {
        setAudioRouteFailureLocked(
            audioSelectionFailure("select-output", "Selected audio output is unavailable"),
            elapsedMilliseconds(started), false);
        return false;
    }
    if (gAudioInputStream && gAudioOutputStream && gAudioInputContext &&
        gAudioOutputContext && gAudioInputDevice == inputDevice &&
        gAudioOutputDevice == outputDevice &&
        Pa_IsStreamActive(gAudioInputStream) == 1 &&
        Pa_IsStreamActive(gAudioOutputStream) == 1) {
        persistAudioInputLocked(inputDevice);
        persistAudioOutputLocked(outputDevice);
        setAudioRouteSuccessLocked(elapsedMilliseconds(started));
        return true;
    }

    closeAudioStreamLocked();
    AudioStreamParameters inputParameters, outputParameters;
    if (!prepareAudioStreamParametersLocked(inputDevice, true, inputParameters) ||
        !prepareAudioStreamParametersLocked(outputDevice, false, outputParameters)) {
        setAudioRouteFailureLocked(
            audioSelectionFailure("select-device", "Audio input or output is unavailable"),
            elapsedMilliseconds(started), false);
        return false;
    }

    resetAudioPipelineLocked(engine);
    gDspWorker.start(engine);

    PaStream* inputStream = nullptr;
    PaStream* outputStream = nullptr;
    auto inputContext = std::make_unique<AudioCallbackContext>(engine, true);
    auto outputContext = std::make_unique<AudioCallbackContext>(engine, true);
    gAudioRouteStatus = "opening";
    gAudioRouteStage = "open-input";
    PaError error = Pa_OpenStream(&inputStream, &inputParameters.value, nullptr,
                                  kSampleRate, kFramesPerBuffer,
                                  paClipOff | paDitherOff, audioInputCallback,
                                  inputContext.get());
    AudioRouteFailure failure;
    if (error != paNoError) {
        failure = captureAudioRouteFailure(
            error, gAudioRouteStage, portAudioDeviceName(inputDevice));
    }
    if (error == paNoError) {
        gAudioRouteStage = "open-output";
        error = Pa_OpenStream(&outputStream, nullptr, &outputParameters.value, kSampleRate,
                              kFramesPerBuffer, paClipOff | paDitherOff,
                              audioOutputCallback, outputContext.get());
        if (error != paNoError) {
            failure = captureAudioRouteFailure(
                error, gAudioRouteStage, portAudioDeviceName(outputDevice));
        }
    }
    if (error == paNoError) {
        gAudioRouteStage = "start-input";
        error = Pa_StartStream(inputStream);
        if (error != paNoError) {
            failure = captureAudioRouteFailure(
                error, gAudioRouteStage, portAudioDeviceName(inputDevice));
        }
    }
    if (error == paNoError) {
        Pa_Sleep(8);
        gAudioRouteStage = "start-output";
        error = Pa_StartStream(outputStream);
        if (error != paNoError) {
            failure = captureAudioRouteFailure(
                error, gAudioRouteStage, portAudioDeviceName(outputDevice));
        }
    }
    if (error != paNoError) {
        inputContext->enabled.store(false, std::memory_order_release);
        outputContext->enabled.store(false, std::memory_order_release);
        if (inputStream && Pa_IsStreamActive(inputStream) == 1)
            Pa_StopStream(inputStream);
        if (outputStream && Pa_IsStreamActive(outputStream) == 1)
            Pa_StopStream(outputStream);
        if (inputStream) Pa_CloseStream(inputStream);
        if (outputStream) Pa_CloseStream(outputStream);
        gDspWorker.stop();
        gAudioInputLatencyMs.store(0.0f, std::memory_order_relaxed);
        gAudioOutputLatencyMs.store(0.0f, std::memory_order_relaxed);
        gAudioReady.store(false, std::memory_order_relaxed);
        setAudioRouteFailureLocked(failure, elapsedMilliseconds(started), false);
        std::cerr << "[audio route] " << gAudioError << "\n";
        return false;
    }

    gAudioInputStream = inputStream;
    gAudioOutputStream = outputStream;
    gAudioInputContext = std::move(inputContext);
    gAudioOutputContext = std::move(outputContext);
    const PaStreamInfo* inputStreamInfo = Pa_GetStreamInfo(inputStream);
    const PaStreamInfo* outputStreamInfo = Pa_GetStreamInfo(outputStream);
    gAudioInputLatencyMs.store(
        inputStreamInfo ? (float)(1000.0 * inputStreamInfo->inputLatency) : 0.0f,
        std::memory_order_relaxed);
    gAudioOutputLatencyMs.store(
        outputStreamInfo ? (float)(1000.0 * outputStreamInfo->outputLatency) : 0.0f,
        std::memory_order_relaxed);

    gAudioInputDevice = inputDevice;
    gAudioOutputDevice = outputDevice;
    gAudioInputName = portAudioDeviceName(inputDevice);
    gAudioOutputName = portAudioDeviceName(gAudioOutputDevice);
    persistAudioInputLocked(gAudioInputDevice);
    persistAudioOutputLocked(gAudioOutputDevice);
    const float elapsedMs = elapsedMilliseconds(started);
    setAudioRouteSuccessLocked(elapsedMs);
    engine->display.inputPeak.store(0.0f, std::memory_order_relaxed);
    engine->display.outputPeak.store(0.0f, std::memory_order_relaxed);
    engine->display.inputOverflows.store(0, std::memory_order_relaxed);
    engine->display.outputUnderflows.store(0, std::memory_order_relaxed);
    gAudioReady.store(true, std::memory_order_relaxed);
    std::cerr << "[audio route] ready " << gAudioInputName << " -> "
              << gAudioOutputName << " in " << std::fixed << std::setprecision(1)
              << elapsedMs << " ms\n";
    return true;
}

static bool openAudioRoute(AudioEngine* engine, PaDeviceIndex inputDevice,
                           PaDeviceIndex outputDevice) {
    std::lock_guard<std::mutex> lock(gAudioMutex);
    return openAudioRouteLocked(engine, inputDevice, outputDevice);
}

static bool switchAudioInput(AudioEngine* engine, PaDeviceIndex inputDevice) {
    std::lock_guard<std::mutex> lock(gAudioMutex);
    const auto started = std::chrono::steady_clock::now();
    enumerateAudioInputsLocked();
    if (!audioDeviceAvailableLocked(inputDevice, true)) {
        setAudioRouteFailureLocked(
            audioSelectionFailure("select-input", "Selected audio input is unavailable"),
            elapsedMilliseconds(started), gAudioReady.load(std::memory_order_relaxed));
        return false;
    }
    if (!gAudioReady.load(std::memory_order_relaxed) || !gAudioInputStream ||
        !gAudioOutputStream || !gAudioInputContext || !gAudioOutputContext) {
        return openAudioRouteLocked(engine, inputDevice, gAudioOutputDevice);
    }
    if (inputDevice == gAudioInputDevice &&
        Pa_IsStreamActive(gAudioInputStream) == 1) {
        persistAudioInputLocked(inputDevice);
        setAudioRouteSuccessLocked(elapsedMilliseconds(started));
        return true;
    }

    AudioStreamParameters parameters;
    if (!prepareAudioStreamParametersLocked(inputDevice, true, parameters)) {
        setAudioRouteFailureLocked(
            audioSelectionFailure("select-input", "Selected audio input is unavailable"),
            elapsedMilliseconds(started), true);
        return false;
    }

    const std::string previousName = gAudioInputName;
    const std::string candidateName = portAudioDeviceName(inputDevice);
    PaStream* candidateStream = nullptr;
    auto candidateContext = std::make_unique<AudioCallbackContext>(engine, false);
    gAudioRouteStatus = "opening";
    gAudioRouteStage = "open-input";
    PaError error = Pa_OpenStream(&candidateStream, &parameters.value, nullptr,
                                  kSampleRate, kFramesPerBuffer,
                                  paClipOff | paDitherOff, audioInputCallback,
                                  candidateContext.get());
    AudioRouteFailure failure;
    if (error != paNoError) {
        failure = captureAudioRouteFailure(error, gAudioRouteStage, candidateName);
    } else {
        gAudioRouteStage = "start-input";
        error = Pa_StartStream(candidateStream);
        if (error != paNoError)
            failure = captureAudioRouteFailure(error, gAudioRouteStage, candidateName);
    }
    if (error != paNoError) {
        candidateContext->enabled.store(false, std::memory_order_release);
        if (candidateStream && Pa_IsStreamActive(candidateStream) == 1)
            Pa_StopStream(candidateStream);
        if (candidateStream) Pa_CloseStream(candidateStream);
        setAudioRouteFailureLocked(failure, elapsedMilliseconds(started), true);
        std::cerr << "[audio route] " << gAudioError << "\n";
        return false;
    }

    gAudioInputContext->enabled.store(false, std::memory_order_release);
    if (Pa_IsStreamActive(gAudioInputStream) == 1) Pa_StopStream(gAudioInputStream);
    Pa_CloseStream(gAudioInputStream);
    candidateContext->enabled.store(true, std::memory_order_release);
    gAudioInputStream = candidateStream;
    gAudioInputContext = std::move(candidateContext);
    gAudioInputDevice = inputDevice;
    gAudioInputName = candidateName;
    const PaStreamInfo* streamInfo = Pa_GetStreamInfo(gAudioInputStream);
    gAudioInputLatencyMs.store(
        streamInfo ? (float)(1000.0 * streamInfo->inputLatency) : 0.0f,
        std::memory_order_relaxed);
    persistAudioInputLocked(inputDevice);
    const float elapsedMs = elapsedMilliseconds(started);
    setAudioRouteSuccessLocked(elapsedMs);
    std::cerr << "[audio route] input " << previousName << " -> " << candidateName
              << " in " << std::fixed << std::setprecision(1) << elapsedMs << " ms\n";
    return true;
}

static bool switchAudioOutput(AudioEngine* engine, PaDeviceIndex outputDevice) {
    std::lock_guard<std::mutex> lock(gAudioMutex);
    const auto started = std::chrono::steady_clock::now();
    enumerateAudioOutputsLocked();
    if (!audioDeviceAvailableLocked(outputDevice, false)) {
        setAudioRouteFailureLocked(
            audioSelectionFailure("select-output", "Selected audio output is unavailable"),
            elapsedMilliseconds(started), gAudioReady.load(std::memory_order_relaxed));
        return false;
    }
    if (!gAudioReady.load(std::memory_order_relaxed) || !gAudioInputStream ||
        !gAudioOutputStream || !gAudioInputContext || !gAudioOutputContext) {
        return openAudioRouteLocked(engine, gAudioInputDevice, outputDevice);
    }
    if (outputDevice == gAudioOutputDevice &&
        Pa_IsStreamActive(gAudioOutputStream) == 1) {
        persistAudioOutputLocked(outputDevice);
        setAudioRouteSuccessLocked(elapsedMilliseconds(started));
        return true;
    }

    AudioStreamParameters parameters;
    if (!prepareAudioStreamParametersLocked(outputDevice, false, parameters)) {
        setAudioRouteFailureLocked(
            audioSelectionFailure("select-output", "Selected audio output is unavailable"),
            elapsedMilliseconds(started), true);
        return false;
    }

    const std::string previousName = gAudioOutputName;
    const std::string candidateName = portAudioDeviceName(outputDevice);
    PaStream* candidateStream = nullptr;
    auto candidateContext = std::make_unique<AudioCallbackContext>(engine, false);
    gAudioRouteStatus = "opening";
    gAudioRouteStage = "open-output";
    PaError error = Pa_OpenStream(&candidateStream, nullptr, &parameters.value,
                                  kSampleRate, kFramesPerBuffer,
                                  paClipOff | paDitherOff, audioOutputCallback,
                                  candidateContext.get());
    AudioRouteFailure failure;
    if (error != paNoError) {
        failure = captureAudioRouteFailure(error, gAudioRouteStage, candidateName);
    } else {
        gAudioRouteStage = "start-output";
        error = Pa_StartStream(candidateStream);
        if (error != paNoError)
            failure = captureAudioRouteFailure(error, gAudioRouteStage, candidateName);
    }
    if (error != paNoError) {
        candidateContext->enabled.store(false, std::memory_order_release);
        if (candidateStream && Pa_IsStreamActive(candidateStream) == 1)
            Pa_StopStream(candidateStream);
        if (candidateStream) Pa_CloseStream(candidateStream);
        setAudioRouteFailureLocked(failure, elapsedMilliseconds(started), true);
        std::cerr << "[audio route] " << gAudioError << "\n";
        return false;
    }

    gAudioOutputContext->enabled.store(false, std::memory_order_release);
    if (Pa_IsStreamActive(gAudioOutputStream) == 1) Pa_StopStream(gAudioOutputStream);
    Pa_CloseStream(gAudioOutputStream);
    rebaseOutputBridgeLocked(engine);
    candidateContext->enabled.store(true, std::memory_order_release);
    gAudioOutputStream = candidateStream;
    gAudioOutputContext = std::move(candidateContext);
    gAudioOutputDevice = outputDevice;
    gAudioOutputName = candidateName;
    const PaStreamInfo* streamInfo = Pa_GetStreamInfo(gAudioOutputStream);
    gAudioOutputLatencyMs.store(
        streamInfo ? (float)(1000.0 * streamInfo->outputLatency) : 0.0f,
        std::memory_order_relaxed);
    persistAudioOutputLocked(outputDevice);
    const float elapsedMs = elapsedMilliseconds(started);
    setAudioRouteSuccessLocked(elapsedMs);
    std::cerr << "[audio route] output " << previousName << " -> " << candidateName
              << " in " << std::fixed << std::setprecision(1) << elapsedMs << " ms\n";
    return true;
}

static bool initializeAudio(AudioEngine* engine) {
    PaError error = Pa_Initialize();
    if (error != paNoError) {
        std::lock_guard<std::mutex> lock(gAudioMutex);
        gAudioError = std::string("PortAudio init failed: ") + Pa_GetErrorText(error);
        std::cerr << gAudioError << "\n";
        return false;
    }
    const PaDeviceIndex defaultInputDevice = Pa_GetDefaultInputDevice();
    const PaDeviceIndex defaultOutputDevice = Pa_GetDefaultOutputDevice();
    PaDeviceIndex inputDevice = defaultInputDevice;
    PaDeviceIndex outputDevice = defaultOutputDevice;
    std::vector<AudioDevice> inputDevices;
    std::vector<AudioDevice> outputDevices;
    {
        std::lock_guard<std::mutex> lock(gAudioMutex);
        gPortAudioInitialized = true;
        enumerateAudioInputsLocked();
        enumerateAudioOutputsLocked();
        inputDevice = preferredAudioInputLocked(inputDevice);
        outputDevice = preferredAudioOutputLocked(outputDevice);
        inputDevices = gAudioInputDevices;
        outputDevices = gAudioOutputDevices;
    }

    if (openAudioRoute(engine, inputDevice, outputDevice)) return true;

    std::string failedStage;
    {
        std::lock_guard<std::mutex> lock(gAudioMutex);
        failedStage = gAudioRouteStage;
    }
    const bool inputFailed = failedStage.find("input") != std::string::npos;
    const bool outputFailed = failedStage.find("output") != std::string::npos;
    if (inputFailed) {
        if (defaultInputDevice != paNoDevice && defaultInputDevice != inputDevice) {
            std::cerr << "[audio route] trying default input "
                      << portAudioDeviceName(defaultInputDevice) << "\n";
            if (openAudioRoute(engine, defaultInputDevice, outputDevice)) return true;
        }
        for (const auto& candidate : inputDevices) {
            if (candidate.index == inputDevice || candidate.index == defaultInputDevice)
                continue;
            std::cerr << "[audio route] trying fallback input " << candidate.name << "\n";
            if (openAudioRoute(engine, candidate.index, outputDevice)) return true;
        }
    } else if (outputFailed) {
        if (defaultOutputDevice != paNoDevice && defaultOutputDevice != outputDevice) {
            std::cerr << "[audio route] trying default output "
                      << portAudioDeviceName(defaultOutputDevice) << "\n";
            if (openAudioRoute(engine, inputDevice, defaultOutputDevice)) return true;
        }
        for (const auto& candidate : outputDevices) {
            if (candidate.index == outputDevice || candidate.index == defaultOutputDevice)
                continue;
            std::cerr << "[audio route] trying fallback output " << candidate.name << "\n";
            if (openAudioRoute(engine, inputDevice, candidate.index)) return true;
        }
    }
    return false;
}

static void shutdownAudio() {
    std::lock_guard<std::mutex> lock(gAudioMutex);
    closeAudioStreamLocked();
    if (gPortAudioInitialized) {
        Pa_Terminate();
        gPortAudioInitialized = false;
    }
}

// ── MIDI ───────────────────────────────────────────────────────────────────

static void handleNoteOn(AudioEngine* eng, int note) {
    if (note < 0 || note > 127) return;
    std::lock_guard<std::mutex> lock(eng->midiMutex);
    uint64_t stamp = ++eng->noteCounter;
    eng->display.beginGlide(note, eng->sampleClock.load(std::memory_order_relaxed));

    int chosen = -1;
    for (int v = 0; v < kMaxVoices; v++)
        if (!eng->voices[v].isAudible()) { chosen = v; break; }

    if (chosen < 0) {  // steal releasing voice
        float lo = 999.0f;
        for (int v = 0; v < kMaxVoices; v++)
            if (!eng->voices[v].gateOn.load(std::memory_order_relaxed) &&
                eng->voices[v].envelope < lo) { lo = eng->voices[v].envelope; chosen = v; }
    }
    if (chosen < 0) {  // steal oldest
        uint64_t oldest = UINT64_MAX;
        for (int v = 0; v < kMaxVoices; v++) {
            uint64_t st = eng->voices[v].stamp.load(std::memory_order_relaxed);
            if (st < oldest) { oldest = st; chosen = v; }
        }
    }
    if (chosen >= 0) {
        eng->voices[chosen].midiNote.store(note, std::memory_order_relaxed);
        eng->voices[chosen].gateOn.store(true,   std::memory_order_relaxed);
        eng->voices[chosen].stamp.store(stamp,   std::memory_order_relaxed);
        eng->voices[chosen].sustained = false;
#ifdef HARMONIZER_CUSTOM_VOICE_EVENTS
        eng->onVoiceAssigned(chosen);
#endif
        eng->display.noteOn(note);
    }
    if (eng->capture.active.load(std::memory_order_acquire))
        eng->capture.appendMidi(eng->sampleClock.load(std::memory_order_relaxed), true, note);
}

static void handleNoteOff(AudioEngine* eng, int note) {
    if (note < 0 || note > 127) return;
    std::lock_guard<std::mutex> lock(eng->midiMutex);
    if (eng->capture.active.load(std::memory_order_acquire))
        eng->capture.appendMidi(eng->sampleClock.load(std::memory_order_relaxed), false, note);
    for (int v = 0; v < kMaxVoices; v++) {
        if (eng->voices[v].midiNote.load(std::memory_order_relaxed) == note &&
            eng->voices[v].gateOn.load(std::memory_order_relaxed)) {
            if (eng->sustainOn) {
                eng->voices[v].sustained = true;
            } else {
                eng->voices[v].gateOn.store(false, std::memory_order_relaxed);
                eng->display.noteOff(note);
            }
        }
    }
}

static void handleSustainOff(AudioEngine* eng) {
    for (int v = 0; v < kMaxVoices; v++) {
        if (eng->voices[v].sustained) {
            eng->voices[v].gateOn.store(false, std::memory_order_relaxed);
            eng->voices[v].sustained = false;
            int n = eng->voices[v].midiNote.load(std::memory_order_relaxed);
            if (n >= 0) eng->display.noteOff(n);
        }
    }
}

// ── Browser Server ─────────────────────────────────────────────────────────

static std::string readTextFile(const char* path) {
    std::ifstream in(path);
    if (!in) return "";
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

static bool sendAll(SocketHandle fd, const std::string& text) {
    const char* data = text.data();
    size_t left = text.size();
    while (left > 0) {
        const int chunk = static_cast<int>(std::min<size_t>(left, 0x7fffffff));
        const int n = ::send(fd, data, chunk, 0);
        if (n <= 0) return false;
        data += n;
        left -= (size_t)n;
    }
    return true;
}

static std::string response(const std::string& status,
                            const std::string& type,
                            const std::string& body)
{
    std::ostringstream out;
    out << "HTTP/1.1 " << status << "\r\n"
        << "Content-Type: " << type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    return out.str();
}

static bool queryFloat(const std::string& query, const std::string& key, float& value) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t end = query.find('&', pos);
        std::string part = query.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        size_t eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == key) {
            char* tail = nullptr;
            float parsed = std::strtof(part.c_str() + eq + 1, &tail);
            if (tail && *tail == '\0') {
                value = parsed;
                return true;
            }
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return false;
}

static std::string queryString(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t end = query.find('&', pos);
        std::string part = query.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        size_t eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == key) {
            return part.substr(eq + 1);
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return "";
}

static std::string controlsJson(AudioEngine* eng) {
    float mix = eng->display.wetDryBalance.load(std::memory_order_relaxed);
    float wet = 0.0f;
    float dry = 0.0f;
    wetDryFromBalance(mix, wet, dry);

    std::ostringstream out;
    out << "\"mix\":" << mix
        << ",\"wet\":" << wet
        << ",\"dry\":" << dry
        << ",\"gainDb\":" << eng->display.monitorGainDb.load(std::memory_order_relaxed)
        << ",\"gate\":" << eng->display.voicedGateRms.load(std::memory_order_relaxed)
        << ",\"stableWindow\":" << eng->display.stableSemitoneWindow.load(std::memory_order_relaxed)
        << ",\"immediacy\":" << eng->display.parallelEarlyBlend.load(std::memory_order_relaxed)
        << ",\"glideAmount\":" << eng->display.glideAmount.load(std::memory_order_relaxed)
        << ",\"glideTimeMs\":" << eng->display.glideTimeMs.load(std::memory_order_relaxed)
        << ",\"chorusMix\":" << eng->display.chorusMix.load(std::memory_order_relaxed)
        << ",\"reverbMix\":" << eng->display.reverbMix.load(std::memory_order_relaxed)
        << ",\"freezeTone\":" << eng->display.freezeTone.load(std::memory_order_relaxed)
        << ",\"unvoicedMode\":\""
        << unvoicedModeKey(eng->display.unvoicedMode.load(std::memory_order_relaxed))
        << "\"";
    for (size_t layer = 0; layer < harmonizer::kFreezeLayerCount; ++layer) {
        out << ",\"freeze" << (layer + 1) << "Level\":"
            << eng->display.freezeLevel[layer].load(std::memory_order_relaxed)
            << ",\"freeze" << (layer + 1) << "Transpose\":"
            << eng->display.freezeTranspose[layer].load(std::memory_order_relaxed);
    }
    out << ",\"freezeHold\":[";
    for (size_t layer = 0; layer < harmonizer::kFreezeLayerCount; ++layer) {
        if (layer) out << ',';
        out << (eng->display.freezeHold[layer].load(std::memory_order_relaxed)
                    ? "true" : "false");
    }
    out << "],\"freezeActive\":[";
    for (size_t layer = 0; layer < harmonizer::kFreezeLayerCount; ++layer) {
        if (layer) out << ',';
        out << (eng->display.freezeActive[layer].load(std::memory_order_relaxed)
                    ? "true" : "false");
    }
    out << "],\"freezeLevel\":[";
    for (size_t layer = 0; layer < harmonizer::kFreezeLayerCount; ++layer) {
        if (layer) out << ',';
        out << eng->display.freezeLevel[layer].load(std::memory_order_relaxed);
    }
    out << "],\"freezeTranspose\":[";
    for (size_t layer = 0; layer < harmonizer::kFreezeLayerCount; ++layer) {
        if (layer) out << ',';
        out << eng->display.freezeTranspose[layer].load(std::memory_order_relaxed);
    }
    out << ']';
    return out.str();
}

static std::string jsonString(const std::string& text) {
    std::ostringstream out;
    out << "\"";
    for (char c : text) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) out << "\\u00" << std::hex << (int)(unsigned char)c << std::dec;
            else out << c;
            break;
        }
    }
    out << "\"";
    return out.str();
}

static const BackendSpec* findBackend(const std::string& key) {
    for (const auto& backend : kBackendSpecs) {
        if (key == backend.key) return &backend;
    }
    return nullptr;
}

static std::string platformExecutableName(const char* base) {
#if defined(_WIN32)
    return std::string(base) + ".exe";
#else
    return base;
#endif
}

static std::filesystem::path resolveBackendExecutable(const BackendSpec& backend) {
    if (std::string(backend.key) == HARMONIZER_BACKEND_KEY &&
        !gExecutablePath.empty()) {
        return gExecutablePath;
    }

    const std::string filename = platformExecutableName(backend.executable);
    std::vector<std::filesystem::path> candidates;
    if (!gExecutablePath.empty()) {
        const auto executableDir = gExecutablePath.parent_path();
        candidates.push_back(executableDir / filename);
        candidates.push_back(executableDir / "build-backend-lab" / filename);
    }

    std::error_code error;
    const auto cwd = std::filesystem::current_path(error);
    if (!error) {
        candidates.push_back(cwd / "build-backend-lab" / filename);
        candidates.push_back(cwd / filename);
    }

    for (const auto& candidate : candidates) {
        error.clear();
        if (!std::filesystem::is_regular_file(candidate, error)) continue;
        auto canonical = std::filesystem::weakly_canonical(candidate, error);
        return error ? candidate : canonical;
    }
    return {};
}

static std::string backendsJson() {
    std::ostringstream out;
    out << "{\"selected\":\"" HARMONIZER_BACKEND_KEY "\",\"backends\":[";
    bool first = true;
    for (const auto& backend : kBackendSpecs) {
        if (!first) out << ",";
        first = false;
        out << "{\"id\":" << jsonString(backend.key)
            << ",\"name\":" << jsonString(backend.name)
            << ",\"available\":"
            << (!resolveBackendExecutable(backend).empty() ? "true" : "false")
            << "}";
    }
    out << "]}";
    return out.str();
}

static int relaunchBackend(int webPort) {
    std::string requested;
    {
        std::lock_guard<std::mutex> lock(gBackendMutex);
        requested = gRequestedBackend;
    }
    if (requested.empty()) return 0;

    const BackendSpec* backend = findBackend(requested);
    const auto executable = backend ? resolveBackendExecutable(*backend)
                                    : std::filesystem::path{};
    if (!backend || executable.empty()) {
        std::cerr << "Backend handoff failed: " << requested << " is unavailable\n";
        return 1;
    }

    const std::string executableText = executable.string();
    const std::string portText = std::to_string(webPort);
    std::cerr << "Switching backend to " << backend->name << " at "
              << executableText << "\n";
#if defined(_WIN32)
    intptr_t child = _spawnl(_P_NOWAIT, executableText.c_str(),
                             executableText.c_str(), "--port", portText.c_str(),
                             nullptr);
    if (child == -1) {
        std::cerr << "Backend handoff failed: " << std::strerror(errno) << "\n";
        return 1;
    }
    return 0;
#else
    execl(executableText.c_str(), executableText.c_str(),
          "--port", portText.c_str(), static_cast<char*>(nullptr));
    std::cerr << "Backend handoff failed: " << std::strerror(errno) << "\n";
    return 1;
#endif
}

static std::string audioOutputsJson() {
    std::lock_guard<std::mutex> lock(gAudioMutex);
    enumerateAudioOutputsLocked();
    std::ostringstream out;
    out << "{\"selected\":" << gAudioOutputDevice << ",\"outputs\":[";
    bool first = true;
    for (const auto& device : gAudioOutputDevices) {
        if (!first) out << ",";
        first = false;
        const PaDeviceInfo* info = Pa_GetDeviceInfo(device.index);
        out << "{\"id\":" << device.index
            << ",\"name\":" << jsonString(device.name)
            << ",\"channels\":" << (info ? info->maxOutputChannels : 0)
            << ",\"defaultSampleRate\":" << (info ? info->defaultSampleRate : 0.0)
            << ",\"lowLatencyMs\":"
            << (info ? 1000.0 * info->defaultLowOutputLatency : 0.0) << "}";
    }
    out << "]}";
    return out.str();
}

static std::string audioInputsJson() {
    std::lock_guard<std::mutex> lock(gAudioMutex);
    enumerateAudioInputsLocked();
    std::ostringstream out;
    out << "{\"selected\":" << gAudioInputDevice << ",\"inputs\":[";
    bool first = true;
    for (const auto& device : gAudioInputDevices) {
        if (!first) out << ",";
        first = false;
        const PaDeviceInfo* info = Pa_GetDeviceInfo(device.index);
        out << "{\"id\":" << device.index
            << ",\"name\":" << jsonString(device.name)
            << ",\"channels\":" << (info ? info->maxInputChannels : 0)
            << ",\"defaultSampleRate\":" << (info ? info->defaultSampleRate : 0.0)
            << ",\"lowLatencyMs\":"
            << (info ? 1000.0 * info->defaultLowInputLatency : 0.0) << "}";
    }
    out << "]}";
    return out.str();
}

static std::string stateJson(AudioEngine* eng) {
    std::string audioError;
    std::string audioInput;
    std::string audioOutput;
    std::string audioRouteStatus;
    std::string audioRouteStage;
    std::string audioHostErrorText;
    float audioRouteElapsedMs = 0.0f;
    bool audioRouteKeptPrevious = false;
    int audioPortAudioError = paNoError;
    long audioHostError = 0;
    PaDeviceIndex audioInputDevice = paNoDevice;
    PaDeviceIndex audioOutputDevice = paNoDevice;
    {
        std::lock_guard<std::mutex> lock(gAudioMutex);
        audioError = gAudioError;
        audioInput = gAudioInputName;
        audioOutput = gAudioOutputName;
        audioRouteStatus = gAudioRouteStatus;
        audioRouteStage = gAudioRouteStage;
        audioRouteElapsedMs = gAudioRouteElapsedMs;
        audioRouteKeptPrevious = gAudioRouteKeptPrevious;
        audioPortAudioError = gAudioPortAudioError;
        audioHostError = gAudioHostError;
        audioHostErrorText = gAudioHostErrorText;
        audioInputDevice = gAudioInputDevice;
        audioOutputDevice = gAudioOutputDevice;
    }

    uint64_t lo = eng->display.noteBitsLo.load(std::memory_order_relaxed);
    uint64_t hi = eng->display.noteBitsHi.load(std::memory_order_relaxed);
    std::ostringstream notes;
    notes << "[";
    bool first = true;
    for (int note = 0; note < 128; note++) {
        bool on = note < 64 ? ((lo >> note) & 1ULL) : ((hi >> (note - 64)) & 1ULL);
        if (!on) continue;
        if (!first) notes << ",";
        first = false;
        notes << note;
    }
    notes << "]";

    constexpr uint64_t kPitchTelemetryBatch = 24;
    const uint64_t telemetryEnd =
        eng->display.pitchTelemetryCount.load(std::memory_order_acquire);
    const uint64_t telemetryStart = telemetryEnd > kPitchTelemetryBatch
        ? telemetryEnd - kPitchTelemetryBatch + 1
        : 1;
    std::ostringstream pitchTelemetry;
    pitchTelemetry << "[";
    bool firstTelemetry = true;
    for (uint64_t sequence = telemetryStart;
         sequence <= telemetryEnd && sequence > 0;
         ++sequence) {
        const PitchTelemetrySample& slot = eng->display.pitchTelemetry[
            (sequence - 1) % SharedDisplay::kPitchTelemetryCapacity];
        if (slot.sequence.load(std::memory_order_acquire) != sequence) continue;

        const uint64_t sample = slot.dspSample.load(std::memory_order_relaxed);
        const uint64_t rawSample =
            slot.rawPitchSample.load(std::memory_order_relaxed);
        const uint64_t earlySample =
            slot.earlyControlSample.load(std::memory_order_relaxed);
        const uint64_t qualitySample =
            slot.qualityControlSample.load(std::memory_order_relaxed);
        const float raw = slot.rawMidi.load(std::memory_order_relaxed);
        const float early =
            slot.earlyControlMidi.load(std::memory_order_relaxed);
        const float quality =
            slot.qualityControlMidi.load(std::memory_order_relaxed);
        const float earlyWeight =
            slot.earlyPathWeight.load(std::memory_order_relaxed);
        const float bend = slot.pitchBend.load(std::memory_order_relaxed);
        const uint64_t sampleNotesLo =
            slot.noteBitsLo.load(std::memory_order_relaxed);
        const uint64_t sampleNotesHi =
            slot.noteBitsHi.load(std::memory_order_relaxed);
        if (slot.sequence.load(std::memory_order_acquire) != sequence) continue;

        if (!firstTelemetry) pitchTelemetry << ",";
        firstTelemetry = false;
        pitchTelemetry << "{\"id\":" << sequence
                       << ",\"sample\":" << sample
                       << ",\"rawSample\":" << rawSample
                       << ",\"rawMidi\":" << raw
                       << ",\"earlySample\":" << earlySample
                       << ",\"earlyMidi\":" << early
                       << ",\"qualitySample\":" << qualitySample
                       << ",\"qualityMidi\":" << quality
                       << ",\"earlyWeight\":" << earlyWeight
                       << ",\"pitchBend\":" << bend
                       << ",\"notes\":[";
        bool firstSampleNote = true;
        for (int note = 0; note < 128; ++note) {
            const bool on = note < 64
                ? ((sampleNotesLo >> note) & 1ULL)
                : ((sampleNotesHi >> (note - 64)) & 1ULL);
            if (!on) continue;
            if (!firstSampleNote) pitchTelemetry << ",";
            firstSampleNote = false;
            pitchTelemetry << note;
        }
        pitchTelemetry << "]}";
    }
    pitchTelemetry << "]";

    std::ostringstream out;
    out << "{"
        << "\"serverInstance\":" << jsonString(gServerInstance)
        << ",\"audioReady\":" << (gAudioReady.load(std::memory_order_relaxed) ? "true" : "false")
        << ",\"audioError\":" << jsonString(audioError)
        << ",\"audioRouteStatus\":" << jsonString(audioRouteStatus)
        << ",\"audioRouteStage\":" << jsonString(audioRouteStage)
        << ",\"audioRouteElapsedMs\":" << audioRouteElapsedMs
        << ",\"audioRouteKeptPrevious\":" << (audioRouteKeptPrevious ? "true" : "false")
        << ",\"audioPortAudioError\":" << audioPortAudioError
        << ",\"audioHostError\":" << audioHostError
        << ",\"audioHostErrorText\":" << jsonString(audioHostErrorText)
        << ",\"audioInput\":" << jsonString(audioInput)
        << ",\"audioOutput\":" << jsonString(audioOutput)
        << ",\"audioInputDevice\":" << audioInputDevice
        << ",\"audioOutputDevice\":" << audioOutputDevice
        << ",\"audioInputLatencyMs\":" << gAudioInputLatencyMs.load(std::memory_order_relaxed)
        << ",\"audioOutputLatencyMs\":" << gAudioOutputLatencyMs.load(std::memory_order_relaxed)
        << ",\"inputPeak\":" << eng->display.inputPeak.load(std::memory_order_relaxed)
        << ",\"outputPeak\":" << eng->display.outputPeak.load(std::memory_order_relaxed)
        << ",\"bridgeFillFrames\":" << eng->display.bridgeFillFrames.load(std::memory_order_relaxed)
        << ",\"bridgeFilteredFillFrames\":" << eng->display.bridgeFilteredFillFrames.load(std::memory_order_relaxed)
        << ",\"bridgeTargetFrames\":" << kOutputBridgeTargetFrames
        << ",\"bridgeMinFrames\":" << eng->display.bridgeMinFrames.load(std::memory_order_relaxed)
        << ",\"bridgeMaxFrames\":" << eng->display.bridgeMaxFrames.load(std::memory_order_relaxed)
        << ",\"bridgeRatePpm\":" << eng->display.bridgeRatePpm.load(std::memory_order_relaxed)
        << ",\"inputCallbackLastMs\":" << eng->display.inputCallbackLastMs.load(std::memory_order_relaxed)
        << ",\"inputCallbackMaxMs\":" << eng->display.inputCallbackMaxMs.load(std::memory_order_relaxed)
        << ",\"outputCallbackLastMs\":" << eng->display.outputCallbackLastMs.load(std::memory_order_relaxed)
        << ",\"outputCallbackMaxMs\":" << eng->display.outputCallbackMaxMs.load(std::memory_order_relaxed)
        << ",\"dspBlockLastMs\":" << eng->display.dspBlockLastMs.load(std::memory_order_relaxed)
        << ",\"dspBlockMaxMs\":" << eng->display.dspBlockMaxMs.load(std::memory_order_relaxed)
        << ",\"pitchDetectorLastMs\":" << eng->display.pitchDetectorLastMs.load(std::memory_order_relaxed)
        << ",\"pitchDetectorMaxMs\":" << eng->display.pitchDetectorMaxMs.load(std::memory_order_relaxed)
        << ",\"dspWorkerBatchMaxMs\":" << eng->display.dspWorkerBatchMaxMs.load(std::memory_order_relaxed)
        << ",\"dspQueueFrames\":" << eng->display.dspQueueFrames.load(std::memory_order_relaxed)
        << ",\"dspQueueMaxFrames\":" << eng->display.dspQueueMaxFrames.load(std::memory_order_relaxed)
        << ",\"callbackFrames\":" << eng->display.callbackFrames.load(std::memory_order_relaxed)
        << ",\"sampleClock\":" << eng->sampleClock.load(std::memory_order_relaxed)
        << ",\"sampleRate\":" << kSampleRate
        << ",\"inputOverflows\":" << eng->display.inputOverflows.load(std::memory_order_relaxed)
        << ",\"outputUnderflows\":" << eng->display.outputUnderflows.load(std::memory_order_relaxed)
        << ",\"testTone\":"
        << (eng->sampleClock.load(std::memory_order_relaxed) <
            eng->testToneEndClock.load(std::memory_order_relaxed) ? "true" : "false")
        << ",\"backendKey\":\"" HARMONIZER_BACKEND_KEY "\""
        << ",\"dspBackend\":\"" HARMONIZER_DSP_BACKEND "\""
        << ",\"dspLatencyMs\":" << eng->dspLatencyMs()
        << ",\"earlyPathLatencyMs\":" << eng->display.earlyPathLatencyMs.load(std::memory_order_relaxed)
        << ",\"qualityPathLatencyMs\":" << eng->display.qualityPathLatencyMs.load(std::memory_order_relaxed)
        << ",\"midiReady\":" << (gMidiReady.load(std::memory_order_relaxed) ? "true" : "false")
        << ",\"midiError\":" << jsonString(gMidiError)
        << ",\"detectedMidi\":" << eng->display.detectedMidi.load(std::memory_order_relaxed)
        << ",\"rawMidi\":" << eng->display.rawMidi.load(std::memory_order_relaxed)
        << ",\"correctionMidi\":" << eng->display.correctionMidi.load(std::memory_order_relaxed)
        << ",\"predictedEarlyMidi\":" << eng->display.predictedEarlyMidi.load(std::memory_order_relaxed)
        << ",\"predictedQualityMidi\":" << eng->display.predictedQualityMidi.load(std::memory_order_relaxed)
        << ",\"pitchSlope\":" << eng->display.pitchSlope.load(std::memory_order_relaxed)
        << ",\"predictorConfidence\":" << eng->display.predictorConfidence.load(std::memory_order_relaxed)
        << ",\"predictorVoiced\":" << (eng->display.predictorVoiced.load(std::memory_order_relaxed) ? "true" : "false")
        << ",\"parallelHandoff\":" << eng->display.parallelHandoff.load(std::memory_order_relaxed)
        << ",\"pitchStable\":" << (eng->display.pitchStable.load(std::memory_order_relaxed) ? "true" : "false")
        << ",\"pitchVoiced\":" << (eng->display.pitchVoiced.load(std::memory_order_relaxed) ? "true" : "false")
        << ",\"pitchRms\":" << eng->display.pitchRms.load(std::memory_order_relaxed)
        << ",\"unvoicedActive\":" << (eng->display.unvoicedActive.load(std::memory_order_relaxed) ? "true" : "false")
        << ",\"sibilanceScore\":" << eng->display.sibilanceScore.load(std::memory_order_relaxed)
        << ",\"pitchBend\":" << eng->pitchBend.load(std::memory_order_relaxed)
        << ",\"capturing\":" << (eng->capture.active.load(std::memory_order_relaxed) ? "true" : "false")
        << ",\"captureSeconds\":" << (double)eng->capture.audioLen.load(std::memory_order_relaxed) / kSampleRate
        << ",\"notes\":" << notes.str()
        << ",\"pitchTelemetry\":" << pitchTelemetry.str()
        << "," << controlsJson(eng)
        << "}";
    return out.str();
}

static void applyControls(AudioEngine* eng, const std::string& query) {
    float v = 0.0f;
    if (queryFloat(query, "mix", v) || queryFloat(query, "blend", v)) {
        eng->display.wetDryBalance.store(clampf(v, 0.0f, 1.0f), std::memory_order_relaxed);
    } else {
        float currentMix = eng->display.wetDryBalance.load(std::memory_order_relaxed);
        float wet = currentMix;
        float dry = 1.0f - currentMix;
        bool hasWet = queryFloat(query, "wet", wet);
        bool hasDry = queryFloat(query, "dry", dry);
        if (hasWet || hasDry) {
            wet = clampf(wet, 0.0f, 1.5f);
            dry = clampf(dry, 0.0f, 1.5f);
            float total = wet + dry;
            eng->display.wetDryBalance.store(total > 0.0001f ? wet / total : 0.0f,
                                             std::memory_order_relaxed);
        }
    }
    if (queryFloat(query, "gate", v))
        eng->display.voicedGateRms.store(clampf(v, 0.0001f, 0.0300f), std::memory_order_relaxed);
    if (queryFloat(query, "gain", v) || queryFloat(query, "gainDb", v))
        eng->display.monitorGainDb.store(clampf(v, 0.0f, 30.0f), std::memory_order_relaxed);
    if (queryFloat(query, "stable", v))
        eng->display.stableSemitoneWindow.store(clampf(v, 0.2f, 2.0f), std::memory_order_relaxed);
    if (queryFloat(query, "immediacy", v))
        eng->display.parallelEarlyBlend.store(clampf(v, 0.0f, 1.0f), std::memory_order_relaxed);
    if (queryFloat(query, "glide", v) || queryFloat(query, "glideAmount", v))
        eng->display.glideAmount.store(clampf(v, 0.0f, 1.0f), std::memory_order_relaxed);
    if (queryFloat(query, "glideTime", v) || queryFloat(query, "glideTimeMs", v))
        eng->display.glideTimeMs.store(clampf(v, 10.0f, 2000.0f), std::memory_order_relaxed);
    if (queryFloat(query, "chorus", v) || queryFloat(query, "chorusMix", v))
        eng->display.chorusMix.store(clampf(v, 0.0f, 1.0f), std::memory_order_relaxed);
    if (queryFloat(query, "reverb", v) || queryFloat(query, "reverbMix", v))
        eng->display.reverbMix.store(clampf(v, 0.0f, 1.0f), std::memory_order_relaxed);
    if (queryFloat(query, "freezeTone", v))
        eng->display.freezeTone.store(clampf(v, 0.0f, 1.0f), std::memory_order_relaxed);
    for (size_t layer = 0; layer < harmonizer::kFreezeLayerCount; ++layer) {
        const std::string prefix = "freeze" + std::to_string(layer + 1);
        if (queryFloat(query, prefix + "Level", v)) {
            eng->display.freezeLevel[layer].store(
                clampf(v, 0.0f, 1.25f), std::memory_order_relaxed);
        }
        if (queryFloat(query, prefix + "Transpose", v)) {
            eng->display.freezeTranspose[layer].store(
                clampf(v, -24.0f, 24.0f), std::memory_order_relaxed);
        }
    }
    std::string unvoicedMode = queryString(query, "unvoiced");
    if (!unvoicedMode.empty()) {
        eng->display.unvoicedMode.store(unvoicedModeFromKey(unvoicedMode),
                                        std::memory_order_relaxed);
    }
}

static bool setFreezeHeld(AudioEngine* eng, int slot, bool held, bool clear) {
    if (slot < 0 || slot >= static_cast<int>(harmonizer::kFreezeLayerCount)) return false;
    eng->display.freezeHold[slot].store(held, std::memory_order_relaxed);
    if (clear) {
        eng->display.freezeClearGeneration[slot].fetch_add(1, std::memory_order_relaxed);
    }
    if (eng->capture.active.load(std::memory_order_acquire)) {
        eng->capture.appendEffect(
            eng->sampleClock.load(std::memory_order_relaxed), slot, held, clear);
    }
    return true;
}

static bool applyFreezeControl(AudioEngine* eng, const std::string& query) {
    float slotValue = -1.0f;
    if (!queryFloat(query, "slot", slotValue)) return false;
    const int slot = static_cast<int>(std::lround(slotValue));
    if (slot < 0 || slot >= static_cast<int>(harmonizer::kFreezeLayerCount)) return false;
    const std::string action = queryString(query, "action");
    if (action == "toggle") {
        const bool held = eng->display.freezeHold[slot].load(std::memory_order_relaxed);
        return setFreezeHeld(eng, slot, !held, false);
    }
    if (action == "on" || action == "down" || action == "capture") {
        return setFreezeHeld(eng, slot, true, false);
    }
    if (action == "off" || action == "up" || action == "release") {
        return setFreezeHeld(eng, slot, false, false);
    }
    if (action == "clear") return setFreezeHeld(eng, slot, false, true);
    return false;
}

static void startTestTone(AudioEngine* eng) {
    uint64_t start = eng->sampleClock.load(std::memory_order_relaxed);
    eng->testToneStartClock.store(start, std::memory_order_relaxed);
    eng->testToneEndClock.store(start + kSampleRate, std::memory_order_relaxed);
}

static void applyBrowserMidi(AudioEngine* eng, const std::string& query) {
    std::string event = queryString(query, "event");
    float value = 0.0f;

    if (event == "on" && queryFloat(query, "note", value)) {
        handleNoteOn(eng, (int)value);
    } else if (event == "off" && queryFloat(query, "note", value)) {
        handleNoteOff(eng, (int)value);
    } else if (event == "cc") {
        float cc = 0.0f;
        if (queryFloat(query, "cc", cc) && queryFloat(query, "value", value)) {
            const int controller = static_cast<int>(cc);
            if (controller == 64) {
                eng->sustainOn = value >= 64.0f;
                if (!eng->sustainOn) handleSustainOff(eng);
            } else if (controller >= 66 && controller <= 68) {
                setFreezeHeld(eng, controller - 66, value >= 64.0f, false);
            }
        }
    } else if (event == "bend" && queryFloat(query, "semitones", value)) {
        eng->pitchBend.store(clampf(value, -kBendRange, kBendRange), std::memory_order_relaxed);
    }
}

static void applyBrowserMidiStatus(const std::string& query) {
    float ready = 0.0f;
    if (queryFloat(query, "ready", ready)) {
        gMidiReady.store(ready > 0.5f, std::memory_order_relaxed);
    }
    gMidiError = queryString(query, "error");
}

// ── Diagnostic capture control ─────────────────────────────────────────────

static bool writeWavFloat(const std::string& path, const float* left,
                          const float* right, size_t frames)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const uint16_t channels   = right ? 2 : 1;
    const uint32_t dataBytes  = (uint32_t)(frames * channels * sizeof(float));
    const uint32_t byteRate   = kSampleRate * channels * (uint32_t)sizeof(float);
    const uint16_t blockAlign = channels * (uint16_t)sizeof(float);

    auto u16 = [&](uint16_t v) { out.write((const char*)&v, 2); };
    auto u32 = [&](uint32_t v) { out.write((const char*)&v, 4); };

    out.write("RIFF", 4); u32(36 + dataBytes); out.write("WAVE", 4);
    out.write("fmt ", 4); u32(16);
    u16(3);   // IEEE float
    u16(channels);
    u32(kSampleRate);
    u32(byteRate);
    u16(blockAlign);
    u16(32);
    out.write("data", 4); u32(dataBytes);
    for (size_t i = 0; i < frames; i++) {
        out.write((const char*)&left[i], sizeof(float));
        if (right) out.write((const char*)&right[i], sizeof(float));
    }
    return out.good();
}

static void captureStart(AudioEngine* eng) {
    Capture& c = eng->capture;
    std::lock_guard<std::mutex> lock(c.controlMutex);
    if (c.active.load(std::memory_order_relaxed)) return;

    const size_t maxSamples = (size_t)Capture::kMaxSeconds * kSampleRate;
    c.mic.assign(maxSamples, 0.0f);
    c.outL.assign(maxSamples, 0.0f);
    c.outR.assign(maxSamples, 0.0f);
    c.frames.assign(maxSamples / kPitchHopSize + 16, Capture::FrameRow{});
    c.midi.assign(4096, Capture::MidiRow{});
    c.effects.assign(4096, Capture::EffectRow{});
    c.audioLen.store(0, std::memory_order_relaxed);
    c.frameLen.store(0, std::memory_order_relaxed);
    c.midiLen.store(0, std::memory_order_relaxed);
    c.effectLen.store(0, std::memory_order_relaxed);
    c.startClock = eng->sampleClock.load(std::memory_order_relaxed);
    c.active.store(true, std::memory_order_release);
}

// Returns the capture directory, or "" if nothing was captured.
static std::string captureStop(AudioEngine* eng) {
    Capture& c = eng->capture;
    std::lock_guard<std::mutex> lock(c.controlMutex);
    if (!c.active.load(std::memory_order_relaxed)) return "";
    c.active.store(false);
    // Let in-flight audio-callback appends finish before serializing.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", std::localtime(&now));
    std::string dir = std::string("captures/cap_") + stamp;
    std::error_code directoryError;
    std::filesystem::create_directories(dir, directoryError);
    if (directoryError) return "";

    size_t nAudio  = c.audioLen.load(std::memory_order_acquire);
    size_t nFrames = c.frameLen.load(std::memory_order_acquire);
    size_t nMidi   = c.midiLen.load(std::memory_order_acquire);
    size_t nEffects = c.effectLen.load(std::memory_order_acquire);

    writeWavFloat(dir + "/mic.wav", c.mic.data(), nullptr, nAudio);
    writeWavFloat(dir + "/output.wav", c.outL.data(), c.outR.data(), nAudio);

    auto relTime = [&](uint64_t sample) {
        return sample >= c.startClock ? (double)(sample - c.startClock) / kSampleRate : 0.0;
    };
    {
        std::ofstream f(dir + "/frames.csv");
        f << std::fixed << std::setprecision(6)
          << "time,rms,raw_hz,folded_hz,median_hz,smoothed_midi,correction_midi,"
             "predicted_early_midi,predicted_quality_midi,pitch_slope_st_per_s,"
             "predictor_confidence,parallel_handoff,stable,voiced,predictor_voiced\n";
        for (size_t i = 0; i < nFrames; i++) {
            const Capture::FrameRow& r = c.frames[i];
            f << relTime(r.sample) << ',' << r.rms << ',' << r.rawHz << ','
              << r.foldedHz << ',' << r.medianHz << ',' << r.smoothedMidi << ','
              << r.correctionMidi << ',' << r.predictedEarlyMidi << ','
              << r.predictedQualityMidi << ',' << r.pitchSlope << ','
              << r.predictorConfidence << ',' << r.parallelHandoff << ','
              << (int)r.stable << ',' << (int)r.voiced << ','
              << (int)r.predictorVoiced << '\n';
        }
    }
    {
        std::ofstream f(dir + "/midi.csv");
        f << std::fixed << std::setprecision(6) << "time,event,note\n";
        for (size_t i = 0; i < nMidi; i++) {
            const Capture::MidiRow& r = c.midi[i];
            f << relTime(r.sample) << ',' << (r.on ? "on" : "off") << ','
              << (int)r.note << '\n';
        }
    }
    {
        std::lock_guard<std::mutex> effectLock(c.effectMutex);
        std::ofstream f(dir + "/effects.csv");
        f << std::fixed << std::setprecision(6) << "time,event,slot\n";
        for (size_t i = 0; i < nEffects; i++) {
            const Capture::EffectRow& r = c.effects[i];
            const char* event = r.clear ? "clear" : r.held ? "on" : "off";
            f << relTime(r.sample) << ',' << event << ',' << (int)r.slot << '\n';
        }
    }
    {
        std::ofstream f(dir + "/meta.json");
        f << "{\"sampleRate\":" << kSampleRate
          << ",\"seconds\":" << std::fixed << std::setprecision(3)
          << (double)nAudio / kSampleRate
          << ",\"audioInput\":" << jsonString(gAudioInputName)
          << ",\"audioOutput\":" << jsonString(gAudioOutputName)
          << "," << controlsJson(eng) << "}\n";
    }

    // Release the ~60 MB of capture buffers.
    c.mic.clear();    c.mic.shrink_to_fit();
    c.outL.clear();   c.outL.shrink_to_fit();
    c.outR.clear();   c.outR.shrink_to_fit();
    c.frames.clear(); c.frames.shrink_to_fit();
    c.midi.clear();   c.midi.shrink_to_fit();
    c.effects.clear(); c.effects.shrink_to_fit();
    return dir;
}

static std::string captureStatusJson(AudioEngine* eng, const std::string& dir = "") {
    std::ostringstream out;
    out << "{\"capturing\":" << (eng->capture.active.load(std::memory_order_relaxed) ? "true" : "false")
        << ",\"seconds\":" << std::fixed << std::setprecision(1)
        << (double)eng->capture.audioLen.load(std::memory_order_relaxed) / kSampleRate;
    if (!dir.empty()) out << ",\"dir\":" << jsonString(dir);
    out << "}\n";
    return out.str();
}

class HttpServer {
public:
    HttpServer(AudioEngine* engine, int port) : engine_(engine), port_(port) {}

    bool start() {
        if (!initializeSockets()) {
            std::cerr << "HTTP socket initialization failed: " << socketErrorText() << "\n";
            return false;
        }
        socketsInitialized_ = true;
        serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd_ == kInvalidSocket) {
            std::cerr << "HTTP socket failed: " << socketErrorText() << "\n";
            shutdownSockets();
            socketsInitialized_ = false;
            return false;
        }

        int yes = 1;
#if defined(_WIN32)
        setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
        setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port_);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (::bind(serverFd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "HTTP bind failed: " << socketErrorText() << "\n";
            closeSocket(serverFd_);
            serverFd_ = kInvalidSocket;
            shutdownSockets();
            socketsInitialized_ = false;
            return false;
        }
        if (::listen(serverFd_, 16) < 0) {
            std::cerr << "HTTP listen failed: " << socketErrorText() << "\n";
            closeSocket(serverFd_);
            serverFd_ = kInvalidSocket;
            shutdownSockets();
            socketsInitialized_ = false;
            return false;
        }

        running_.store(true);
        thread_ = std::thread([this] { acceptLoop(); });
        return true;
    }

    void stop() {
        running_.store(false);
        if (serverFd_ != kInvalidSocket) {
            shutdownSocket(serverFd_);
            closeSocket(serverFd_);
            serverFd_ = kInvalidSocket;
        }
        if (thread_.joinable()) thread_.join();
        for (int attempt = 0; attempt < 100 && activeClients_.load() > 0; attempt++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (socketsInitialized_) {
            shutdownSockets();
            socketsInitialized_ = false;
        }
    }

private:
    AudioEngine* engine_ = nullptr;
    int port_ = kDefaultWebPort;
    SocketHandle serverFd_ = kInvalidSocket;
    bool socketsInitialized_ = false;
    std::atomic<bool> running_{false};
    std::atomic<int> activeClients_{0};
    std::thread thread_;

    void acceptLoop() {
        while (running_.load()) {
            SocketHandle client = ::accept(serverFd_, nullptr, nullptr);
            if (client == kInvalidSocket) {
                if (running_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            activeClients_.fetch_add(1);
            std::thread([this, client] {
                handleClient(client);
                activeClients_.fetch_sub(1);
            }).detach();
        }
    }

    void handleEvents(SocketHandle client) {
        std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n";
        if (!sendAll(client, header)) return;

        while (gRunning.load() && running_.load()) {
            std::string event = "event: state\ndata: " + stateJson(engine_) + "\n\n";
            if (!sendAll(client, event)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void handleClient(SocketHandle client) {
        char buffer[8192] = {};
        const int n = ::recv(client, buffer, static_cast<int>(sizeof(buffer) - 1), 0);
        if (n <= 0) { closeSocket(client); return; }

        std::istringstream req(std::string(buffer, (size_t)n));
        std::string method, target, version;
        req >> method >> target >> version;
        if (method != "GET") {
            sendAll(client, response("405 Method Not Allowed", "text/plain", "GET only\n"));
            closeSocket(client);
            return;
        }

        size_t q = target.find('?');
        std::string path = target.substr(0, q);
        std::string query = (q == std::string::npos) ? "" : target.substr(q + 1);

        if (path == "/events") {
            handleEvents(client);
            closeSocket(client);
            return;
        }

        if (path == "/" || path == "/index.html") {
            std::string html = readTextFile(kWebIndexPath);
            if (html.empty()) html = "<!doctype html><title>Harmonizer</title><p>Missing web/index.html</p>";
            sendAll(client, response("200 OK", "text/html; charset=utf-8", html));
        } else if (path == "/health") {
            std::string audioError;
            {
                std::lock_guard<std::mutex> lock(gAudioMutex);
                audioError = gAudioError;
            }
            sendAll(client, response("200 OK", "application/json",
                std::string("{\"ok\":true,\"audioReady\":") +
                (gAudioReady.load(std::memory_order_relaxed) ? "true" : "false") +
                ",\"audioError\":" + jsonString(audioError) +
                ",\"midiReady\":" + (gMidiReady.load(std::memory_order_relaxed) ? "true" : "false") +
                ",\"midiError\":" + jsonString(gMidiError) + "}\n"));
        } else if (path == "/api/state") {
            sendAll(client, response("200 OK", "application/json", stateJson(engine_) + "\n"));
        } else if (path == "/api/backends") {
            sendAll(client, response("200 OK", "application/json", backendsJson() + "\n"));
        } else if (path == "/api/backend") {
            std::string key = queryString(query, "name");
            std::string clientInstance = queryString(query, "instance");
            const BackendSpec* backend = findBackend(key);
            if (clientInstance != gServerInstance) {
                sendAll(client, response("409 Conflict", "application/json",
                                         "{\"error\":\"stale-client\"}\n"));
            } else if (!backend) {
                sendAll(client, response("400 Bad Request", "application/json",
                                         "{\"error\":\"unknown-backend\"}\n"));
            } else if (key == HARMONIZER_BACKEND_KEY) {
                sendAll(client, response("200 OK", "application/json",
                    std::string("{\"ok\":true,\"switching\":false,\"backend\":") +
                    jsonString(key) + "}\n"));
            } else if (engine_->capture.active.load(std::memory_order_relaxed)) {
                sendAll(client, response("409 Conflict", "application/json",
                                         "{\"error\":\"capture-active\"}\n"));
            } else if (resolveBackendExecutable(*backend).empty()) {
                sendAll(client, response("409 Conflict", "application/json",
                                         "{\"error\":\"backend-unavailable\"}\n"));
            } else {
                {
                    std::lock_guard<std::mutex> lock(gBackendMutex);
                    gRequestedBackend = key;
                }
                sendAll(client, response("200 OK", "application/json",
                    std::string("{\"ok\":true,\"switching\":true,\"backend\":") +
                    jsonString(key) + "}\n"));
                gRunning.store(false);
            }
        } else if (path == "/api/audio-inputs") {
            sendAll(client, response("200 OK", "application/json", audioInputsJson() + "\n"));
        } else if (path == "/api/audio-input") {
            float device = -1.0f;
            if (!queryFloat(query, "device", device)) {
                sendAll(client, response("400 Bad Request", "application/json",
                                         "{\"error\":\"missing-device\"}\n"));
            } else {
                const bool switched = switchAudioInput(
                    engine_, (PaDeviceIndex)std::lround(device));
                sendAll(client, response(switched ? "200 OK" : "409 Conflict",
                                         "application/json", stateJson(engine_) + "\n"));
            }
        } else if (path == "/api/audio-outputs") {
            sendAll(client, response("200 OK", "application/json", audioOutputsJson() + "\n"));
        } else if (path == "/api/audio-output") {
            float device = -1.0f;
            if (!queryFloat(query, "device", device)) {
                sendAll(client, response("400 Bad Request", "application/json",
                                         "{\"error\":\"missing-device\"}\n"));
            } else {
                const bool switched = switchAudioOutput(
                    engine_, (PaDeviceIndex)std::lround(device));
                sendAll(client, response(switched ? "200 OK" : "409 Conflict",
                                         "application/json", stateJson(engine_) + "\n"));
            }
        } else if (path == "/api/test-tone") {
            startTestTone(engine_);
            sendAll(client, response("200 OK", "application/json", stateJson(engine_) + "\n"));
        } else if (path == "/api/control") {
            applyControls(engine_, query);
            sendAll(client, response("200 OK", "application/json", stateJson(engine_) + "\n"));
        } else if (path == "/api/freeze") {
            if (!applyFreezeControl(engine_, query)) {
                sendAll(client, response("400 Bad Request", "application/json",
                                         "{\"error\":\"invalid-freeze-control\"}\n"));
            } else {
                sendAll(client, response("200 OK", "application/json",
                                         stateJson(engine_) + "\n"));
            }
        } else if (path == "/api/midi") {
            applyBrowserMidi(engine_, query);
            sendAll(client, response("200 OK", "application/json", stateJson(engine_) + "\n"));
        } else if (path == "/api/midi-status") {
            applyBrowserMidiStatus(query);
            sendAll(client, response("200 OK", "application/json", stateJson(engine_) + "\n"));
        } else if (path == "/api/capture") {
            std::string action = queryString(query, "action");
            if (action == "start") {
                captureStart(engine_);
                sendAll(client, response("200 OK", "application/json", captureStatusJson(engine_)));
            } else if (action == "stop") {
                std::string dir = captureStop(engine_);
                sendAll(client, response("200 OK", "application/json", captureStatusJson(engine_, dir)));
            } else {
                sendAll(client, response("200 OK", "application/json", captureStatusJson(engine_)));
            }
        } else {
            sendAll(client, response("404 Not Found", "text/plain", "not found\n"));
        }
        closeSocket(client);
    }
};

// ── Offline render (replay a capture through the exact engine) ─────────────

static bool readWavFloatMono(const std::string& path, std::vector<float>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string bytes = buf.str();
    size_t pos = bytes.find("data");
    if (pos == std::string::npos || pos + 8 > bytes.size()) return false;
    // Capture WAVs are written by writeWavFloat: mono float32.
    const float* samples = (const float*)(bytes.data() + pos + 8);
    size_t n = (bytes.size() - pos - 8) / sizeof(float);
    out.assign(samples, samples + n);
    return true;
}

static int runRender(const std::string& capDir) {
    std::vector<float> mic;
    if (!readWavFloatMono(capDir + "/mic.wav", mic)) {
        std::cerr << "render: cannot read " << capDir << "/mic.wav\n";
        return 1;
    }

    struct Event { size_t sample; bool on; int note; };
    std::vector<Event> events;
    {
        std::ifstream f(capDir + "/midi.csv");
        std::string line;
        std::getline(f, line);   // header
        while (std::getline(f, line)) {
            std::istringstream row(line);
            std::string t, ev, note;
            if (!std::getline(row, t, ',') || !std::getline(row, ev, ',') ||
                !std::getline(row, note, ',')) continue;
            events.push_back({ (size_t)(std::strtod(t.c_str(), nullptr) * kSampleRate),
                               ev == "on", std::atoi(note.c_str()) });
        }
    }

    struct EffectEvent { size_t sample; int slot; bool held; bool clear; };
    std::vector<EffectEvent> effectEvents;
    {
        std::ifstream f(capDir + "/effects.csv");
        std::string line;
        std::getline(f, line);   // header
        while (std::getline(f, line)) {
            std::istringstream row(line);
            std::string time, event, slot;
            if (!std::getline(row, time, ',') || !std::getline(row, event, ',') ||
                !std::getline(row, slot, ',')) continue;
            effectEvents.push_back({
                (size_t)(std::strtod(time.c_str(), nullptr) * kSampleRate),
                std::atoi(slot.c_str()), event == "on", event == "clear" });
        }
    }

    auto* eng = new AudioEngine();
    eng->init();

    // Restore the controls the capture was made with.
    {
        std::string meta = readTextFile((capDir + "/meta.json").c_str());
        auto metaFloat = [&](const char* key, float fallback) {
            size_t p = meta.find(std::string("\"") + key + "\":");
            return p == std::string::npos
                ? fallback
                : std::strtof(meta.c_str() + p + std::strlen(key) + 3, nullptr);
        };
        auto metaString = [&](const char* key, const char* fallback) {
            std::string marker = std::string("\"") + key + "\":";
            size_t p = meta.find(marker);
            if (p == std::string::npos) return std::string(fallback);
            p += marker.size();
            while (p < meta.size() && std::isspace((unsigned char)meta[p])) p++;
            if (p >= meta.size() || meta[p] != '"') return std::string(fallback);
            size_t end = meta.find('"', p + 1);
            return end == std::string::npos
                ? std::string(fallback)
                : meta.substr(p + 1, end - p - 1);
        };
        eng->display.wetDryBalance.store(metaFloat("mix", kDefaultWetDryBalance));
        eng->display.monitorGainDb.store(metaFloat("gainDb", kDefaultMonitorGainDb));
        eng->display.voicedGateRms.store(metaFloat("gate", kDefaultVoicedGateRms));
        eng->display.stableSemitoneWindow.store(metaFloat("stableWindow", kDefaultStableSemitoneWindow));
        eng->display.parallelEarlyBlend.store(metaFloat("immediacy", kDefaultParallelEarlyBlend));
        eng->display.glideAmount.store(metaFloat("glideAmount", kDefaultGlideAmount));
        eng->display.glideTimeMs.store(metaFloat("glideTimeMs", kDefaultGlideTimeMs));
        eng->display.chorusMix.store(metaFloat("chorusMix", kDefaultChorusMix));
        eng->display.reverbMix.store(metaFloat("reverbMix", kDefaultReverbMix));
        eng->display.freezeTone.store(metaFloat("freezeTone", kDefaultFreezeTone));
        for (size_t layer = 0; layer < harmonizer::kFreezeLayerCount; ++layer) {
            const std::string prefix = "freeze" + std::to_string(layer + 1);
            eng->display.freezeLevel[layer].store(
                metaFloat((prefix + "Level").c_str(), kDefaultFreezeLevel));
            eng->display.freezeTranspose[layer].store(
                metaFloat((prefix + "Transpose").c_str(), kDefaultFreezeTranspose));
        }
        eng->display.unvoicedMode.store(unvoicedModeFromKey(
            metaString("unvoicedMode", unvoicedModeKey(static_cast<int>(kDefaultUnvoicedMode)))));
#ifdef HARMONIZER_CUSTOM_PITCH_CONTROL
        eng->configurePitchExperiments(
            metaFloat("flutterCompensation", 0.0f),
            (int)std::lround(metaFloat(
                "earlyPitchOffsetSamples",
                (float)AudioEngine::kDefaultEarlyPitchOffsetSamples)),
            metaFloat("parallelEarlyPersistent", 0.0f) >= 0.5f);
#endif
    }

    std::vector<float> outL(mic.size()), outR(mic.size());
    size_t ev = 0;
    size_t effectEvent = 0;
    for (size_t n = 0; n < mic.size(); n++) {
        while (ev < events.size() && events[ev].sample <= n) {
            if (events[ev].on) handleNoteOn(eng, events[ev].note);
            else               handleNoteOff(eng, events[ev].note);
            ev++;
        }
        while (effectEvent < effectEvents.size() &&
               effectEvents[effectEvent].sample <= n) {
            const EffectEvent& event = effectEvents[effectEvent];
            setFreezeHeld(eng, event.slot, event.held, event.clear);
            effectEvent++;
        }
        processSample(eng, mic[n], outL[n], outR[n]);
    }

    std::string outPath = capDir + "/render.wav";
    bool ok = writeWavFloat(outPath, outL.data(), outR.data(), mic.size());
    std::cerr << "render: " << (ok ? "wrote " : "FAILED writing ") << outPath
              << " (" << (double)mic.size() / kSampleRate << " s, "
              << events.size() << " midi events, " << effectEvents.size()
              << " effect events; DSP block max "
              << eng->display.dspBlockMaxMs.load(std::memory_order_relaxed)
              << " ms; acceptedHz " << eng->detector.lastAcceptedHz << ")\n";
    delete eng;
    return ok ? 0 : 1;
}

// ── Signal ─────────────────────────────────────────────────────────────────

static void onSignal(int) { gRunning.store(false); }

// ── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    std::error_code executableError;
    gExecutablePath = std::filesystem::weakly_canonical(
        std::filesystem::absolute(argv[0], executableError), executableError);
    if (executableError) gExecutablePath = argv[0];
    gServerInstance = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());

    int webPort = kDefaultWebPort;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            webPort = std::atoi(argv[++i]);
        } else if (arg == "--render" && i + 1 < argc) {
            // Offline: replay captures/cap_<stamp>/ through the engine,
            // write render.wav next to it. No audio device, no server.
            return runRender(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: " << argv[0] << " [--port 8794] [--render <capture-dir>]\n";
            return 0;
        }
    }

    auto* engine = new AudioEngine();
    engine->init();
    initializeAudio(engine);

    gMidiError = "browser-midi-pending";

    HttpServer server(engine, webPort);
    if (!server.start()) {
        std::cerr << "HTTP server failed on 127.0.0.1:" << webPort << "\n";
        shutdownAudio();
        delete engine;
        return 1;
    }

    std::cerr << "\n"
        "=============================================\n"
        "  Harmonizer Web (" HARMONIZER_DSP_TITLE ")\n"
        "  http://127.0.0.1:" << webPort << "/\n"
        "  Sing into mic + use Web MIDI in the browser\n"
        "  Browser controls: blend | gate | stability\n"
        "  Ctrl+C to quit\n"
        "=============================================\n\n";

    while (gRunning.load()) Pa_Sleep(50);

    server.stop();
    shutdownAudio();
    delete engine;
    return relaunchBackend(webPort);
}

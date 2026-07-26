/*
 * Real-time Vocal Harmonizer (Jacob Collier–style)
 *
 * Mono mic in → pitch detection + MIDI-driven pitch shifting → stereo out
 *
 * Libraries: PortAudio, Rubber Band (LiveShifter), aubio, RtMidi
 *
 * Build (macOS, Homebrew):
 *   clang++ -O3 -std=c++17 harmonizer_midi.cpp \
 *     -I/opt/homebrew/include -L/opt/homebrew/lib \
 *     -lportaudio -lrubberband -laubio -lrtmidi -lpthread \
 *     -framework CoreAudio -framework CoreMIDI -framework CoreFoundation \
 *     -o harmonizer
 *
 * Features:
 *   - 16-voice polyphonic harmonizer with formant preservation
 *   - Stereo output: low notes center, high notes wide (M/S style)
 *   - Per-voice attack/release envelope (MIDI-gated)
 *   - Sustain pedal (CC 64), pitch bend (±2 semitones)
 *   - Optional portamento/glide
 *   - Voice stealing with proper release tails
 *   - Soft clipping to prevent output overload
 */

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

#include <portaudio.h>
#include <pa_mac_core.h>
#include <rubberband/RubberBandLiveShifter.h>
#include <aubio/aubio.h>
#include <rtmidi/RtMidi.h>

using RubberBand::RubberBandLiveShifter;

// ═══════════════════════════════════════════════════════════════════════════
//  Configuration — tweak these to taste
// ═══════════════════════════════════════════════════════════════════════════

static constexpr int   kSampleRate      = 44100;
static constexpr int   kFramesPerBuffer = 64;       // PortAudio callback size
static constexpr int   kMaxVoices       = 16;

// Pitch detection (aubio)
static constexpr int   kPitchWinSize    = 2048;
static constexpr int   kPitchHopSize    = 512;

// Envelope
static constexpr float kAttackSec       = 0.005f;   // 5 ms attack
static constexpr float kReleaseSec      = 0.080f;   // 80 ms release

// Portamento (set kGlideEnabled = true to activate)
static constexpr bool  kGlideEnabled    = false;
static constexpr float kGlideTimeSec    = 0.060f;   // 90% travel time

// Gains
static constexpr float kDryGain         = 0.55f;
static constexpr float kWetPerVoice     = 0.22f;    // per-voice wet gain
static constexpr float kPitchGateRms    = 0.0100f;

// Pitch bend range in semitones
static constexpr float kBendRange       = 2.0f;

// ═══════════════════════════════════════════════════════════════════════════

static std::atomic<bool> gRunning{true};

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

// ── Pitch Detector (aubio) ─────────────────────────────────────────────────

struct PitchDetector {
    aubio_pitch_t* au  = nullptr;
    fvec_t*        in  = nullptr;
    fvec_t*        out = nullptr;

    PitchDetector() {
        au = new_aubio_pitch("yinfft", kPitchWinSize, kPitchHopSize, kSampleRate);
        aubio_pitch_set_unit(au, "Hz");
        aubio_pitch_set_silence(au, -50.0f);
        aubio_pitch_set_tolerance(au, 0.80f);
        in  = new_fvec(kPitchHopSize);
        out = new_fvec(1);
    }

    ~PitchDetector() {
        if (au)  del_aubio_pitch(au);
        if (in)  del_fvec(in);
        if (out) del_fvec(out);
    }

    float detect(const float* samples) {
        float rms = 0.0f;
        for (int i = 0; i < kPitchHopSize; i++) {
            in->data[i] = samples[i];
            rms += samples[i] * samples[i];
        }
        if (std::sqrt(rms / kPitchHopSize) < kPitchGateRms) return -1.0f;

        aubio_pitch_do(au, in, out);
        float f = fvec_get_sample(out, 0);
        if (f <= 0.0f) return -1.0f;
        if (f < noteToFreq(36.0f) || f > noteToFreq(84.0f)) return -1.0f;
        return f;
    }
};

// ── Voice ──────────────────────────────────────────────────────────────────

struct Voice {
    RubberBandLiveShifter rb;

    // Written by MIDI thread (via atomics)
    std::atomic<bool>     gateOn{false};
    std::atomic<int>      midiNote{-1};
    std::atomic<uint64_t> stamp{0};

    // Audio thread only
    float envelope     = 0.0f;
    float currentRatio = 1.0f;
    float targetRatio  = 1.0f;
    float panL         = 0.707f;
    float panR         = 0.707f;
    bool  sustained    = false;

    // Precomputed
    float attackCoeff  = 0.0f;
    float releaseCoeff = 0.0f;
    float glideCoeff   = 1.0f;  // 1.0 = instant

    Voice()
        : rb(kSampleRate, 1, 0,
             RubberBandLiveShifter::OptionFormantPreserved |
             RubberBandLiveShifter::OptionWindowShort)
    {
        attackCoeff  = 1.0f - std::exp(-1.0f / (kAttackSec  * kSampleRate));
        releaseCoeff = 1.0f - std::exp(-1.0f / (kReleaseSec * kSampleRate));
        if (kGlideEnabled && kGlideTimeSec > 0.0f) {
            glideCoeff = 1.0f - std::pow(0.1f, 1.0f / (kGlideTimeSec * kSampleRate));
        }
    }

    bool isAudible() const {
        return gateOn.load(std::memory_order_relaxed) || envelope > 0.001f;
    }

    float tickEnvelope() {
        float tgt = gateOn.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        float c = (tgt > envelope) ? attackCoeff : releaseCoeff;
        envelope += (tgt - envelope) * c;
        if (envelope < 0.0001f) envelope = 0.0f;
        return envelope;
    }

    void tickGlide() {
        if (std::fabs(currentRatio - targetRatio) > 1e-5f)
            currentRatio += (targetRatio - currentRatio) * glideCoeff;
        else
            currentRatio = targetRatio;
    }

    void updatePan() {
        int n = midiNote.load(std::memory_order_relaxed);
        if (n < 0) return;
        // Low notes (≤C3) → center, high notes (≥C5) → wide
        float spread = clampf((float)(n - 48) / 24.0f, 0.0f, 1.0f);
        // Alternate sides for stereo width
        float side  = (n % 2 == 0) ? -1.0f : 1.0f;
        float angle = side * spread;
        // Equal-power panning
        float theta = (angle + 1.0f) * 0.25f * (float)M_PI;
        panL = std::cos(theta);
        panR = std::sin(theta);
    }
};

// ── Audio Engine ───────────────────────────────────────────────────────────

struct AudioEngine {
    PitchDetector detector;
    Voice         voices[kMaxVoices];
    size_t        blockSize = 0;

    // Buffers (sized to RubberBand block size)
    float* inputBuf  = nullptr;
    float* outputL   = nullptr;
    float* outputR   = nullptr;
    float* shiftBuf  = nullptr;
    int    bufPos    = 0;

    // Pitch detection accumulator (runs at its own cadence)
    float  pitchBuf[kPitchHopSize] = {};
    int    pitchPos       = 0;
    float  detectedMidi   = -1.0f;
    float  prevDetectedMidi = -1.0f;
    bool   pitchStable    = false;

    // MIDI state
    std::mutex           midiMutex;
    bool                 sustainOn = false;
    uint64_t             noteCounter = 0;
    std::atomic<float>   pitchBendSemitones{0.0f};

    void init() {
        blockSize = voices[0].rb.getBlockSize();
        inputBuf  = new float[blockSize]();
        outputL   = new float[blockSize]();
        outputR   = new float[blockSize]();
        shiftBuf  = new float[blockSize]();

        std::cerr << "Rubber Band block size: " << blockSize
                  << " (" << (blockSize * 1000.0 / kSampleRate) << " ms)\n";
    }

    ~AudioEngine() {
        delete[] inputBuf;
        delete[] outputL;
        delete[] outputR;
        delete[] shiftBuf;
    }

    // Called when inputBuf has a full block ready
    void processBlock() {
        // Dry signal → center
        for (size_t i = 0; i < blockSize; i++) {
            outputL[i] = inputBuf[i] * kDryGain;
            outputR[i] = inputBuf[i] * kDryGain;
        }

        float bend = pitchBendSemitones.load(std::memory_order_relaxed);

        for (int v = 0; v < kMaxVoices; v++) {
            if (!voices[v].isAudible()) continue;

            // Update target pitch ratio when pitch is stable
            int note = voices[v].midiNote.load(std::memory_order_relaxed);
            if (note > 0 && detectedMidi > 0.0f && pitchStable) {
                float target = (float)note + bend;
                float r = noteToFreq(target) / noteToFreq(detectedMidi);
                voices[v].targetRatio = clampf(r, 0.25f, 4.0f);
            }

            // Advance glide for the block
            for (size_t s = 0; s < blockSize; s++) voices[v].tickGlide();

            voices[v].rb.setPitchScale((double)voices[v].currentRatio);

            // Pitch-shift the block
            float* inArr[]  = { inputBuf };
            float* outArr[] = { shiftBuf };
            voices[v].rb.shift(inArr, outArr);

            // Mix with per-sample envelope and stereo panning
            voices[v].updatePan();
            float pL = voices[v].panL;
            float pR = voices[v].panR;
            for (size_t i = 0; i < blockSize; i++) {
                float env = voices[v].tickEnvelope();
                float s = shiftBuf[i] * env * kWetPerVoice;
                outputL[i] += s * pL;
                outputR[i] += s * pR;
            }
        }

        // Soft clip to prevent overload
        for (size_t i = 0; i < blockSize; i++) {
            outputL[i] = std::tanh(outputL[i]);
            outputR[i] = std::tanh(outputR[i]);
        }
    }
};

// ── PortAudio Callback ─────────────────────────────────────────────────────

static int audioCallback(const void* input, void* output,
                         unsigned long frames,
                         const PaStreamCallbackTimeInfo*,
                         PaStreamCallbackFlags,
                         void* userData)
{
    auto* eng       = static_cast<AudioEngine*>(userData);
    const float* mic = static_cast<const float*>(input);
    float* stereo   = static_cast<float*>(output);
    const int blk   = (int)eng->blockSize;

    if (!mic) {
        std::memset(stereo, 0, sizeof(float) * frames * 2);
        return paContinue;
    }

    for (unsigned long i = 0; i < frames; i++) {
        float sample = mic[i];

        // ── Feed pitch detector ──
        eng->pitchBuf[eng->pitchPos++] = sample;
        if (eng->pitchPos >= kPitchHopSize) {
            float freq = eng->detector.detect(eng->pitchBuf);
            float midi = freqToMidi(freq);
            eng->prevDetectedMidi = eng->detectedMidi;
            eng->detectedMidi = midi;
            eng->pitchStable = (midi > 0.0f && eng->prevDetectedMidi > 0.0f &&
                                std::fabs(midi - eng->prevDetectedMidi) < 0.6f);
            eng->pitchPos = 0;
        }

        // ── Accumulate input, read previous output ──
        eng->inputBuf[eng->bufPos] = sample;
        stereo[i * 2]     = eng->outputL[eng->bufPos];
        stereo[i * 2 + 1] = eng->outputR[eng->bufPos];
        eng->bufPos++;

        if (eng->bufPos >= blk) {
            eng->processBlock();
            eng->bufPos = 0;
        }
    }

    return paContinue;
}

// ── MIDI ───────────────────────────────────────────────────────────────────

static inline bool isNoteOn(const std::vector<unsigned char>& m) {
    return m.size() >= 3 && (m[0] & 0xF0) == 0x90 && m[2] != 0;
}

static inline bool isNoteOff(const std::vector<unsigned char>& m) {
    return m.size() >= 3 &&
           ((m[0] & 0xF0) == 0x80 || ((m[0] & 0xF0) == 0x90 && m[2] == 0));
}

static inline bool isCC(const std::vector<unsigned char>& m, int cc) {
    return m.size() >= 3 && (m[0] & 0xF0) == 0xB0 && m[1] == cc;
}

static inline bool isPitchBend(const std::vector<unsigned char>& m) {
    return m.size() >= 3 && (m[0] & 0xF0) == 0xE0;
}

static void handleNoteOn(AudioEngine* eng, int note) {
    std::lock_guard<std::mutex> lock(eng->midiMutex);
    uint64_t stamp = ++eng->noteCounter;

    // 1. Free voice (not audible)
    int chosen = -1;
    for (int v = 0; v < kMaxVoices; v++) {
        if (!eng->voices[v].isAudible()) { chosen = v; break; }
    }

    // 2. Voice in release phase (gate off, still fading)
    if (chosen < 0) {
        float lowestEnv = 999.0f;
        for (int v = 0; v < kMaxVoices; v++) {
            if (!eng->voices[v].gateOn.load(std::memory_order_relaxed) &&
                eng->voices[v].envelope < lowestEnv) {
                lowestEnv = eng->voices[v].envelope;
                chosen = v;
            }
        }
    }

    // 3. Steal oldest active voice
    if (chosen < 0) {
        uint64_t oldest = UINT64_MAX;
        for (int v = 0; v < kMaxVoices; v++) {
            uint64_t s = eng->voices[v].stamp.load(std::memory_order_relaxed);
            if (s < oldest) { oldest = s; chosen = v; }
        }
    }

    if (chosen >= 0) {
        eng->voices[chosen].midiNote.store(note, std::memory_order_relaxed);
        eng->voices[chosen].gateOn.store(true, std::memory_order_relaxed);
        eng->voices[chosen].stamp.store(stamp, std::memory_order_relaxed);
        eng->voices[chosen].sustained = false;
    }
}

static void handleNoteOff(AudioEngine* eng, int note) {
    std::lock_guard<std::mutex> lock(eng->midiMutex);
    for (int v = 0; v < kMaxVoices; v++) {
        if (eng->voices[v].midiNote.load(std::memory_order_relaxed) == note &&
            eng->voices[v].gateOn.load(std::memory_order_relaxed)) {
            if (eng->sustainOn) {
                eng->voices[v].sustained = true;
            } else {
                eng->voices[v].gateOn.store(false, std::memory_order_relaxed);
            }
        }
    }
}

static void handleSustainOff(AudioEngine* eng) {
    for (int v = 0; v < kMaxVoices; v++) {
        if (eng->voices[v].sustained) {
            eng->voices[v].gateOn.store(false, std::memory_order_relaxed);
            eng->voices[v].sustained = false;
        }
    }
}

static void midiCallback(double, std::vector<unsigned char>* msg, void* ud) {
    auto* eng = static_cast<AudioEngine*>(ud);
    if (!msg || msg->empty()) return;

    if (isNoteOn(*msg)) {
        handleNoteOn(eng, (*msg)[1]);
    } else if (isNoteOff(*msg)) {
        handleNoteOff(eng, (*msg)[1]);
    } else if (isCC(*msg, 64)) {
        bool on = (*msg)[2] >= 64;
        eng->sustainOn = on;
        if (!on) handleSustainOff(eng);
    } else if (isPitchBend(*msg)) {
        int raw = ((*msg)[2] << 7) | (*msg)[1];
        float semitones = ((float)raw - 8192.0f) / 8192.0f * kBendRange;
        eng->pitchBendSemitones.store(semitones, std::memory_order_relaxed);
    }
}

// ── Signal handler ─────────────────────────────────────────────────────────

static void onSignal(int) { gRunning.store(false); }

// ── Main ───────────────────────────────────────────────────────────────────

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    auto* engine = new AudioEngine();
    engine->init();

    // ── PortAudio ──
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "PortAudio init: " << Pa_GetErrorText(err) << "\n";
        return 1;
    }

    PaStreamParameters inParams{}, outParams{};
    inParams.device       = Pa_GetDefaultInputDevice();
    inParams.channelCount = 1;
    inParams.sampleFormat = paFloat32;
    inParams.suggestedLatency = 0;

    outParams.device       = Pa_GetDefaultOutputDevice();
    outParams.channelCount = 2;    // stereo output
    outParams.sampleFormat = paFloat32;
    outParams.suggestedLatency = 0;

    PaMacCoreStreamInfo macInfo;
    PaMacCore_SetupStreamInfo(&macInfo, paMacCorePro);
    inParams.hostApiSpecificStreamInfo  = &macInfo;
    outParams.hostApiSpecificStreamInfo = &macInfo;

    PaStream* stream = nullptr;
    err = Pa_OpenStream(&stream, &inParams, &outParams,
                        kSampleRate, kFramesPerBuffer,
                        paClipOff | paDitherOff,
                        audioCallback, engine);
    if (err != paNoError) {
        std::cerr << "PortAudio open: " << Pa_GetErrorText(err) << "\n";
        Pa_Terminate();
        return 1;
    }

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        std::cerr << "PortAudio start: " << Pa_GetErrorText(err) << "\n";
        Pa_CloseStream(stream);
        Pa_Terminate();
        return 1;
    }

    // ── MIDI ──
    std::unique_ptr<RtMidiIn> midiIn;
    try {
        midiIn = std::make_unique<RtMidiIn>();
        unsigned int n = midiIn->getPortCount();
        if (n == 0) {
            std::cerr << "No MIDI ports found.\n";
        } else {
            std::cerr << "MIDI ports:\n";
            for (unsigned int i = 0; i < n; i++)
                std::cerr << "  [" << i << "] " << midiIn->getPortName(i) << "\n";
            midiIn->openPort(0);
            midiIn->ignoreTypes(false, false, false);
            midiIn->setCallback(&midiCallback, engine);
            std::cerr << "Opened MIDI port 0.\n";
        }
    } catch (RtMidiError& e) {
        std::cerr << "MIDI error: " << e.getMessage() << "\n";
    }

    std::cerr << "\n"
        "======================================\n"
        "  Harmonizer running - " << kMaxVoices << " voices\n"
        "  Sing into mic + hold MIDI keys\n"
        "  Sustain pedal (CC 64) supported\n"
        "  Pitch bend supported\n"
        "  Ctrl+C to quit\n"
        "======================================\n\n";

    while (gRunning.load()) Pa_Sleep(50);

    std::cerr << "Shutting down...\n";
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    delete engine;
    return 0;
}

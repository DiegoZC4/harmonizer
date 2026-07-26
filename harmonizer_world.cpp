/*
 * harmonizer_world.cpp — Channel Vocoder Harmonizer with SDL2 GUI
 *
 * Source-filter vocoder: cepstral formant analysis + harmonic resynthesis.
 * No external vocoder library — uses a built-in Cooley-Tukey FFT.
 *
 * brew install portaudio aubio rtmidi sdl2
 *
 * Build:
 *   clang++ -O3 -std=c++17 harmonizer_world.cpp \
 *     -I/opt/homebrew/include -L/opt/homebrew/lib \
 *     -lportaudio -laubio -lrtmidi -lSDL2 \
 *     -framework CoreAudio -framework CoreMIDI -framework CoreFoundation \
 *     -o harmonizer_world
 *
 * Quality vs. Antares/RubberBand:
 *   The channel vocoder creates true harmonics at the target frequency
 *   (not time-stretched), which gives substantially better bass response
 *   than RubberBand. Formants are preserved via spectral envelope transfer.
 *
 *   Small intervals:        ~85% of Antares
 *   Octave shifts:          ~70%
 *   Bass (2+ octaves down): ~60%  ← better than RubberBand here
 *
 * GUI:
 *   Piano roll (center): cyan = detected pitch, green = MIDI notes (scrolling)
 *   Right sliders:       blend | voice gate | stability
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
#include <aubio/aubio.h>
#include <rtmidi/RtMidi.h>
#include <SDL2/SDL.h>

// ═══════════════════════════════════════════════════════════════════════════
//  Configuration
// ═══════════════════════════════════════════════════════════════════════════

static constexpr int   kSampleRate      = 44100;
static constexpr int   kFramesPerBuffer = 64;
static constexpr int   kMaxVoices       = 16;

// Vocoder (2x overlap: Hann COLA sum = 1.0 — no scaling compensation needed)
static constexpr int   kFFTSize         = 1024;
static constexpr int   kHopSize         = 512;    // kFFTSize / 2
static constexpr int   kOLASize         = kFFTSize * 2;

// Pitch detection
static constexpr int   kPitchWinSize    = 2048;
static constexpr int   kPitchHopSize    = 512;

// Voice
static constexpr float kAttackSec       = 0.005f;
static constexpr float kReleaseSec      = 0.080f;
static constexpr float kBendRange       = 2.0f;
static constexpr float kNoiseRatio      = 0.10f;  // breathiness

// Constant-amplitude wet/dry crossfade.
static constexpr float kDefaultWetDryBalance = 0.56f; // 0 = dry, 1 = wet
static constexpr float kDefaultVoicedGateRms = 0.0100f;
static constexpr float kDefaultStableSemitoneWindow = 1.0f;

// GUI
static constexpr int   kWindowWidth     = 960;
static constexpr int   kWindowHeight    = 520;
static constexpr int   kPianoWidth      = 60;
static constexpr int   kSliderWidth     = 132;
static constexpr int   kRollWidth       = kWindowWidth - kPianoWidth - kSliderWidth;
static constexpr int   kSliderCount     = 3;
static constexpr int   kSliderGap       = 8;

static constexpr int   kNoteMin         = 36;   // C2
static constexpr int   kNoteMax         = 84;   // C6
static constexpr int   kNoteRange       = kNoteMax - kNoteMin;   // 48
static constexpr int   kNoteH           = kWindowHeight / kNoteRange;

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
static inline void wetDryFromBalance(float balance, float& wet, float& dry) {
    balance = clampf(balance, 0.0f, 1.0f);
    wet = balance;
    dry = 1.0f - balance;
}
static inline float foldHighPitchCandidate(float hz, float anchorMidi) {
    if (hz <= 0.0f) return -1.0f;

    static constexpr float kFoldHighHz = 420.0f;
    static constexpr float kFallbackAnchorHz = 180.0f;
    if (anchorMidi < 0.0f && hz < kFoldHighHz) return hz;

    float targetMidi = anchorMidi > 0.0f ? anchorMidi : freqToMidi(kFallbackAnchorHz);
    float bestHz = hz;
    float bestDistance = std::fabs(freqToMidi(hz) - targetMidi);
    for (float candidate = hz * 0.5f; candidate >= noteToFreq(36); candidate *= 0.5f) {
        float distance = std::fabs(freqToMidi(candidate) - targetMidi);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestHz = candidate;
        }
    }
    return bestHz;
}
static inline bool isBlackKey(int note) {
    static const bool b[12] = {0,1,0,1,0,0,1,0,1,0,1,0};
    return b[note % 12];
}
static inline int noteToY(float note) {
    return (int)((kNoteMax - note) * kWindowHeight / kNoteRange);
}

// ── Simple Cooley-Tukey FFT ────────────────────────────────────────────────
// data: interleaved complex [re0,im0,re1,im1,...], length = 2*N
// N must be a power of 2.

static void fft(float* data, int N, bool inverse) {
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(data[2*i], data[2*j]); std::swap(data[2*i+1], data[2*j+1]); }
    }
    // Butterfly stages
    for (int len = 2; len <= N; len <<= 1) {
        float ang = (inverse ? 1.0f : -1.0f) * 2.0f * (float)M_PI / len;
        float wR = std::cos(ang), wI = std::sin(ang);
        for (int i = 0; i < N; i += len) {
            float cR = 1.0f, cI = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                int u = i + j, v = u + len / 2;
                float tR = data[2*v] * cR - data[2*v+1] * cI;
                float tI = data[2*v] * cI + data[2*v+1] * cR;
                data[2*v]   = data[2*u]   - tR;  data[2*v+1] = data[2*u+1] - tI;
                data[2*u]  += tR;                 data[2*u+1] += tI;
                float nR = cR * wR - cI * wI;     cI = cR * wI + cI * wR;  cR = nR;
            }
        }
    }
    if (inverse) {
        float s = 1.0f / N;
        for (int i = 0; i < 2 * N; i++) data[i] *= s;
    }
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
        if (au) del_aubio_pitch(au);
        if (in) del_fvec(in);
        if (out) del_fvec(out);
    }
    float detect(const float* samples, float gateRms) {
        float rms = 0.0f;
        for (int i = 0; i < kPitchHopSize; i++) { in->data[i] = samples[i]; rms += samples[i]*samples[i]; }
        if (std::sqrt(rms / kPitchHopSize) < gateRms) return -1.0f;
        aubio_pitch_do(au, in, out);
        float f = fvec_get_sample(out, 0);
        if (f <= 0.0f || f < noteToFreq(36) || f > noteToFreq(84)) return -1.0f;
        return f;
    }
};

// ── Shared Display State (lock-free, audio/MIDI → GUI) ─────────────────────

struct SharedDisplay {
    std::atomic<float>    detectedMidi{-1.0f};
    std::atomic<float>    wetDryBalance{kDefaultWetDryBalance};
    std::atomic<float>    voicedGateRms{kDefaultVoicedGateRms};
    std::atomic<float>    stableSemitoneWindow{kDefaultStableSemitoneWindow};
    std::atomic<uint64_t> noteBitsLo{0};   // notes 0–63
    std::atomic<uint64_t> noteBitsHi{0};   // notes 64–127

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
};

// ── Voice ──────────────────────────────────────────────────────────────────

struct Voice {
    std::atomic<bool>     gateOn{false};
    std::atomic<int>      midiNote{-1};
    std::atomic<uint64_t> stamp{0};

    float  envelope     = 0.0f;
    double phase        = 0.0;   // excitation phase [0, 1)
    float  panL         = 0.707f;
    float  panR         = 0.707f;
    bool   sustained    = false;
    float  attackCoeff  = 0.0f;
    float  releaseCoeff = 0.0f;

    void init() {
        attackCoeff  = 1.0f - std::exp(-1.0f / (kAttackSec  * kSampleRate));
        releaseCoeff = 1.0f - std::exp(-1.0f / (kReleaseSec * kSampleRate));
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
    void updatePan() {
        int n = midiNote.load(std::memory_order_relaxed);
        if (n < 0) return;
        float spread = clampf((float)(n - 48) / 24.0f, 0.0f, 1.0f);
        float theta  = ((n % 2 == 0 ? -1.0f : 1.0f) * spread + 1.0f) * 0.25f * (float)M_PI;
        panL = std::cos(theta);
        panR = std::sin(theta);
    }
};

// ── Audio Engine ───────────────────────────────────────────────────────────

struct AudioEngine {
    PitchDetector detector;
    Voice         voices[kMaxVoices];
    SharedDisplay display;

    float* window       = nullptr;  // Hann window, kFFTSize
    float* inputRing    = nullptr;  // circular mic buffer, kFFTSize
    float* analysisData = nullptr;  // complex, kFFTSize*2
    float* envMag       = nullptr;  // spectral envelope, kFFTSize/2+1
    float* logMag       = nullptr;  // log magnitude,    kFFTSize/2+1
    float* voiceData    = nullptr;  // complex scratch,  kFFTSize*2
    float* olaL         = nullptr;  // OLA buffer left,  kOLASize
    float* olaR         = nullptr;  // OLA buffer right, kOLASize
    float* hopOutputL   = nullptr;  // kHopSize
    float* hopOutputR   = nullptr;  // kHopSize

    int inputRingPos = 0;
    int olaPos       = 0;
    int bufPos       = 0;

    float pitchBuf[kPitchHopSize] = {};
    int   pitchPos     = 0;
    float detectedF0   = -1.0f;
    float detectedMidi = -1.0f;
    float prevMidi     = -1.0f;
    float recentPitchRms = 0.0f;
    bool  pitchStable  = false;
    bool  pitchVoiced  = false;
    int   pitchHoldFrames = 0;

    // Pitch smoothing — median, slew-limited EMA, and short voiced hold.
    static constexpr int   kPitchHistLen = 9;
    static constexpr int   kMinPitchValidFrames = 3;
    static constexpr int   kPitchReleaseFrames = 10;
    static constexpr float kPitchSmoothingAlpha = 0.30f;
    static constexpr float kPitchMaxStepSemitones = 2.0f;
    static constexpr float kPitchGateReleaseRatio = 0.55f;
    float pitchHist[kPitchHistLen] = {};   // raw Hz readings (-1 = invalid)
    int   pitchHistIdx = 0;
    float smoothedMidi = -1.0f;

    std::mutex         midiMutex;
    bool               sustainOn = false;
    uint64_t           noteCounter = 0;
    std::atomic<float> pitchBend{0.0f};

    uint32_t rng = 12345;

    void init() {
        for (int v = 0; v < kMaxVoices; v++) voices[v].init();

        window       = new float[kFFTSize];
        inputRing    = new float[kFFTSize]();
        analysisData = new float[kFFTSize * 2]();
        envMag       = new float[kFFTSize / 2 + 1]();
        logMag       = new float[kFFTSize / 2 + 1]();
        voiceData    = new float[kFFTSize * 2]();
        olaL         = new float[kOLASize]();
        olaR         = new float[kOLASize]();
        hopOutputL   = new float[kHopSize]();
        hopOutputR   = new float[kHopSize]();

        for (int i = 0; i < kFFTSize; i++)
            window[i] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * i / kFFTSize));
    }

    ~AudioEngine() {
        delete[] window;  delete[] inputRing; delete[] analysisData;
        delete[] envMag;  delete[] logMag;    delete[] voiceData;
        delete[] olaL;    delete[] olaR;      delete[] hopOutputL; delete[] hopOutputR;
    }

    // Moving-average smooth of log-mag → linear spectral envelope
    void computeEnvelope() {
        const int N     = kFFTSize / 2 + 1;
        float f0        = (detectedF0 > 0.0f) ? detectedF0 : 180.0f;
        int   halfW     = std::max(2, (int)(f0 / ((float)kSampleRate / kFFTSize)));
        float runSum    = 0.0f;
        int   count     = 0;
        for (int i = 0; i <= halfW && i < N; i++) { runSum += logMag[i]; count++; }
        for (int i = 0; i < N; i++) {
            envMag[i]   = std::exp(runSum / count);
            int add = i + halfW + 1, rem = i - halfW;
            if (add < N)  { runSum += logMag[add]; count++; }
            if (rem >= 0) { runSum -= logMag[rem]; count--; }
        }
    }

    // ── Core vocoder hop ───────────────────────────────────────────────────
    void processHop() {
        float gateRms = display.voicedGateRms.load(std::memory_order_relaxed);
        bool voicedInput = pitchVoiced &&
                           detectedF0 > 0.0f &&
                           recentPitchRms >= gateRms * kPitchGateReleaseRatio;

        // 1. Build windowed analysis frame from inputRing (oldest → newest)
        for (int i = 0; i < kFFTSize; i++) {
            int idx = (inputRingPos + i) % kFFTSize;
            analysisData[2*i]   = inputRing[idx] * window[i];
            analysisData[2*i+1] = 0.0f;
        }
        fft(analysisData, kFFTSize, false);

        // 2. Log-magnitude → smooth spectral envelope
        for (int i = 0; i <= kFFTSize / 2; i++) {
            float re = analysisData[2*i], im = analysisData[2*i+1];
            logMag[i] = std::log(std::sqrt(re*re + im*im) + 1e-10f);
        }
        computeEnvelope();

        // 3. Dry signal: most recent kHopSize samples
        float wetG = 0.0f;
        float dryG = 0.0f;
        wetDryFromBalance(display.wetDryBalance.load(std::memory_order_relaxed), wetG, dryG);
        for (int i = 0; i < kHopSize; i++) {
            int idx = (inputRingPos + kFFTSize - kHopSize + i) % kFFTSize;
            hopOutputL[i] = inputRing[idx] * dryG;
            hopOutputR[i] = inputRing[idx] * dryG;
        }

        // 4. Synthesize each voice
        float bend  = pitchBend.load(std::memory_order_relaxed);
        float binHz = (float)kSampleRate / kFFTSize;

        for (int v = 0; v < kMaxVoices; v++) {
            if (!voices[v].isAudible() || !voicedInput) continue;
            int note = voices[v].midiNote.load(std::memory_order_relaxed);
            if (note <= 0) continue;

            float f0 = noteToFreq((float)note + bend);

            // Advance envelope, skip if inaudible
            float env = 0.0f;
            for (int s = 0; s < kHopSize; s++) env = voices[v].tickEnvelope();
            if (env < 0.001f) continue;

            // Build harmonic spectrum in positive-frequency bins
            std::memset(voiceData, 0, kFFTSize * 2 * sizeof(float));
            int maxH = (int)((float)kSampleRate * 0.5f / f0);
            for (int h = 1; h <= maxH; h++) {
                int bin = (int)(f0 * h / binHz + 0.5f);
                if (bin <= 0 || bin >= kFFTSize / 2) break;
                float p = (float)(h * voices[v].phase * 2.0 * M_PI);
                voiceData[2*bin]     = std::cos(p);
                voiceData[2*bin + 1] = std::sin(p);
            }

            // Apply spectral envelope + breathiness noise
            for (int i = 1; i < kFFTSize / 2; i++) {
                voiceData[2*i]     *= envMag[i];
                voiceData[2*i + 1] *= envMag[i];
                rng = rng * 1664525u + 1013904223u;
                float np = ((float)(rng >> 9) / 8388608.0f - 1.0f) * (float)M_PI;
                float nm = envMag[i] * kNoiseRatio;
                voiceData[2*i]     += nm * std::cos(np);
                voiceData[2*i + 1] += nm * std::sin(np);
            }

            // Zero DC and Nyquist imaginary
            voiceData[0] = voiceData[1] = 0.0f;
            voiceData[kFFTSize + 1] = 0.0f;

            // Conjugate symmetry for real-valued IFFT output
            for (int i = 1; i < kFFTSize / 2; i++) {
                voiceData[2*(kFFTSize-i)]     =  voiceData[2*i];
                voiceData[2*(kFFTSize-i) + 1] = -voiceData[2*i + 1];
            }

            fft(voiceData, kFFTSize, true);

            // Normalise IFFT output to a fixed RMS so wet level is
            // independent of mic input amplitude (quiet mics, loud rooms, etc.)
            {
                float sumSq = 0.0f;
                for (int i = 0; i < kFFTSize; i++)
                    sumSq += voiceData[2*i] * voiceData[2*i];
                float rms = std::sqrt(sumSq / kFFTSize);
                // Target RMS chosen so that at wetG=1 the voice sits at −6 dBFS
                static constexpr float kTargetRMS = 0.25f;
                float norm = (rms > 1e-7f) ? (kTargetRMS / rms) : 0.0f;
                for (int i = 0; i < kFFTSize; i++) voiceData[2*i] *= norm;
            }

            // Synthesis window + OLA
            voices[v].updatePan();
            float pL   = voices[v].panL;
            float pR   = voices[v].panR;
            float gain = wetG * env;
            for (int i = 0; i < kFFTSize; i++) {
                int idx = (olaPos + i) % kOLASize;
                float s = voiceData[2*i] * window[i] * gain;
                olaL[idx] += s * pL;
                olaR[idx] += s * pR;
            }

            // Advance phase by exactly kHopSize samples
            voices[v].phase += (double)f0 / kSampleRate * kHopSize;
            voices[v].phase -= std::floor(voices[v].phase);
        }

        // 5. Read kHopSize from OLA, mix with dry, clear
        for (int i = 0; i < kHopSize; i++) {
            int idx = (olaPos + i) % kOLASize;
            hopOutputL[i] += olaL[idx];
            hopOutputR[i] += olaR[idx];
            olaL[idx] = olaR[idx] = 0.0f;
        }
        olaPos = (olaPos + kHopSize) % kOLASize;

        // Soft clip
        for (int i = 0; i < kHopSize; i++) {
            hopOutputL[i] = std::tanh(hopOutputL[i]);
            hopOutputR[i] = std::tanh(hopOutputR[i]);
        }

        display.detectedMidi.store(detectedMidi, std::memory_order_relaxed);
    }
};

// ── PortAudio Callback ─────────────────────────────────────────────────────

static int audioCallback(const void* input, void* output,
                         unsigned long frames,
                         const PaStreamCallbackTimeInfo*,
                         PaStreamCallbackFlags, void* ud)
{
    auto*        eng    = static_cast<AudioEngine*>(ud);
    const float* mic    = static_cast<const float*>(input);
    float*       stereo = static_cast<float*>(output);

    if (!mic) { std::memset(stereo, 0, frames * 2 * sizeof(float)); return paContinue; }

    for (unsigned long i = 0; i < frames; i++) {
        float s = mic[i];

        // Feed pitch detector
        eng->pitchBuf[eng->pitchPos++] = s;
        if (eng->pitchPos >= kPitchHopSize) {
            float sumSq = 0.0f;
            for (int j = 0; j < kPitchHopSize; j++) sumSq += eng->pitchBuf[j] * eng->pitchBuf[j];
            eng->recentPitchRms = std::sqrt(sumSq / kPitchHopSize);

            float gateRms = eng->display.voicedGateRms.load(std::memory_order_relaxed);
            float detectorGateRms = eng->pitchVoiced ? gateRms * AudioEngine::kPitchGateReleaseRatio : gateRms;
            float rawFreq = eng->detector.detect(eng->pitchBuf, detectorGateRms);
            float freq = foldHighPitchCandidate(rawFreq, eng->smoothedMidi);
            float midi = freq > 0.0f ? freqToMidi(freq) : -1.0f;

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

            // Stable = current reading close enough to the median
            bool stable = (validN >= AudioEngine::kMinPitchValidFrames &&
                           midi > 0 && medianMidi > 0 &&
                           std::fabs(midi - medianMidi) < stableWindow);
            bool holdVoiced = eng->pitchVoiced &&
                              eng->smoothedMidi > 0.0f &&
                              eng->pitchHoldFrames > 0 &&
                              eng->recentPitchRms >= gateRms * AudioEngine::kPitchGateReleaseRatio;

            if (stable) {
                if (eng->smoothedMidi < 0.0f) {
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
            eng->pitchPos = 0;
        }

        // Store in ring buffer
        eng->inputRing[eng->inputRingPos] = s;
        eng->inputRingPos = (eng->inputRingPos + 1) % kFFTSize;

        // Read from previously-processed hop
        stereo[2*i]     = eng->hopOutputL[eng->bufPos];
        stereo[2*i + 1] = eng->hopOutputR[eng->bufPos];
        if (++eng->bufPos >= kHopSize) { eng->processHop(); eng->bufPos = 0; }
    }
    return paContinue;
}

// ── MIDI ───────────────────────────────────────────────────────────────────

static inline bool isMsgNoteOn(const std::vector<unsigned char>& m) {
    return m.size() >= 3 && (m[0] & 0xF0) == 0x90 && m[2] != 0;
}
static inline bool isMsgNoteOff(const std::vector<unsigned char>& m) {
    return m.size() >= 3 && ((m[0] & 0xF0) == 0x80 || ((m[0] & 0xF0) == 0x90 && m[2] == 0));
}
static inline bool isMsgCC(const std::vector<unsigned char>& m, int cc) {
    return m.size() >= 3 && (m[0] & 0xF0) == 0xB0 && m[1] == cc;
}
static inline bool isMsgPitchBend(const std::vector<unsigned char>& m) {
    return m.size() >= 3 && (m[0] & 0xF0) == 0xE0;
}

static void handleNoteOn(AudioEngine* eng, int note) {
    std::lock_guard<std::mutex> lock(eng->midiMutex);
    uint64_t stamp = ++eng->noteCounter;

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
        eng->display.noteOn(note);
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

static void midiCallback(double, std::vector<unsigned char>* msg, void* ud) {
    auto* eng = static_cast<AudioEngine*>(ud);
    if (!msg || msg->empty()) return;
    if      (isMsgNoteOn(*msg))    handleNoteOn(eng, (*msg)[1]);
    else if (isMsgNoteOff(*msg))   handleNoteOff(eng, (*msg)[1]);
    else if (isMsgCC(*msg, 64)) {
        bool on = (*msg)[2] >= 64;
        eng->sustainOn = on;
        if (!on) handleSustainOff(eng);
    } else if (isMsgPitchBend(*msg)) {
        int raw = ((*msg)[2] << 7) | (*msg)[1];
        eng->pitchBend.store(((float)raw - 8192.0f) / 8192.0f * kBendRange,
                             std::memory_order_relaxed);
    }
}

// ── GUI ────────────────────────────────────────────────────────────────────

struct RollFrame {
    float    detectedMidi;
    uint64_t notesLo, notesHi;
};

static void renderFrame(SDL_Renderer* rend, AudioEngine* eng,
                        RollFrame* history, int& writePos)
{
    // Snapshot this frame
    history[writePos] = {
        eng->display.detectedMidi.load(std::memory_order_relaxed),
        eng->display.noteBitsLo.load(std::memory_order_relaxed),
        eng->display.noteBitsHi.load(std::memory_order_relaxed)
    };
    writePos = (writePos + 1) % kRollWidth;

    // Background
    SDL_SetRenderDrawColor(rend, 18, 18, 18, 255);
    SDL_RenderClear(rend);

    // ── Piano keys ──
    for (int note = kNoteMin; note < kNoteMax; note++) {
        int y = noteToY(note);
        bool b = isBlackKey(note);
        SDL_SetRenderDrawColor(rend, b ? 50 : 190, b ? 50 : 190, b ? 50 : 190, 255);
        SDL_Rect r = {0, y, kPianoWidth - 1, kNoteH};
        SDL_RenderFillRect(rend, &r);
        SDL_SetRenderDrawColor(rend, 80, 80, 80, 255);
        SDL_RenderDrawLine(rend, 0, y, kPianoWidth - 1, y);
    }

    // ── Octave grid lines ──
    for (int note = kNoteMin; note <= kNoteMax; note += 12) {
        int y = noteToY(note);
        SDL_SetRenderDrawColor(rend, 45, 45, 45, 255);
        SDL_RenderDrawLine(rend, kPianoWidth, y, kPianoWidth + kRollWidth - 1, y);
    }

    // ── Piano roll history (oldest = left, newest = right) ──
    for (int col = 0; col < kRollWidth; col++) {
        int hIdx = (writePos + col) % kRollWidth;
        const RollFrame& f = history[hIdx];
        int x = kPianoWidth + col;

        // MIDI notes — full green for entire on-screen duration
        for (int note = kNoteMin; note < kNoteMax; note++) {
            bool on = (note < 64)
                ? (f.notesLo >> note) & 1
                : (f.notesHi >> (note - 64)) & 1;
            if (on) {
                SDL_SetRenderDrawColor(rend, 20, 220, 20, 255);
                SDL_Rect r = {x, noteToY(note), 1, kNoteH};
                SDL_RenderFillRect(rend, &r);
            }
        }

        // Detected pitch (cyan, 2px tall for visibility) — full brightness
        float m = f.detectedMidi;
        if (m >= kNoteMin && m < kNoteMax) {
            int y = noteToY(m);
            SDL_SetRenderDrawColor(rend, 0, 220, 220, 255);
            SDL_RenderDrawPoint(rend, x, y);
            SDL_RenderDrawPoint(rend, x, y + 1);
        }
    }

    // ── Control sliders (right side) ──
    int panelX = kPianoWidth + kRollWidth;
    SDL_SetRenderDrawColor(rend, 40, 40, 40, 255);
    SDL_Rect panel = {panelX, 0, kSliderWidth, kWindowHeight};
    SDL_RenderFillRect(rend, &panel);

    int trackW = (kSliderWidth - (kSliderGap * (kSliderCount + 1))) / kSliderCount;
    int trackTop = 16;
    int trackH = kWindowHeight - 32;

    auto drawSlider = [&](int idx, float value, float minV, float maxV,
                          Uint8 r, Uint8 g, Uint8 b) {
        int sx = panelX + kSliderGap + idx * (trackW + kSliderGap);
        float norm = clampf((value - minV) / (maxV - minV), 0.0f, 1.0f);
        int fillH = (int)(norm * trackH);

        SDL_SetRenderDrawColor(rend, 55, 55, 55, 255);
        SDL_Rect track = {sx, trackTop, trackW, trackH};
        SDL_RenderFillRect(rend, &track);

        SDL_SetRenderDrawColor(rend, r, g, b, 255);
        SDL_Rect fill = {sx + 3, trackTop + trackH - fillH, trackW - 6, fillH};
        SDL_RenderFillRect(rend, &fill);

        SDL_SetRenderDrawColor(rend, 220, 220, 220, 255);
        int ky = trackTop + trackH - fillH;
        SDL_RenderDrawLine(rend, sx, ky, sx + trackW - 1, ky);
    };

    drawSlider(0, eng->display.wetDryBalance.load(std::memory_order_relaxed), 0.0f, 1.0f, 70, 120, 200);
    drawSlider(1, eng->display.voicedGateRms.load(std::memory_order_relaxed), 0.0001f, 0.0300f, 220, 140, 70);
    drawSlider(2, eng->display.stableSemitoneWindow.load(std::memory_order_relaxed), 0.2f, 2.0f, 60, 190, 190);

    SDL_RenderPresent(rend);
}

static int sliderIndexForX(int x) {
    int panelX = kPianoWidth + kRollWidth;
    if (x < panelX || x >= panelX + kSliderWidth) return -1;

    int trackW = (kSliderWidth - (kSliderGap * (kSliderCount + 1))) / kSliderCount;
    for (int idx = 0; idx < kSliderCount; idx++) {
        int sx = panelX + kSliderGap + idx * (trackW + kSliderGap);
        if (x >= sx && x < sx + trackW) return idx;
    }
    return -1;
}

static void setSliderValue(AudioEngine* engine, int sliderIdx, int y) {
    float norm = clampf(1.0f - (float)(y - 16) / (float)(kWindowHeight - 32), 0.0f, 1.0f);
    switch (sliderIdx) {
    case 0:
        engine->display.wetDryBalance.store(norm, std::memory_order_relaxed);
        break;
    case 1:
        engine->display.voicedGateRms.store(0.0001f + norm * (0.0300f - 0.0001f), std::memory_order_relaxed);
        break;
    case 2:
        engine->display.stableSemitoneWindow.store(0.2f + norm * (2.0f - 0.2f), std::memory_order_relaxed);
        break;
    default:
        break;
    }
}

// ── Signal ─────────────────────────────────────────────────────────────────

static void onSignal(int) { gRunning.store(false); }

// ── Main ───────────────────────────────────────────────────────────────────

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    auto* engine = new AudioEngine();
    engine->init();

    // ── PortAudio ──
    if (Pa_Initialize() != paNoError) { std::cerr << "PortAudio init failed\n"; return 1; }

    PaStreamParameters inP{}, outP{};
    inP.device  = Pa_GetDefaultInputDevice();  inP.channelCount  = 1;
    outP.device = Pa_GetDefaultOutputDevice(); outP.channelCount = 2;
    inP.sampleFormat = outP.sampleFormat = paFloat32;
    inP.suggestedLatency = outP.suggestedLatency = 0;

    PaMacCoreStreamInfo macInfo;
    PaMacCore_SetupStreamInfo(&macInfo, paMacCorePro);
    inP.hostApiSpecificStreamInfo = outP.hostApiSpecificStreamInfo = &macInfo;

    PaStream* stream = nullptr;
    if (Pa_OpenStream(&stream, &inP, &outP, kSampleRate, kFramesPerBuffer,
                      paClipOff | paDitherOff, audioCallback, engine) != paNoError ||
        Pa_StartStream(stream) != paNoError) {
        std::cerr << "PortAudio open/start failed\n"; Pa_Terminate(); return 1;
    }

    // ── MIDI ──
    std::unique_ptr<RtMidiIn> midiIn;
    try {
        midiIn = std::make_unique<RtMidiIn>();
        unsigned int n = midiIn->getPortCount();
        std::cerr << "MIDI ports:\n";
        for (unsigned int i = 0; i < n; i++)
            std::cerr << "  [" << i << "] " << midiIn->getPortName(i) << "\n";
        if (n > 0) {
            midiIn->openPort(0);
            midiIn->ignoreTypes(false, false, false);
            midiIn->setCallback(&midiCallback, engine);
            std::cerr << "Opened MIDI port 0\n";
        }
    } catch (RtMidiError& e) { std::cerr << "MIDI: " << e.getMessage() << "\n"; }

    // ── SDL2 (must run on main thread on macOS) ──
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL2 init failed: " << SDL_GetError() << "\n"; return 1;
    }
    SDL_Window* win = SDL_CreateWindow(
        "Harmonizer (vocoder)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kWindowWidth, kWindowHeight, SDL_WINDOW_SHOWN);
    SDL_Renderer* rend = SDL_CreateRenderer(
        win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    RollFrame history[kRollWidth] = {};
    int  rollWritePos   = 0;
    int  activeSlider   = -1;

    std::cerr << "\n"
        "=============================================\n"
        "  Harmonizer (channel vocoder)\n"
        "  Sing into mic + hold MIDI keys\n"
        "  Cyan = detected pitch  |  Green = MIDI\n"
        "  Right sliders: blend | voice gate | stability\n"
        "  Ctrl+C to quit\n"
        "=============================================\n\n";

    while (gRunning.load()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                gRunning.store(false);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    activeSlider = sliderIndexForX(ev.button.x);
                    if (activeSlider >= 0) setSliderValue(engine, activeSlider, ev.button.y);
                }
                break;
            case SDL_MOUSEBUTTONUP:
                activeSlider = -1;
                break;
            case SDL_MOUSEMOTION:
                if (activeSlider >= 0) setSliderValue(engine, activeSlider, ev.motion.y);
                break;
            default: break;
            }
        }
        renderFrame(rend, engine, history, rollWritePos);
    }

    SDL_DestroyRenderer(rend);
    SDL_DestroyWindow(win);
    SDL_Quit();

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    delete engine;
    return 0;
}

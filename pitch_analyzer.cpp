#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <aubio/aubio.h>

static constexpr int kSampleRate = 44100;
static constexpr int kPitchWinSize = 2048;
static constexpr int kPitchHopSize = 512;
static constexpr int kMinMidi = 33;
static constexpr int kMaxMidi = 84;
static constexpr int kLowPitchHandoffMidi = 37;
static constexpr float kLowPitchMinConfidence = 0.85f;
static constexpr int kPitchHistLen = 9;
static constexpr int kMinPitchValidFrames = 3;
static constexpr int kPitchReleaseFrames = 10;
static constexpr float kDefaultGateRms = 0.0100f;
static constexpr float kDefaultStableSemitoneWindow = 1.0f;
static constexpr float kPitchSmoothingAlpha = 0.30f;
static constexpr float kPitchMaxStepSemitones = 2.0f;
static constexpr float kPitchSnapSemitones = 1.5f;
static constexpr float kPitchGateReleaseRatio = 0.55f;

static inline float noteToFreq(float n) {
    return 440.0f * std::pow(2.0f, (n - 69.0f) / 12.0f);
}

static inline float freqToMidi(float f) {
    return (f > 0.0f) ? 69.0f + 12.0f * std::log2(f / 440.0f) : -1.0f;
}

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
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
    for (float candidate = hz * 0.5f; candidate >= noteToFreq((float)kMinMidi); candidate *= 0.5f) {
        float distance = std::fabs(freqToMidi(candidate) - targetMidi);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestHz = candidate;
        }
    }
    return bestHz;
}

static inline float alignOctaveToExpected(float hz, float expectedMidi) {
    if (hz <= 0.0f || expectedMidi <= 0.0f) return hz;

    float bestHz = hz;
    float bestDistance = std::fabs(freqToMidi(hz) - expectedMidi);
    for (int octave = -4; octave <= 4; octave++) {
        float candidate = std::ldexp(hz, octave);
        float candidateMidi = freqToMidi(candidate);
        if (candidateMidi < kMinMidi || candidateMidi > kMaxMidi) continue;
        float distance = std::fabs(candidateMidi - expectedMidi);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestHz = candidate;
        }
    }
    return bestHz;
}

static uint16_t u16le(const unsigned char* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t u32le(const unsigned char* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t readU32(std::istream& in) {
    unsigned char b[4] = {};
    in.read((char*)b, 4);
    if (!in) throw std::runtime_error("unexpected EOF while reading WAV chunk");
    return u32le(b);
}

struct Options {
    std::string wavPath;
    std::string refPath;
    std::string csvPath;
    float gateRms = kDefaultGateRms;
    float stableWindow = kDefaultStableSemitoneWindow;
    float minConfidence = -std::numeric_limits<float>::infinity();
    float minVoicedRecall = -1.0f;
    float maxMedianCents = -1.0f;
    float expectedMidi = -1.0f;
};

static void usage(const char* argv0) {
    std::cerr
        << "usage: " << argv0 << " [options] <audio.wav> [reference_f0.csv]\n"
        << "\n"
        << "options:\n"
        << "  --csv <path>                 write per-frame diagnostics\n"
        << "  --gate-rms <value>           RMS gate before aubio pitch detection\n"
        << "  --stable-window <semitones>  median stability window\n"
        << "  --min-confidence <0..1>      reject aubio frames below this confidence\n"
        << "  --min-voiced-recall <0..1>   fail if recall is lower\n"
        << "  --max-median-cents <cents>   fail if median absolute error is higher\n"
        << "  --expected-midi <note>       octave-align analysis near this MIDI note\n";
}

static Options parseOptions(int argc, char** argv) {
    Options opt;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto needValue = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        if (arg == "--csv") opt.csvPath = needValue("--csv");
        else if (arg == "--gate-rms") opt.gateRms = std::stof(needValue("--gate-rms"));
        else if (arg == "--stable-window") opt.stableWindow = std::stof(needValue("--stable-window"));
        else if (arg == "--min-confidence") opt.minConfidence = std::stof(needValue("--min-confidence"));
        else if (arg == "--min-voiced-recall") opt.minVoicedRecall = std::stof(needValue("--min-voiced-recall"));
        else if (arg == "--max-median-cents") opt.maxMedianCents = std::stof(needValue("--max-median-cents"));
        else if (arg == "--expected-midi") opt.expectedMidi = std::stof(needValue("--expected-midi"));
        else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else if (!arg.empty() && arg[0] == '-') {
            throw std::runtime_error("unknown option: " + arg);
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.empty() || positional.size() > 2) {
        usage(argv[0]);
        std::exit(2);
    }

    opt.wavPath = positional[0];
    if (positional.size() == 2) opt.refPath = positional[1];
    return opt;
}

struct WavAudio {
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> mono;
};

static float readPcmSample(const unsigned char* p, uint16_t audioFormat, uint16_t bitsPerSample) {
    if (audioFormat == 3 && bitsPerSample == 32) {
        float v = 0.0f;
        std::memcpy(&v, p, sizeof(float));
        return v;
    }
    if (audioFormat != 1) {
        throw std::runtime_error("unsupported WAV format: expected PCM int or 32-bit float");
    }

    switch (bitsPerSample) {
    case 8:
        return ((int)p[0] - 128) / 128.0f;
    case 16: {
        int16_t v = (int16_t)u16le(p);
        return (float)v / 32768.0f;
    }
    case 24: {
        int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
        if (v & 0x800000) v |= ~0xFFFFFF;
        return (float)v / 8388608.0f;
    }
    case 32: {
        int32_t v = (int32_t)u32le(p);
        return (float)v / 2147483648.0f;
    }
    default:
        throw std::runtime_error("unsupported WAV bit depth");
    }
}

static WavAudio readWav(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open WAV: " + path);

    char riff[4] = {};
    char wave[4] = {};
    in.read(riff, 4);
    (void)readU32(in);
    in.read(wave, 4);
    if (std::strncmp(riff, "RIFF", 4) != 0 || std::strncmp(wave, "WAVE", 4) != 0) {
        throw std::runtime_error("not a RIFF/WAVE file: " + path);
    }

    uint16_t audioFormat = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t blockAlign = 0;
    uint16_t bitsPerSample = 0;
    std::vector<unsigned char> data;

    while (in && (!audioFormat || data.empty())) {
        char id[4] = {};
        in.read(id, 4);
        if (!in) break;
        uint32_t size = readU32(in);

        if (std::strncmp(id, "fmt ", 4) == 0) {
            std::vector<unsigned char> fmt(size);
            in.read((char*)fmt.data(), size);
            if (fmt.size() < 16) throw std::runtime_error("short WAV fmt chunk");
            audioFormat = u16le(fmt.data());
            channels = u16le(fmt.data() + 2);
            sampleRate = u32le(fmt.data() + 4);
            blockAlign = u16le(fmt.data() + 12);
            bitsPerSample = u16le(fmt.data() + 14);
        } else if (std::strncmp(id, "data", 4) == 0) {
            data.resize(size);
            in.read((char*)data.data(), size);
        } else {
            in.seekg(size, std::ios::cur);
        }
        if (size & 1) in.seekg(1, std::ios::cur);
    }

    if (!audioFormat || data.empty()) throw std::runtime_error("WAV missing fmt or data chunk");
    if (channels == 0 || blockAlign == 0) throw std::runtime_error("invalid WAV channel layout");
    if (sampleRate != kSampleRate) {
        std::ostringstream msg;
        msg << "expected " << kSampleRate << " Hz WAV, got " << sampleRate
            << " Hz. Convert with: ffmpeg -i in.ext -ac 1 -ar 44100 out.wav";
        throw std::runtime_error(msg.str());
    }

    const int bytesPerSample = bitsPerSample / 8;
    if (bytesPerSample <= 0) throw std::runtime_error("invalid WAV bit depth");

    WavAudio audio;
    audio.sampleRate = (int)sampleRate;
    audio.channels = channels;
    const size_t frames = data.size() / blockAlign;
    audio.mono.reserve(frames);

    for (size_t f = 0; f < frames; f++) {
        const unsigned char* frame = data.data() + f * blockAlign;
        float sum = 0.0f;
        for (int ch = 0; ch < channels; ch++) {
            sum += readPcmSample(frame + ch * bytesPerSample, audioFormat, bitsPerSample);
        }
        audio.mono.push_back(sum / (float)channels);
    }

    return audio;
}

struct ReferencePoint {
    double time = 0.0;
    float hz = 0.0f;
};

static std::vector<ReferencePoint> readReferenceF0(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open reference F0 CSV: " + path);

    std::vector<ReferencePoint> points;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream row(line);
        ReferencePoint p;
        if (row >> p.time >> p.hz) points.push_back(p);
    }
    return points;
}

static float nearestReferenceHz(const std::vector<ReferencePoint>& points, double time, size_t& cursor) {
    if (points.empty()) return -1.0f;
    while (cursor + 1 < points.size() &&
           std::fabs(points[cursor + 1].time - time) <= std::fabs(points[cursor].time - time)) {
        cursor++;
    }
    return points[cursor].hz;
}

struct AubioPitchDetector {
    aubio_pitch_t* au = nullptr;
    aubio_pitch_t* lowAu = nullptr;
    fvec_t* in = nullptr;
    fvec_t* out = nullptr;
    fvec_t* lowOut = nullptr;
    float gateRms = kDefaultGateRms;
    float minConfidence = 0.0f;

    AubioPitchDetector(float gate, float confidence) : gateRms(gate), minConfidence(confidence) {
        au = new_aubio_pitch("yinfft", kPitchWinSize, kPitchHopSize, kSampleRate);
        lowAu = new_aubio_pitch("yin", kPitchWinSize, kPitchHopSize, kSampleRate);
        aubio_pitch_set_unit(au, "Hz");
        aubio_pitch_set_unit(lowAu, "Hz");
        aubio_pitch_set_silence(au, -50.0f);
        aubio_pitch_set_silence(lowAu, -50.0f);
        aubio_pitch_set_tolerance(au, 0.40f);
        aubio_pitch_set_tolerance(lowAu, 0.15f);
        in = new_fvec(kPitchHopSize);
        out = new_fvec(1);
        lowOut = new_fvec(1);
    }

    ~AubioPitchDetector() {
        if (au) del_aubio_pitch(au);
        if (lowAu) del_aubio_pitch(lowAu);
        if (in) del_fvec(in);
        if (out) del_fvec(out);
        if (lowOut) del_fvec(lowOut);
    }

    float detect(const float* samples, float gateOverride, float& rmsOut, float& confidenceOut) {
        float sumSq = 0.0f;
        for (int i = 0; i < kPitchHopSize; i++) {
            in->data[i] = samples[i];
            sumSq += samples[i] * samples[i];
        }
        rmsOut = std::sqrt(sumSq / kPitchHopSize);
        // Always feed the detector so its internal analysis window stays
        // contiguous across gated frames; gate only the result. Skipping the
        // call leaves stale audio spliced into the window, so the first frames
        // after the gate reopens read garbage.
        aubio_pitch_do(au, in, out);
        aubio_pitch_do(lowAu, in, lowOut);
        confidenceOut = aubio_pitch_get_confidence(au);
        if (rmsOut < gateOverride) return -1.0f;

        float f = fvec_get_sample(out, 0);
        float lowF = fvec_get_sample(lowOut, 0);
        float lowConfidence = aubio_pitch_get_confidence(lowAu);
        bool primaryNearFloor = f <= 0.0f || f < noteToFreq((float)kLowPitchHandoffMidi);
        bool clearLowPitch = lowF >= noteToFreq((float)kMinMidi) &&
                             lowF < noteToFreq(36.0f) &&
                             lowConfidence >= kLowPitchMinConfidence;
        if (primaryNearFloor && clearLowPitch) {
            f = lowF;
            confidenceOut = lowConfidence;
        }
        if (confidenceOut < minConfidence) return -1.0f;
        // No upper range check here: octave-up errors are folded back into
        // range by the caller (foldHighPitchCandidate), which needs to see them.
        if (f <= 0.0f || f < noteToFreq((float)kMinMidi)) {
            return -1.0f;
        }
        return f;
    }
};

struct StablePitch {
    bool stable = false;
    bool voiced = false;
    float detectedMidi = -1.0f;
    float detectedF0 = -1.0f;
};

struct PitchStabilizer {
    float pitchHist[kPitchHistLen] = {};
    int pitchHistIdx = 0;
    float smoothedMidi = -1.0f;
    bool pitchVoiced = false;
    int pitchHoldFrames = 0;

    bool isVoiced() const {
        return pitchVoiced;
    }

    StablePitch push(float rawHz, float rms, float gateRms, float stableWindow) {
        pitchHist[pitchHistIdx] = rawHz;
        pitchHistIdx = (pitchHistIdx + 1) % kPitchHistLen;

        float sorted[kPitchHistLen] = {};
        int validN = 0;
        for (int k = 0; k < kPitchHistLen; k++) {
            if (pitchHist[k] > 0.0f) sorted[validN++] = pitchHist[k];
        }

        float medianFreq = -1.0f;
        if (validN > 0) {
            std::sort(sorted, sorted + validN);
            medianFreq = sorted[validN / 2];
        }

        float medianMidi = freqToMidi(medianFreq);
        // Stable = a majority of recent valid readings agree with the median.
        // Judging the history against itself (instead of requiring the current
        // frame to be valid and near the median) lets single dropped or
        // glitched frames pass without breaking the contour.
        int agreeN = 0;
        if (medianMidi > 0.0f) {
            for (int k = 0; k < validN; k++) {
                if (std::fabs(freqToMidi(sorted[k]) - medianMidi) < stableWindow) agreeN++;
            }
        }
        // The RMS check keeps stale history from holding "stable" into
        // silence after a phrase ends.
        bool stable = validN >= kMinPitchValidFrames &&
                      medianMidi > 0.0f &&
                      2 * agreeN > validN &&
                      rms >= gateRms * kPitchGateReleaseRatio;
        bool holdVoiced = pitchVoiced &&
                          smoothedMidi > 0.0f &&
                          pitchHoldFrames > 0 &&
                          rms >= gateRms * kPitchGateReleaseRatio;

        StablePitch result;
        result.stable = stable;
        if (stable) {
            if (smoothedMidi < 0.0f ||
                std::fabs(medianMidi - smoothedMidi) > kPitchSnapSemitones) {
                // Note change (or fresh latch): the median already agrees on
                // the new pitch, so snap rather than glide through the gap.
                smoothedMidi = medianMidi;
            } else {
                float delta = clampf(medianMidi - smoothedMidi,
                                     -kPitchMaxStepSemitones,
                                     kPitchMaxStepSemitones);
                float targetMidi = smoothedMidi + delta;
                smoothedMidi += (targetMidi - smoothedMidi) * kPitchSmoothingAlpha;
            }
            pitchVoiced = true;
            pitchHoldFrames = kPitchReleaseFrames;
            result.voiced = true;
            result.detectedMidi = smoothedMidi;
            result.detectedF0 = noteToFreq(smoothedMidi);
        } else if (holdVoiced) {
            pitchHoldFrames--;
            result.voiced = true;
            result.detectedMidi = smoothedMidi;
            result.detectedF0 = noteToFreq(smoothedMidi);
        } else {
            pitchVoiced = false;
            pitchHoldFrames = 0;
            smoothedMidi = -1.0f;
        }
        return result;
    }
};

struct FrameResult {
    double time = 0.0;
    float rms = 0.0f;
    float confidence = 0.0f;
    float rawHz = -1.0f;
    bool stable = false;
    bool voiced = false;
    float detectedMidi = -1.0f;
    float detectedHz = -1.0f;
    float refHz = -1.0f;
    float centsError = std::numeric_limits<float>::quiet_NaN();
};

static float centsBetween(float hz, float refHz) {
    return 1200.0f * std::log2(hz / refHz);
}

static std::vector<FrameResult> analyze(
    const WavAudio& audio,
    const std::vector<ReferencePoint>& reference,
    const Options& opt)
{
    AubioPitchDetector detector(opt.gateRms, opt.minConfidence);
    PitchStabilizer stabilizer;
    if (opt.expectedMidi > 0.0f) stabilizer.smoothedMidi = opt.expectedMidi;
    std::vector<FrameResult> frames;
    size_t refCursor = 0;

    for (size_t start = 0; start + kPitchHopSize <= audio.mono.size(); start += kPitchHopSize) {
        FrameResult f;
        f.time = ((double)start + kPitchHopSize * 0.5) / kSampleRate;
        float detectorGate = stabilizer.isVoiced()
            ? opt.gateRms * kPitchGateReleaseRatio
            : opt.gateRms;
        float rawHz = detector.detect(audio.mono.data() + start, detectorGate, f.rms, f.confidence);
        f.rawHz = opt.expectedMidi > 0.0f
            ? alignOctaveToExpected(rawHz, opt.expectedMidi)
            : foldHighPitchCandidate(rawHz, stabilizer.smoothedMidi);
        if (f.rawHz > 0.0f &&
            (f.rawHz < noteToFreq((float)kMinMidi) || f.rawHz > noteToFreq((float)kMaxMidi))) {
            f.rawHz = -1.0f;
        }

        StablePitch stable = stabilizer.push(f.rawHz, f.rms, opt.gateRms, opt.stableWindow);
        f.stable = stable.stable;
        f.voiced = stable.voiced;
        f.detectedMidi = stable.detectedMidi;
        f.detectedHz = stable.detectedF0;

        if (!reference.empty()) {
            f.refHz = nearestReferenceHz(reference, f.time, refCursor);
            if (f.voiced && f.detectedHz > 0.0f && f.refHz > 0.0f) {
                f.centsError = centsBetween(f.detectedHz, f.refHz);
            }
        }

        frames.push_back(f);
    }
    return frames;
}

static void writeCsv(const std::string& path, const std::vector<FrameResult>& frames) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write CSV: " + path);

    out << "time,rms,confidence,raw_hz,stable,voiced,detected_midi,detected_hz,reference_hz,cents_error\n";
    out << std::fixed << std::setprecision(6);
    for (const FrameResult& f : frames) {
        out << f.time << ','
            << f.rms << ','
            << f.confidence << ','
            << f.rawHz << ','
            << (f.stable ? 1 : 0) << ','
            << (f.voiced ? 1 : 0) << ','
            << f.detectedMidi << ','
            << f.detectedHz << ','
            << f.refHz << ',';
        if (std::isfinite(f.centsError)) out << f.centsError;
        out << '\n';
    }
}

static float percentile(std::vector<float> values, float p) {
    if (values.empty()) return std::numeric_limits<float>::quiet_NaN();
    std::sort(values.begin(), values.end());
    float idx = p * (float)(values.size() - 1);
    size_t lo = (size_t)std::floor(idx);
    size_t hi = (size_t)std::ceil(idx);
    if (lo == hi) return values[lo];
    float frac = idx - (float)lo;
    return values[lo] * (1.0f - frac) + values[hi] * frac;
}

static int printSummaryAndStatus(const Options& opt, const std::vector<FrameResult>& frames) {
    int refVoiced = 0;
    int refUnvoiced = 0;
    int detectedVoiced = 0;
    int bothVoiced = 0;
    int falsePositives = 0;
    std::vector<float> absCents;

    for (const FrameResult& f : frames) {
        bool ref = f.refHz > 0.0f;
        bool detected = f.voiced && f.detectedHz > 0.0f;
        if (ref) refVoiced++;
        else refUnvoiced++;
        if (detected) detectedVoiced++;
        if (ref && detected) bothVoiced++;
        if (!ref && detected) falsePositives++;
        if (std::isfinite(f.centsError)) absCents.push_back(std::fabs(f.centsError));
    }

    float recall = refVoiced > 0 ? (float)bothVoiced / (float)refVoiced : 0.0f;
    float falsePositiveRate = refUnvoiced > 0 ? (float)falsePositives / (float)refUnvoiced : 0.0f;
    float median = percentile(absCents, 0.5f);
    float p95 = percentile(absCents, 0.95f);

    std::cout << "audio: " << opt.wavPath << "\n";
    if (!opt.refPath.empty()) std::cout << "reference: " << opt.refPath << "\n";
    std::cout << "frames: " << frames.size()
              << "  detected_voiced: " << detectedVoiced << "\n";

    if (!opt.refPath.empty()) {
        std::cout << std::fixed << std::setprecision(3)
                  << "voiced_recall: " << recall
                  << "  false_positive_rate: " << falsePositiveRate << "\n";
        std::cout << std::setprecision(1)
                  << "median_abs_cents: " << median
                  << "  p95_abs_cents: " << p95
                  << "  compared_frames: " << absCents.size() << "\n";
    }

    bool pass = true;
    if (opt.minVoicedRecall >= 0.0f && recall < opt.minVoicedRecall) pass = false;
    if (opt.maxMedianCents >= 0.0f && std::isfinite(median) && median > opt.maxMedianCents) pass = false;
    if (opt.maxMedianCents >= 0.0f && !std::isfinite(median)) pass = false;

    if (opt.minVoicedRecall >= 0.0f || opt.maxMedianCents >= 0.0f) {
        std::cout << (pass ? "status: PASS\n" : "status: FAIL\n");
    }
    return pass ? 0 : 2;
}

int main(int argc, char** argv) {
    try {
        Options opt = parseOptions(argc, argv);
        WavAudio audio = readWav(opt.wavPath);
        std::vector<ReferencePoint> reference;
        if (!opt.refPath.empty()) reference = readReferenceF0(opt.refPath);

        std::vector<FrameResult> frames = analyze(audio, reference, opt);
        if (!opt.csvPath.empty()) writeCsv(opt.csvPath, frames);
        return printSummaryAndStatus(opt, frames);
    } catch (const std::exception& e) {
        std::cerr << "pitch_analyzer: " << e.what() << "\n";
        return 1;
    }
}

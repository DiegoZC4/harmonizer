#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kFftSize = 16384;

uint16_t u16le(const unsigned char* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

uint32_t u32le(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t readU32(std::istream& in) {
    unsigned char bytes[4] = {};
    in.read(reinterpret_cast<char*>(bytes), 4);
    if (!in) throw std::runtime_error("unexpected EOF while reading WAV");
    return u32le(bytes);
}

float readSample(const unsigned char* p, uint16_t format, uint16_t bits) {
    if (format == 3 && bits == 32) {
        float value = 0.0f;
        std::memcpy(&value, p, sizeof(value));
        return value;
    }
    if (format != 1) {
        throw std::runtime_error("unsupported WAV encoding (expected PCM or float32)");
    }
    switch (bits) {
    case 8:
        return (static_cast<int>(p[0]) - 128) / 128.0f;
    case 16:
        return static_cast<float>(static_cast<int16_t>(u16le(p))) / 32768.0f;
    case 24: {
        int32_t value = static_cast<int32_t>(p[0]) |
                        (static_cast<int32_t>(p[1]) << 8) |
                        (static_cast<int32_t>(p[2]) << 16);
        if (value & 0x800000) value |= ~0xFFFFFF;
        return static_cast<float>(value) / 8388608.0f;
    }
    case 32:
        return static_cast<float>(static_cast<int32_t>(u32le(p))) / 2147483648.0f;
    default:
        throw std::runtime_error("unsupported WAV bit depth");
    }
}

struct Audio {
    int sampleRate = 0;
    std::vector<float> mono;
};

Audio readWav(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open WAV: " + path);

    char riff[4] = {};
    char wave[4] = {};
    in.read(riff, 4);
    (void)readU32(in);
    in.read(wave, 4);
    if (std::strncmp(riff, "RIFF", 4) != 0 ||
        std::strncmp(wave, "WAVE", 4) != 0) {
        throw std::runtime_error("not a RIFF/WAVE file: " + path);
    }

    uint16_t format = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t blockAlign = 0;
    uint16_t bits = 0;
    std::vector<unsigned char> data;
    while (in && (!format || data.empty())) {
        char id[4] = {};
        in.read(id, 4);
        if (!in) break;
        const uint32_t size = readU32(in);
        if (std::strncmp(id, "fmt ", 4) == 0) {
            std::vector<unsigned char> fmt(size);
            in.read(reinterpret_cast<char*>(fmt.data()), size);
            if (fmt.size() < 16) throw std::runtime_error("short WAV fmt chunk");
            format = u16le(fmt.data());
            channels = u16le(fmt.data() + 2);
            sampleRate = u32le(fmt.data() + 4);
            blockAlign = u16le(fmt.data() + 12);
            bits = u16le(fmt.data() + 14);
        } else if (std::strncmp(id, "data", 4) == 0) {
            data.resize(size);
            in.read(reinterpret_cast<char*>(data.data()), size);
        } else {
            in.seekg(size, std::ios::cur);
        }
        if (size & 1U) in.seekg(1, std::ios::cur);
    }
    if (!format || data.empty() || channels == 0 || blockAlign == 0) {
        throw std::runtime_error("WAV is missing usable fmt/data chunks");
    }

    const int bytesPerSample = bits / 8;
    if (bytesPerSample <= 0) throw std::runtime_error("invalid WAV bit depth");
    Audio audio;
    audio.sampleRate = static_cast<int>(sampleRate);
    const size_t frames = data.size() / blockAlign;
    audio.mono.reserve(frames);
    for (size_t frame = 0; frame < frames; ++frame) {
        const unsigned char* source = data.data() + frame * blockAlign;
        float sum = 0.0f;
        for (uint16_t channel = 0; channel < channels; ++channel) {
            sum += readSample(source + channel * bytesPerSample, format, bits);
        }
        audio.mono.push_back(sum / static_cast<float>(channels));
    }
    return audio;
}

struct Options {
    std::string wavPath;
    double start = 0.0;
    double duration = -1.0;
    double expectedMidi = -1.0;
    double searchSemitones = 1.0;
    double maxCents = -1.0;
    double minRms = -1.0;
    bool json = false;
};

void usage(const char* executable) {
    std::cerr
        << "usage: " << executable << " [options] <audio.wav>\n\n"
        << "  --start <seconds>            analysis start time\n"
        << "  --duration <seconds>         analysis duration\n"
        << "  --expected-midi <note>       constrain F0 peak near a MIDI note\n"
        << "  --search-semitones <amount>  radius around expected note (default 1)\n"
        << "  --max-cents <cents>          fail when expected-note error is larger\n"
        << "  --min-rms <linear>           fail when the segment is quieter\n"
        << "  --json                       print one JSON object\n";
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value = [&](const char* name) {
            if (index + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
            return std::string(argv[++index]);
        };
        if (argument == "--start") options.start = std::stod(value("--start"));
        else if (argument == "--duration") options.duration = std::stod(value("--duration"));
        else if (argument == "--expected-midi") options.expectedMidi = std::stod(value("--expected-midi"));
        else if (argument == "--search-semitones") options.searchSemitones = std::stod(value("--search-semitones"));
        else if (argument == "--max-cents") options.maxCents = std::stod(value("--max-cents"));
        else if (argument == "--min-rms") options.minRms = std::stod(value("--min-rms"));
        else if (argument == "--json") options.json = true;
        else if (argument == "--help" || argument == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else if (!argument.empty() && argument[0] == '-') {
            throw std::runtime_error("unknown option: " + argument);
        } else if (options.wavPath.empty()) {
            options.wavPath = argument;
        } else {
            throw std::runtime_error("too many input files");
        }
    }
    if (options.wavPath.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    if (options.start < 0.0 || options.duration == 0.0 || options.searchSemitones <= 0.0) {
        throw std::runtime_error("invalid analysis range");
    }
    return options;
}

void fft(std::vector<std::complex<double>>& values) {
    const size_t size = values.size();
    for (size_t index = 1, reversed = 0; index < size; ++index) {
        size_t bit = size >> 1;
        for (; reversed & bit; bit >>= 1) reversed ^= bit;
        reversed ^= bit;
        if (index < reversed) std::swap(values[index], values[reversed]);
    }
    for (size_t length = 2; length <= size; length <<= 1) {
        const std::complex<double> step = std::polar(1.0, -2.0 * kPi / static_cast<double>(length));
        for (size_t base = 0; base < size; base += length) {
            std::complex<double> phase(1.0, 0.0);
            for (size_t offset = 0; offset < length / 2; ++offset) {
                const std::complex<double> even = values[base + offset];
                const std::complex<double> odd = values[base + offset + length / 2] * phase;
                values[base + offset] = even + odd;
                values[base + offset + length / 2] = even - odd;
                phase *= step;
            }
        }
    }
}

double midiToHz(double midi) {
    return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
}

double hzToMidi(double hz) {
    return 69.0 + 12.0 * std::log2(hz / 440.0);
}

struct Analysis {
    double rms = 0.0;
    double peakHz = 0.0;
    double peakMidi = 0.0;
    double centsError = std::numeric_limits<double>::quiet_NaN();
    double centroidHz = 0.0;
    double rolloff95Hz = 0.0;
    double harmonicEnergyRatio = 0.0;
    size_t windows = 0;
};

Analysis analyze(const Audio& audio, const Options& options) {
    const size_t begin = std::min(audio.mono.size(),
        static_cast<size_t>(std::llround(options.start * audio.sampleRate)));
    const size_t requestedEnd = options.duration > 0.0
        ? begin + static_cast<size_t>(std::llround(options.duration * audio.sampleRate))
        : audio.mono.size();
    const size_t end = std::min(audio.mono.size(), requestedEnd);
    if (end <= begin) throw std::runtime_error("analysis segment is empty");

    Analysis result;
    double squareSum = 0.0;
    for (size_t sample = begin; sample < end; ++sample) {
        squareSum += static_cast<double>(audio.mono[sample]) * audio.mono[sample];
    }
    result.rms = std::sqrt(squareSum / static_cast<double>(end - begin));

    std::vector<double> power(kFftSize / 2 + 1, 0.0);
    std::vector<std::complex<double>> window(kFftSize);
    const size_t hop = kFftSize / 4;
    size_t position = begin;
    while (position < end) {
        if (position + kFftSize > end && result.windows > 0) break;
        for (size_t index = 0; index < kFftSize; ++index) {
            const size_t source = position + index;
            const double sample = source < end ? audio.mono[source] : 0.0;
            const double hann = 0.5 - 0.5 * std::cos(
                2.0 * kPi * static_cast<double>(index) / static_cast<double>(kFftSize - 1));
            window[index] = sample * hann;
        }
        fft(window);
        for (size_t bin = 0; bin < power.size(); ++bin) power[bin] += std::norm(window[bin]);
        ++result.windows;
        if (position + hop <= position) break;
        position += hop;
    }
    if (result.windows == 0) throw std::runtime_error("no FFT windows were analyzed");
    for (double& value : power) value /= static_cast<double>(result.windows);

    const double binHz = static_cast<double>(audio.sampleRate) / kFftSize;
    const double minimumHz = options.expectedMidi > 0.0
        ? midiToHz(options.expectedMidi - options.searchSemitones)
        : 40.0;
    const double maximumHz = options.expectedMidi > 0.0
        ? midiToHz(options.expectedMidi + options.searchSemitones)
        : std::min(2000.0, audio.sampleRate * 0.5);
    const size_t firstBin = std::max<size_t>(1, static_cast<size_t>(std::ceil(minimumHz / binHz)));
    const size_t lastBin = std::min(power.size() - 2,
        static_cast<size_t>(std::floor(maximumHz / binHz)));
    if (firstBin > lastBin) throw std::runtime_error("expected-note search range is outside the spectrum");

    size_t peakBin = firstBin;
    for (size_t bin = firstBin + 1; bin <= lastBin; ++bin) {
        if (power[bin] > power[peakBin]) peakBin = bin;
    }
    const double left = std::log(std::max(power[peakBin - 1], 1e-30));
    const double center = std::log(std::max(power[peakBin], 1e-30));
    const double right = std::log(std::max(power[peakBin + 1], 1e-30));
    const double denominator = left - 2.0 * center + right;
    const double offset = std::fabs(denominator) > 1e-12
        ? std::clamp(0.5 * (left - right) / denominator, -0.5, 0.5)
        : 0.0;
    result.peakHz = (static_cast<double>(peakBin) + offset) * binHz;
    result.peakMidi = hzToMidi(result.peakHz);
    if (options.expectedMidi > 0.0) {
        result.centsError = (result.peakMidi - options.expectedMidi) * 100.0;
    }

    const size_t metricFirst = std::max<size_t>(1, static_cast<size_t>(std::ceil(40.0 / binHz)));
    const size_t metricLast = std::min(power.size() - 1,
        static_cast<size_t>(std::floor(std::min(12000.0, audio.sampleRate * 0.5) / binHz)));
    double totalPower = 0.0;
    double weightedPower = 0.0;
    for (size_t bin = metricFirst; bin <= metricLast; ++bin) {
        totalPower += power[bin];
        weightedPower += power[bin] * bin * binHz;
    }
    if (totalPower > 0.0) {
        result.centroidHz = weightedPower / totalPower;
        const double rolloffTarget = totalPower * 0.95;
        double cumulative = 0.0;
        for (size_t bin = metricFirst; bin <= metricLast; ++bin) {
            cumulative += power[bin];
            if (cumulative >= rolloffTarget) {
                result.rolloff95Hz = bin * binHz;
                break;
            }
        }
        double harmonicPower = 0.0;
        for (int harmonic = 1; harmonic <= 20; ++harmonic) {
            const double frequency = result.peakHz * harmonic;
            if (frequency > metricLast * binHz) break;
            const size_t bin = static_cast<size_t>(std::llround(frequency / binHz));
            const size_t low = bin > 1 ? bin - 1 : bin;
            const size_t high = std::min(metricLast, bin + 1);
            for (size_t nearby = low; nearby <= high; ++nearby) harmonicPower += power[nearby];
        }
        result.harmonicEnergyRatio = std::min(1.0, harmonicPower / totalPower);
    }
    return result;
}

int printResult(const Options& options, const Audio& audio, const Analysis& analysis) {
    bool pass = true;
    if (options.minRms >= 0.0 && analysis.rms < options.minRms) pass = false;
    if (options.maxCents >= 0.0 &&
        (!std::isfinite(analysis.centsError) ||
         std::fabs(analysis.centsError) > options.maxCents)) pass = false;

    std::cout << std::fixed << std::setprecision(3);
    if (options.json) {
        std::cout << "{\"audio\":\"" << options.wavPath
                  << "\",\"sampleRate\":" << audio.sampleRate
                  << ",\"windows\":" << analysis.windows
                  << ",\"rms\":" << analysis.rms
                  << ",\"peakHz\":" << analysis.peakHz
                  << ",\"peakMidi\":" << analysis.peakMidi;
        if (std::isfinite(analysis.centsError)) {
            std::cout << ",\"centsError\":" << analysis.centsError;
        }
        std::cout << ",\"centroidHz\":" << analysis.centroidHz
                  << ",\"rolloff95Hz\":" << analysis.rolloff95Hz
                  << ",\"harmonicEnergyRatio\":" << analysis.harmonicEnergyRatio
                  << ",\"status\":\"" << (pass ? "PASS" : "FAIL") << "\"}\n";
    } else {
        std::cout << "audio: " << options.wavPath << "\n"
                  << "segment: " << options.start << " s, "
                  << (options.duration > 0.0 ? options.duration : -1.0) << " s"
                  << "  windows: " << analysis.windows << "\n"
                  << "rms: " << analysis.rms
                  << "  peak: " << analysis.peakHz << " Hz (MIDI "
                  << analysis.peakMidi << ")\n";
        if (std::isfinite(analysis.centsError)) {
            std::cout << "expected MIDI: " << options.expectedMidi
                      << "  cents error: " << analysis.centsError << "\n";
        }
        std::cout << "centroid: " << analysis.centroidHz
                  << " Hz  rolloff95: " << analysis.rolloff95Hz
                  << " Hz  harmonic ratio: " << analysis.harmonicEnergyRatio << "\n";
        if (options.minRms >= 0.0 || options.maxCents >= 0.0) {
            std::cout << "status: " << (pass ? "PASS" : "FAIL") << "\n";
        }
    }
    return pass ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        const Audio audio = readWav(options.wavPath);
        return printResult(options, audio, analyze(audio, options));
    } catch (const std::exception& error) {
        std::cerr << "spectrum_analyzer: " << error.what() << '\n';
        return 1;
    }
}

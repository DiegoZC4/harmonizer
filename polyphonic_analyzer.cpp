#include "analysis_audio.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using harmonizer::analysis::Audio;

struct Options {
    std::string wavPath;
    std::string csvPath;
    std::vector<double> expectedMidi;
    double start = 0.0;
    double duration = -1.0;
    double searchCents = 35.0;
    double maxErrorCents = 10.0;
    double maxMaskedErrorCents = 20.0;
    double minSnrDb = 6.0;
    double minSpuriousRelativeDb = -18.0;
    double minRms = 0.00005;
    double minPresentRatio = 1.0;
    int maxSpurious = 0;
    size_t fftSize = 65536;
    bool json = false;
};

void usage(const char* executable) {
    std::cerr
        << "usage: " << executable << " [options] <audio.wav>\n\n"
        << "  --midi <notes>               comma-separated expected MIDI notes\n"
        << "  --start <seconds>            analysis start time\n"
        << "  --duration <seconds>         analysis duration\n"
        << "  --search-cents <cents>       peak-search radius (default 35)\n"
        << "  --max-error-cents <cents>    independently visible pitch tolerance (default 10)\n"
        << "  --max-masked-error-cents <c> harmonic-masked tolerance (default 20)\n"
        << "  --min-snr-db <dB>            required fundamental prominence (default 6)\n"
        << "  --spurious-relative-db <dB>  minimum level versus strongest voice (default -18)\n"
        << "  --min-rms <linear>           minimum segment RMS\n"
        << "  --min-present-ratio <0..1>   required fraction of requested voices\n"
        << "  --max-spurious <count>       tolerated unexplained semitone peaks\n"
        << "  --fft-size <power-of-two>    analysis FFT size (default 65536)\n"
        << "  --csv <path>                 write one diagnostic row per voice\n"
        << "  --json                       print one JSON object\n\n"
        << "A target that coincides with another voice's harmonic is marked masked. It\n"
        << "must still pass, but a mono mixture cannot prove that voice independently.\n";
}

std::vector<double> parseMidiList(std::string text) {
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream input(text);
    std::vector<double> notes;
    double note = 0.0;
    while (input >> note) notes.push_back(note);
    if (notes.empty()) throw std::runtime_error("--midi did not contain any notes");
    std::sort(notes.begin(), notes.end());
    notes.erase(std::unique(notes.begin(), notes.end(),
                            [](double a, double b) { return std::fabs(a - b) < 0.001; }),
                notes.end());
    return notes;
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value = [&](const char* name) {
            if (index + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return std::string(argv[++index]);
        };
        if (argument == "--midi") options.expectedMidi = parseMidiList(value("--midi"));
        else if (argument == "--start") options.start = std::stod(value("--start"));
        else if (argument == "--duration") options.duration = std::stod(value("--duration"));
        else if (argument == "--search-cents") {
            options.searchCents = std::stod(value("--search-cents"));
        } else if (argument == "--max-error-cents") {
            options.maxErrorCents = std::stod(value("--max-error-cents"));
        } else if (argument == "--max-masked-error-cents") {
            options.maxMaskedErrorCents =
                std::stod(value("--max-masked-error-cents"));
        } else if (argument == "--min-snr-db") {
            options.minSnrDb = std::stod(value("--min-snr-db"));
        } else if (argument == "--spurious-relative-db") {
            options.minSpuriousRelativeDb =
                std::stod(value("--spurious-relative-db"));
        } else if (argument == "--min-rms") {
            options.minRms = std::stod(value("--min-rms"));
        } else if (argument == "--min-present-ratio") {
            options.minPresentRatio = std::stod(value("--min-present-ratio"));
        } else if (argument == "--max-spurious") {
            options.maxSpurious = std::stoi(value("--max-spurious"));
        } else if (argument == "--fft-size") {
            options.fftSize = static_cast<size_t>(std::stoull(value("--fft-size")));
        } else if (argument == "--csv") {
            options.csvPath = value("--csv");
        } else if (argument == "--json") {
            options.json = true;
        } else if (argument == "--help" || argument == "-h") {
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
    if (options.wavPath.empty() || options.expectedMidi.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    if (options.start < 0.0 || options.duration == 0.0 ||
        options.searchCents <= 0.0 || options.searchCents >= 50.0 ||
        options.maxErrorCents < 0.0 || options.maxMaskedErrorCents < 0.0 ||
        options.minPresentRatio < 0.0 ||
        options.minPresentRatio > 1.0 || options.maxSpurious < 0 ||
        options.fftSize < 2048 || (options.fftSize & (options.fftSize - 1)) != 0) {
        throw std::runtime_error("invalid analyzer option");
    }
    return options;
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    double result = values[middle];
    if ((values.size() & 1U) == 0) {
        std::nth_element(values.begin(), values.begin() + middle - 1, values.end());
        result = 0.5 * (result + values[middle - 1]);
    }
    return result;
}

struct Spectrum {
    std::vector<double> power;
    double binHz = 0.0;
    double rms = 0.0;
    size_t windows = 0;
};

Spectrum makeSpectrum(const Audio& audio, const Options& options) {
    const size_t begin = std::min(
        audio.mono.size(),
        static_cast<size_t>(std::llround(options.start * audio.sampleRate)));
    const size_t requestedEnd = options.duration > 0.0
        ? begin + static_cast<size_t>(std::llround(options.duration * audio.sampleRate))
        : audio.mono.size();
    const size_t end = std::min(audio.mono.size(), requestedEnd);
    if (end <= begin) throw std::runtime_error("analysis segment is empty");

    Spectrum spectrum;
    spectrum.rms = harmonizer::analysis::rms(audio.mono, begin, end);
    spectrum.binHz = static_cast<double>(audio.sampleRate) / options.fftSize;
    spectrum.power.assign(options.fftSize / 2 + 1, 0.0);
    std::vector<std::complex<double>> frame(options.fftSize);

    const size_t hop = options.fftSize / 4;
    size_t position = begin;
    while (position < end) {
        if (position + options.fftSize > end && spectrum.windows > 0) break;
        for (size_t sample = 0; sample < options.fftSize; ++sample) {
            const size_t source = position + sample;
            const double value = source < end ? audio.mono[source] : 0.0;
            const double window = 0.5 - 0.5 * std::cos(
                2.0 * harmonizer::analysis::kPi * static_cast<double>(sample) /
                static_cast<double>(options.fftSize - 1));
            frame[sample] = value * window;
        }
        harmonizer::analysis::fft(frame);
        for (size_t bin = 0; bin < spectrum.power.size(); ++bin) {
            spectrum.power[bin] += std::norm(frame[bin]);
        }
        ++spectrum.windows;
        position += hop;
    }
    if (spectrum.windows == 0) throw std::runtime_error("no FFT windows were analyzed");
    for (double& value : spectrum.power) value /= static_cast<double>(spectrum.windows);
    return spectrum;
}

struct Peak {
    double hz = 0.0;
    double midi = 0.0;
    double centsError = 0.0;
    double snrDb = -std::numeric_limits<double>::infinity();
    double harmonicSnrDb = -std::numeric_limits<double>::infinity();
    double power = 0.0;
    double relativeDb = 0.0;
};

double localNoiseFloor(const Spectrum& spectrum, size_t centerBin) {
    const size_t radius = std::max<size_t>(20, centerBin / 18);
    const size_t low = centerBin > radius ? centerBin - radius : 1;
    const size_t high = std::min(spectrum.power.size() - 1, centerBin + radius);
    std::vector<double> neighbors;
    neighbors.reserve(high - low + 1);
    for (size_t bin = low; bin <= high; ++bin) {
        if (bin + 4 >= centerBin && bin <= centerBin + 4) continue;
        neighbors.push_back(spectrum.power[bin]);
    }
    return std::max(median(std::move(neighbors)), 1e-30);
}

double harmonicScoreAt(const Spectrum& spectrum, double fundamentalHz) {
    double harmonicWeight = 0.0;
    double harmonicScore = 0.0;
    for (int harmonic = 1; harmonic <= 8; ++harmonic) {
        const double frequency = fundamentalHz * harmonic;
        if (frequency >= spectrum.binHz * (spectrum.power.size() - 3)) break;
        const size_t centerBin =
            static_cast<size_t>(std::llround(frequency / spectrum.binHz));
        double harmonicPower = 0.0;
        for (size_t bin = centerBin - 1; bin <= centerBin + 1; ++bin) {
            harmonicPower = std::max(harmonicPower, spectrum.power[bin]);
        }
        const double harmonicFloor = localNoiseFloor(spectrum, centerBin);
        const double snr = 10.0 * std::log10(
            std::max(harmonicPower, 1e-30) / harmonicFloor);
        const double weight = 1.0 / std::sqrt(static_cast<double>(harmonic));
        harmonicScore += std::clamp(snr, -20.0, 60.0) * weight;
        harmonicWeight += weight;
    }
    return harmonicWeight > 0.0
        ? harmonicScore / harmonicWeight
        : -std::numeric_limits<double>::infinity();
}

Peak findPeak(const Spectrum& spectrum, double targetMidi, double searchCents) {
    const double radiusSemitones = searchCents / 100.0;
    const double lowHz = harmonizer::analysis::midiToHz(targetMidi - radiusSemitones);
    const double highHz = harmonizer::analysis::midiToHz(targetMidi + radiusSemitones);
    const size_t firstBin = std::max<size_t>(
        2, static_cast<size_t>(std::ceil(lowHz / spectrum.binHz)));
    const size_t lastBin = std::min(
        spectrum.power.size() - 3,
        static_cast<size_t>(std::floor(highHz / spectrum.binHz)));
    if (firstBin > lastBin) throw std::runtime_error("MIDI target is outside spectrum");

    size_t peakBin = firstBin;
    double bestScore = harmonicScoreAt(spectrum, firstBin * spectrum.binHz);
    for (size_t bin = firstBin + 1; bin <= lastBin; ++bin) {
        const double score = harmonicScoreAt(spectrum, bin * spectrum.binHz);
        if (score > bestScore) {
            bestScore = score;
            peakBin = bin;
        }
    }
    const double left =
        harmonicScoreAt(spectrum, (peakBin - 1) * spectrum.binHz);
    const double center = bestScore;
    const double right =
        harmonicScoreAt(spectrum, (peakBin + 1) * spectrum.binHz);
    const double denominator = left - 2.0 * center + right;
    const double offset = std::fabs(denominator) > 1e-12
        ? std::clamp(0.5 * (left - right) / denominator, -0.5, 0.5)
        : 0.0;

    Peak peak;
    peak.hz = (static_cast<double>(peakBin) + offset) * spectrum.binHz;
    peak.midi = harmonizer::analysis::hzToMidi(peak.hz);
    peak.centsError = (peak.midi - targetMidi) * 100.0;
    const double floor = localNoiseFloor(spectrum, peakBin);
    peak.snrDb = 10.0 * std::log10(
        std::max(spectrum.power[peakBin], 1e-30) / floor);
    peak.power = spectrum.power[peakBin];
    peak.harmonicSnrDb = harmonicScoreAt(spectrum, peak.hz);
    return peak;
}

bool explainedByHarmonic(double midi, const std::vector<double>& expected,
                         double toleranceCents) {
    const double frequency = harmonizer::analysis::midiToHz(midi);
    for (double note : expected) {
        const double fundamental = harmonizer::analysis::midiToHz(note);
        for (int harmonic = 2; harmonic <= 12; ++harmonic) {
            const double harmonicMidi =
                harmonizer::analysis::hzToMidi(fundamental * harmonic);
            if (std::fabs((midi - harmonicMidi) * 100.0) <= toleranceCents) {
                return true;
            }
            if (fundamental * harmonic > frequency * 1.05) break;
        }
    }
    return false;
}

bool maskedByRequestedHarmonic(double midi, const std::vector<double>& expected,
                               double toleranceCents) {
    for (double other : expected) {
        if (std::fabs(other - midi) < 0.001 || other > midi) continue;
        for (int harmonic = 2; harmonic <= 12; ++harmonic) {
            const double harmonicMidi = harmonizer::analysis::hzToMidi(
                harmonizer::analysis::midiToHz(other) * harmonic);
            if (std::fabs((midi - harmonicMidi) * 100.0) <= toleranceCents) {
                return true;
            }
            if (harmonicMidi > midi + 1.0) break;
        }
    }
    return false;
}

struct VoiceResult {
    double expectedMidi = 0.0;
    Peak peak;
    bool masked = false;
    bool present = false;
};

struct Analysis {
    Spectrum spectrum;
    std::vector<VoiceResult> voices;
    std::vector<VoiceResult> spurious;
    size_t present = 0;
    bool pass = false;
};

Analysis analyze(const Audio& audio, const Options& options) {
    Analysis analysis;
    analysis.spectrum = makeSpectrum(audio, options);
    for (double midi : options.expectedMidi) {
        VoiceResult voice;
        voice.expectedMidi = midi;
        voice.peak = findPeak(analysis.spectrum, midi, options.searchCents);
        voice.masked = maskedByRequestedHarmonic(
            midi, options.expectedMidi, options.searchCents);
        const double tolerance = voice.masked
            ? options.maxMaskedErrorCents
            : options.maxErrorCents;
        voice.present = std::fabs(voice.peak.centsError) <= tolerance &&
                        voice.peak.snrDb >= options.minSnrDb;
        analysis.present += voice.present ? 1U : 0U;
        analysis.voices.push_back(voice);
    }
    double strongestRequestedPower = 1e-30;
    for (const VoiceResult& voice : analysis.voices) {
        strongestRequestedPower =
            std::max(strongestRequestedPower, voice.peak.power);
    }
    for (VoiceResult& voice : analysis.voices) {
        voice.peak.relativeDb = 10.0 * std::log10(
            std::max(voice.peak.power, 1e-30) / strongestRequestedPower);
    }

    const int scanLow = std::max(24, static_cast<int>(std::floor(
        options.expectedMidi.front() - 12.0)));
    const int scanHigh = std::min(108, static_cast<int>(std::ceil(
        options.expectedMidi.back() + 12.0)));
    for (int midi = scanLow; midi <= scanHigh; ++midi) {
        bool requested = false;
        for (double expected : options.expectedMidi) {
            if (std::fabs(expected - midi) < 0.45) {
                requested = true;
                break;
            }
        }
        if (requested ||
            explainedByHarmonic(midi, options.expectedMidi, options.searchCents)) {
            continue;
        }
        VoiceResult candidate;
        candidate.expectedMidi = midi;
        candidate.peak = findPeak(analysis.spectrum, midi, options.searchCents);
        candidate.peak.relativeDb = 10.0 * std::log10(
            std::max(candidate.peak.power, 1e-30) / strongestRequestedPower);
        candidate.present =
            std::fabs(candidate.peak.centsError) <= options.searchCents &&
            candidate.peak.snrDb >= options.minSnrDb + 6.0 &&
            candidate.peak.harmonicSnrDb >= options.minSnrDb &&
            candidate.peak.relativeDb >= options.minSpuriousRelativeDb;
        if (candidate.present) analysis.spurious.push_back(candidate);
    }

    const double presentRatio =
        static_cast<double>(analysis.present) / analysis.voices.size();
    analysis.pass = analysis.spectrum.rms >= options.minRms &&
                    presentRatio + 1e-12 >= options.minPresentRatio &&
                    static_cast<int>(analysis.spurious.size()) <= options.maxSpurious;
    return analysis;
}

std::string jsonString(const std::string& text) {
    std::string escaped = "\"";
    for (char character : text) {
        if (character == '\\' || character == '"') escaped.push_back('\\');
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

void writeCsv(const Options& options, const Analysis& analysis) {
    if (options.csvPath.empty()) return;
    std::ofstream output(options.csvPath);
    if (!output) throw std::runtime_error("cannot write CSV: " + options.csvPath);
    output << "kind,expected_midi,peak_hz,peak_midi,cents_error,snr_db,"
              "harmonic_snr_db,relative_db,masked,status\n";
    auto row = [&](const char* kind, const VoiceResult& voice) {
        output << kind << ',' << voice.expectedMidi << ',' << voice.peak.hz << ','
               << voice.peak.midi << ',' << voice.peak.centsError << ','
               << voice.peak.snrDb << ',' << voice.peak.harmonicSnrDb << ','
               << voice.peak.relativeDb << ','
               << (voice.masked ? 1 : 0) << ','
               << (voice.present ? "PASS" : "FAIL") << '\n';
    };
    for (const VoiceResult& voice : analysis.voices) row("requested", voice);
    for (const VoiceResult& voice : analysis.spurious) row("spurious", voice);
}

void printResult(const Options& options, const Analysis& analysis) {
    const double ratio = static_cast<double>(analysis.present) / analysis.voices.size();
    std::cout << std::fixed << std::setprecision(3);
    if (options.json) {
        std::cout << "{\"audio\":" << jsonString(options.wavPath)
                  << ",\"sampleRate\":"
                  << static_cast<long long>(std::llround(
                         analysis.spectrum.binHz * options.fftSize))
                  << ",\"windows\":" << analysis.spectrum.windows
                  << ",\"rms\":" << analysis.spectrum.rms
                  << ",\"expected\":" << analysis.voices.size()
                  << ",\"present\":" << analysis.present
                  << ",\"presentRatio\":" << ratio
                  << ",\"spurious\":" << analysis.spurious.size()
                  << ",\"voices\":[";
        for (size_t index = 0; index < analysis.voices.size(); ++index) {
            const VoiceResult& voice = analysis.voices[index];
            if (index) std::cout << ',';
            std::cout << "{\"midi\":" << voice.expectedMidi
                      << ",\"peakHz\":" << voice.peak.hz
                      << ",\"peakMidi\":" << voice.peak.midi
                      << ",\"centsError\":" << voice.peak.centsError
                      << ",\"snrDb\":" << voice.peak.snrDb
                      << ",\"harmonicSnrDb\":" << voice.peak.harmonicSnrDb
                      << ",\"relativeDb\":" << voice.peak.relativeDb
                      << ",\"masked\":" << (voice.masked ? "true" : "false")
                      << ",\"status\":\"" << (voice.present ? "PASS" : "FAIL")
                      << "\"}";
        }
        std::cout << "],\"spuriousNotes\":[";
        for (size_t index = 0; index < analysis.spurious.size(); ++index) {
            if (index) std::cout << ',';
            std::cout << analysis.spurious[index].expectedMidi;
        }
        std::cout << "],\"status\":\"" << (analysis.pass ? "PASS" : "FAIL")
                  << "\"}\n";
        return;
    }

    std::cout << "MIDI   measured   error    F0 SNR   harmonic   relative  mask  status\n";
    for (const VoiceResult& voice : analysis.voices) {
        std::cout << std::setw(5) << voice.expectedMidi << "  "
                  << std::setw(8) << voice.peak.midi << "  "
                  << std::showpos << std::setw(7) << voice.peak.centsError
                  << std::noshowpos << " c  "
                  << std::setw(7) << voice.peak.snrDb << " dB  "
                  << std::setw(7) << voice.peak.harmonicSnrDb << " dB  "
                  << std::setw(8) << voice.peak.relativeDb << " dB  "
                  << std::setw(4) << (voice.masked ? "yes" : "no") << "  "
                  << (voice.present ? "PASS" : "FAIL") << '\n';
    }
    std::cout << "voices: " << analysis.present << '/' << analysis.voices.size()
              << "  spurious: " << analysis.spurious.size()
              << "  RMS: " << analysis.spectrum.rms
              << "  status: " << (analysis.pass ? "PASS" : "FAIL") << '\n';
    if (!analysis.spurious.empty()) {
        std::cout << "unexplained semitone peaks:";
        for (const VoiceResult& voice : analysis.spurious) {
            std::cout << ' ' << voice.expectedMidi;
        }
        std::cout << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        const Audio audio = harmonizer::analysis::readWav(options.wavPath);
        const Analysis analysis = analyze(audio, options);
        writeCsv(options, analysis);
        printResult(options, analysis);
        return analysis.pass ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "polyphonic_analyzer: " << error.what() << '\n';
        return 1;
    }
}

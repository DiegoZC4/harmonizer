#include "analysis_audio.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using harmonizer::analysis::Audio;

enum class EnvelopeMethod {
    Cepstral,
    Lpc
};

struct Options {
    std::string referencePath;
    std::string candidatePath;
    std::string csvPath;
    std::string envelopeCsvPath;
    std::vector<double> expectedFormants;
    double referenceStart = 0.0;
    double candidateStart = 0.0;
    double duration = -1.0;
    double maxDriftPercent = 8.0;
    double maxEnvelopeDb = 3.0;
    double minRms = 0.00005;
    double minimumHz = 150.0;
    double maximumHz = 5000.0;
    int lpcOrder = 32;
    double lifterMs = 3.0;
    EnvelopeMethod method = EnvelopeMethod::Cepstral;
    bool json = false;
};

void usage(const char* executable) {
    std::cerr
        << "usage: " << executable << " [options] <candidate.wav>\n\n"
        << "  --reference <audio.wav>          dry/reference recording\n"
        << "  --expected-formants <Hz,...>     expected F1,F2,F3 search anchors\n"
        << "  --reference-start <seconds>      reference segment start\n"
        << "  --candidate-start <seconds>      candidate segment start\n"
        << "  --duration <seconds>             segment duration\n"
        << "  --max-drift-percent <percent>    maximum per-formant drift\n"
        << "  --max-envelope-db <dB>           maximum normalized LPC-envelope RMS\n"
        << "  --min-rms <linear>               minimum RMS for both segments\n"
        << "  --frequency-range <low,high>     envelope comparison range\n"
        << "  --method cepstral|lpc            default cepstral\n"
        << "  --lifter-ms <milliseconds>       cepstral envelope cutoff (default 3)\n"
        << "  --lpc-order <order>              LPC order (default 32)\n"
        << "  --csv <path>                     write formant diagnostics\n"
        << "  --envelope-csv <path>            write normalized envelope curves\n"
        << "  --json                           print one JSON object\n";
}

std::vector<double> parseNumbers(std::string text, const char* name) {
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream input(text);
    std::vector<double> values;
    double value = 0.0;
    while (input >> value) values.push_back(value);
    if (values.empty()) throw std::runtime_error(std::string(name) + " is empty");
    return values;
}

EnvelopeMethod parseMethod(const std::string& text) {
    if (text == "cepstral") return EnvelopeMethod::Cepstral;
    if (text == "lpc") return EnvelopeMethod::Lpc;
    throw std::runtime_error("--method must be cepstral or lpc");
}

const char* methodName(EnvelopeMethod method) {
    return method == EnvelopeMethod::Cepstral ? "cepstral" : "lpc";
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
        if (argument == "--reference") {
            options.referencePath = value("--reference");
        } else if (argument == "--expected-formants") {
            options.expectedFormants =
                parseNumbers(value("--expected-formants"), "--expected-formants");
        } else if (argument == "--reference-start") {
            options.referenceStart = std::stod(value("--reference-start"));
        } else if (argument == "--candidate-start") {
            options.candidateStart = std::stod(value("--candidate-start"));
        } else if (argument == "--duration") {
            options.duration = std::stod(value("--duration"));
        } else if (argument == "--max-drift-percent") {
            options.maxDriftPercent = std::stod(value("--max-drift-percent"));
        } else if (argument == "--max-envelope-db") {
            options.maxEnvelopeDb = std::stod(value("--max-envelope-db"));
        } else if (argument == "--min-rms") {
            options.minRms = std::stod(value("--min-rms"));
        } else if (argument == "--frequency-range") {
            const std::vector<double> range =
                parseNumbers(value("--frequency-range"), "--frequency-range");
            if (range.size() != 2) {
                throw std::runtime_error("--frequency-range needs low,high");
            }
            options.minimumHz = range[0];
            options.maximumHz = range[1];
        } else if (argument == "--lpc-order") {
            options.lpcOrder = std::stoi(value("--lpc-order"));
        } else if (argument == "--lifter-ms") {
            options.lifterMs = std::stod(value("--lifter-ms"));
        } else if (argument == "--method") {
            options.method = parseMethod(value("--method"));
        } else if (argument == "--csv") {
            options.csvPath = value("--csv");
        } else if (argument == "--envelope-csv") {
            options.envelopeCsvPath = value("--envelope-csv");
        } else if (argument == "--json") {
            options.json = true;
        } else if (argument == "--help" || argument == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else if (!argument.empty() && argument[0] == '-') {
            throw std::runtime_error("unknown option: " + argument);
        } else if (options.candidatePath.empty()) {
            options.candidatePath = argument;
        } else {
            throw std::runtime_error("too many input files");
        }
    }
    if (options.referencePath.empty() || options.candidatePath.empty() ||
        options.expectedFormants.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    if (options.referenceStart < 0.0 || options.candidateStart < 0.0 ||
        options.duration == 0.0 || options.maxDriftPercent < 0.0 ||
        options.maxEnvelopeDb < 0.0 || options.minimumHz < 50.0 ||
        options.maximumHz <= options.minimumHz || options.lpcOrder < 8 ||
        options.lpcOrder > 80 || options.lifterMs <= 0.2 ||
        options.lifterMs > 5.0) {
        throw std::runtime_error("invalid analyzer option");
    }
    std::sort(options.expectedFormants.begin(), options.expectedFormants.end());
    for (double frequency : options.expectedFormants) {
        if (frequency <= options.minimumHz || frequency >= options.maximumHz) {
            throw std::runtime_error("expected formant lies outside frequency range");
        }
    }
    return options;
}

bool levinsonDurbin(const std::vector<double>& correlation, int order,
                    std::vector<double>& coefficients, double& error) {
    coefficients.assign(static_cast<size_t>(order + 1), 0.0);
    coefficients[0] = 1.0;
    error = correlation[0];
    if (!std::isfinite(error) || error <= 1e-12) return false;

    std::vector<double> previous(coefficients.size(), 0.0);
    for (int index = 1; index <= order; ++index) {
        double sum = correlation[static_cast<size_t>(index)];
        for (int inner = 1; inner < index; ++inner) {
            sum += coefficients[static_cast<size_t>(inner)] *
                   correlation[static_cast<size_t>(index - inner)];
        }
        const double reflection = std::clamp(-sum / error, -0.999, 0.999);
        previous = coefficients;
        coefficients[static_cast<size_t>(index)] = reflection;
        for (int inner = 1; inner < index; ++inner) {
            coefficients[static_cast<size_t>(inner)] =
                previous[static_cast<size_t>(inner)] +
                reflection * previous[static_cast<size_t>(index - inner)];
        }
        error *= 1.0 - reflection * reflection;
        if (!std::isfinite(error) || error <= 1e-14) return false;
    }
    return true;
}

struct Envelope {
    std::vector<double> frequencies;
    std::vector<double> db;
    double segmentRms = 0.0;
    size_t frames = 0;
};

Envelope analyzeLpcEnvelope(const Audio& audio, double start, double duration,
                            const Options& options) {
    constexpr size_t frameSize = 2048;
    constexpr size_t hopSize = 512;
    constexpr double preEmphasis = 0.97;
    constexpr double gridStepHz = 5.0;

    const size_t begin = std::min(
        audio.mono.size(),
        static_cast<size_t>(std::llround(start * audio.sampleRate)));
    const size_t requestedEnd = duration > 0.0
        ? begin + static_cast<size_t>(std::llround(duration * audio.sampleRate))
        : audio.mono.size();
    const size_t end = std::min(audio.mono.size(), requestedEnd);
    if (end <= begin || end - begin < frameSize) {
        throw std::runtime_error("formant segment is too short");
    }

    Envelope envelope;
    envelope.segmentRms = harmonizer::analysis::rms(audio.mono, begin, end);
    for (double frequency = options.minimumHz;
         frequency <= options.maximumHz + 0.1; frequency += gridStepHz) {
        envelope.frequencies.push_back(frequency);
    }
    envelope.db.assign(envelope.frequencies.size(), 0.0);

    std::vector<double> frame(frameSize);
    std::vector<double> correlation(static_cast<size_t>(options.lpcOrder + 1));
    std::vector<double> coefficients;
    for (size_t position = begin; position + frameSize <= end; position += hopSize) {
        double squareSum = 0.0;
        float previous = position > 0 ? audio.mono[position - 1] : 0.0f;
        for (size_t sample = 0; sample < frameSize; ++sample) {
            const float current = audio.mono[position + sample];
            const double emphasized = current - preEmphasis * previous;
            previous = current;
            const double window = 0.5 - 0.5 * std::cos(
                2.0 * harmonizer::analysis::kPi * static_cast<double>(sample) /
                static_cast<double>(frameSize - 1));
            frame[sample] = emphasized * window;
            squareSum += static_cast<double>(current) * current;
        }
        const double frameRms = std::sqrt(squareSum / frameSize);
        if (frameRms < std::max(options.minRms * 0.5, 1e-6)) continue;

        for (int lag = 0; lag <= options.lpcOrder; ++lag) {
            double sum = 0.0;
            for (size_t sample = static_cast<size_t>(lag);
                 sample < frameSize; ++sample) {
                sum += frame[sample] * frame[sample - static_cast<size_t>(lag)];
            }
            correlation[static_cast<size_t>(lag)] = sum;
        }
        correlation[0] *= 1.000001;
        double predictionError = 0.0;
        if (!levinsonDurbin(correlation, options.lpcOrder,
                           coefficients, predictionError)) {
            continue;
        }

        for (size_t bin = 0; bin < envelope.frequencies.size(); ++bin) {
            const double omega = 2.0 * harmonizer::analysis::kPi *
                                 envelope.frequencies[bin] / audio.sampleRate;
            std::complex<double> denominator(1.0, 0.0);
            for (int coefficient = 1; coefficient <= options.lpcOrder;
                 ++coefficient) {
                denominator += coefficients[static_cast<size_t>(coefficient)] *
                    std::polar(1.0, -omega * coefficient);
            }
            const double magnitude = std::sqrt(predictionError) /
                                     std::max(std::abs(denominator), 1e-12);
            envelope.db[bin] += 20.0 * std::log10(std::max(magnitude, 1e-12));
        }
        ++envelope.frames;
    }
    if (envelope.frames == 0) throw std::runtime_error("no voiced LPC frames");
    for (double& value : envelope.db) value /= static_cast<double>(envelope.frames);

    std::vector<double> smoothed(envelope.db.size());
    constexpr int smoothingRadius = 4;
    for (size_t index = 0; index < envelope.db.size(); ++index) {
        const size_t low = index > smoothingRadius ? index - smoothingRadius : 0;
        const size_t high = std::min(
            envelope.db.size() - 1, index + static_cast<size_t>(smoothingRadius));
        double sum = 0.0;
        for (size_t nearby = low; nearby <= high; ++nearby) sum += envelope.db[nearby];
        smoothed[index] = sum / static_cast<double>(high - low + 1);
    }
    envelope.db = std::move(smoothed);
    return envelope;
}

Envelope analyzeCepstralEnvelope(const Audio& audio, double start, double duration,
                                 const Options& options) {
    constexpr size_t frameSize = 8192;
    constexpr size_t hopSize = 1024;
    constexpr double preEmphasis = 0.97;
    constexpr double gridStepHz = 5.0;

    const size_t begin = std::min(
        audio.mono.size(),
        static_cast<size_t>(std::llround(start * audio.sampleRate)));
    const size_t requestedEnd = duration > 0.0
        ? begin + static_cast<size_t>(std::llround(duration * audio.sampleRate))
        : audio.mono.size();
    const size_t end = std::min(audio.mono.size(), requestedEnd);
    if (end <= begin || end - begin < frameSize) {
        throw std::runtime_error("formant segment is too short");
    }

    Envelope envelope;
    envelope.segmentRms = harmonizer::analysis::rms(audio.mono, begin, end);
    for (double frequency = options.minimumHz;
         frequency <= options.maximumHz + 0.1; frequency += gridStepHz) {
        envelope.frequencies.push_back(frequency);
    }
    envelope.db.assign(envelope.frequencies.size(), 0.0);

    const size_t lifterSamples = std::clamp<size_t>(
        static_cast<size_t>(std::llround(
            options.lifterMs * audio.sampleRate / 1000.0)),
        4, frameSize / 4);
    std::vector<std::complex<double>> spectrum(frameSize);
    for (size_t position = begin; position + frameSize <= end; position += hopSize) {
        double squareSum = 0.0;
        float previous = position > 0 ? audio.mono[position - 1] : 0.0f;
        for (size_t sample = 0; sample < frameSize; ++sample) {
            const float current = audio.mono[position + sample];
            const double emphasized = current - preEmphasis * previous;
            previous = current;
            const double window = 0.5 - 0.5 * std::cos(
                2.0 * harmonizer::analysis::kPi * static_cast<double>(sample) /
                static_cast<double>(frameSize - 1));
            spectrum[sample] = emphasized * window;
            squareSum += static_cast<double>(current) * current;
        }
        const double frameRms = std::sqrt(squareSum / frameSize);
        if (frameRms < std::max(options.minRms * 0.5, 1e-6)) continue;

        harmonizer::analysis::fft(spectrum);
        for (std::complex<double>& value : spectrum) {
            value = std::log(std::max(std::abs(value), 1e-12));
        }
        harmonizer::analysis::fft(spectrum, true);
        for (size_t quefrency = lifterSamples + 1;
             quefrency + lifterSamples + 1 < frameSize; ++quefrency) {
            spectrum[quefrency] = 0.0;
        }
        harmonizer::analysis::fft(spectrum);

        const double binHz = static_cast<double>(audio.sampleRate) / frameSize;
        for (size_t index = 0; index < envelope.frequencies.size(); ++index) {
            const double bin = envelope.frequencies[index] / binHz;
            const size_t low = std::min(
                frameSize / 2 - 1, static_cast<size_t>(std::floor(bin)));
            const size_t high = std::min(frameSize / 2, low + 1);
            const double blend = bin - std::floor(bin);
            const double logMagnitude =
                spectrum[low].real() * (1.0 - blend) +
                spectrum[high].real() * blend;
            envelope.db[index] +=
                20.0 * logMagnitude / std::log(10.0);
        }
        ++envelope.frames;
    }
    if (envelope.frames == 0) {
        throw std::runtime_error("no voiced cepstral-envelope frames");
    }
    for (double& value : envelope.db) value /= static_cast<double>(envelope.frames);

    std::vector<double> smoothed(envelope.db.size());
    constexpr int smoothingRadius = 3;
    for (size_t index = 0; index < envelope.db.size(); ++index) {
        const size_t low = index > smoothingRadius ? index - smoothingRadius : 0;
        const size_t high = std::min(
            envelope.db.size() - 1, index + static_cast<size_t>(smoothingRadius));
        double sum = 0.0;
        for (size_t nearby = low; nearby <= high; ++nearby) sum += envelope.db[nearby];
        smoothed[index] = sum / static_cast<double>(high - low + 1);
    }
    envelope.db = std::move(smoothed);
    return envelope;
}

Envelope analyzeEnvelope(const Audio& audio, double start, double duration,
                         const Options& options) {
    return options.method == EnvelopeMethod::Cepstral
        ? analyzeCepstralEnvelope(audio, start, duration, options)
        : analyzeLpcEnvelope(audio, start, duration, options);
}

double normalizedEnvelopeDistance(const Envelope& reference,
                                  const Envelope& candidate) {
    if (reference.db.size() != candidate.db.size()) {
        throw std::runtime_error("reference and candidate envelope grids differ");
    }
    const double referenceMean =
        std::accumulate(reference.db.begin(), reference.db.end(), 0.0) /
        reference.db.size();
    const double candidateMean =
        std::accumulate(candidate.db.begin(), candidate.db.end(), 0.0) /
        candidate.db.size();
    double squareSum = 0.0;
    for (size_t index = 0; index < reference.db.size(); ++index) {
        const double difference =
            (reference.db[index] - referenceMean) -
            (candidate.db[index] - candidateMean);
        squareSum += difference * difference;
    }
    return std::sqrt(squareSum / reference.db.size());
}

double formantNear(const Envelope& envelope, double lowerHz, double upperHz) {
    size_t best = envelope.db.size();
    for (size_t index = 1; index + 1 < envelope.db.size(); ++index) {
        if (envelope.frequencies[index] < lowerHz ||
            envelope.frequencies[index] > upperHz) {
            continue;
        }
        if (best == envelope.db.size() || envelope.db[index] > envelope.db[best]) {
            best = index;
        }
    }
    if (best == envelope.db.size()) {
        throw std::runtime_error("formant search range is outside envelope");
    }

    const double peakDb = envelope.db[best];
    double weightedFrequency = 0.0;
    double weightSum = 0.0;
    for (size_t index = 0; index < envelope.db.size(); ++index) {
        if (std::fabs(envelope.frequencies[index] -
                      envelope.frequencies[best]) > 80.0) {
            continue;
        }
        const double weight = std::exp(
            std::clamp((envelope.db[index] - peakDb) / 6.0, -12.0, 0.0));
        weightedFrequency += envelope.frequencies[index] * weight;
        weightSum += weight;
    }
    return weightSum > 0.0
        ? weightedFrequency / weightSum
        : envelope.frequencies[best];
}

struct FormantResult {
    double expectedHz = 0.0;
    double referenceHz = 0.0;
    double candidateHz = 0.0;
    double driftHz = 0.0;
    double driftPercent = 0.0;
    bool pass = false;
};

struct Analysis {
    Envelope reference;
    Envelope candidate;
    std::vector<FormantResult> formants;
    double envelopeDistanceDb = 0.0;
    bool pass = false;
};

Analysis analyze(const Audio& referenceAudio, const Audio& candidateAudio,
                 const Options& options) {
    if (referenceAudio.sampleRate != candidateAudio.sampleRate) {
        throw std::runtime_error("reference and candidate sample rates differ");
    }
    Analysis analysis;
    analysis.reference = analyzeEnvelope(
        referenceAudio, options.referenceStart, options.duration, options);
    analysis.candidate = analyzeEnvelope(
        candidateAudio, options.candidateStart, options.duration, options);
    analysis.envelopeDistanceDb =
        normalizedEnvelopeDistance(analysis.reference, analysis.candidate);

    bool allFormantsPass = true;
    for (size_t index = 0; index < options.expectedFormants.size(); ++index) {
        const double expected = options.expectedFormants[index];
        const double midpointLower = index == 0
            ? options.minimumHz
            : 0.5 * (options.expectedFormants[index - 1] + expected);
        const double midpointUpper = index + 1 == options.expectedFormants.size()
            ? options.maximumHz
            : 0.5 * (expected + options.expectedFormants[index + 1]);
        const double radius = std::max(180.0, expected * 0.25);
        const double lower = std::max(midpointLower, expected - radius);
        const double upper = std::min(midpointUpper, expected + radius);
        FormantResult result;
        result.expectedHz = expected;
        result.referenceHz = formantNear(analysis.reference, lower, upper);
        result.candidateHz = formantNear(analysis.candidate, lower, upper);
        result.driftHz = result.candidateHz - result.referenceHz;
        result.driftPercent =
            100.0 * result.driftHz / std::max(result.referenceHz, 1.0);
        result.pass = std::fabs(result.driftPercent) <= options.maxDriftPercent;
        allFormantsPass = allFormantsPass && result.pass;
        analysis.formants.push_back(result);
    }
    analysis.pass = allFormantsPass &&
                    analysis.reference.segmentRms >= options.minRms &&
                    analysis.candidate.segmentRms >= options.minRms &&
                    analysis.envelopeDistanceDb <= options.maxEnvelopeDb;
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
    output << "expected_hz,reference_hz,candidate_hz,drift_hz,drift_percent,status\n";
    for (const FormantResult& formant : analysis.formants) {
        output << formant.expectedHz << ',' << formant.referenceHz << ','
               << formant.candidateHz << ',' << formant.driftHz << ','
               << formant.driftPercent << ','
               << (formant.pass ? "PASS" : "FAIL") << '\n';
    }
}

void writeEnvelopeCsv(const Options& options, const Analysis& analysis) {
    if (options.envelopeCsvPath.empty()) return;
    std::ofstream output(options.envelopeCsvPath);
    if (!output) {
        throw std::runtime_error(
            "cannot write envelope CSV: " + options.envelopeCsvPath);
    }
    const double referenceMean =
        std::accumulate(analysis.reference.db.begin(),
                        analysis.reference.db.end(), 0.0) /
        analysis.reference.db.size();
    const double candidateMean =
        std::accumulate(analysis.candidate.db.begin(),
                        analysis.candidate.db.end(), 0.0) /
        analysis.candidate.db.size();
    output << "frequency_hz,reference_db,candidate_db,difference_db\n";
    for (size_t index = 0; index < analysis.reference.db.size(); ++index) {
        const double reference = analysis.reference.db[index] - referenceMean;
        const double candidate = analysis.candidate.db[index] - candidateMean;
        output << analysis.reference.frequencies[index] << ','
               << reference << ',' << candidate << ','
               << candidate - reference << '\n';
    }
}

void printResult(const Options& options, const Analysis& analysis) {
    std::cout << std::fixed << std::setprecision(3);
    if (options.json) {
        std::cout << "{\"reference\":" << jsonString(options.referencePath)
                  << ",\"candidate\":" << jsonString(options.candidatePath)
                  << ",\"method\":\"" << methodName(options.method) << "\""
                  << ",\"referenceRms\":" << analysis.reference.segmentRms
                  << ",\"candidateRms\":" << analysis.candidate.segmentRms
                  << ",\"referenceFrames\":" << analysis.reference.frames
                  << ",\"candidateFrames\":" << analysis.candidate.frames
                  << ",\"envelopeDistanceDb\":" << analysis.envelopeDistanceDb
                  << ",\"formants\":[";
        for (size_t index = 0; index < analysis.formants.size(); ++index) {
            const FormantResult& formant = analysis.formants[index];
            if (index) std::cout << ',';
            std::cout << "{\"expectedHz\":" << formant.expectedHz
                      << ",\"referenceHz\":" << formant.referenceHz
                      << ",\"candidateHz\":" << formant.candidateHz
                      << ",\"driftHz\":" << formant.driftHz
                      << ",\"driftPercent\":" << formant.driftPercent
                      << ",\"status\":\"" << (formant.pass ? "PASS" : "FAIL")
                      << "\"}";
        }
        std::cout << "],\"status\":\"" << (analysis.pass ? "PASS" : "FAIL")
                  << "\"}\n";
        return;
    }

    std::cout << "formant  reference  candidate   drift       status\n";
    for (size_t index = 0; index < analysis.formants.size(); ++index) {
        const FormantResult& formant = analysis.formants[index];
        std::cout << 'F' << (index + 1) << "       "
                  << std::setw(8) << formant.referenceHz << "  "
                  << std::setw(9) << formant.candidateHz << "  "
                  << std::showpos << std::setw(7) << formant.driftPercent
                  << std::noshowpos << "%  "
                  << (formant.pass ? "PASS" : "FAIL") << '\n';
    }
    std::cout << "normalized envelope RMS: " << analysis.envelopeDistanceDb
              << " dB  status: " << (analysis.pass ? "PASS" : "FAIL") << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        const Audio reference = harmonizer::analysis::readWav(options.referencePath);
        const Audio candidate = harmonizer::analysis::readWav(options.candidatePath);
        const Analysis analysis = analyze(reference, candidate, options);
        writeCsv(options, analysis);
        writeEnvelopeCsv(options, analysis);
        printResult(options, analysis);
        return analysis.pass ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "formant_analyzer: " << error.what() << '\n';
        return 1;
    }
}

#include "analysis_audio.hpp"

#include <portaudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

enum class ProbeMode {
    Waveform,
    Envelope
};

struct Options {
    bool listDevices = false;
    bool selfTest = false;
    bool json = false;
    std::string inputDevice;
    std::string outputDevice;
    std::string capturePath;
    ProbeMode mode = ProbeMode::Envelope;
    int inputChannel = 0;
    int outputChannel = 0;
    int sampleRate = 44100;
    unsigned long framesPerBuffer = 128;
    int repeats = 12;
    double intervalMs = 500.0;
    double preRollMs = 500.0;
    double maxLatencyMs = 300.0;
    double amplitude = 0.05;
    double minCorrelation = 0.20;
    double maxP95Ms = -1.0;
    double maxJitterMs = -1.0;
};

void usage(const char* executable) {
    std::cerr
        << "usage:\n"
        << "  " << executable << " --list-devices\n"
        << "  " << executable << " --self-test [--mode waveform|envelope]\n"
        << "  " << executable << " --input <name|index> --output <name|index> [options]\n\n"
        << "  --input-channel <index>      zero-based input channel\n"
        << "  --output-channel <index>     zero-based output channel\n"
        << "  --sample-rate <Hz>           default 44100\n"
        << "  --frames <count>             PortAudio callback quantum (default 128)\n"
        << "  --repeats <count>            probe repetitions (default 12)\n"
        << "  --interval-ms <ms>           spacing between probes (default 500)\n"
        << "  --max-latency-ms <ms>        correlation search range (default 300)\n"
        << "  --amplitude <0..0.25>        output level (default 0.05)\n"
        << "  --mode waveform|envelope     cable/unity or pitch-shifter-safe analysis\n"
        << "  --min-correlation <0..1>     required normalized correlation\n"
        << "  --max-p95-ms <ms>            optional measured-latency failure threshold\n"
        << "  --max-jitter-ms <ms>         optional P95-minus-P50 failure threshold\n"
        << "  --capture <path.wav>         save recorded input\n"
        << "  --json                       print one JSON object\n\n"
        << "Waveform mode is most precise for a cable, dry path, or virtual unity route.\n"
        << "Envelope mode uses a coded voiced burst that survives pitch shifting. Hold\n"
        << "A3 (MIDI 57) for a unison harmonizer measurement and avoid acoustic feedback.\n";
}

ProbeMode parseMode(const std::string& text) {
    if (text == "waveform") return ProbeMode::Waveform;
    if (text == "envelope") return ProbeMode::Envelope;
    throw std::runtime_error("--mode must be waveform or envelope");
}

const char* modeName(ProbeMode mode) {
    return mode == ProbeMode::Waveform ? "waveform" : "envelope";
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
        if (argument == "--list-devices") options.listDevices = true;
        else if (argument == "--self-test") options.selfTest = true;
        else if (argument == "--json") options.json = true;
        else if (argument == "--input") options.inputDevice = value("--input");
        else if (argument == "--output") options.outputDevice = value("--output");
        else if (argument == "--capture") options.capturePath = value("--capture");
        else if (argument == "--mode") options.mode = parseMode(value("--mode"));
        else if (argument == "--input-channel") {
            options.inputChannel = std::stoi(value("--input-channel"));
        } else if (argument == "--output-channel") {
            options.outputChannel = std::stoi(value("--output-channel"));
        } else if (argument == "--sample-rate") {
            options.sampleRate = std::stoi(value("--sample-rate"));
        } else if (argument == "--frames") {
            options.framesPerBuffer =
                static_cast<unsigned long>(std::stoul(value("--frames")));
        } else if (argument == "--repeats") {
            options.repeats = std::stoi(value("--repeats"));
        } else if (argument == "--interval-ms") {
            options.intervalMs = std::stod(value("--interval-ms"));
        } else if (argument == "--max-latency-ms") {
            options.maxLatencyMs = std::stod(value("--max-latency-ms"));
        } else if (argument == "--amplitude") {
            options.amplitude = std::stod(value("--amplitude"));
        } else if (argument == "--min-correlation") {
            options.minCorrelation = std::stod(value("--min-correlation"));
        } else if (argument == "--max-p95-ms") {
            options.maxP95Ms = std::stod(value("--max-p95-ms"));
        } else if (argument == "--max-jitter-ms") {
            options.maxJitterMs = std::stod(value("--max-jitter-ms"));
        } else if (argument == "--help" || argument == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown option: " + argument);
        }
    }

    if (options.listDevices || options.selfTest) return options;
    if (options.inputDevice.empty() || options.outputDevice.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    if (options.inputChannel < 0 || options.outputChannel < 0 ||
        options.sampleRate < 8000 || options.framesPerBuffer == 0 ||
        options.repeats < 3 || options.repeats > 100 ||
        options.intervalMs < 150.0 || options.maxLatencyMs <= 0.0 ||
        options.maxLatencyMs + 20.0 >= options.intervalMs ||
        options.amplitude <= 0.0 || options.amplitude > 0.25 ||
        options.minCorrelation < 0.0 || options.minCorrelation > 1.0) {
        throw std::runtime_error("invalid latency probe option");
    }
    return options;
}

struct PortAudioSession {
    PortAudioSession() {
        const PaError error = Pa_Initialize();
        if (error != paNoError) {
            throw std::runtime_error(
                std::string("PortAudio initialization failed: ") +
                Pa_GetErrorText(error));
        }
    }
    ~PortAudioSession() {
        Pa_Terminate();
    }
};

std::string lowercase(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

bool integerText(const std::string& text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

PaDeviceIndex findDevice(const std::string& spec, bool input) {
    const int count = Pa_GetDeviceCount();
    if (count < 0) {
        throw std::runtime_error(
            std::string("cannot enumerate PortAudio devices: ") +
            Pa_GetErrorText(count));
    }
    if (integerText(spec)) {
        const int index = std::stoi(spec);
        if (index < 0 || index >= count) throw std::runtime_error("device index is out of range");
        const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
        if (!info || (input ? info->maxInputChannels : info->maxOutputChannels) <= 0) {
            throw std::runtime_error("selected device has no requested-direction channels");
        }
        return index;
    }

    const std::string needle = lowercase(spec);
    std::vector<PaDeviceIndex> exact;
    std::vector<PaDeviceIndex> partial;
    for (PaDeviceIndex index = 0; index < count; ++index) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
        if (!info || (input ? info->maxInputChannels : info->maxOutputChannels) <= 0) {
            continue;
        }
        const std::string name = lowercase(info->name ? info->name : "");
        if (name == needle) exact.push_back(index);
        else if (name.find(needle) != std::string::npos) partial.push_back(index);
    }
    const std::vector<PaDeviceIndex>& matches = exact.empty() ? partial : exact;
    if (matches.empty()) throw std::runtime_error("no matching device: " + spec);
    if (matches.size() > 1) {
        std::string message = "ambiguous device '" + spec + "':";
        for (PaDeviceIndex index : matches) {
            message += " [" + std::to_string(index) + "] " +
                       Pa_GetDeviceInfo(index)->name;
        }
        throw std::runtime_error(message);
    }
    return matches.front();
}

void listDevices() {
    const int count = Pa_GetDeviceCount();
    if (count < 0) {
        throw std::runtime_error(
            std::string("cannot enumerate PortAudio devices: ") +
            Pa_GetErrorText(count));
    }
    std::cout << "index  in  out  sample-rate  host API       name\n";
    for (PaDeviceIndex index = 0; index < count; ++index) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
        const PaHostApiInfo* host = info ? Pa_GetHostApiInfo(info->hostApi) : nullptr;
        if (!info) continue;
        std::cout << std::setw(5) << index << "  "
                  << std::setw(2) << info->maxInputChannels << "  "
                  << std::setw(3) << info->maxOutputChannels << "  "
                  << std::setw(11) << static_cast<int>(std::llround(info->defaultSampleRate))
                  << "  " << std::setw(13) << (host ? host->name : "unknown")
                  << "  " << info->name << '\n';
    }
}

struct ProbeSignal {
    std::vector<float> probe;
    std::vector<float> output;
    std::vector<size_t> events;
};

std::vector<float> waveformProbe(double amplitude) {
    constexpr size_t length = 1024;
    std::vector<float> probe(length);
    uint32_t state = 0x5EED1234U;
    for (size_t index = 0; index < length; ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const double random =
            (static_cast<double>(state) / std::numeric_limits<uint32_t>::max()) *
            2.0 - 1.0;
        const double window = 0.5 - 0.5 * std::cos(
            2.0 * harmonizer::analysis::kPi * index / (length - 1));
        probe[index] = static_cast<float>(amplitude * random * window);
    }
    return probe;
}

std::vector<float> envelopeProbe(int sampleRate, double amplitude) {
    constexpr size_t length = 8192;
    constexpr size_t chipLength = 512;
    const int chips[] = {
        1, 0, 1, 1, 0, 0, 1, 0,
        1, 0, 0, 0, 1, 1, 1, 0
    };
    std::vector<float> probe(length);
    for (size_t index = 0; index < length; ++index) {
        const double time = static_cast<double>(index) / sampleRate;
        const double chip = chips[(index / chipLength) %
                                  (sizeof(chips) / sizeof(chips[0]))]
            ? 1.0 : 0.18;
        const double carrier =
            std::sin(2.0 * harmonizer::analysis::kPi * 220.0 * time) +
            0.45 * std::sin(2.0 * harmonizer::analysis::kPi * 440.0 * time) +
            0.20 * std::sin(2.0 * harmonizer::analysis::kPi * 660.0 * time);
        const size_t edgeLength = 256;
        double edge = 1.0;
        if (index < edgeLength) {
            edge = 0.5 - 0.5 * std::cos(
                harmonizer::analysis::kPi * index / edgeLength);
        } else if (index + edgeLength >= length) {
            edge = 0.5 - 0.5 * std::cos(
                harmonizer::analysis::kPi * (length - 1 - index) / edgeLength);
        }
        probe[index] = static_cast<float>(amplitude * 0.60 * chip * edge * carrier);
    }
    return probe;
}

ProbeSignal makeSignal(const Options& options) {
    ProbeSignal signal;
    signal.probe = options.mode == ProbeMode::Waveform
        ? waveformProbe(options.amplitude)
        : envelopeProbe(options.sampleRate, options.amplitude);
    const size_t preRoll = static_cast<size_t>(
        std::llround(options.preRollMs * options.sampleRate / 1000.0));
    const size_t interval = static_cast<size_t>(
        std::llround(options.intervalMs * options.sampleRate / 1000.0));
    const size_t tail = static_cast<size_t>(
        std::llround((options.maxLatencyMs + 150.0) *
                     options.sampleRate / 1000.0));
    const size_t total = preRoll +
                         static_cast<size_t>(options.repeats - 1) * interval +
                         signal.probe.size() + tail;
    signal.output.assign(total, 0.0f);
    for (int repeat = 0; repeat < options.repeats; ++repeat) {
        const size_t event = preRoll + static_cast<size_t>(repeat) * interval;
        signal.events.push_back(event);
        for (size_t index = 0; index < signal.probe.size(); ++index) {
            signal.output[event + index] += signal.probe[index];
        }
    }
    return signal;
}

std::vector<double> rmsEnvelope(const std::vector<float>& signal) {
    constexpr size_t window = 256;
    std::vector<double> prefix(signal.size() + 1, 0.0);
    for (size_t index = 0; index < signal.size(); ++index) {
        prefix[index + 1] = prefix[index] +
                            static_cast<double>(signal[index]) * signal[index];
    }
    std::vector<double> envelope(signal.size());
    for (size_t index = 0; index < signal.size(); ++index) {
        const size_t begin = index >= window - 1 ? index - (window - 1) : 0;
        const size_t count = index - begin + 1;
        envelope[index] = std::sqrt(
            (prefix[index + 1] - prefix[begin]) / static_cast<double>(count));
    }
    return envelope;
}

std::vector<double> feature(const std::vector<float>& signal, ProbeMode mode) {
    if (mode == ProbeMode::Envelope) return rmsEnvelope(signal);
    return std::vector<double>(signal.begin(), signal.end());
}

struct CorrelationPeak {
    double lagSamples = 0.0;
    double coefficient = 0.0;
};

double correlationAt(const std::vector<double>& referenceZeroMean,
                     double referenceEnergy,
                     const std::vector<double>& captured,
                     const std::vector<double>& prefix,
                     const std::vector<double>& prefixSquares,
                     size_t start) {
    const size_t length = referenceZeroMean.size();
    if (start + length > captured.size()) return 0.0;
    const double sum = prefix[start + length] - prefix[start];
    const double squareSum =
        prefixSquares[start + length] - prefixSquares[start];
    const double centeredEnergy =
        std::max(0.0, squareSum - sum * sum / static_cast<double>(length));
    if (centeredEnergy <= 1e-20 || referenceEnergy <= 1e-20) return 0.0;
    double dot = 0.0;
    for (size_t index = 0; index < length; ++index) {
        dot += referenceZeroMean[index] * captured[start + index];
    }
    return dot / std::sqrt(referenceEnergy * centeredEnergy);
}

CorrelationPeak findCorrelation(const std::vector<double>& reference,
                                const std::vector<double>& captured,
                                size_t event, size_t maxLag) {
    std::vector<double> centered = reference;
    const double mean = std::accumulate(centered.begin(), centered.end(), 0.0) /
                        centered.size();
    double referenceEnergy = 0.0;
    for (double& value : centered) {
        value -= mean;
        referenceEnergy += value * value;
    }
    std::vector<double> prefix(captured.size() + 1, 0.0);
    std::vector<double> prefixSquares(captured.size() + 1, 0.0);
    for (size_t index = 0; index < captured.size(); ++index) {
        prefix[index + 1] = prefix[index] + captured[index];
        prefixSquares[index + 1] =
            prefixSquares[index] + captured[index] * captured[index];
    }

    size_t bestLag = 0;
    double best = 0.0;
    const size_t maximum = std::min(
        maxLag, captured.size() > event + reference.size()
            ? captured.size() - event - reference.size()
            : 0);
    for (size_t lag = 0; lag <= maximum; ++lag) {
        const double coefficient = correlationAt(
            centered, referenceEnergy, captured, prefix, prefixSquares, event + lag);
        if (std::fabs(coefficient) > std::fabs(best)) {
            best = coefficient;
            bestLag = lag;
        }
    }

    double fractional = 0.0;
    if (bestLag > 0 && bestLag < maximum) {
        const double left = std::fabs(correlationAt(
            centered, referenceEnergy, captured, prefix, prefixSquares,
            event + bestLag - 1));
        const double center = std::fabs(best);
        const double right = std::fabs(correlationAt(
            centered, referenceEnergy, captured, prefix, prefixSquares,
            event + bestLag + 1));
        const double denominator = left - 2.0 * center + right;
        if (std::fabs(denominator) > 1e-12) {
            fractional = std::clamp(
                0.5 * (left - right) / denominator, -0.5, 0.5);
        }
    }
    return {static_cast<double>(bestLag) + fractional, std::fabs(best)};
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(values.begin(), values.end());
    const double position = fraction * (values.size() - 1);
    const size_t low = static_cast<size_t>(std::floor(position));
    const size_t high = static_cast<size_t>(std::ceil(position));
    const double blend = position - low;
    return values[low] * (1.0 - blend) + values[high] * blend;
}

struct Result {
    std::vector<double> latenciesMs;
    std::vector<double> correlations;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double jitterMs = 0.0;
    double minimumCorrelation = 0.0;
    double reportedInputMs = 0.0;
    double reportedOutputMs = 0.0;
    int overflows = 0;
    int underflows = 0;
    bool pass = false;
};

Result analyzeCapture(const ProbeSignal& signal,
                      const std::vector<float>& captured,
                      const Options& options) {
    const std::vector<double> reference = feature(signal.probe, options.mode);
    const std::vector<double> captureFeature = feature(captured, options.mode);
    const size_t maxLag = static_cast<size_t>(
        std::llround(options.maxLatencyMs * options.sampleRate / 1000.0));

    Result result;
    for (size_t event : signal.events) {
        const CorrelationPeak peak =
            findCorrelation(reference, captureFeature, event, maxLag);
        result.correlations.push_back(peak.coefficient);
        if (peak.coefficient >= options.minCorrelation) {
            result.latenciesMs.push_back(
                peak.lagSamples * 1000.0 / options.sampleRate);
        }
    }
    result.minimumCorrelation = result.correlations.empty()
        ? 0.0
        : *std::min_element(result.correlations.begin(), result.correlations.end());
    result.p50Ms = percentile(result.latenciesMs, 0.50);
    result.p95Ms = percentile(result.latenciesMs, 0.95);
    result.p99Ms = percentile(result.latenciesMs, 0.99);
    result.jitterMs = result.p95Ms - result.p50Ms;
    result.pass = result.latenciesMs.size() == signal.events.size() &&
                  (options.maxP95Ms < 0.0 || result.p95Ms <= options.maxP95Ms) &&
                  (options.maxJitterMs < 0.0 ||
                   result.jitterMs <= options.maxJitterMs);
    return result;
}

struct CallbackState {
    const std::vector<float>* output = nullptr;
    std::vector<float>* captured = nullptr;
    std::atomic<size_t> position{0};
    int inputChannels = 0;
    int outputChannels = 0;
    int inputChannel = 0;
    int outputChannel = 0;
    std::atomic<int> overflows{0};
    std::atomic<int> underflows{0};
};

int audioCallback(const void* inputBuffer, void* outputBuffer,
                  unsigned long framesPerBuffer,
                  const PaStreamCallbackTimeInfo*,
                  PaStreamCallbackFlags statusFlags, void* userData) {
    auto* state = static_cast<CallbackState*>(userData);
    const auto* input = static_cast<const float*>(inputBuffer);
    auto* output = static_cast<float*>(outputBuffer);
    const size_t first = state->position.fetch_add(framesPerBuffer);
    if (statusFlags & paInputOverflow) state->overflows.fetch_add(1);
    if (statusFlags & paOutputUnderflow) state->underflows.fetch_add(1);

    for (unsigned long frame = 0; frame < framesPerBuffer; ++frame) {
        const size_t position = first + frame;
        if (output) {
            for (int channel = 0; channel < state->outputChannels; ++channel) {
                output[frame * state->outputChannels + channel] = 0.0f;
            }
            if (position < state->output->size()) {
                output[frame * state->outputChannels + state->outputChannel] =
                    (*state->output)[position];
            }
        }
        if (position < state->captured->size()) {
            (*state->captured)[position] = input
                ? input[frame * state->inputChannels + state->inputChannel]
                : 0.0f;
        }
    }
    return first + framesPerBuffer >= state->output->size()
        ? paComplete
        : paContinue;
}

Result runLive(const Options& options, const ProbeSignal& signal,
               std::vector<float>& captured) {
    const PaDeviceIndex inputIndex = findDevice(options.inputDevice, true);
    const PaDeviceIndex outputIndex = findDevice(options.outputDevice, false);
    const PaDeviceInfo* inputInfo = Pa_GetDeviceInfo(inputIndex);
    const PaDeviceInfo* outputInfo = Pa_GetDeviceInfo(outputIndex);
    if (options.inputChannel >= inputInfo->maxInputChannels ||
        options.outputChannel >= outputInfo->maxOutputChannels) {
        throw std::runtime_error("selected channel is outside the device channel count");
    }

    PaStreamParameters inputParameters{};
    inputParameters.device = inputIndex;
    inputParameters.channelCount = options.inputChannel + 1;
    inputParameters.sampleFormat = paFloat32;
    inputParameters.suggestedLatency = inputInfo->defaultLowInputLatency;

    PaStreamParameters outputParameters{};
    outputParameters.device = outputIndex;
    outputParameters.channelCount = options.outputChannel + 1;
    outputParameters.sampleFormat = paFloat32;
    outputParameters.suggestedLatency = outputInfo->defaultLowOutputLatency;

    const PaError supported = Pa_IsFormatSupported(
        &inputParameters, &outputParameters, options.sampleRate);
    if (supported != paFormatIsSupported) {
        throw std::runtime_error(
            std::string("selected full-duplex route is unsupported: ") +
            Pa_GetErrorText(supported));
    }

    captured.assign(signal.output.size(), 0.0f);
    CallbackState state;
    state.output = &signal.output;
    state.captured = &captured;
    state.inputChannels = inputParameters.channelCount;
    state.outputChannels = outputParameters.channelCount;
    state.inputChannel = options.inputChannel;
    state.outputChannel = options.outputChannel;

    PaStream* stream = nullptr;
    PaError error = Pa_OpenStream(
        &stream, &inputParameters, &outputParameters, options.sampleRate,
        options.framesPerBuffer, paClipOff, audioCallback, &state);
    if (error != paNoError) {
        throw std::runtime_error(
            std::string("PortAudio stream open failed: ") + Pa_GetErrorText(error));
    }
    struct StreamCloser {
        PaStream* stream = nullptr;
        ~StreamCloser() {
            if (stream) Pa_CloseStream(stream);
        }
    } closer{stream};

    const PaStreamInfo* streamInfo = Pa_GetStreamInfo(stream);
    std::cerr << "latency probe: " << outputInfo->name << " channel "
              << options.outputChannel << " -> " << inputInfo->name << " channel "
              << options.inputChannel << ", " << modeName(options.mode)
              << " mode, amplitude " << options.amplitude << '\n';

    error = Pa_StartStream(stream);
    if (error != paNoError) {
        throw std::runtime_error(
            std::string("PortAudio stream start failed: ") + Pa_GetErrorText(error));
    }
    while ((error = Pa_IsStreamActive(stream)) == 1) Pa_Sleep(20);
    if (error < 0) {
        throw std::runtime_error(
            std::string("PortAudio stream failed: ") + Pa_GetErrorText(error));
    }
    error = Pa_StopStream(stream);
    if (error != paNoError && error != paStreamIsStopped) {
        throw std::runtime_error(
            std::string("PortAudio stream stop failed: ") + Pa_GetErrorText(error));
    }

    Result result = analyzeCapture(signal, captured, options);
    result.reportedInputMs =
        streamInfo ? streamInfo->inputLatency * 1000.0 : 0.0;
    result.reportedOutputMs =
        streamInfo ? streamInfo->outputLatency * 1000.0 : 0.0;
    result.overflows = state.overflows.load();
    result.underflows = state.underflows.load();
    result.pass = result.pass && result.overflows == 0 && result.underflows == 0;
    return result;
}

Result runSelfTest(const Options& options) {
    ProbeSignal signal = makeSignal(options);
    const size_t delay = 1234;
    std::vector<float> captured(signal.output.size(), 0.0f);
    uint32_t noiseState = 0x12345678U;
    for (size_t index = 0; index + delay < captured.size(); ++index) {
        noiseState = noiseState * 1664525U + 1013904223U;
        const float noise = static_cast<float>(
            (static_cast<double>(noiseState) /
             std::numeric_limits<uint32_t>::max() - 0.5) * 0.0002);
        captured[index + delay] = 0.67f * signal.output[index] + noise;
    }
    Result result = analyzeCapture(signal, captured, options);
    const double expectedMs = delay * 1000.0 / options.sampleRate;
    const double toleranceMs =
        options.mode == ProbeMode::Waveform ? 0.10 : 1.50;
    result.pass = result.pass &&
                  std::fabs(result.p50Ms - expectedMs) <= toleranceMs &&
                  result.jitterMs <= toleranceMs;
    return result;
}

void printResult(const Options& options, const Result& result, bool selfTest) {
    std::cout << std::fixed << std::setprecision(3);
    if (options.json) {
        auto number = [](double value) {
            if (std::isfinite(value)) {
                std::cout << value;
            } else {
                std::cout << "null";
            }
        };
        std::cout << "{\"mode\":\"" << modeName(options.mode)
                  << "\",\"selfTest\":" << (selfTest ? "true" : "false")
                  << ",\"validRepeats\":" << result.latenciesMs.size()
                  << ",\"requestedRepeats\":" << options.repeats
                  << ",\"p50Ms\":";
        number(result.p50Ms);
        std::cout << ",\"p95Ms\":";
        number(result.p95Ms);
        std::cout << ",\"p99Ms\":";
        number(result.p99Ms);
        std::cout << ",\"jitterMs\":";
        number(result.jitterMs);
        std::cout << ",\"minCorrelation\":";
        number(result.minimumCorrelation);
        std::cout << ",\"reportedInputMs\":";
        number(result.reportedInputMs);
        std::cout << ",\"reportedOutputMs\":";
        number(result.reportedOutputMs);
        std::cout << ",\"inputOverflows\":" << result.overflows
                  << ",\"outputUnderflows\":" << result.underflows
                  << ",\"status\":\"" << (result.pass ? "PASS" : "FAIL")
                  << "\"}\n";
        return;
    }
    std::cout << "round trip: P50 " << result.p50Ms
              << " ms  P95 " << result.p95Ms
              << " ms  P99 " << result.p99Ms
              << " ms  jitter " << result.jitterMs << " ms\n"
              << "valid probes: " << result.latenciesMs.size() << '/'
              << options.repeats << "  minimum correlation: "
              << result.minimumCorrelation << "\n";
    if (!selfTest) {
        std::cout << "PortAudio reports input " << result.reportedInputMs
                  << " ms + output " << result.reportedOutputMs
                  << " ms; xruns " << result.overflows << " in / "
                  << result.underflows << " out\n";
    }
    std::cout << "status: " << (result.pass ? "PASS" : "FAIL") << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (options.selfTest) {
            const Result result = runSelfTest(options);
            printResult(options, result, true);
            return result.pass ? 0 : 2;
        }

        PortAudioSession portAudio;
        if (options.listDevices) {
            listDevices();
            return 0;
        }
        const ProbeSignal signal = makeSignal(options);
        std::vector<float> captured;
        const Result result = runLive(options, signal, captured);
        if (!options.capturePath.empty()) {
            harmonizer::analysis::writeMonoFloatWav(
                options.capturePath, options.sampleRate, captured);
        }
        printResult(options, result, false);
        return result.pass ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "latency_probe: " << error.what() << '\n';
        return 1;
    }
}

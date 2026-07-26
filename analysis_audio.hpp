#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace harmonizer::analysis {

constexpr double kPi = 3.14159265358979323846;

inline uint16_t u16le(const unsigned char* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t u32le(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint32_t readU32(std::istream& input) {
    unsigned char bytes[4] = {};
    input.read(reinterpret_cast<char*>(bytes), 4);
    if (!input) throw std::runtime_error("unexpected EOF while reading WAV");
    return u32le(bytes);
}

inline float readSample(const unsigned char* p, uint16_t format, uint16_t bits) {
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
    int channels = 0;
    std::vector<float> mono;
};

inline Audio readWav(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open WAV: " + path);

    char riff[4] = {};
    char wave[4] = {};
    input.read(riff, 4);
    (void)readU32(input);
    input.read(wave, 4);
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

    while (input && (!format || data.empty())) {
        char id[4] = {};
        input.read(id, 4);
        if (!input) break;
        const uint32_t size = readU32(input);
        if (std::strncmp(id, "fmt ", 4) == 0) {
            std::vector<unsigned char> fmt(size);
            input.read(reinterpret_cast<char*>(fmt.data()), size);
            if (fmt.size() < 16) throw std::runtime_error("short WAV fmt chunk");
            format = u16le(fmt.data());
            channels = u16le(fmt.data() + 2);
            sampleRate = u32le(fmt.data() + 4);
            blockAlign = u16le(fmt.data() + 12);
            bits = u16le(fmt.data() + 14);
            if (format == 0xFFFE && fmt.size() >= 40) {
                format = u16le(fmt.data() + 24);
            }
        } else if (std::strncmp(id, "data", 4) == 0) {
            data.resize(size);
            input.read(reinterpret_cast<char*>(data.data()), size);
        } else {
            input.seekg(size, std::ios::cur);
        }
        if (size & 1U) input.seekg(1, std::ios::cur);
    }

    if (!format || data.empty() || channels == 0 || blockAlign == 0) {
        throw std::runtime_error("WAV is missing usable fmt/data chunks");
    }
    const int bytesPerSample = bits / 8;
    if (bytesPerSample <= 0) throw std::runtime_error("invalid WAV bit depth");

    Audio audio;
    audio.sampleRate = static_cast<int>(sampleRate);
    audio.channels = static_cast<int>(channels);
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

inline void writeU16(std::ostream& output, uint16_t value) {
    const unsigned char bytes[2] = {
        static_cast<unsigned char>(value & 0xFF),
        static_cast<unsigned char>((value >> 8) & 0xFF)
    };
    output.write(reinterpret_cast<const char*>(bytes), 2);
}

inline void writeU32(std::ostream& output, uint32_t value) {
    const unsigned char bytes[4] = {
        static_cast<unsigned char>(value & 0xFF),
        static_cast<unsigned char>((value >> 8) & 0xFF),
        static_cast<unsigned char>((value >> 16) & 0xFF),
        static_cast<unsigned char>((value >> 24) & 0xFF)
    };
    output.write(reinterpret_cast<const char*>(bytes), 4);
}

inline void writeMonoFloatWav(const std::string& path, int sampleRate,
                              const std::vector<float>& samples) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot write WAV: " + path);
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    output.write("RIFF", 4);
    writeU32(output, 36U + dataBytes);
    output.write("WAVE", 4);
    output.write("fmt ", 4);
    writeU32(output, 16);
    writeU16(output, 3);
    writeU16(output, 1);
    writeU32(output, static_cast<uint32_t>(sampleRate));
    writeU32(output, static_cast<uint32_t>(sampleRate * sizeof(float)));
    writeU16(output, sizeof(float));
    writeU16(output, 32);
    output.write("data", 4);
    writeU32(output, dataBytes);
    output.write(reinterpret_cast<const char*>(samples.data()), dataBytes);
    if (!output) throw std::runtime_error("failed while writing WAV: " + path);
}

inline size_t nextPowerOfTwo(size_t value) {
    size_t result = 1;
    while (result < value) {
        if (result > std::numeric_limits<size_t>::max() / 2) {
            throw std::runtime_error("FFT size overflow");
        }
        result <<= 1;
    }
    return result;
}

inline void fft(std::vector<std::complex<double>>& values, bool inverse = false) {
    const size_t size = values.size();
    if (size == 0 || (size & (size - 1)) != 0) {
        throw std::runtime_error("FFT size must be a nonzero power of two");
    }
    for (size_t index = 1, reversed = 0; index < size; ++index) {
        size_t bit = size >> 1;
        for (; reversed & bit; bit >>= 1) reversed ^= bit;
        reversed ^= bit;
        if (index < reversed) std::swap(values[index], values[reversed]);
    }
    const double direction = inverse ? 1.0 : -1.0;
    for (size_t length = 2; length <= size; length <<= 1) {
        const std::complex<double> step =
            std::polar(1.0, direction * 2.0 * kPi / static_cast<double>(length));
        for (size_t base = 0; base < size; base += length) {
            std::complex<double> phase(1.0, 0.0);
            for (size_t offset = 0; offset < length / 2; ++offset) {
                const std::complex<double> even = values[base + offset];
                const std::complex<double> odd =
                    values[base + offset + length / 2] * phase;
                values[base + offset] = even + odd;
                values[base + offset + length / 2] = even - odd;
                phase *= step;
            }
        }
    }
    if (inverse) {
        for (std::complex<double>& value : values) value /= static_cast<double>(size);
    }
}

inline double midiToHz(double midi) {
    return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
}

inline double hzToMidi(double hz) {
    return hz > 0.0 ? 69.0 + 12.0 * std::log2(hz / 440.0)
                    : -std::numeric_limits<double>::infinity();
}

inline double rms(const std::vector<float>& samples, size_t begin, size_t end) {
    begin = std::min(begin, samples.size());
    end = std::min(end, samples.size());
    if (end <= begin) return 0.0;
    double sum = 0.0;
    for (size_t index = begin; index < end; ++index) {
        sum += static_cast<double>(samples[index]) * samples[index];
    }
    return std::sqrt(sum / static_cast<double>(end - begin));
}

} // namespace harmonizer::analysis

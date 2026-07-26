// PITCH SHIFTER

#include <chrono>
#include <iostream>
#include <vector>
#include <unistd.h>
#include <portaudio.h>
#include <rubberband/RubberBandLiveShifter.h>

#define SAMPLE_RATE 44100
#define FRAMES_PER_BUFFER 512
#define CHANNELS 1
#define PITCH_SHIFT_RATIO (pow(2.0, 5.0 / 12.0))  // Shift down a perfect fourth
using namespace RubberBand;
static auto lastCallbackTime = std::chrono::high_resolution_clock::now();

// PortAudio callback
static int audioCallback(const void* inputBuffer, void* outputBuffer,
                         unsigned long framesPerBuffer,
                         const PaStreamCallbackTimeInfo* timeInfo,
                         PaStreamCallbackFlags statusFlags,
                         void* userData) {
    if (!inputBuffer) return paContinue;

    auto* shifter = static_cast<RubberBandLiveShifter*>(userData);

    // Cast PortAudio buffers to float* (since we're using paFloat32)
    float* in = (float*)inputBuffer;
    float* out = (float*)outputBuffer;

    // Wrap pointers in an array (RubberBand expects `float* const*`)
    float* inputArray[] = { in };
    float* outputArray[] = { out };

    // Process the pitch shift
    shifter->shift(inputArray, outputArray);

    // // Measure the time since last callback
    // auto now = std::chrono::high_resolution_clock::now();
    // double elapsed_ms = std::chrono::duration<double, std::milli>(now - lastCallbackTime).count();
    // lastCallbackTime = now;
    // std::cout << "Callback Interval: " << elapsed_ms << " ms" << std::endl;

    // memcpy(outputBuffer, inputBuffer, framesPerBuffer * sizeof(float));

    return paContinue;
}

int main() {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
        return 1;
    }

    RubberBandLiveShifter* liveShifter;
    liveShifter = new RubberBandLiveShifter(SAMPLE_RATE, CHANNELS, 0, RubberBandLiveShifter::OptionFormantPreserved | RubberBandLiveShifter::OptionWindowShort);
    liveShifter->setPitchScale(PITCH_SHIFT_RATIO);

    PaStream* stream;
    err = Pa_OpenDefaultStream(&stream, CHANNELS, CHANNELS, paFloat32, SAMPLE_RATE, FRAMES_PER_BUFFER, audioCallback, liveShifter);
    if (err != paNoError) {
        std::cerr << "PortAudio open stream error: " << Pa_GetErrorText(err) << std::endl;
        Pa_Terminate();
        return 1;
    }

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        std::cerr << "PortAudio start stream error: " << Pa_GetErrorText(err) << std::endl;
        Pa_CloseStream(stream);
        Pa_Terminate();
        return 1;
    }

    std::cout << "Listening... Press Enter to stop." << std::endl;
    std::cout << "Block Size: " << liveShifter->getBlockSize() << " Delay: " << liveShifter->getStartDelay() << std::endl;
    std::cin.get();  // Block until Enter is pressed

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();

    delete liveShifter;

    return 0;
}









// PITCH DETECTOR

#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <portaudio.h>
#include <aubio/aubio.h>
#include <SDL2/SDL.h>

#define SAMPLE_RATE 44100
#define BUFFER_SIZE 2048
#define HOP_SIZE 256
#define CHANNELS 1
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 400
#define CONFIDENCE_THRESHOLD 0.8
#define MIN_PITCH 36
#define MAX_PITCH 84

const float c_1 = 440 * std::pow(2, -69.0/12);
std::vector<float> pitchHistory(WINDOW_WIDTH, -1.0f);

struct PitchDetector {
    aubio_pitch_t* pitch;
    fvec_t* in;
    fvec_t* out;

    PitchDetector() {
        pitch = new_aubio_pitch("yinfft", BUFFER_SIZE, HOP_SIZE, SAMPLE_RATE);
        in = new_fvec(HOP_SIZE);
        out = new_fvec(1);
    }

    ~PitchDetector() {
        del_aubio_pitch(pitch);
        del_fvec(in);
        del_fvec(out);
    }

    float detectPitch(const float* buffer) {
        // float max = 0;
        // float min = 0;
        float rms = 0;
        for (size_t i = 0; i < HOP_SIZE; i++) {
            // float sample = buffer[i] / 32768.0f;
            float sample = buffer[i];
            in->data[i] = sample;
            rms += sample*sample;
            // max = std::max(max, buffer[i]/ 32768.0f);
            // min = std::min(min, buffer[i]/ 32768.0f);
        }
        aubio_pitch_do(pitch, in, out);
        float frequency = fvec_get_sample(out, 0);
        float pitch = 12 * log2(frequency/c_1);

        rms = std::sqrtf(rms);
        if (rms < 0.1 || pitch < MIN_PITCH || pitch > MAX_PITCH)
            return -1.0f;
        std::cout << "Freq: " << frequency << " RMS: " << rms << " Pitch:" << pitch << std::endl;
        // std::cout << "Min: " << min << " | Max: " << max << std::endl;
        return pitch;
    }
};

static int audioCallback(const void* inputBuffer, void* outputBuffer,
                         unsigned long framesPerBuffer,
                         const PaStreamCallbackTimeInfo* timeInfo,
                         PaStreamCallbackFlags statusFlags,
                         void* userData) {
    PitchDetector* detector = static_cast<PitchDetector*>(userData);

    if (!inputBuffer) return paContinue;

    float logPitch = detector->detectPitch(static_cast<const float*>(inputBuffer));
    // Update pitch history (for plotting)
    pitchHistory.erase(pitchHistory.begin());
    pitchHistory.push_back(logPitch);

    return paContinue;
}

int main() {
    // Initialize PortAudio with error checking
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
        return 1;
    }

    PitchDetector detector;

    // Open audio stream with error checking
    PaStream* stream;
    err = Pa_OpenDefaultStream(&stream, CHANNELS, 0, paFloat32, SAMPLE_RATE, HOP_SIZE, audioCallback, &detector);
    if (err != paNoError) {
        std::cerr << "PortAudio open stream error: " << Pa_GetErrorText(err) << std::endl;
        Pa_Terminate();
        return 1;
    }

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        std::cerr << "PortAudio start stream error: " << Pa_GetErrorText(err) << std::endl;
        Pa_CloseStream(stream);
        Pa_Terminate();
        return 1;
    }

    // Initialize SDL with error checking
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        Pa_Terminate();
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("Pitch Visualization",
                                         SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                         WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    if (!window) {
        std::cerr << "Window could not be created! SDL_Error: " << SDL_GetError() << std::endl;
        SDL_Quit();
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        Pa_Terminate();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        std::cerr << "Renderer could not be created! SDL_Error: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        Pa_Terminate();
        return 1;
    }
    const float PITCH_RANGE = MAX_PITCH - MIN_PITCH;

    bool running = true;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) running = false;
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        // Draw grid lines (optional)
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        for (int i = 0; i <= PITCH_RANGE/12 * 5; i++) {
            int y = WINDOW_HEIGHT - ((i*12+1)/5 + 1) * WINDOW_HEIGHT / PITCH_RANGE;
            SDL_RenderDrawLine(renderer, 0, y, WINDOW_WIDTH, y);
        }

        // Draw pitch history
        SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
        for (size_t i = 1; i < pitchHistory.size(); i++) {
            if (pitchHistory[i - 1] > 0 && pitchHistory[i] > 0) {
                int x1 = (i - 1);
                int x2 = i;

                // Map log pitch to window height with better scaling
                int y1 = WINDOW_HEIGHT - static_cast<int>((pitchHistory[i - 1] - MIN_PITCH) /
                                                         PITCH_RANGE * WINDOW_HEIGHT);
                int y2 = WINDOW_HEIGHT - static_cast<int>((pitchHistory[i] - MIN_PITCH) /
                                                         PITCH_RANGE * WINDOW_HEIGHT);

                // Clamp y values to window boundaries
                y1 = std::max(0, std::min(WINDOW_HEIGHT, y1));
                y2 = std::max(0, std::min(WINDOW_HEIGHT, y2));

                SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);  // ~60 FPS
    }

    // Clean up resources
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();

    return 0;
}











// AMPLITUDE DISPLAY

#include <iostream>
#include <vector>
#include <portaudio.h>
#include <SDL2/SDL.h>

#define SAMPLE_RATE 44100
#define FRAMES_PER_BUFFER 512
#define CHANNELS 1
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 400

std::vector<float> audioData(FRAMES_PER_BUFFER, 0.0f);

static int audioCallback(const void* inputBuffer, void* outputBuffer,
                         unsigned long framesPerBuffer,
                         const PaStreamCallbackTimeInfo* timeInfo,
                         PaStreamCallbackFlags statusFlags,
                         void* userData) {
    const float* input = static_cast<const float*>(inputBuffer);
    for (size_t i = 0; i < framesPerBuffer; i++) {
        audioData[i] = input ? input[i] : 0.0f;
    }
    return paContinue;
}

int main() {
    // Initialize PortAudio
    Pa_Initialize();
    PaStream* stream;
    Pa_OpenDefaultStream(&stream, CHANNELS, 0, paFloat32, SAMPLE_RATE, FRAMES_PER_BUFFER, audioCallback, nullptr);
    Pa_StartStream(stream);

    // Initialize SDL2
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("Microphone Waveform", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    bool running = true;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
        }

        // Clear screen
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        // Draw waveform
        SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
        for (size_t i = 1; i < FRAMES_PER_BUFFER; i++) {
            int x1 = (i - 1) * WINDOW_WIDTH / FRAMES_PER_BUFFER;
            int y1 = WINDOW_HEIGHT / 2 - (int)(audioData[i - 1] * (WINDOW_HEIGHT / 2));
            int x2 = i * WINDOW_WIDTH / FRAMES_PER_BUFFER;
            int y2 = WINDOW_HEIGHT / 2 - (int)(audioData[i] * (WINDOW_HEIGHT / 2));
            SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);  // ~60 FPS
    }

    // Cleanup
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    return 0;
}
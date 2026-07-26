#include <chrono>
#include <iostream>
#include <vector>
#include <unistd.h>
#include <portaudio.h>
#include <rubberband/RubberBandLiveShifter.h>
#include <aubio/aubio.h>
#include <SDL2/SDL.h>
#include <pa_mac_core.h>
#include <pthread.h>

#define SAMPLE_RATE 44100
#define FRAMES_PER_BUFFER 64
#define CHANNELS 1
#define BUFFER_SIZE 2048
#define HOP_SIZE 512
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 400

using namespace RubberBand;

const float c_1 = 440.0f * std::pow(2.0f, -69.0f / 12.0f);

const int MIN_NOTE = 36;
const int MAX_NOTE = 84;

float note2Freq(int note){ return c_1 * std::pow(2.0f, note / 12.0f);}

const float MIN_FREQUENCY = note2Freq(MIN_NOTE);
const float MAX_FREQUENCY = note2Freq(MAX_NOTE);

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

    float detectFrequency(const float* buffer) {
        float rms = 0;
        for (size_t i = 0; i < HOP_SIZE; i++) {
            rms += buffer[i]*buffer[i];
            in->data[i] = buffer[i];
        }
        aubio_pitch_do(pitch, in, out);
        float frequency = fvec_get_sample(out, 0);

        rms = std::sqrtf(rms/HOP_SIZE);
        if (rms < 0.0004 || frequency < MIN_FREQUENCY || frequency > MAX_FREQUENCY)
            return -1.0f;
        return frequency;
    }
};

struct Shifter {
    RubberBandLiveShifter* shifter;
    bool active = false;
    float targetNote = 0;

    Shifter() {
        shifter = new RubberBandLiveShifter(SAMPLE_RATE, CHANNELS, 0,
            RubberBandLiveShifter::OptionFormantPreserved | RubberBandLiveShifter::OptionWindowShort);
    }

    ~Shifter() {
        delete shifter;
    }
};

struct AudioProcessor {
    Shifter shifters[4];
    PitchDetector detector;
    std::mutex pitchMutex;
};

static float inputCircularBuffer[HOP_SIZE] = {0};
static float outputCircularBuffer[HOP_SIZE] = {0};
static int bufferFill = 0;
static int bufferReadPos = 0;

static int audioCallback(const void* inputBuffer, void* outputBuffer,
                         unsigned long framesPerBuffer,
                         const PaStreamCallbackTimeInfo* timeInfo,
                         PaStreamCallbackFlags statusFlags,
                         void* userData) {
    if (!inputBuffer) return paContinue;

    auto* processor = static_cast<AudioProcessor*>(userData);
    float* in = (float*)inputBuffer;
    float* out = (float*)outputBuffer;

    // Copy new samples into the circular buffer
    for (int i = 0; i < FRAMES_PER_BUFFER; i++) {
        inputCircularBuffer[(bufferFill + i) % HOP_SIZE] = in[i];
    }

    bufferFill += FRAMES_PER_BUFFER;

    // Only process when buffer is full (every 512 samples)
    if (bufferFill == HOP_SIZE) {
        bufferFill = 0;  // Reset bufferFill since we are processing now

        // Call pitch detection
        float frequency = processor->detector.detectFrequency(inputCircularBuffer);
        pitchHistory.erase(pitchHistory.begin());
        pitchHistory.push_back(12 * log2(frequency/c_1));

        // Call pitch shifting
        float* inputArray[] = { inputCircularBuffer };
        float* outputArray[] = { outputCircularBuffer };

        // processor->shifters[0].shifter->shift(inputArray, outputArray);

        int activeCount = 0;
        for (int i = 0; i < 4; i++) {
            if (processor->shifters[i].active) {
                activeCount++;
            }
        }
        if (activeCount > 0) {
            float mixFactor = 8.0f / activeCount; // Each shifter contributes equally

            float mixBuffer[HOP_SIZE] = {0};  // Separate buffer for accumulation

            for (int i = 0; i < 4; i++) {
                if (processor->shifters[i].active) {
                    float tempOutput[HOP_SIZE] = {0};
                    float* tempOutputArray[] = { tempOutput };

                    std::lock_guard<std::mutex> lock(processor->pitchMutex);
                    processor->shifters[i].shifter->shift(inputArray, tempOutputArray);

                    // Accumulate into mixBuffer
                    for (int j = 0; j < HOP_SIZE; j++) {
                        mixBuffer[j] += tempOutput[j];
                    }
                }
            }

            // Copy mixed buffer to output, applying mix factor
            for (int j = 0; j < HOP_SIZE; j++) {
                outputArray[0][j] = mixBuffer[j] * mixFactor;
            }
        } else {
            for (int j = 0; j < HOP_SIZE; j++) {
                outputArray[0][j] = 0;
            }
        }

        bufferReadPos = 0;  // Reset read position
    }

    // Copy processed samples to output buffer in 64-sample chunks
    for (int i = 0; i < FRAMES_PER_BUFFER; i++) {
        out[i] = outputCircularBuffer[(bufferReadPos + i) % HOP_SIZE];
    }

    bufferReadPos = (bufferReadPos + FRAMES_PER_BUFFER) % HOP_SIZE;

    return paContinue;
}

const SDL_Keycode keys[] = {SDLK_LSHIFT, SDLK_a, SDLK_z, SDLK_s, SDLK_x, SDLK_d, SDLK_c, SDLK_v, SDLK_g, SDLK_b, SDLK_h, SDLK_n, SDLK_m, SDLK_k, SDLK_COMMA, SDLK_l, SDLK_PERIOD, SDLK_SEMICOLON, SDLK_SLASH, SDLK_TAB, SDLK_1, SDLK_q, SDLK_2, SDLK_w, SDLK_e, SDLK_4, SDLK_r, SDLK_5, SDLK_t, SDLK_6, SDLK_y, SDLK_u, SDLK_8, SDLK_i, SDLK_9, SDLK_o, SDLK_p, SDLK_MINUS, SDLK_LEFTBRACKET, SDLK_EQUALS, SDLK_RIGHTBRACKET, SDLK_BACKSPACE, SDLK_BACKSLASH};

void keyPress(SDL_Keycode k, bool press, AudioProcessor* AP) {
    int note = -1; // Default invalid note
    for (int i = 0; i < (sizeof(keys) / sizeof(keys[0])); i++) {
        if (k == keys[i]) {
            note = 40 + i;
            break; // Stop after finding the key
        }
    }
    if (note == -1) return; // Invalid key, exit early

    if (!press) {
        // Deactivate the shifter with the matching note
        for (int i = 0; i < 4; i++) {
            if (AP->shifters[i].targetNote == note) {
                AP->shifters[i].active = false;
            }
        }
        return;
    }

    // Try to find an inactive shifter first
    Shifter* selectedShifter = nullptr;
    for (int i = 0; i < 4; i++) {
        if (!AP->shifters[i].active) {
            selectedShifter = &AP->shifters[i];
            break;
        }
    }

    // If no inactive shifter was found, find the closest target note
    if (!selectedShifter) {
        selectedShifter = &AP->shifters[0]; // Start with first shifter
        for (int i = 1; i < 4; i++) {
            if (std::abs(AP->shifters[i].targetNote - note) < std::abs(selectedShifter->targetNote - note)) {
                selectedShifter = &AP->shifters[i];
            }
        }
    }

    // Assign the selected shifter to the new note and activate it
    selectedShifter->targetNote = note;
    selectedShifter->active = true;
}

int main() {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
        return 1;
    }

    AudioProcessor* processor = new AudioProcessor();

    PaStreamParameters inputParams, outputParams;
    PaStream* stream;

    // Get default input device
    inputParams.device = Pa_GetDefaultInputDevice();
    inputParams.channelCount = CHANNELS;
    inputParams.sampleFormat = paFloat32;
    inputParams.suggestedLatency = 0;

    // Get default output device
    outputParams.device = Pa_GetDefaultOutputDevice();
    outputParams.channelCount = CHANNELS;
    outputParams.sampleFormat = paFloat32;
    outputParams.suggestedLatency = 0;

    // Open the stream with explicit low-latency settings
    err = Pa_OpenStream(&stream, &inputParams, &outputParams, SAMPLE_RATE, FRAMES_PER_BUFFER, paClipOff | paPrimeOutputBuffersUsingStreamCallback, audioCallback, processor);
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

    const PaStreamInfo* streamInfo = Pa_GetStreamInfo(stream);
    std::cout << "PortAudio Latency: "
          << " Input: " << streamInfo->inputLatency * 1000 << " ms"
          << " | Output: " << streamInfo->outputLatency * 1000 << " ms"
          << " | Total: " << (streamInfo->inputLatency + streamInfo->outputLatency) * 1000 << " ms"
          << std::endl;

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
    const float PITCH_RANGE = MAX_NOTE - MIN_NOTE;

    bool running = true;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            else if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                keyPress(event.key.keysym.sym, true, processor);  // Handle Note On
            } else if (event.type == SDL_KEYUP) {
                keyPress(event.key.keysym.sym, false, processor);  // Handle Note Off
            }
        }
        bool stablePitch = false;
        if (pitchHistory.size() >= 2) {
            float prev1 = pitchHistory.back();  // Last element
            float prev2 = *(std::prev(pitchHistory.end(), 2));
            stablePitch = abs(prev1-prev2) < 1;
            if (prev1 > 0 && prev2 > 0 && stablePitch){
                for (int i = 0; i<4; i++){
                    std::lock_guard<std::mutex> lock(processor->pitchMutex);
                    processor->shifters[i].shifter->setPitchScale(note2Freq(processor->shifters[i].targetNote)/note2Freq(prev1));
                }
            }
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
                int y1 = WINDOW_HEIGHT - static_cast<int>((pitchHistory[i - 1] - MIN_NOTE) /
                                                         PITCH_RANGE * WINDOW_HEIGHT);
                int y2 = WINDOW_HEIGHT - static_cast<int>((pitchHistory[i] - MIN_NOTE) /
                                                         PITCH_RANGE * WINDOW_HEIGHT);

                // Clamp y values to window boundaries
                y1 = std::max(0, std::min(WINDOW_HEIGHT, y1));
                y2 = std::max(0, std::min(WINDOW_HEIGHT, y2));

                SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(5);  // ~60 FPS
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
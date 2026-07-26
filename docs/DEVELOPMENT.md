# Development

## Dependencies

Harmonizer requires CMake 3.20+, Ninja, a C++17 compiler, pkg-config,
PortAudio, and aubio. CMake downloads the pinned Rubber Band 4.0.0 source and
verifies its checksum.

### macOS

```bash
brew install cmake ninja pkg-config portaudio aubio
```

### Ubuntu or Debian

```bash
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build pkg-config \
  portaudio19-dev libaubio-dev
```

### Windows

Use an MSYS2 UCRT64 shell:

```bash
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-portaudio \
  mingw-w64-ucrt-x86_64-aubio
```

## Build and test

```bash
cmake --preset test
cmake --build --preset test
ctest --preset test
node public/test-pitch.mjs
```

Start the native control surface with:

```bash
./build/test/harmonizer_web
```

Then open `http://127.0.0.1:8794/`.

## Audio fixtures

Run `./scripts/fetch_vocadito.sh` to install the optional annotated singing
dataset locally. It is deliberately ignored by Git. The fast CTest suite does
not require downloaded audio; the larger quality audits do.

## Repository layout

- `harmonizer_web.cpp`: native server, audio routing, MIDI, and DSP orchestration
- `harmonizer_rubberband_engine.hpp`: production Live 512 engine
- `backends/`: experimental engines and shared control components
- `web/`: native application's browser control surface
- `public/`: install-free Web Audio edition
- `tests/`: deterministic C++ regression tests
- `scripts/`: builds, packaging, quality audits, and fixture helpers
- `packaging/`: launchers and platform metadata
- `docs/`: contributor and release documentation

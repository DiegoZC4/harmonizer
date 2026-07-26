# Harmonizer

A live, MIDI-controlled vocal harmonizer with formant-preserving pitch shifting.
The microphone and DSP run locally; the browser is the control surface.

[![CI](https://github.com/DiegoZC4/harmonizer/actions/workflows/ci.yml/badge.svg)](https://github.com/DiegoZC4/harmonizer/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/DiegoZC4/harmonizer)](https://github.com/DiegoZC4/harmonizer/releases/latest)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)

## Choose an edition

The [browser edition](https://diegozc4.github.io/harmonizer/) works without
installing anything on current
Chrome or Edge for macOS, Windows, Linux, and ChromeOS. Audio never leaves the
browser. Use headphones, press **Start audio**, choose the microphone/output,
and play harmony notes with MIDI or the computer keyboard.

The [native edition](https://github.com/DiegoZC4/harmonizer/releases/latest)
uses PortAudio, aubio, and Rubber Band 4 for lower-level audio routing and the
same browser GUI. Choose it for rehearsals and the Live 512 pitch shifter.

## Native downloads

- **macOS:** `Harmonizer-macOS-universal.dmg`
- **Windows 10 or 11:** `Harmonizer-Windows-x64-Setup.exe`
- **Linux x86_64:** `Harmonizer-Linux-x86_64.AppImage`

Every release also includes a portable Windows ZIP, a macOS ZIP, corresponding
source, and SHA-256 checksums. See `QUICKSTART.md` for first-launch and audio
setup.

## Source build

Clone the repository and run the installer for the current operating system:

```bash
git clone https://github.com/DiegoZC4/harmonizer.git
cd harmonizer
./scripts/install-macos.sh
```

```bash
./scripts/install-linux.sh
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install-windows.ps1
```

Installers build only the tested Rubber Band Live 512 backend by default; pass
`--all-backends` to include comparison engines. Contributor setup and test
commands are in `docs/DEVELOPMENT.md`.

## Manual build

Install PortAudio, aubio, CMake, a C++17 compiler, and pkg-config. Then:

```bash
rubberband_source="$(./scripts/fetch_rubberband.sh)"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHARMONIZER_BUILD_BACKEND_LAB=ON \
  -DFETCHCONTENT_SOURCE_DIR_RUBBERBAND="$rubberband_source"
cmake --build build --parallel
```

Rubber Band 4.0.0 is fetched from its official repository and verified by
SHA-256 because older operating-system packages do not include the
`RubberBandLiveShifter` API used here.

## Pitch-shifter backends

The native GUI has a **Pitch shifter** menu for Rubber Band Live 512, a parallel
R2 + Live 512 experiment, the 128-sample LiveShifter fork, Rubber Band R2 Short,
and Signalsmith Stretch.
Changing it briefly restarts the native audio engine on the same URL; only one
backend runs at a time, and the saved audio route and controls are restored.
The parallel backend runs a 23.2 ms R2 onset path beside a 68.2 ms Live512
quality path (DSP-path estimates, before device latency). Its onset-aware F0
tracker uses a robust velocity update, and the R2 control timestamp is aligned
to the measured shifter response. Live512 retains the tested one-hop control
delay required by its internal buffered audio. The persisted
**Immediacy** draggable number sets the maximum R2 onset contribution. On a
vocal onset, source-pitch jump, or new MIDI voice, R2 anchors the attack and
then yields to full-quality Live512 over 30 ms after that path arrives. `0%`
is Live512 only and the default is `25% onset`.
Rubber Band Live 512 and the parallel backend also expose an **Unvoiced** pill:
**TC bypass** sends one
latency-aligned, unshifted consonant signal while pitch is unavailable, whereas
**Hold ratio** keeps pitch-shifting with the last stable source F0. The choice
is saved locally and included in diagnostic captures.

## Collier-inspired performance controls

The native C++ output bus now has three independent **Freeze** layers. Each can
be used as a latch or momentary hold, has its own level and continuous
`-24..+24` semitone transposition, and resets its reverb state after release so
the next capture does not inherit the old chord. MIDI CC 66, 67, and 68 trigger
layers 1, 2, and 3. The shared **Freeze tone** control moves the held texture
from closed/dark to open.

**Glide amount** and **Glide time** add portamento from the previously played
note to the exact new MIDI target. Optional stereo **Chorus** and musical
**Reverb** sit after the harmony bus. All of these use the existing draggable
number controls and localStorage persistence; glide, chorus, and reverb default
to off so the reference Live512 sound remains unchanged.

The freeze is an in-process Freeverb-style implementation inspired by the
documented behavior, not the original Ambience plugin or Bloomberg's private
configuration. See
`research/jacob_collier/PHkKMJ6DCZQ.transcript-notes.md` for the timestamped
2022 walkthrough and `research/jacob_collier/youtube_feature_catalog.md` for
the source catalog.

Separate input and output device clocks are reconciled by a PI-controlled
fractional bridge resampler. Audio callbacks only move samples; a wake-driven
real-time worker runs DSP, with the parallel quality voices spread across four
persistent lanes. The diagnostics expose bridge occupancy/rate correction,
callback and worker maxima, queue depth, xruns, and PortAudio-reported device
latency.

## Audio-device diagnostics

Input and output changes probe only the requested side. The current route stays
active until the replacement stream has opened and started, and a failed choice
is not saved. The browser reports the failing `open-input`, `start-input`,
`open-output`, or `start-output` stage, elapsed time, PortAudio code, and native
host error. `/api/state` exposes the same fields as `audioRoute*`,
`audioPortAudioError`, and `audioHostError*`.

When launched from DevHub on this Mac, native stderr and rebuild output are in:

```bash
tail -f /private/tmp/devhub-harmonizer-web.log
```

Direct terminal launches print the same route messages to stderr. For a macOS
USB-device failure, Audio MIDI Setup is the first independent check; reconnect
the device if CoreAudio cannot start it there either.

Build every backend and start the single-port development server with:

```bash
./scripts/build_backend_lab.sh
./scripts/run_harmonizer_web.sh
```

The standalone backend commands remain available for offline benchmarking.
See `backend_lab/README.md` for pinned dependencies, rejected candidates,
latency accounting, and listenable benchmark renders.

Run the predictor, dynamic-handoff, vibrato-lock, and parallel unvoiced
regressions with:

```bash
make test-parallel-control
make test-latency-control
make test-parallel-matrix
make test-collier-features
make test-polyphonic
make test-roundtrip-latency
```

## Keyboard layout

By default, the physical computer-keyboard map begins at F3 on Left Shift,
reaches B4 on Slash, continues at C5 on Tab, and reaches B6 on Backslash. The
**Keyboard octave** draggable number transposes the whole map by octaves, and
the corresponding physical keys are labeled on the piano display. Web MIDI
devices can be selected independently from the MIDI input menu.

## Testing

```bash
STRICT=1 make test-pitch
make test-vibrato-lock
make test-unvoiced-modes
make test-polyphonic
make test-formants
make test-roundtrip-latency
node public/test-pitch.mjs
```

The native fixture suite uses annotated monophonic singing recordings. The
unvoiced test renders the same synthetic vowel-to-sibilant phrase through both
articulation modes. The public test checks the browser pitch detector without
microphone input. The Collier feature test latches a real Vocadito phrase, then
analyzes the frozen output during digital silence across seven transposition
cases. It writes FFT metrics, WAV renders, and spectrograms to
`backend_lab/results/collier_features/`.

The polyphonic test renders one, four, eight, and sixteen simultaneous native
voices, measures every requested fundamental with a 65,536-point FFT, and
flags unexplained peaks that are both locally prominent and musically loud.
Targets hidden under another voice's harmonic are labeled `masked`; additional
upper-register eight-note cases expose those notes independently.

The formant audit renders deterministic `/a/`, `/i/`, and `/u/` source-filter
signals at `-24`, `-12`, `0`, `+12`, and `+24` semitones. Its low-quefrency
cepstral envelope suppresses the harmonic comb before comparing F1-F3 and the
full normalized spectral envelope. This is an aspirational quality gate, so it
is expected to fail when a backend audibly moves formants.

List devices and measure a cable or virtual round trip with:

```bash
./build-backend-lab/latency_probe --list-devices
./build-backend-lab/latency_probe \
  --input "BlackHole 2ch" --output "BlackHole 2ch" \
  --sample-rate 48000 --mode waveform --capture loopback.wav
```

Use `--mode waveform` for a dry, cable, or unity route. Use
`--mode envelope` through the harmonizer while holding A3/MIDI 57 at unison;
the coded voiced probe preserves its timing signature through pitch shifting.
The tool reports measured P50/P95/P99, jitter, normalized correlation, xruns,
and PortAudio's independently reported input/output latency. Keep the probe
level low and use a wired/virtual return path, not an open speaker and mic.

## License

Harmonizer is distributed under GPL-3.0-or-later. Rubber Band is
GPL-2.0-or-later, aubio is GPL-3.0-or-later, PortAudio uses its permissive
license, and the browser build of Signalsmith Stretch is MIT-licensed. The
optional Vocadito test dataset is CC BY 4.0 and downloaded separately; source
and attribution details are in `fixtures/README.md`. Corresponding Harmonizer
source is attached to every native release.

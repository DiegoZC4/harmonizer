# Vocal Harmonizer — Architecture & Notes

For the system-level emulation target derived from Benjamin Bloomberg's thesis,
see [bloomberg_thesis_notes.md](bloomberg_thesis_notes.md).

## Goal
Recreate Jacob Collier's harmonizer using free/open libraries.
Mono mic in → pitch detection + MIDI-controlled pitch shifting → stereo out.

## Libraries
| Library | Purpose |
|---------|---------|
| **PortAudio** | Low-latency audio I/O (wraps CoreAudio on macOS) |
| **Rubber Band** (LiveShifter) | Real-time formant-preserving pitch shift |
| **aubio** (yinfft) | Live pitch detection of input voice |
| **Web MIDI** | Browser keyboard input forwarded to the native server |
| **RtMidi** | MIDI keyboard input for the legacy terminal-only build |

## Build (macOS, Homebrew)
```bash
brew install portaudio rubberband aubio rtmidi
cd /path/to/harmonizer
make harmonizer_web
```

## Canonical Browser-Controlled Native App

`harmonizer_web` is the canonical live application. Despite the name, the
browser handles only controls, MIDI forwarding, and visualization. The native
C++ process owns separate PortAudio input/output streams, aubio pitch tracking, 16 Rubber Band
LiveShifters, formant preservation, envelopes, panning, capture, and mixing.

```bash
cd /path/to/harmonizer
make harmonizer_web
./harmonizer_web --port 8794
```

Then open:

```text
http://127.0.0.1:8794/
```

The active DSP lives in `harmonizer_rubberband_engine.hpp`. It uses
`RubberBandLiveShifter` with `OptionFormantPreserved | OptionWindowShort` and
locks each shifted voice to its held MIDI pitch. The raw detector estimate from
the previous 512-sample hop (11.6 ms) drives the inverse correction ratio, which
removes slow input vibrato without the phase error of a 50 ms-old estimate.
A paired, clamped flutter compensator suppresses the doubled-rate ripple created
inside Rubber Band by fast ratio changes without reacting to genuine note jumps.
The slower nine-frame median remains separate for the displayed contour and
voiced-state gate. Unvoiced audio has two selectable behaviors. **TC bypass**,
the default, detects high-frequency nonperiodic articulation and crossfades the
wet harmony bus to one latency-aligned unshifted signal. **Hold ratio** preserves
the previous experiment, keeping the last stable source F0 while energetic
unvoiced audio continues. The server reports the active
backend and measured DSP latency through `/api/state`; the browser diagnostics
show both values. A lock-free stereo ring joins the USB-mic input stream to the
selected output stream, avoiding fragile cross-device CoreAudio duplex units.
Its read side uses PI-controlled fractional resampling so independent hardware
clocks cannot slowly fill or drain the ring. The PortAudio callbacks only copy
samples; a wake-driven real-time worker owns pitch detection and shifting, and
the parallel quality path distributes its voices across four persistent worker
lanes. Browser diagnostics report those timings, queue depth, bridge occupancy,
clock correction, xruns, and PortAudio's input/output latency separately from
the DSP-path estimate.
The Input and Output menus switch one native PortAudio stream at a time without
restarting the server; audio never enters the browser. A candidate stream opens
and starts with a muted callback before it replaces the working side, so a
failed device does not destroy or overwrite the current route. The selected
devices are saved by name in `.harmonizer_input_device` and
`.harmonizer_output_device` only after a successful handoff. The browser and
`/api/state` report the failed PortAudio stage, native host code, elapsed time,
and whether the previous route was kept. DevHub writes the native log to
`/private/tmp/devhub-harmonizer-web.log`. `test output` sends a one-second
440 Hz tone through the same PortAudio stream while bypassing mic gain, pitch
detection, MIDI, Rubber Band, and Blend.

## Computer Keyboard

The browser can play harmony notes without a MIDI device. It maps physical key
positions, so the layout remains piano-shaped even if the operating-system
keyboard layout changes:

| Row | White notes | Black notes |
|---|---|---|
| F3-B4 | `Left Shift Z X C V B N M , . /` | `A S D G H K L ;` |
| C5-B6 | `Tab Q W E R T Y U I O P [ ] Backslash` | `1 2 4 5 6 8 9 - = Delete` |

Mapped keydown/keyup events use the same native MIDI-note endpoint as Web MIDI,
support chords, ignore key repeat, and release every held note when the window
loses focus.

## Rehearsal Mac App

Build the self-contained Apple Silicon app and transfer ZIP with:

```bash
make app-macos
```

This writes `dist/Harmonizer.app` and `dist/Harmonizer-macOS-arm64.zip`. The packaged
backend statically links PortAudio, aubio, Rubber Band, and libsamplerate, so the
destination Mac does not need Homebrew. On launch it stores preferences, logs,
and captures in `~/Library/Application Support/Harmonizer`, starts the native
server, and opens the browser GUI. Select the plugged-in microphone and playback
device in the GUI. The rehearsal build is ad-hoc signed and ARM64-only.

For a public one-click release, publish the source and ZIP together under a
GPL-compatible project license: [aubio is GPL-3.0-or-later](https://aubio.org/)
and [Rubber Band is GPL-2.0-or-later](https://breakfastquay.com/rubberband/license.html).
Build separate ARM64 and Intel artifacts (or a universal binary), then sign with
an Apple Developer ID and submit the ZIP/DMG for
[notarization](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution).
There is no Tauri project in this repository; Tauri can later wrap this same C++
backend as an [external sidecar](https://v2.tauri.app/develop/sidecar/), but it is
not required for the first public release.

The older targets remain as references:

```bash
make harmonizer        # terminal/RtMidi Rubber Band prototype
make harmonizer_world  # legacy SDL/additive-vocoder experiment
```

## Fast Launch / Reload Loop

When launched from DevHub, `scripts/run_harmonizer_web.sh` keeps a small
supervisor running. It uses macOS file-system events, not a polling loop. Edits
to `harmonizer_web.cpp`, `harmonizer_rubberband_engine.hpp`, `web/index.html`,
or the `Makefile` trigger a rebuild and server restart automatically. After C++
edits, wait for the DevHub log to show the relaunch, then refresh the page.

The web target has no SDL2 window. It serves `web/index.html`, streams live
pitch/MIDI state over `/events`, accepts control changes at `/api/control`, and
reports readiness at `/health`.

The browser roll draws smoothed/held pitch in cyan and raw pre-stability pitch
in amber. If the status says `holding`, a short voiced-release window is
bridging a dropped aubio frame so the wet harmony does not stutter. If the status
says `below gate`, PortAudio is receiving mic signal but the current RMS is
still below the Gate slider, so lower the gate or raise the input level before
debugging the pitch algorithm itself. The Blend slider is a constant-amplitude
wet/dry crossfade: wet is `blend`, dry is `1 - blend`, so the derived gains sum
to 1. At `100% wet`, hearing no dry voice is therefore expected. Monitor gain is
a separate post-analysis preamp applied equally to the dry and Rubber Band paths;
the `+18 dB` default matches the measured difference between the live mic and the
audible output-test reference without changing the wet/dry ratio. The side panel
shows the active PortAudio route, live input/output peak meters, and callback
xrun counts. If the input meter moves but the output meter does not, inspect the
blend, MIDI notes, Gate, and voiced state. If both meters move but nothing is
audible, choose the physical output jack being monitored and check its system
or hardware volume.

## Offline Pitch Detection Testing

The live mic loop is useful for feel, but it is a rough way to debug pitch
detection. Use the offline analyzer against known singing fixtures first:

```bash
cd /path/to/harmonizer
make pitch_analyzer
./pitch_analyzer \
  fixtures/vocadito/Audio/vocadito_1.wav \
  fixtures/vocadito/Annotations/F0/vocadito_1_f0.csv \
  --csv fixtures/pitch_reports/vocadito_1.csv
```

Run a small fixture suite:

```bash
make test-pitch
```

The analyzer defaults to the same RMS pitch gate now used by the live builds
(`0.01`). To sweep the main pitch-stability knobs:

```bash
GATE_RMS=0.015 make test-pitch
STABLE_WINDOW=0.4 make test-pitch
```

To turn the diagnostic suite into a pass/fail gate:

```bash
STRICT=1 make test-pitch
```

The fixture set is `Vocadito`: 40 short solo, monophonic singing excerpts with
frame-level F0 annotations. It lives in `fixtures/vocadito`; see
`fixtures/README.md` for source, license, and refresh commands.

To add your own singing sample, normalize it first:

```bash
ffmpeg -i input.ext -ac 1 -ar 44100 -sample_fmt s16 fixtures/custom/my_voice.wav
./pitch_analyzer fixtures/custom/my_voice.wav --csv fixtures/pitch_reports/my_voice.csv
```

## Diagnostic Capture (live takes)

When a live take sounds bad, hit **record** in the browser GUI (or
`curl 'http://127.0.0.1:8794/api/capture?action=start'`), sing with MIDI as
usual, then hit **stop**. Up to 120 s lands in `captures/cap_<stamp>/`:

- `mic.wav` — raw dry mic (mono float32; feed straight into `pitch_analyzer`)
- `output.wav` — the processed stereo output you actually heard
- `frames.csv` — per-11.6 ms engine pitch state: `time,rms,raw_hz,folded_hz,median_hz,smoothed_midi,correction_midi,stable,voiced`
- `midi.csv` — timestamped note on/off events
- `meta.json` — devices and control settings at stop time

Note the rough timestamp of anything that sounded wrong. The capture replays
deterministically: `mic.wav` + `midi.csv` reproduce the take, `output.wav`
shows what the engine did to it, and `frames.csv` says what the engine
believed at that moment. Sustain-pedal CC is not captured.

Replay a capture offline through the exact live engine (same per-sample code
path) after changing the DSP — no re-singing needed:

```bash
./harmonizer_web --render captures/cap_<stamp>   # writes render.wav next to mic.wav
```

It restores the mix/gate/stability/unvoiced settings from `meta.json`; edit that
file (e.g. `"mix":1.0` and `"unvoicedMode":"tc-bypass"`) to render variants.

`make test-vibrato-lock` generates a one-semitone, 2 Hz synthetic vibrato,
renders it through the exact native engine against a held C4, and fails if the
output pitch starts oscillating again.

`make test-unvoiced-modes` renders a voiced A3 followed by high-passed noise
through both unvoiced modes. It verifies that TC bypass passes a centered direct
consonant, Hold ratio keeps producing stereo shifted consonants, and both close
cleanly into silence.

`make test-parallel-control` verifies the onset-aware alpha-beta F0 tracker,
the R2-to-Live512 dynamic handoff, settled quality equivalence, vibrato lock,
and both unvoiced modes through the parallel backend. The tracker timestamps an
aubio result at the center of its 2048-sample window and projects pitch and
pitch slope to each 128-sample R2 block. Live512 deliberately keeps the previous
512-sample detector hop because its pitch control acts on internally buffered
audio; forward projection regressed the synthetic 2 Hz test.

`make test-parallel-matrix` renders 2, 4, and 6 Hz contours at two depths and
three intervals. It disabled the parallel backend's sign-blind flutter term and
selected a `-1184`-sample R2 control offset from a bracketed sweep. The tracker
matrix adds deterministic noise, outliers, glissandi, note steps, dropouts, and
multi-rate vibrato; its slope innovation is capped at 0.12 semitone so one bad
F0 frame cannot create a large projected spike. `make test-latency-control`
runs ten-minute virtual `+/-100 ppm` bridge-clock simulations.

## Features (v2 rewrite)
- [x] 16-voice polyphonic harmonizer (up from 4)
- [x] Stereo output with M/S-style panning (low=center, high=wide)
- [x] Per-voice attack/release envelope (MIDI-gated, 5ms/80ms)
- [x] MIDI sustain pedal (CC 64)
- [x] MIDI pitch bend (±2 semitones)
- [x] Flat MIDI pitch locking with stabilized input-pitch compensation
- [x] Onset-aware F0 prediction for the early R2 path
- [x] Dynamic R2-to-Live512 onset handoff
- [x] Selectable TC-style unvoiced bypass and held-F0 consonant shifting
- [x] Voice stealing (release → oldest priority)
- [x] Soft clipping (tanh) to prevent output overload
- [x] Decoupled pitch detection and RubberBand block sizes

## Patent-grounded IVL/TC reconstruction roadmap

This is the source-of-truth roadmap for the native C++ engine. It distinguishes
historical disclosure from our implementation choices; a patent in the IVL/TC
lineage is evidence of a documented method, not proof that VoiceLive Touch 2 or
a current Antares release runs identical firmware.

### Public evidence

- TC Electronic's official [VoiceLive Touch 2 service site](https://service-tcgroup.tcelectronic.com/voicelive_touch2_techserver.asp)
  publishes main/touch/USB schematics, top and bottom PCB layouts, service notes,
  and an old firmware binary. Its [parts list](https://service-tcgroup.tcelectronic.com/files/tech_service/voicelivetouch2/spareparts_voicelive_touch2.pdf)
  identifies the DSP56720, AT32UC3B0128 controller, flash, SDRAM, and AKM codecs.
- IVL [US 4,688,464](https://patents.google.com/patent/US4688464A/en)
  describes threshold-crossing pitch detection and corroboration.
- IVL [US 5,231,671](https://patentimages.storage.googleapis.com/cf/ff/88/18e5551f4b7a23/US5231671.pdf)
  describes F0 validation, harmony-note lookup, period-synchronous Hanning
  windows, octave-error checks, and direct bypass for sibilants.
- IVL [US 5,567,901](https://patents.google.com/patent/US5567901A/en)
  combines resampling with windowed pitch shifting to manipulate pitch and
  spectral envelope/timbre separately.
- Antares [US 5,973,252](https://patents.google.com/patent/US5973252A/en)
  describes efficient autocorrelation-derived period tracking, MIDI/scale
  targets, smoothed correction ratios, and unity resampling while pitch is
  unknown.

### Block mapping

| Processing block | Historical disclosure | Current native implementation | Next evidence-driven step |
|---|---|---|---|
| Audio I/O | ADC/DAC, DSP, circular memory; Touch 2 board documentation exposes the physical signal path | Separate PortAudio streams, PI-controlled fractional bridge resampling, and a wake-driven real-time DSP worker | Measure physical cable loopback; PortAudio-reported device latency is now separate from DSP latency |
| Level and periodicity | IVL tests level, threshold crossings, and whether the signal is periodic | RMS gate plus aubio confidence/history | Save periodicity confidence per frame instead of reducing it immediately to a Boolean |
| Sibilance | US 5,231,671 identifies rapid, large high-frequency variations and bypasses shifting | A high-band energy ratio plus sign-crossing count normalized to the patent's 8 ms interval; score is exposed in `/api/state` | Calibrate against labeled `s`, `sh`, `ch`, `f`, `t`, breaths, soprano vowels, and stage noise |
| F0 estimation | IVL threshold timing; Antares efficient autocorrelation over candidate periods | 2048-sample aubio YINFFT plus a strict low-register YIN lane down to A1 | Compare period candidates and octave errors against the fixture annotations, not by ear alone |
| Note validation | Previous estimate, onset counters, octave-error subroutine, acceptable ranges | Nine-frame display/gate contour plus a robust alpha-beta tracker; large jumps reset slope, velocity innovations are capped, and confidence is smoothed per sample | Calibrate confidence and jump thresholds on labeled note transitions |
| Harmony targets | Reference note selects harmony notes; MIDI can provide desired pitch | Browser/native MIDI notes are exact output targets; input vibrato is removed from target pitch | Preserve this behavior; it matches the performance requirement |
| Pitch shifting | Lent-style period-synchronous extraction and Hanning-window replication | Sixteen formant-preserved LiveShifters plus a selectable parallel backend: empirically aligned R2 onset yields over 30 ms to one-hop-aligned Live512 quality | Build any patent/PSOLA reconstruction as another isolated backend, never by replacing Live512 |
| Timbre/formants | US 5,567,901 combines resampling and windowed shifting | Rubber Band's formant-preserved mode | Measure spectral-envelope displacement by interval and vowel before choosing another shifter |
| Unvoiced articulation | US 5,231,671 sends sampled input directly when sibilance/no note is detected; Antares uses unity resampling when pitch is unknown | `TC bypass` linearly crossfades over 6 ms to one delay-aligned direct articulation bus while shifters continue silently | Tune detector thresholds from captures; do not invent F0 for noise |
| Experimental alternative | Not the documented historical behavior | `Hold ratio` latches only a stable source F0, keeps the ratio through energetic unvoiced audio, and forgets it after 0.8 s of silence | Retain for blind A/B listening because it may suit some material despite weaker historical support |
| Voice/mix stage | Patent combines input and harmony signals; production details remain unknown | MIDI envelopes, per-voice pan, `1/sqrt(N)` wet normalization, constant-amplitude Blend, `tanh` limiter | Compare loudness and consonant localization using captured phrases |

### Implementation order

1. **Done:** deterministic capture/replay, flat MIDI locking, Live512 baseline,
   selectable unvoiced modes, detector telemetry, robust R2 control, dynamic
   quality handoff, bridge clock control, real-time worker isolation, and
   synthetic A/B regression.
2. **Calibrate articulation:** collect short labeled consonant-vowel captures and
   quantify misses/false positives, especially high soprano vowels and noisy rooms.
3. **Measure formants:** compare vowel spectral envelopes before/after each
   interval and backend; use those measurements to decide whether Rubber Band is
   the remaining bottleneck.
4. **Prototype historical shifters in isolation:** implement a period-synchronous
   Hanning/TD-PSOLA backend and benchmark quality/latency beside Live512.
5. **Tune the mix as a performance instrument:** articulation width, voice pan,
   per-voice EQ, chorus, and reverb come after pitch and unvoiced transitions are
   objectively stable.

## Quality vs. Original (honest assessment)

The original used **Antares Harmony Engine** (commercial) in Reaper with custom scripting.

| Scenario | Our quality vs Antares |
|----------|----------------------|
| Small intervals (3rds, 5ths) | ~85% — RubberBand formant preservation is solid |
| Octave shifts | ~65% — artifacts become noticeable |
| Bass (2+ octaves down) | ~50% — Antares excels at fundamental reinforcement |
| Fast passages | ~70% — latency + pitch tracking limits agility |
| Overall "choir" feel | ~75% — very usable, clearly not Antares |

### Where we lose
- **Low end**: Antares generates strong fundamentals even for huge downward shifts. RubberBand thins out.
- **Latency**: Live512 is about 68.2 ms and the parallel R2 attack about 23.2 ms in the DSP path. On this Mac, PortAudio separately reports about 10.2 ms input and 5.3 ms output; a physical cable loopback is still required for true microphone-to-headphone latency.
- **Artifact quality**: Antares' proprietary algorithm handles transients and formants more gracefully.

### Where we're competitive
- **High harmony voices**: RubberBand with formant preservation sounds very natural for upward shifts.
- **Responsiveness**: Custom envelope + voice allocation gives tight MIDI feel.
- **Flexibility**: 16 voices, sustain, pitch bend, stereo panning — all customizable.

## Future improvements (if needed)
- **WORLD vocoder** instead of RubberBand — better voice-specific quality, used in singing synthesis
- **TD-PSOLA** for lower latency on small shifts
- **Sub-harmonic synthesis** to boost bass fundamentals (like Antares does)
- **ML pitch detection** (CREPE) for more stable tracking
- **JUCE port** for GUI, plugin format (VST/AU), better buffer management
- **Per-voice EQ** (Bloomberg had custom EQ per voice)
- **Freeze mode** (hold current sound indefinitely)

## Architecture
```
USB Mic → PortAudio callback → lock-free input ring
                ↓
        Wake-driven real-time DSP worker
                ↓
        ┌── Pitch Detector (aubio) ── stable display/gate contour
        │          ├── timestamped pitch+slope prediction → R2 onset
        │          └── previous-hop alignment → Live512 quality
        ├── Sibilance detector ────── unvoiced mode
        │
        ├── Voice 0: aligned R2 onset → 30 ms handoff → Live512 → envelope → pan
        ├── Voice 1: ...
        ├── ...
        └── Voice 15: ...
                ↓
        TC mode: shifted bus ↔ aligned direct articulation
                ↓
        Constant-amplitude dry/wet blend → tanh soft clip
                ↓
        PI-controlled fractional output bridge
                ↓
        PortAudio (stereo out) → speakers/headphones

MIDI Keyboard → Browser Web MIDI → `/api/midi` → native voice allocation
                                           → CC 64 → sustain
                                           → pitch bend → global detune
```

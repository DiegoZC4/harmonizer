#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build-backend-lab"
rubberband_source="$($root/scripts/fetch_rubberband.sh)"
signalsmith_sources="$($root/scripts/fetch_signalsmith.sh)"
signalsmith_stretch="$(printf '%s\n' "$signalsmith_sources" | sed -n '1p')"
signalsmith_linear="$(printf '%s\n' "$signalsmith_sources" | sed -n '2p')"

cmake -S "$root" -B "$build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DHARMONIZER_BUILD_BACKEND_LAB=ON \
  -DFETCHCONTENT_SOURCE_DIR_RUBBERBAND="$rubberband_source" \
  -DFETCHCONTENT_SOURCE_DIR_SIGNALSMITH_STRETCH="$signalsmith_stretch" \
  -DFETCHCONTENT_SOURCE_DIR_SIGNALSMITH_LINEAR="$signalsmith_linear"

cmake --build "$build" --config Release --parallel --target \
  harmonizer_web \
  harmonizer_web_parallel \
  harmonizer_web_rubberband_live_reference \
  harmonizer_web_rubberband_live128 \
  harmonizer_web_rubberband_r2 \
  harmonizer_web_signalsmith \
  harmonizer_pitch_analyzer \
  harmonizer_spectrum_analyzer \
  harmonizer_polyphonic_analyzer \
  harmonizer_formant_analyzer \
  harmonizer_latency_probe \
  output_bridge_controller_test \
  parallel_pitch_tracker_test \
  collier_effects_test

printf '\nBackend lab built in %s\n' "$build"

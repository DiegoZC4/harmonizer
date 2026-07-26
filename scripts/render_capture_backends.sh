#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build-backend-lab"
capture="${1:-}"

if [[ -z "$capture" || ! -f "$capture/mic.wav" || ! -f "$capture/midi.csv" || ! -f "$capture/meta.json" ]]; then
  printf 'usage: %s <capture-directory>\n' "$0" >&2
  exit 2
fi
if [[ ! -x "$build/harmonizer_web_signalsmith" ]]; then
  "$root/scripts/build_backend_lab.sh"
fi

capture="$(cd "$capture" && pwd)"
stamp="$(date +%Y%m%d_%H%M%S)"
name="$(basename "$capture")"
results="$root/backend_lab/capture_results/${name}_$stamp"
mkdir -p "$results"

backends=(
  "parallel:harmonizer_web_parallel"
  "live_reference:harmonizer_web_rubberband_live_reference"
  "live128:harmonizer_web_rubberband_live128"
  "rubberband_r2:harmonizer_web_rubberband_r2"
  "signalsmith:harmonizer_web_signalsmith"
)

for spec in "${backends[@]}"; do
  backend="${spec%%:*}"
  binary="$build/${spec#*:}"
  case_dir="$results/$backend"
  mkdir -p "$case_dir"
  cp "$capture/mic.wav" "$case_dir/mic.wav"
  cp "$capture/midi.csv" "$case_dir/midi.csv"
  jq '.mix = 1 | .wet = 1 | .dry = 0' "$capture/meta.json" > "$case_dir/meta.json"
  "$binary" --render "$case_dir" > "$case_dir/render.log" 2>&1
done

printf 'Capture renders written to %s\n' "$results"

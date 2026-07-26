#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-backend-lab"
RENDERER="${HARMONIZER_TEST_BINARY:-$BUILD/harmonizer_web}"
ANALYZER="$BUILD/formant_analyzer"
RESULTS="$ROOT/backend_lab/results/formant_preservation"
MAX_DRIFT_PERCENT="${MAX_DRIFT_PERCENT:-8}"
MAX_ENVELOPE_DB="${MAX_ENVELOPE_DB:-3}"

if [[ ! -x "$RENDERER" || ! -x "$ANALYZER" ]]; then
  "$ROOT/scripts/build_backend_lab.sh"
fi

mkdir -p "$RESULTS"

generate_vowel() {
  local name="$1"
  local f1="$2"
  local f2="$3"
  local f3="$4"
  local output="$RESULTS/$name-source.wav"

  # A1 saw excitation provides dense, deterministic harmonics. Three narrow EQ
  # resonances create a known source-filter envelope without adding a test-only
  # synthesis dependency to the production backend.
  ffmpeg -hide_banner -loglevel error \
    -f lavfi -i 'anullsrc=r=44100:cl=mono:d=0.25' \
    -f lavfi \
    -i 'aevalsrc=0.012*(2*(t*55-floor(t*55+0.5))):s=44100:d=3.5' \
    -f lavfi -i 'anullsrc=r=44100:cl=mono:d=0.25' \
    -filter_complex \
      "[1:a]highpass=f=70,equalizer=f=$f1:t=q:w=5:g=18,equalizer=f=$f2:t=q:w=7:g=16,equalizer=f=$f3:t=q:w=9:g=14,lowpass=f=7000[v];[0:a][v][2:a]concat=n=3:v=0:a=1[out]" \
    -map '[out]' -ar 44100 -ac 1 -c:a pcm_f32le "$output" -y
}

generate_vowel a 730 1090 2440
generate_vowel i 270 2290 3010
generate_vowel u 300 870 2240

printf 'vowel,transpose_semitones,target_midi,envelope_distance_db,max_formant_drift_percent,status\n' \
  > "$RESULTS/summary.csv"

failures=0

render_case() {
  local vowel="$1"
  local formants="$2"
  local transpose="$3"
  local target="$4"
  local label
  if [[ "$transpose" == -* ]]; then
    label="down${transpose#-}"
  else
    label="up$transpose"
  fi
  local case_dir="$RESULTS/${vowel}_${label}"
  mkdir -p "$case_dir"
  cp "$RESULTS/$vowel-source.wav" "$case_dir/mic.wav"
  printf 'time,event,note\n0.000,on,%s\n3.750,off,%s\n' "$target" "$target" \
    > "$case_dir/midi.csv"
  printf '{"mix":1.0,"gainDb":0.0,"gate":0.001,"stableWindow":1.0,"immediacy":0.0,"unvoicedMode":"hold-ratio","chorusMix":0.0,"reverbMix":0.0}\n' \
    > "$case_dir/meta.json"

  "$RENDERER" --render "$case_dir" > "$case_dir/render.log" 2>&1

  local status
  if "$ANALYZER" \
      --reference "$case_dir/mic.wav" \
      --expected-formants "$formants" \
      --reference-start 1.1 \
      --candidate-start 1.1 \
      --duration 1.6 \
      --max-drift-percent "$MAX_DRIFT_PERCENT" \
      --max-envelope-db "$MAX_ENVELOPE_DB" \
      --csv "$case_dir/formants.csv" \
      --envelope-csv "$case_dir/envelope.csv" \
      --json "$case_dir/render.wav" \
      | tee "$case_dir/analysis.json"; then
    status=PASS
  else
    status=FAIL
    failures=$((failures + 1))
  fi

  if [[ -s "$case_dir/analysis.json" ]]; then
    awk -v vowel="$vowel" -v transpose="$transpose" -v target="$target" \
        -v status="$status" '
      {
        envelope = $0
        sub(/.*"envelopeDistanceDb":/, "", envelope)
        sub(/,.*/, "", envelope)
        rest = $0
        max_drift = 0
        while (match(rest, /"driftPercent":-?[0-9.]+/)) {
          value = substr(rest, RSTART, RLENGTH)
          sub(/.*:/, "", value)
          value += 0
          if (value < 0) value = -value
          if ((value + 0) > (max_drift + 0)) max_drift = value
          rest = substr(rest, RSTART + RLENGTH)
        }
        printf "%s,%s,%s,%s,%.3f,%s\n",
               vowel, transpose, target, envelope, max_drift, status
      }
    ' "$case_dir/analysis.json" >> "$RESULTS/summary.csv"
  else
    printf '%s,%s,%s,NaN,NaN,%s\n' \
      "$vowel" "$transpose" "$target" "$status" >> "$RESULTS/summary.csv"
  fi
}

for spec in \
  'a 730,1090,2440' \
  'i 270,2290,3010' \
  'u 300,870,2240'
do
  read -r vowel formants <<< "$spec"
  render_case "$vowel" "$formants" -24 9
  render_case "$vowel" "$formants" -12 21
  render_case "$vowel" "$formants" 0 33
  render_case "$vowel" "$formants" 12 45
  render_case "$vowel" "$formants" 24 57
done

if (( failures > 0 )); then
  printf 'Formant preservation: FAIL (%d/%d cases)\nResults: %s\n' \
    "$failures" 15 "$RESULTS" >&2
  exit 2
fi

printf 'Formant preservation: PASS\nResults: %s\n' "$RESULTS"

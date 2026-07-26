#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-backend-lab"
RENDERER="${HARMONIZER_TEST_BINARY:-$BUILD/harmonizer_web}"
ANALYZER="$BUILD/polyphonic_analyzer"
RESULTS="$ROOT/backend_lab/results/polyphonic_chords"
MAX_ERROR_CENTS="${MAX_ERROR_CENTS:-10}"
MAX_MASKED_ERROR_CENTS="${MAX_MASKED_ERROR_CENTS:-20}"
MIN_SNR_DB="${MIN_SNR_DB:-4}"
MAX_SPURIOUS="${MAX_SPURIOUS:-0}"

if [[ ! -x "$RENDERER" || ! -x "$ANALYZER" ]]; then
  "$ROOT/scripts/build_backend_lab.sh"
fi

mkdir -p "$RESULTS"

# A low-level, harmonic-rich C4 source keeps every requested output fundamental
# measurable without driving a 16-voice chord into the output ceiling.
ffmpeg -hide_banner -loglevel error \
  -f lavfi -i 'anullsrc=r=44100:cl=mono:d=0.25' \
  -f lavfi \
  -i 'aevalsrc=0.018*sin(2*PI*261.625565*t)+0.009*sin(4*PI*261.625565*t)+0.006*sin(6*PI*261.625565*t)+0.004*sin(8*PI*261.625565*t)+0.003*sin(10*PI*261.625565*t):s=44100:d=3.5' \
  -f lavfi -i 'anullsrc=r=44100:cl=mono:d=0.25' \
  -filter_complex '[0:a][1:a][2:a]concat=n=3:v=0:a=1[out]' \
  -map '[out]' -ar 44100 -ac 1 -c:a pcm_f32le "$RESULTS/mic.wav" -y

printf 'case,requested,present,spurious,status\n' > "$RESULTS/summary.csv"
failures=0

render_case() {
  local name="$1"
  local notes="$2"
  local case_dir="$RESULTS/$name"
  mkdir -p "$case_dir"
  cp "$RESULTS/mic.wav" "$case_dir/mic.wav"
  printf 'time,event,note\n' > "$case_dir/midi.csv"
  IFS=',' read -r -a note_array <<< "$notes"
  local note
  for note in "${note_array[@]}"; do
    printf '0.000,on,%s\n' "$note" >> "$case_dir/midi.csv"
  done
  for note in "${note_array[@]}"; do
    printf '3.750,off,%s\n' "$note" >> "$case_dir/midi.csv"
  done
  printf '{"mix":1.0,"gainDb":0.0,"gate":0.001,"stableWindow":1.0,"immediacy":0.0,"unvoicedMode":"hold-ratio","chorusMix":0.0,"reverbMix":0.0}\n' \
    > "$case_dir/meta.json"

  "$RENDERER" --render "$case_dir" > "$case_dir/render.log" 2>&1
  if "$ANALYZER" \
      --midi "$notes" \
      --start 1.1 \
      --duration 2.0 \
      --max-error-cents "$MAX_ERROR_CENTS" \
      --max-masked-error-cents "$MAX_MASKED_ERROR_CENTS" \
      --min-snr-db "$MIN_SNR_DB" \
      --max-spurious "$MAX_SPURIOUS" \
      --csv "$case_dir/voices.csv" \
      --json "$case_dir/render.wav" \
      | tee "$case_dir/analysis.json"; then
    :
  else
    failures=$((failures + 1))
  fi

  awk -v name="$name" '
    {
      requested = $0
      sub(/.*"expected":/, "", requested)
      sub(/,.*/, "", requested)
      present = $0
      sub(/.*"present":/, "", present)
      sub(/,.*/, "", present)
      spurious = $0
      sub(/.*"spurious":/, "", spurious)
      sub(/,.*/, "", spurious)
      status = $0
      sub(/.*"status":"/, "", status)
      sub(/".*/, "", status)
      printf "%s,%s,%s,%s,%s\n", name, requested, present, spurious, status
    }
  ' "$case_dir/analysis.json" >> "$RESULTS/summary.csv"
}

render_case single '60'
render_case four_voice '48,55,60,64'
render_case eight_voice '43,47,50,54,57,61,64,68'
render_case upper_mid_eight '68,69,71,72,73,74,75,76'
render_case upper_high_eight '78,79,81,82,84,85,87,89'
render_case sixteen_voice '36,40,43,47,50,54,57,61,64,68,71,75,78,82,85,89'

if (( failures > 0 )); then
  printf 'Polyphonic voice accounting: FAIL (%d/%d cases)\nResults: %s\n' \
    "$failures" 6 "$RESULTS" >&2
  exit 2
fi

printf 'Polyphonic voice accounting: PASS\nResults: %s\n' "$RESULTS"

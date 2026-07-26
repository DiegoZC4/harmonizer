#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-backend-lab"
RENDERER="$BUILD/harmonizer_web"
ANALYZER="$BUILD/spectrum_analyzer"
SOURCE="$ROOT/fixtures/vocadito/Audio/vocadito_1.wav"
RESULTS="$ROOT/backend_lab/results/collier_features"

if [[ ! -x "$RENDERER" || ! -x "$ANALYZER" ]]; then
  "$ROOT/scripts/build_backend_lab.sh"
fi

mkdir -p "$RESULTS"

# A real a cappella phrase feeds the exact native harmonizer for 1.7 seconds.
# The remaining 2.3 seconds are digital silence, so any late output must come
# from the latched freeze rather than a live input or detector hold.
ffmpeg -hide_banner -loglevel error \
  -ss 1.0 -t 1.7 -i "$SOURCE" \
  -f lavfi -t 2.3 -i 'anullsrc=r=44100:cl=mono' \
  -filter_complex \
    '[0:a]aresample=44100,aformat=sample_fmts=flt:channel_layouts=mono[voice];[voice][1:a]concat=n=2:v=0:a=1[out]' \
  -map '[out]' -ar 44100 -ac 1 -c:a pcm_f32le "$RESULTS/mic.wav" -y

printf 'time,event,note\n0.000,on,60\n3.800,off,60\n' > "$RESULTS/midi.csv"
printf 'time,event,slot\n1.400,on,0\n3.700,off,0\n' > "$RESULTS/effects.csv"

run_case() {
  local name="$1"
  local transpose="$2"
  local expected_midi="$3"
  local tone="${4:-1.0}"
  local case_dir="$RESULTS/$name"
  mkdir -p "$case_dir"
  cp "$RESULTS/mic.wav" "$case_dir/mic.wav"
  cp "$RESULTS/midi.csv" "$case_dir/midi.csv"
  cp "$RESULTS/effects.csv" "$case_dir/effects.csv"
  printf '{"mix":1.0,"gainDb":12.0,"gate":0.004,"stableWindow":1.0,"unvoicedMode":"hold-ratio","glideAmount":0.0,"chorusMix":0.0,"reverbMix":0.0,"freezeTone":%s,"freeze1Level":1.0,"freeze1Transpose":%s}\n' \
    "$tone" "$transpose" > "$case_dir/meta.json"

  "$RENDERER" --render "$case_dir" >/dev/null
  "$ANALYZER" --start 2.2 --duration 1.2 \
    --expected-midi "$expected_midi" --search-semitones 1.25 \
    --max-cents 55 --min-rms 0.0001 --json "$case_dir/render.wav" \
    | tee "$case_dir/spectrum.json"
}

run_case unison 0 60
run_case major-third-up 4 64
run_case fourth-up 5 65
run_case fifth-up 7 67
run_case fourth-down -5 55
run_case fifth-down -7 53
run_case octave-up 12 72
run_case closed-tone 0 60 0.0

peak_midi() {
  sed -E 's/.*"peakMidi":(-?[0-9.]+).*/\1/' "$1/spectrum.json"
}

centroid_hz() {
  sed -E 's/.*"centroidHz":(-?[0-9.]+).*/\1/' "$1/spectrum.json"
}

BASE_MIDI="$(peak_midi "$RESULTS/unison")"
printf 'case,requested_semitones,measured_semitones,error_cents\n' > "$RESULTS/interval_accuracy.csv"
for spec in \
  'major-third-up 4' \
  'fourth-up 5' \
  'fifth-up 7' \
  'fourth-down -5' \
  'fifth-down -7' \
  'octave-up 12'
do
  read -r name requested <<< "$spec"
  measured="$(peak_midi "$RESULTS/$name")"
  awk -v name="$name" -v base="$BASE_MIDI" -v measured="$measured" \
      -v requested="$requested" '
    BEGIN {
      interval = measured - base
      cents = (interval - requested) * 100.0
      printf "%s,%g,%.3f,%.1f\n", name, requested, interval, cents
      if (cents < -20.0 || cents > 20.0) exit 2
    }
  ' | tee -a "$RESULTS/interval_accuracy.csv"
done

OPEN_CENTROID="$(centroid_hz "$RESULTS/unison")"
CLOSED_CENTROID="$(centroid_hz "$RESULTS/closed-tone")"
awk -v open="$OPEN_CENTROID" -v closed="$CLOSED_CENTROID" '
  BEGIN {
    printf "freeze tone: open centroid %.1f Hz, closed centroid %.1f Hz\n", open, closed
    if (closed >= open * 0.92) exit 2
  }
'

ffmpeg -hide_banner -loglevel error -ss 1.4 -t 2.0 \
  -i "$RESULTS/unison/render.wav" \
  -lavfi 'showspectrumpic=s=1600x600:legend=1:color=viridis:scale=log:fscale=log:start=60:stop=4000:drange=85' \
  "$RESULTS/unison-spectrum.png" -y
ffmpeg -hide_banner -loglevel error -ss 1.4 -t 2.0 \
  -i "$RESULTS/fifth-up/render.wav" \
  -lavfi 'showspectrumpic=s=1600x600:legend=1:color=viridis:scale=log:fscale=log:start=60:stop=4000:drange=85' \
  "$RESULTS/fifth-up-spectrum.png" -y

printf 'Collier feature spectral matrix: PASS\nResults: %s\n' "$RESULTS"

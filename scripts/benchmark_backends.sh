#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build-backend-lab"
stamp="$(date +%Y%m%d_%H%M%S)"
results="$root/backend_lab/results/$stamp"
source_dir="$results/source"
mkdir -p "$source_dir"

if [[ ! -x "$build/harmonizer_web_signalsmith" || ! -x "$build/pitch_analyzer" ]]; then
  "$root/scripts/build_backend_lab.sh"
fi

# C4 with one semitone peak-to-peak of 2 Hz vibrato and three harmonics.
ffmpeg -hide_banner -loglevel error -f lavfi \
  -i "aevalsrc=0.16*sin(2*PI*(261.625565*t+(261.625565*0.02930224/(4*PI))*(1-cos(4*PI*t))))+0.08*sin(4*PI*(261.625565*t+(261.625565*0.02930224/(4*PI))*(1-cos(4*PI*t))))+0.04*sin(6*PI*(261.625565*t+(261.625565*0.02930224/(4*PI))*(1-cos(4*PI*t)))):s=44100:d=6" \
  -ar 44100 -ac 1 -c:a pcm_f32le "$source_dir/mic.wav" -y

printf 'backend,target_midi,reported_latency_ms,mean_error_semitones,pitch_p2p_semitones,pitch_sd_semitones,vibrato_2hz_amplitude_semitones\n' > "$results/summary.csv"

backends=(
  "current:harmonizer_web"
  "parallel:harmonizer_web_parallel"
  "live_reference:harmonizer_web_rubberband_live_reference"
  "live128:harmonizer_web_rubberband_live128"
  "rubberband_r2:harmonizer_web_rubberband_r2"
  "signalsmith:harmonizer_web_signalsmith"
)
targets=(48 55 60 67 72)

for spec in "${backends[@]}"; do
  name="${spec%%:*}"
  binary="$build/${spec#*:}"
  for target in "${targets[@]}"; do
    case_dir="$results/${name}_midi_${target}"
    mkdir -p "$case_dir"
    cp "$source_dir/mic.wav" "$case_dir/mic.wav"
    printf 'time,event,note\n0.0,on,%s\n6.0,off,%s\n' "$target" "$target" > "$case_dir/midi.csv"
    printf '{"mix":1.0,"gainDb":0.0,"gate":0.001,"stableWindow":1.0,"immediacy":0.25}\n' > "$case_dir/meta.json"

    "$binary" --render "$case_dir" > "$case_dir/render.log" 2>&1
    "$build/pitch_analyzer" \
      --expected-midi "$target" \
      --csv "$case_dir/pitch.csv" \
      --gate-rms 0.001 \
      "$case_dir/render.wav" >/dev/null

    latency="$(awk '/DSP path/ {for (i=1; i<=NF; i++) if ($i == "path") {print $(i+1); exit}}' "$case_dir/render.log")"
    awk -F, -v backend="$name" -v target="$target" -v latency="$latency" '
      NR > 1 && $1 >= 1 && $1 <= 5 && $4 > 0 {
        midi = 69 + 12 * log($4 / 440) / log(2)
        error = midi - target
        if (!n || midi < lo) lo = midi
        if (!n || midi > hi) hi = midi
        sum += error
        sum_sq += error * error
        sin_2hz += error * sin(4 * 3.141592653589793 * $1)
        cos_2hz += error * cos(4 * 3.141592653589793 * $1)
        n++
      }
      END {
        mean = sum / n
        sd = sqrt(sum_sq / n - mean * mean)
        sin_amp = 2 * sin_2hz / n
        cos_amp = 2 * cos_2hz / n
        amp = sqrt(sin_amp * sin_amp + cos_amp * cos_amp)
        printf "%s,%d,%.4f,%+.5f,%.5f,%.5f,%.5f\n", backend, target, latency, mean, hi-lo, sd, amp
      }
    ' "$case_dir/pitch.csv" >> "$results/summary.csv"
  done
done

printf 'Backend benchmark: %s\n\n' "$results"
if command -v column >/dev/null 2>&1; then
  column -s, -t "$results/summary.csv"
else
  cat "$results/summary.csv"
fi

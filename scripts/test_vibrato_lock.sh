#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/harmonizer-vibrato.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

printf 'time,event,note\n0.0,on,60\n6.0,off,60\n' > "$TMP_DIR/midi.csv"
printf '{"mix":1.0,"gainDb":0.0,"gate":0.001,"stableWindow":1.0}\n' > "$TMP_DIR/meta.json"

# C4 with a one-semitone peak-to-peak, 2 Hz frequency modulation and three
# harmonics. The phase integral keeps the generated instantaneous F0 correct.
ffmpeg -hide_banner -loglevel error -f lavfi \
  -i "aevalsrc=0.16*sin(2*PI*(261.625565*t+(261.625565*0.02930224/(4*PI))*(1-cos(4*PI*t))))+0.08*sin(4*PI*(261.625565*t+(261.625565*0.02930224/(4*PI))*(1-cos(4*PI*t))))+0.04*sin(6*PI*(261.625565*t+(261.625565*0.02930224/(4*PI))*(1-cos(4*PI*t)))):s=44100:d=6" \
  -ar 44100 -ac 1 -c:a pcm_f32le "$TMP_DIR/mic.wav" -y

"$ROOT/harmonizer_web" --render "$TMP_DIR"
"$ROOT/pitch_analyzer" --csv "$TMP_DIR/input.csv" --gate-rms 0.001 "$TMP_DIR/mic.wav" >/dev/null
"$ROOT/pitch_analyzer" --csv "$TMP_DIR/output.csv" --gate-rms 0.001 "$TMP_DIR/render.wav" >/dev/null

INPUT_P2P="$(awk -F, '
  NR > 1 && $1 >= 1 && $1 <= 5 && $4 > 0 {
    midi = 69 + 12 * log($4 / 440) / log(2)
    if (!n || midi < lo) lo = midi
    if (!n || midi > hi) hi = midi
    n++
  }
  END { printf "%.6f", hi - lo }
' "$TMP_DIR/input.csv")"

awk -F, -v input_p2p="$INPUT_P2P" '
  NR > 1 && $1 >= 1 && $1 <= 5 && $4 > 0 {
    midi = 69 + 12 * log($4 / 440) / log(2)
    error = midi - 60
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
    p2p = hi - lo
    sin_amp = 2 * sin_2hz / n
    cos_amp = 2 * cos_2hz / n
    vibrato_amp = sqrt(sin_amp * sin_amp + cos_amp * cos_amp)
    pass = input_p2p >= 0.90 && p2p <= 0.30 && sd <= 0.08 &&
           vibrato_amp <= 0.05 && mean >= -0.10 && mean <= 0.10
    printf "vibrato lock: input %.3f st p-p -> output %.3f st p-p, sd %.3f st, 2 Hz amplitude %.3f st, mean error %+.3f st: %s\n",
           input_p2p, p2p, sd, vibrato_amp, mean, pass ? "PASS" : "FAIL"
    exit(pass ? 0 : 1)
  }
' "$TMP_DIR/output.csv"

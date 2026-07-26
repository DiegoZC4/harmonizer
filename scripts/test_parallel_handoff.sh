#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-backend-lab"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/harmonizer-parallel.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

if [[ ! -x "$BUILD/harmonizer_web_parallel" || ! -x "$BUILD/pitch_analyzer" ]]; then
  "$ROOT/scripts/build_backend_lab.sh"
fi

printf 'time,event,note\n0.0,on,60\n2.6,off,60\n' > "$TMP_DIR/midi.csv"

# A short silence makes arrival time measurable. The voiced section carries
# one semitone peak-to-peak of 2 Hz vibrato so the same render also guards the
# path-specific F0 projection.
ffmpeg -hide_banner -loglevel error \
  -f lavfi -i 'anullsrc=r=44100:cl=mono:d=0.25' \
  -f lavfi \
  -i "aevalsrc=0.16*sin(2*PI*(261.625565*t+(261.625565*0.02930224/(4*PI))*(1-cos(4*PI*t))))+0.08*sin(4*PI*(261.625565*t+(261.625565*0.02930224/(4*PI))*(1-cos(4*PI*t))))+0.04*sin(6*PI*(261.625565*t+(261.625565*0.02930224/(4*PI))*(1-cos(4*PI*t)))):s=44100:d=2.1" \
  -f lavfi -i 'anullsrc=r=44100:cl=mono:d=0.35' \
  -filter_complex '[0:a][1:a][2:a]concat=n=3:v=0:a=1[out]' \
  -map '[out]' -ar 44100 -ac 1 -c:a pcm_f32le "$TMP_DIR/mic.wav" -y

render_case() {
  local name="$1"
  local immediacy="$2"
  local dir="$TMP_DIR/$name"
  mkdir -p "$dir"
  cp "$TMP_DIR/mic.wav" "$dir/mic.wav"
  cp "$TMP_DIR/midi.csv" "$dir/midi.csv"
  printf '{"mix":1.0,"gainDb":0.0,"gate":0.001,"stableWindow":1.0,"immediacy":%s}\n' \
    "$immediacy" > "$dir/meta.json"
  "$BUILD/harmonizer_web_parallel" --render "$dir" > "$dir/render.log" 2>&1
}

render_case quality 0.0
render_case handoff 1.0

first_sound() {
  ffmpeg -hide_banner -nostats -i "$1" \
    -af 'silencedetect=noise=-55dB:d=0.005' -f null - 2>&1 |
    awk '/silence_end:/ {
      for (i = 1; i <= NF; i++) if ($i == "silence_end:") { print $(i + 1); exit }
    }'
}

QUALITY_ONSET="$(first_sound "$TMP_DIR/quality/render.wav")"
HANDOFF_ONSET="$(first_sound "$TMP_DIR/handoff/render.wav")"

QUALITY_STEADY_MD5="$(ffmpeg -hide_banner -loglevel error -ss 1.20 -t 0.50 \
  -i "$TMP_DIR/quality/render.wav" -map 0:a -f md5 -)"
HANDOFF_STEADY_MD5="$(ffmpeg -hide_banner -loglevel error -ss 1.20 -t 0.50 \
  -i "$TMP_DIR/handoff/render.wav" -map 0:a -f md5 -)"

"$BUILD/pitch_analyzer" --csv "$TMP_DIR/handoff-pitch.csv" --gate-rms 0.001 \
  "$TMP_DIR/handoff/render.wav" >/dev/null

awk -F, -v quality_onset="$QUALITY_ONSET" -v handoff_onset="$HANDOFF_ONSET" \
  -v steady_equal="$([[ "$QUALITY_STEADY_MD5" == "$HANDOFF_STEADY_MD5" ]] && echo 1 || echo 0)" '
  NR > 1 && $1 >= 1.0 && $1 <= 2.0 && $4 > 0 {
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
    sin_amp = 2 * sin_2hz / n
    cos_amp = 2 * cos_2hz / n
    vibrato_amp = sqrt(sin_amp * sin_amp + cos_amp * cos_amp)
    onset_lead = quality_onset - handoff_onset
    pass = n > 0 && onset_lead >= 0.025 && steady_equal == 1 &&
           hi - lo <= 0.35 && sd <= 0.10 && vibrato_amp <= 0.06 &&
           mean >= -0.12 && mean <= 0.12
    printf "parallel handoff: early %.3f s, quality %.3f s, lead %.1f ms\n",
           handoff_onset, quality_onset, onset_lead * 1000
    printf "settled path: %s; pitch %.3f st p-p, sd %.3f st, 2 Hz %.3f st, mean %+.3f st\n",
           (steady_equal == 1 ? "quality-identical" : "DIFFERS"),
           hi - lo, sd, vibrato_amp, mean
    printf "parallel predictor/handoff: %s\n", (pass ? "PASS" : "FAIL")
    exit(pass ? 0 : 1)
  }
' "$TMP_DIR/handoff-pitch.csv"

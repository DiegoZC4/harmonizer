#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-backend-lab"
BINARY="${HARMONIZER_TEST_BINARY:-$BUILD/harmonizer_web_parallel}"
ANALYZER="$BUILD/pitch_analyzer"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/harmonizer-a1.XXXXXX")"
if [[ "${KEEP_FIXTURE:-0}" == "1" ]]; then
  printf 'keeping A1 fixture at %s\n' "$TMP_DIR"
else
  trap 'rm -rf "$TMP_DIR"' EXIT
fi

if [[ ! -x "$BINARY" || ! -x "$ANALYZER" ]]; then
  "$ROOT/scripts/build_backend_lab.sh"
fi

printf 'time,event,note\n0.0,on,45\n2.4,off,45\n' > "$TMP_DIR/midi.csv"
printf '{"mix":1.0,"gainDb":0.0,"gate":0.001,"stableWindow":1.0,"immediacy":0.25}\n' \
  > "$TMP_DIR/meta.json"
ffmpeg -hide_banner -loglevel error \
  -f lavfi -i 'anullsrc=r=44100:cl=mono:d=0.25' \
  -f lavfi -i 'aevalsrc=0.16*sin(2*PI*55*t)+0.08*sin(4*PI*55*t)+0.04*sin(6*PI*55*t):s=44100:d=2.0' \
  -f lavfi -i 'anullsrc=r=44100:cl=mono:d=0.25' \
  -filter_complex '[0:a][1:a][2:a]concat=n=3:v=0:a=1[out]' \
  -map '[out]' -ar 44100 -ac 1 -c:a pcm_f32le "$TMP_DIR/mic.wav" -y

"$BINARY" --render "$TMP_DIR" > /dev/null 2> "$TMP_DIR/render.log"
cat "$TMP_DIR/render.log" >&2
"$ANALYZER" --csv "$TMP_DIR/output.csv" --gate-rms 0.001 \
  "$TMP_DIR/render.wav" > /dev/null
OUTPUT_DB="$(ffmpeg -hide_banner -nostats -ss 0.75 -t 1.10 \
  -i "$TMP_DIR/render.wav" -af volumedetect -f null - 2>&1 | \
  awk '/mean_volume:/ { print $(NF - 1) }')"
ACCEPTED_HZ="$(awk '/acceptedHz/ {
  for (i = 1; i <= NF; i++) if ($i == "acceptedHz") { print $(i + 1); exit }
}' "$TMP_DIR/render.log")"

awk -F, -v output_db="$OUTPUT_DB" -v accepted_hz="$ACCEPTED_HZ" '
  NR > 1 && $1 >= 0.75 && $1 <= 1.85 && $4 > 0 {
    midi = 69 + 12 * log($4 / 440) / log(2)
    error = midi - 45
    if (!n || midi < lo) lo = midi
    if (!n || midi > hi) hi = midi
    abs_error += error < 0 ? -error : error
    n++
  }
  END {
    mean_abs = n > 0 ? abs_error / n : 999
    p2p = n > 0 ? hi - lo : 999
    detector_ok = accepted_hz >= 53.0 && accepted_hz <= 57.0
    output_pitch_ok = n >= 50 && mean_abs <= 0.25 && p2p <= 0.50
    pass = detector_ok && output_db > -60
    printf "A1 through harmonizer: detector %.3f Hz, %.1f dB, output pitch %s: %s\n",
           accepted_hz, output_db,
           output_pitch_ok ? "tracks target" : "contains low residual",
           pass ? "PASS" : "FAIL"
    exit(pass ? 0 : 1)
  }
' "$TMP_DIR/output.csv"

#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="${HARMONIZER_TEST_BINARY:-$ROOT/harmonizer_web}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/harmonizer-unvoiced.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

printf 'time,event,note\n0.0,on,60\n0.0,on,64\n0.0,on,67\n2.2,off,60\n2.2,off,64\n2.2,off,67\n' > "$TMP_DIR/midi.csv"
# One second of an A3-like vowel establishes F0, followed by a one-second
# high-frequency noise consonant and one second of silence.
ffmpeg -hide_banner -loglevel error \
  -f lavfi -i 'aevalsrc=0.18*sin(2*PI*220*t)+0.09*sin(4*PI*220*t)+0.045*sin(6*PI*220*t):s=44100:d=1' \
  -f lavfi -i 'anoisesrc=color=white:amplitude=0.08:sample_rate=44100:d=1' \
  -f lavfi -i 'anullsrc=r=44100:cl=mono:d=1' \
  -filter_complex '[1:a]highpass=f=4000[n];[0:a][n][2:a]concat=n=3:v=0:a=1[out]' \
  -map '[out]' -ar 44100 -ac 1 -c:a pcm_f32le "$TMP_DIR/mic.wav" -y

printf '{"mix":1.0,"gainDb":0.0,"gate":0.01,"stableWindow":1.0,"unvoicedMode":"tc-bypass"}\n' > "$TMP_DIR/meta.json"
"$BINARY" --render "$TMP_DIR" >/dev/null
mv "$TMP_DIR/render.wav" "$TMP_DIR/render-tc.wav"

printf '{"mix":1.0,"gainDb":0.0,"gate":0.01,"stableWindow":1.0,"unvoicedMode":"hold-ratio"}\n' > "$TMP_DIR/meta.json"
"$BINARY" --render "$TMP_DIR" >/dev/null
mv "$TMP_DIR/render.wav" "$TMP_DIR/render-hold.wav"

mean_volume() {
  local file="$1"
  local start="$2"
  local duration="$3"
  ffmpeg -hide_banner -nostats -ss "$start" -t "$duration" \
    -i "$file" -af volumedetect -f null - 2>&1 |
    awk '/mean_volume:/ { print $(NF - 1) }'
}

stereo_difference_volume() {
  local file="$1"
  local start="$2"
  local duration="$3"
  ffmpeg -hide_banner -nostats -ss "$start" -t "$duration" \
    -i "$file" -af 'pan=mono|c0=c0-c1,volumedetect' -f null - 2>&1 |
    awk '/mean_volume:/ { print $(NF - 1) }'
}

TC_SIBILANT_DB="$(mean_volume "$TMP_DIR/render-tc.wav" 1.45 0.40)"
TC_SILENCE_DB="$(mean_volume "$TMP_DIR/render-tc.wav" 2.70 0.20)"
TC_STEREO_DIFF_DB="$(stereo_difference_volume "$TMP_DIR/render-tc.wav" 1.45 0.40)"
HOLD_SIBILANT_DB="$(mean_volume "$TMP_DIR/render-hold.wav" 1.45 0.40)"
HOLD_STEREO_DIFF_DB="$(stereo_difference_volume "$TMP_DIR/render-hold.wav" 1.45 0.40)"

awk -v tc_sibilant="$TC_SIBILANT_DB" \
    -v tc_silence="$TC_SILENCE_DB" \
    -v tc_diff="$TC_STEREO_DIFF_DB" \
    -v hold_sibilant="$HOLD_SIBILANT_DB" \
    -v hold_diff="$HOLD_STEREO_DIFF_DB" '
  BEGIN {
    pass = tc_sibilant > -45.0 && tc_silence < -70.0 && tc_diff < -80.0 &&
           hold_sibilant > -45.0 && hold_diff > -70.0
    printf "TC bypass: sibilant %.1f dB, silence %.1f dB, L-R %.1f dB\n",
           tc_sibilant, tc_silence, tc_diff
    printf "hold ratio: sibilant %.1f dB, L-R %.1f dB\n",
           hold_sibilant, hold_diff
    printf "unvoiced modes: %s\n", pass ? "PASS" : "FAIL"
    exit(pass ? 0 : 1)
  }
'

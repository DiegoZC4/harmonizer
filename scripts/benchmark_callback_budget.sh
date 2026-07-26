#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-backend-lab"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/harmonizer-callback.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

if [[ ! -x "$BUILD/harmonizer_web_parallel" ]]; then
  "$ROOT/scripts/build_backend_lab.sh"
fi

ffmpeg -hide_banner -loglevel error \
  -f lavfi \
  -i 'aevalsrc=0.12*sin(2*PI*220*t)+0.06*sin(4*PI*220*t)+0.03*sin(6*PI*220*t):s=44100:d=2.0' \
  -ar 44100 -ac 1 -c:a pcm_f32le "$TMP_DIR/mic.wav" -y

printf '{"mix":1.0,"gainDb":0.0,"gate":0.001,"stableWindow":1.0,"immediacy":0.0}\n' \
  > "$TMP_DIR/meta.json"

printf 'voices,dsp_block_max_ms,callback_budget_ms,status\n'
for voices in 1 4 8 16; do
  printf 'time,event,note\n' > "$TMP_DIR/midi.csv"
  for ((voice = 0; voice < voices; voice++)); do
    printf '0.0,on,%d\n' "$((48 + voice))" >> "$TMP_DIR/midi.csv"
  done
  for ((voice = 0; voice < voices; voice++)); do
    printf '1.8,off,%d\n' "$((48 + voice))" >> "$TMP_DIR/midi.csv"
  done

  LOG="$TMP_DIR/render-$voices.log"
  "$BUILD/harmonizer_web_parallel" --render "$TMP_DIR" > /dev/null 2> "$LOG"
  MAX_MS="$(sed -n 's/.*DSP block max \([0-9.]*\) ms).*/\1/p' "$LOG" | tail -1)"
  awk -v voices="$voices" -v max_ms="$MAX_MS" 'BEGIN {
    budget = 64 * 1000 / 44100
    status = max_ms < budget ? "within" : "over"
    printf "%d,%.3f,%.3f,%s\n", voices, max_ms, budget, status
  }'
done

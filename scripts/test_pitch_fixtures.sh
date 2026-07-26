#!/bin/zsh

set -euo pipefail

script_dir="${0:A:h}"
project_dir="${script_dir:h}"
cd "$project_dir"

dataset="fixtures/vocadito"
report_dir="fixtures/pitch_reports"
tracks=(1 2 6 10 20 31)

if [[ ! -x ./pitch_analyzer ]]; then
  make pitch_analyzer
fi

if command -v ffmpeg >/dev/null 2>&1; then
  low_pitch_tmp="$(mktemp -d "${TMPDIR:-/tmp}/harmonizer-low-pitch.XXXXXX")"
  trap 'rm -rf "$low_pitch_tmp"' EXIT
  ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "sine=frequency=55:sample_rate=44100:duration=2" \
    -af "volume=0.2" -ac 1 -c:a pcm_s16le "$low_pitch_tmp/a1.wav"
  awk 'BEGIN { for (i = 0; i < 173; i++) printf "%.9f,55.0\n", i * 512 / 44100 }' \
    > "$low_pitch_tmp/a1_f0.csv"

  echo
  echo "== synthetic_A1 =="
  ./pitch_analyzer "$low_pitch_tmp/a1.wav" "$low_pitch_tmp/a1_f0.csv" \
    --gate-rms 0.001 --min-voiced-recall 0.70 --max-median-cents 25
fi

if [[ ! -d "$dataset/Audio" || ! -d "$dataset/Annotations/F0" ]]; then
  cat <<'MSG'
Missing fixtures/vocadito.

Download and extract Vocadito with:
  mkdir -p /private/tmp/harmonizer-fixtures fixtures/vocadito
  curl -L 'https://zenodo.org/records/5578807/files/vocadito.zip?download=1' -o /private/tmp/harmonizer-fixtures/vocadito.zip
  unzip -q /private/tmp/harmonizer-fixtures/vocadito.zip -d fixtures/vocadito -x '__MACOSX/*'
MSG
  exit 1
fi

mkdir -p "$report_dir"

for id in "${tracks[@]}"; do
  wav="$dataset/Audio/vocadito_${id}.wav"
  ref="$dataset/Annotations/F0/vocadito_${id}_f0.csv"
  csv="$report_dir/vocadito_${id}.csv"

  args=("$wav" "$ref" --csv "$csv")
  if [[ "${GATE_RMS:-}" != "" ]]; then
    args+=(--gate-rms "${GATE_RMS}")
  fi
  if [[ "${STABLE_WINDOW:-}" != "" ]]; then
    args+=(--stable-window "${STABLE_WINDOW}")
  fi
  if [[ "${MIN_CONFIDENCE:-0}" != "0" ]]; then
    args+=(--min-confidence "${MIN_CONFIDENCE}")
  fi
  if [[ "${STRICT:-0}" == "1" ]]; then
    args+=(--min-voiced-recall 0.50 --max-median-cents 150)
  fi

  echo
  echo "== vocadito_${id} =="
  ./pitch_analyzer "${args[@]}"
  echo "wrote $csv"
done

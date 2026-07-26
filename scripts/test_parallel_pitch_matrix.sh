#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-backend-lab"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/harmonizer-pitch-matrix.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

if [[ ! -x "$BUILD/harmonizer_web_parallel" || ! -x "$BUILD/pitch_analyzer" ]]; then
  "$ROOT/scripts/build_backend_lab.sh"
fi

RESULTS="$TMP_DIR/results.csv"
printf 'path,rate_hz,depth_st,target_midi,flutter,offset_samples,n,mean_st,sd_st,vibrato_st,peak_to_peak_st\n' > "$RESULTS"

make_fixture() {
  local rate="$1"
  local depth="$2"
  local wav="$TMP_DIR/input-${rate}-${depth}.wav"
  if [[ -f "$wav" ]]; then
    printf '%s\n' "$wav"
    return
  fi

  local phase="2*PI*261.625565*t+(261.625565*(0.057762265*${depth})/${rate})*(1-cos(2*PI*${rate}*t))"
  ffmpeg -hide_banner -loglevel error \
    -f lavfi -i 'anullsrc=r=44100:cl=mono:d=0.25' \
    -f lavfi -i "aevalsrc=0.16*sin(${phase})+0.08*sin(2*(${phase}))+0.04*sin(3*(${phase})):s=44100:d=2.1" \
    -f lavfi -i 'anullsrc=r=44100:cl=mono:d=0.35' \
    -filter_complex '[0:a][1:a][2:a]concat=n=3:v=0:a=1[out]' \
    -map '[out]' -ar 44100 -ac 1 -c:a pcm_f32le "$wav" -y
  printf '%s\n' "$wav"
}

analyze_render() {
  local wav="$1"
  local target="$2"
  local rate="$3"
  local csv="$4"
  "$BUILD/pitch_analyzer" --csv "$csv" --gate-rms 0.001 "$wav" >/dev/null
  awk -F, -v target="$target" -v rate="$rate" '
    NR > 1 && $1 >= 0.85 && $1 <= 2.15 && $4 > 0 {
      midi = 69 + 12 * log($4 / 440) / log(2)
      error = midi - target
      if (!n || error < lo) lo = error
      if (!n || error > hi) hi = error
      sum += error
      sum_sq += error * error
      sin_f += error * sin(2 * 3.141592653589793 * rate * $1)
      cos_f += error * cos(2 * 3.141592653589793 * rate * $1)
      n++
    }
    END {
      mean = sum / n
      sd = sqrt(sum_sq / n - mean * mean)
      amplitude = 2 * sqrt(sin_f * sin_f + cos_f * cos_f) / n
      printf "%d,%.6f,%.6f,%.6f,%.6f", n, mean, sd, amplitude, hi - lo
    }
  ' "$csv"
}

render_case() {
  local path="$1"
  local rate="$2"
  local depth="$3"
  local target="$4"
  local flutter="$5"
  local offset="$6"
  local immediacy="$7"
  local persistent="$8"
  local name="${path}-${rate}-${depth}-${target}-${flutter}-${offset}"
  local dir="$TMP_DIR/$name"
  mkdir -p "$dir"
  cp "$(make_fixture "$rate" "$depth")" "$dir/mic.wav"
  printf 'time,event,note\n0.0,on,%s\n2.6,off,%s\n' "$target" "$target" > "$dir/midi.csv"
  printf '{"mix":1.0,"gainDb":0.0,"gate":0.001,"stableWindow":1.0,"immediacy":%s,"flutterCompensation":%s,"earlyPitchOffsetSamples":%s,"parallelEarlyPersistent":%s}\n' \
    "$immediacy" "$flutter" "$offset" "$persistent" > "$dir/meta.json"
  "$BUILD/harmonizer_web_parallel" --render "$dir" > /dev/null 2> "$dir/render.log"
  local metrics
  metrics="$(analyze_render "$dir/render.wav" "$target" "$rate" "$dir/pitch.csv")"
  printf '%s,%s,%s,%s,%s,%s,%s\n' \
    "$path" "$rate" "$depth" "$target" "$flutter" "$offset" "$metrics" >> "$RESULTS"
}

for rate in 2 4 6; do
  for depth in 0.25 0.50; do
    for target in 55 60 67; do
      render_case quality "$rate" "$depth" "$target" 0 0 0 0
      render_case quality "$rate" "$depth" "$target" 1 0 0 0
    done
  done
done

for rate in 2 4 6; do
  for offset in 0 -128 -256 -384 -512 -640 -768 -896 -1024 -1088 -1120 -1152 -1184 -1216 -1248 -1280 -1536 -1792 -2048; do
    render_case early "$rate" 0.50 60 0 "$offset" 1 1
  done
done

awk -F, '
  NR > 1 && $1 == "quality" {
    key = $5
    sd[key] += $9
    vib[key] += $10
    n[key]++
  }
  END {
    for (key in n)
      printf "quality flutter %s: mean sd %.4f st, residual vibrato %.4f st\n",
             key, sd[key] / n[key], vib[key] / n[key]
  }
' "$RESULTS" | sort

awk -F, '
  NR > 1 && $1 == "early" {
    key = $6
    sd[key] += $9
    vib[key] += $10
    n[key]++
  }
  END {
    for (key in n)
      printf "early offset %s: mean sd %.4f st, residual vibrato %.4f st\n",
             key, sd[key] / n[key], vib[key] / n[key]
  }
' "$RESULTS" | sort -n -k3

QUALITY_WINNER="$(awk -F, '
  NR > 1 && $1 == "quality" { score[$5] += $9 + $10; n[$5]++ }
  END { for (key in n) if (!set || score[key] / n[key] < best) {
    set = 1; best = score[key] / n[key]; winner = key
  } print winner }
' "$RESULTS")"
EARLY_WINNER="$(awk -F, '
  NR > 1 && $1 == "early" { score[$6] += $9 + $10; n[$6]++ }
  END { for (key in n) if (!set || score[key] / n[key] < best) {
    set = 1; best = score[key] / n[key]; winner = key
  } print winner }
' "$RESULTS")"

printf 'matrix winners: flutter=%s, early offset=%s samples\n' \
  "$QUALITY_WINNER" "$EARLY_WINNER"

if [[ "$QUALITY_WINNER" != "0" ]]; then
  printf 'parallel pitch matrix: FAIL (flutter compensation won)\n' >&2
  exit 1
fi
if (( EARLY_WINNER < -1280 || EARLY_WINNER > -1024 )); then
  printf 'parallel pitch matrix: FAIL (R2 optimum left the measured basin)\n' >&2
  exit 1
fi
printf 'parallel pitch matrix: PASS\n'

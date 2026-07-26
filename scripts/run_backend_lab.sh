#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build-backend-lab"
backend="${1:-}"

case "$backend" in
  live|reference)
    binary="$build/harmonizer_web_rubberband_live_reference"
    port="${2:-8795}"
    ;;
  parallel)
    binary="$build/harmonizer_web_parallel"
    port="${2:-8799}"
    ;;
  r2)
    binary="$build/harmonizer_web_rubberband_r2"
    port="${2:-8796}"
    ;;
  live128)
    binary="$build/harmonizer_web_rubberband_live128"
    port="${2:-8798}"
    ;;
  signalsmith)
    binary="$build/harmonizer_web_signalsmith"
    port="${2:-8797}"
    ;;
  *)
    printf 'usage: %s {live|parallel|live128|r2|signalsmith} [port]\n' "$0" >&2
    exit 2
    ;;
esac

if [[ ! -x "$binary" ]]; then
  "$root/scripts/build_backend_lab.sh"
fi

url="http://127.0.0.1:$port/"
"$binary" --port "$port" &
server_pid=$!
cleanup() {
  kill "$server_pid" 2>/dev/null || true
  wait "$server_pid" 2>/dev/null || true
}
trap cleanup INT TERM EXIT

for _attempt in $(seq 1 100); do
  if curl -fsS "${url}health" >/dev/null 2>&1; then
    printf 'Backend %s is running at %s\n' "$backend" "$url"
    if command -v open >/dev/null 2>&1; then
      open "$url"
    elif command -v xdg-open >/dev/null 2>&1; then
      xdg-open "$url" >/dev/null 2>&1 || true
    fi
    wait "$server_pid"
    exit $?
  fi
  sleep 0.05
done

printf 'Backend failed to start at %s\n' "$url" >&2
exit 1

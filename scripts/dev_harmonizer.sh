#!/bin/zsh

set -u

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <make-target> <source-file> [more-source-files...]"
  exit 1
fi

target="$1"
shift
sources=("$@")
app_path="./$target"
app_pid=""

get_latest_mtime() {
  local latest=0
  local file mtime
  for file in "$@"; do
    [[ -e "$file" ]] || continue
    mtime=$(stat -f "%m" "$file")
    if (( mtime > latest )); then
      latest=$mtime
    fi
  done
  echo "$latest"
}

stop_app() {
  if [[ -n "$app_pid" ]] && kill -0 "$app_pid" 2>/dev/null; then
    kill "$app_pid" 2>/dev/null || true
    wait "$app_pid" 2>/dev/null || true
  fi
  app_pid=""
}

build_and_run() {
  stop_app
  printf "\n[%s] rebuilding %s\n" "$(date '+%H:%M:%S')" "$target"
  if ! make "$target"; then
    echo "build failed; waiting for the next file change"
    return 1
  fi

  printf "[%s] launching %s\n" "$(date '+%H:%M:%S')" "$app_path"
  "$app_path" &
  app_pid=$!
  return 0
}

trap 'stop_app; exit 0' INT TERM EXIT

last_mtime=$(get_latest_mtime "${sources[@]}")
build_and_run

while true; do
  sleep 1
  current_mtime=$(get_latest_mtime "${sources[@]}")
  if [[ "$current_mtime" != "$last_mtime" ]]; then
    last_mtime="$current_mtime"
    build_and_run
  fi
done

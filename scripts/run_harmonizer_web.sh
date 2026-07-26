#!/bin/zsh

set -euo pipefail

cd "${0:A:h:h}"

port="${HARMONIZER_WEB_PORT:-8794}"
watch="${HARMONIZER_WEB_WATCH:-1}"
binary="./build-backend-lab/harmonizer_web"
sources=(
  harmonizer_web.cpp
  harmonizer_rubberband_engine.hpp
  collier_effects.hpp
  web/index.html
  CMakeLists.txt
  scripts/build_backend_lab.sh
  backends/*.cpp
  backends/*.hpp
)
watcher="./watch_files_macos"
app_pid=""
watcher_pid=""
watch_fd=""
event_pipe="${TMPDIR:-/tmp}/harmonizer-web-watch.$$"

stop_app() {
  if [[ -n "$app_pid" ]] && kill -0 "$app_pid" 2>/dev/null; then
    kill "$app_pid" 2>/dev/null || true
    wait "$app_pid" 2>/dev/null || true
  fi
  app_pid=""
}

stop_watcher() {
  if [[ -n "${watch_fd:-}" ]]; then
    exec {watch_fd}<&- 2>/dev/null || true
    watch_fd=""
  fi
  if [[ -n "$watcher_pid" ]] && kill -0 "$watcher_pid" 2>/dev/null; then
    kill "$watcher_pid" 2>/dev/null || true
    wait "$watcher_pid" 2>/dev/null || true
  fi
  watcher_pid=""
  rm -f "$event_pipe"
}

start_watcher() {
  stop_watcher
  rm -f "$event_pipe"
  mkfifo "$event_pipe"
  "$watcher" "${sources[@]}" > "$event_pipe" &
  watcher_pid=$!
  exec {watch_fd}<"$event_pipe"
  rm -f "$event_pipe"
}

cleanup() {
  stop_watcher
  stop_app
  exit 0
}

build_and_restart() {
  printf "\n[%s] rebuilding harmonizer backends\n" "$(date '+%H:%M:%S')"
  if ! ./scripts/build_backend_lab.sh; then
    echo "build failed; keeping the current server process"
    return 1
  fi

  stop_app
  printf "[%s] launching %s --port %s\n" "$(date '+%H:%M:%S')" "$binary" "$port"
  "$binary" --port "$port" &
  app_pid=$!
}

if [[ "$watch" == "0" || "$watch" == "false" ]]; then
  ./scripts/build_backend_lab.sh
  exec "$binary" --port "$port"
fi

trap 'cleanup' INT TERM EXIT

make watch_files_macos
echo "watching the native server, all DSP backends, browser GUI, and CMake files with macOS file events"
build_and_restart
start_watcher

while true; do
  if IFS= read -r _event <&$watch_fd; then
    build_and_restart
  else
    echo "file-event watcher exited; restarting watcher in 1 second"
    sleep 1
    start_watcher
  fi
done

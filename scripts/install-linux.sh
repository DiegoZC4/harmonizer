#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_backends=OFF
launch=true
for argument in "$@"; do
  case "$argument" in
    --all-backends) build_backends=ON ;;
    --no-launch) launch=false ;;
  esac
done

if ! command -v apt-get >/dev/null 2>&1; then
  printf 'This installer supports Ubuntu and Debian. See README.md for other distributions.\n' >&2
  exit 1
fi

sudo apt-get update
sudo apt-get install -y \
  build-essential cmake pkg-config curl ca-certificates \
  portaudio19-dev libaubio-dev

rubberband_source="$($root/scripts/fetch_rubberband.sh)"
build="$root/build-linux"
dist="$root/dist/Harmonizer-Linux"

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release \
  -DHARMONIZER_BUILD_BACKEND_LAB="$build_backends" \
  -DFETCHCONTENT_SOURCE_DIR_RUBBERBAND="$rubberband_source"
cmake --build "$build" --parallel
cmake --install "$build" --prefix "$dist"
cp "$root/packaging/linux/Harmonizer" "$dist/Harmonizer"
chmod +x "$dist/Harmonizer" "$dist/harmonizer_web"

printf '\nInstalled to %s\n' "$dist"
if [[ "$launch" == true ]]; then
  exec "$dist/Harmonizer"
fi

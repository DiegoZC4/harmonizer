#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build/release}"
output_dir="${HARMONIZER_OUTPUT_DIR:-$root/dist}"
dist="$output_dir/Harmonizer-Windows-x64"
archive="$output_dir/Harmonizer-Windows-x64.zip"
mingw_prefix="${MINGW_PREFIX:-/ucrt64}"

if [[ ! -x "$build/harmonizer_web.exe" ]]; then
  printf 'Missing executable: %s\n' "$build/harmonizer_web.exe" >&2
  exit 1
fi

rm -rf "$dist" "$archive"
cmake --install "$build" --prefix "$dist"
cp "$root/packaging/windows/Harmonizer.cmd" "$dist/Harmonizer.cmd"
cp "$root/packaging/windows/Harmonizer.ps1" "$dist/Harmonizer.ps1"

copy_runtime_dependencies() {
  local candidate dependency basename
  for candidate in "$dist/harmonizer_web.exe" "$dist"/*.dll; do
    [[ -f "$candidate" ]] || continue
    while read -r dependency; do
      [[ "$dependency" == "$mingw_prefix"/bin/* ]] || continue
      basename="$(basename "$dependency")"
      [[ -f "$dist/$basename" ]] || cp "$dependency" "$dist/$basename"
    done < <(ldd "$candidate" | awk '/=> \// { print $3 }')
  done
}

for _pass in 1 2 3 4; do
  before="$(find "$dist" -maxdepth 1 -name '*.dll' | wc -l)"
  copy_runtime_dependencies
  after="$(find "$dist" -maxdepth 1 -name '*.dll' | wc -l)"
  [[ "$before" == "$after" ]] && break
done

if ldd "$dist/harmonizer_web.exe" | grep -q 'not found'; then
  ldd "$dist/harmonizer_web.exe" >&2
  printf 'One or more Windows runtime libraries were not bundled.\n' >&2
  exit 1
fi

(
  cd "$output_dir"
  zip -X -qr "$(basename "$archive")" "$(basename "$dist")"
)

printf 'Built %s\n' "$archive"

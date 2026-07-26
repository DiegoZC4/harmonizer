#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build/release}"
binary="$build/harmonizer_web"
output_dir="${HARMONIZER_OUTPUT_DIR:-$root/dist}"
appdir="$output_dir/Harmonizer.AppDir"
tools="$root/.deps/tools"
linuxdeploy="$tools/linuxdeploy-x86_64.AppImage"
appimagetool="$tools/appimagetool-x86_64.AppImage"
linuxdeploy_url="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage"
linuxdeploy_sha256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
appimagetool_url="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
appimagetool_sha256="a6d71e2b6cd66f8e8d16c37ad164658985e0cf5fcaa950c90a482890cb9d13e0"
output="$output_dir/Harmonizer-Linux-x86_64.AppImage"

verify_sha256() {
  local file="$1"
  local expected="$2"
  printf '%s  %s\n' "$expected" "$file" | sha256sum --check --status
}

fetch_tool() {
  local destination="$1"
  local url="$2"
  local expected="$3"
  if [[ ! -f "$destination" ]] || ! verify_sha256 "$destination" "$expected"; then
    curl --fail --location --retry 3 "$url" --output "$destination"
  fi
  verify_sha256 "$destination" "$expected"
  chmod +x "$destination"
}

if [[ ! -x "$binary" ]]; then
  printf 'Missing executable: %s\n' "$binary" >&2
  exit 1
fi
if [[ "$(uname -m)" != "x86_64" ]]; then
  printf 'The release AppImage must be built on x86_64 Linux.\n' >&2
  exit 1
fi

mkdir -p "$output_dir" "$tools"
fetch_tool "$linuxdeploy" "$linuxdeploy_url" "$linuxdeploy_sha256"
fetch_tool "$appimagetool" "$appimagetool_url" "$appimagetool_sha256"

rm -rf "$appdir" "$output"
mkdir -p \
  "$appdir/usr/bin" \
  "$appdir/usr/share/applications" \
  "$appdir/usr/share/harmonizer/web" \
  "$appdir/usr/share/icons/hicolor/scalable/apps" \
  "$appdir/usr/share/doc/harmonizer"

cp "$binary" "$appdir/usr/bin/harmonizer_web"
cp "$root/packaging/linux/Harmonizer" "$appdir/usr/bin/Harmonizer"
cp "$root/web/index.html" "$appdir/usr/share/harmonizer/web/index.html"
cp "$root/packaging/linux/harmonizer.desktop" \
  "$appdir/usr/share/applications/harmonizer.desktop"
cp "$root/packaging/linux/harmonizer.svg" \
  "$appdir/usr/share/icons/hicolor/scalable/apps/harmonizer.svg"
cp "$root/README.md" "$root/QUICKSTART.md" "$root/LICENSE" \
  "$root/THIRD_PARTY_NOTICES.md" "$appdir/usr/share/doc/harmonizer/"
chmod +x "$appdir/usr/bin/Harmonizer" "$appdir/usr/bin/harmonizer_web"

"$linuxdeploy" --appimage-extract-and-run \
  --appdir "$appdir" \
  --executable "$appdir/usr/bin/harmonizer_web" \
  --desktop-file "$appdir/usr/share/applications/harmonizer.desktop" \
  --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/harmonizer.svg"

rm -f "$appdir/AppRun"
cp "$root/packaging/linux/AppRun" "$appdir/AppRun"
chmod +x "$appdir/AppRun"
ARCH=x86_64 "$appimagetool" --appimage-extract-and-run "$appdir" "$output"
chmod +x "$output"

printf 'Built %s\n' "$output"

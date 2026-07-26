#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="4.0.0"
sha256="24300f48a8014b7c863b573a9647e61b1b19b37875e2cdd92005e64c6424d266"
deps="$root/.deps"
source_dir="$deps/rubberband-$version"
archive="$deps/rubberband-v$version.tar.gz"

if [[ -f "$source_dir/rubberband/RubberBandLiveShifter.h" ]]; then
  printf '%s\n' "$source_dir"
  exit 0
fi

mkdir -p "$deps"
curl --fail --location --retry 3 \
  "https://github.com/breakfastquay/rubberband/archive/refs/tags/v$version.tar.gz" \
  --output "$archive"

if command -v sha256sum >/dev/null 2>&1; then
  actual="$(sha256sum "$archive" | awk '{print $1}')"
else
  actual="$(shasum -a 256 "$archive" | awk '{print $1}')"
fi
if [[ "$actual" != "$sha256" ]]; then
  printf 'Rubber Band checksum mismatch\n' >&2
  exit 1
fi

staging="$deps/.rubberband-$version-staging"
rm -rf "$staging" "$source_dir"
mkdir -p "$staging"
tar -xzf "$archive" -C "$staging"
mv "$staging/rubberband-$version" "$source_dir"
rmdir "$staging"
printf '%s\n' "$source_dir"

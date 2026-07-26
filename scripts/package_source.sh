#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(<"$root/VERSION.txt")"
output_dir="${HARMONIZER_OUTPUT_DIR:-$root/dist}"
archive="$output_dir/Harmonizer-source.zip"
prefix="Harmonizer-$version/"

if ! git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  printf 'Source packages must be created from a Git commit.\n' >&2
  exit 1
fi

mkdir -p "$output_dir"
rm -f "$archive"
git -C "$root" archive \
  --format=zip \
  --prefix="$prefix" \
  --output="$archive" \
  HEAD

printf 'Built %s from %s\n' "$archive" "$(git -C "$root" rev-parse --short HEAD)"

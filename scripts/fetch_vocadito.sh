#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps="$root/.deps"
archive="$deps/vocadito.zip"
destination="$root/fixtures/vocadito"
staging="$deps/.vocadito-staging"
url="https://zenodo.org/api/records/5578807/files/vocadito.zip/content"
expected_md5="dea40fd18f14d899643c4ba221b33a46"

checksum() {
  if command -v md5 >/dev/null 2>&1; then
    md5 -q "$1"
  else
    md5sum "$1" | awk '{ print $1 }'
  fi
}

if [[ -f "$destination/vocadito_metadata.csv" ]]; then
  printf 'Vocadito is already installed at %s\n' "$destination"
  exit 0
fi

mkdir -p "$deps"
if [[ ! -f "$archive" ]] || [[ "$(checksum "$archive")" != "$expected_md5" ]]; then
  curl --fail --location --retry 3 "$url" --output "$archive"
fi
if [[ "$(checksum "$archive")" != "$expected_md5" ]]; then
  printf 'Vocadito checksum mismatch: %s\n' "$archive" >&2
  exit 1
fi

rm -rf "$staging"
mkdir -p "$staging"
unzip -q "$archive" -d "$staging" -x '__MACOSX/*'
rm -rf "$destination"
mkdir -p "$(dirname "$destination")"
mv "$staging" "$destination"

printf 'Installed Vocadito at %s\n' "$destination"

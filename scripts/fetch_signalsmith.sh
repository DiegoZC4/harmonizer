#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps="$root/.deps"
stretch_commit="57b93f4e9206a089a45387eaa39bdc9f310d3308"
stretch_sha="ad02e24334438b203e81d44f6c9906f3c6773e90a4ea923bb3e73d15697187d6"
linear_version="0.3.1"
linear_sha="b294471f1306baa4d968b230a8836924680e9ca068a667f4659259d66edbe9bc"

checksum() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

fetch_source() {
  local name="$1" url="$2" expected="$3" marker="$4"
  local source_dir="$deps/$name" archive="$deps/$name.tar.gz"
  if [[ -f "$source_dir/$marker" ]]; then
    printf '%s\n' "$source_dir"
    return
  fi

  mkdir -p "$deps"
  curl --fail --location --retry 3 "$url" --output "$archive"
  if [[ "$(checksum "$archive")" != "$expected" ]]; then
    printf '%s checksum mismatch\n' "$name" >&2
    exit 1
  fi

  local staging="$deps/.$name-staging"
  rm -rf "$staging" "$source_dir"
  mkdir -p "$staging" "$source_dir"
  tar -xzf "$archive" -C "$staging"
  local extracted
  extracted="$(find "$staging" -mindepth 1 -maxdepth 1 -type d | head -1)"
  cp -R "$extracted"/. "$source_dir"/
  rm -rf "$staging"
  printf '%s\n' "$source_dir"
}

fetch_source \
  "signalsmith-stretch-$stretch_commit" \
  "https://github.com/Signalsmith-Audio/signalsmith-stretch/archive/$stretch_commit.tar.gz" \
  "$stretch_sha" \
  "signalsmith-stretch.h"
fetch_source \
  "signalsmith-linear-$linear_version" \
  "https://github.com/Signalsmith-Audio/linear/archive/refs/tags/$linear_version.tar.gz" \
  "$linear_sha" \
  "CMakeLists.txt"

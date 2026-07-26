#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(<"$root/VERSION.txt")"

if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  printf 'VERSION.txt is not semantic: %s\n' "$version" >&2
  exit 1
fi

required=(
  CHANGELOG.md
  LICENSE
  README.md
  THIRD_PARTY_NOTICES.md
  VERSION.txt
  .github/workflows/ci.yml
  .github/workflows/release.yml
)
for path in "${required[@]}"; do
  if [[ ! -f "$root/$path" ]]; then
    printf 'Missing release file: %s\n' "$path" >&2
    exit 1
  fi
done

git -C "$root" diff --check
for forbidden in \
  'Benjamin_Bloomberg_Making_Musical_Magic_Live_2020.pdf' \
  'research/jacob_collier/PHkKMJ6DCZQ.mp4' \
  'fixtures/vocadito/Audio'; do
  if git -C "$root" ls-files | grep -q "$forbidden"; then
    printf 'Generated or copyrighted material is tracked: %s\n' "$forbidden" >&2
    exit 1
  fi
done

cmake --preset test
cmake --build --preset test
ctest --preset test
node "$root/public/test-pitch.mjs"

printf 'Release checks passed for %s\n' "$version"

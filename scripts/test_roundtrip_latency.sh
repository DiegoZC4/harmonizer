#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROBE="$ROOT/build-backend-lab/latency_probe"

if [[ ! -x "$PROBE" ]]; then
  "$ROOT/scripts/build_backend_lab.sh"
fi

"$PROBE" --self-test --mode waveform --json
"$PROBE" --self-test --mode envelope --json

printf 'Round-trip latency correlator self-tests: PASS\n'

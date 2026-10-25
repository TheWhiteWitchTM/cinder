#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QMAKE="${QMAKE:-}"
if [[ -z "$QMAKE" ]]; then
    if [[ -x "$HOME/Qt/6.12.0/gcc_64/bin/qmake" ]]; then
        QMAKE="$HOME/Qt/6.12.0/gcc_64/bin/qmake"
    elif command -v qmake6 >/dev/null 2>&1; then
        QMAKE="$(command -v qmake6)"
    else
        QMAKE="$(command -v qmake)"
    fi
fi
mkdir -p "$ROOT/build"
cd "$ROOT/build"
"$QMAKE" "$ROOT/cinder.pro"
make -j"$(nproc)"
echo "Built $ROOT/build/cinder"

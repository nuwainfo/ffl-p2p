#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${PYTHON:-python3}"
ARCH="${ARCH:-$(uname -m)}"
OUT="$ROOT/out/native-macos"
BUILD="$OUT/build"
FAKE_PLUM=0
CLEAN=0

for argument in "$@"; do
    case "$argument" in
        --fake-plum) FAKE_PLUM=1 ;;
        --clean) CLEAN=1 ;;
        *) echo "Unknown argument: $argument" >&2; exit 2 ;;
    esac
done

for command in cmake git otool "$PYTHON"; do
    command -v "$command" >/dev/null 2>&1 || { echo "Missing required command: $command" >&2; exit 1; }
done

if [[ $CLEAN -eq 1 ]]; then
    rm -rf "$OUT"
fi

cmake_args=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$ARCH")
if [[ $FAKE_PLUM -eq 1 ]]; then
    cmake_args+=(-DFFL_P2P_FAKE_PLUM=ON)
fi
if [[ -n "${MACOSX_DEPLOYMENT_TARGET:-}" ]]; then
    cmake_args+=(-DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET")
fi

if [[ $FAKE_PLUM -eq 1 ]]; then
    "$PYTHON" "$ROOT/scripts/Bootstrap.py" --fake-plum
else
    "$PYTHON" "$ROOT/scripts/Bootstrap.py"
fi
cmake -S "$ROOT" -B "$BUILD" "${cmake_args[@]}"
cmake --build "$BUILD" --target _ffl_p2p --parallel

extension="$(find "$BUILD" -type f -name '_ffl_p2p*.so' -print -quit)"
[[ -n "$extension" ]] || { echo "Built _ffl_p2p extension was not found" >&2; exit 1; }
cp "$extension" "$ROOT/src/ffl_p2p/$(basename "$extension")"

dependencies="$(otool -L "$extension")"
printf '%s\n' "$dependencies"
if grep -Eiq '(libjuice|libplum)\.(dylib|so)' <<<"$dependencies"; then
    echo "The extension has dynamically linked third-party dependencies." >&2
    exit 1
fi

echo "[PASS] Native macOS build completed: $ROOT/src/ffl_p2p/$(basename "$extension")"

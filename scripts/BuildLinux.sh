#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${PYTHON:-python3}"
OUT="$ROOT/out/native-linux"
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

for command in cmake git ldd "$PYTHON"; do
    command -v "$command" >/dev/null 2>&1 || { echo "Missing required command: $command" >&2; exit 1; }
done

if [[ $CLEAN -eq 1 ]]; then
    rm -rf "$OUT"
fi

bootstrap_args=()
cmake_args=(-DCMAKE_BUILD_TYPE=Release)
if [[ $FAKE_PLUM -eq 1 ]]; then
    bootstrap_args+=(--fake-plum)
    cmake_args+=(-DFFL_P2P_FAKE_PLUM=ON)
fi

"$PYTHON" "$ROOT/scripts/Bootstrap.py" "${bootstrap_args[@]}"
cmake -S "$ROOT" -B "$BUILD" "${cmake_args[@]}"
cmake --build "$BUILD" --target _ffl_p2p --parallel

extension="$(find "$BUILD" -type f -name '_ffl_p2p*.so' -print -quit)"
[[ -n "$extension" ]] || { echo "Built _ffl_p2p extension was not found" >&2; exit 1; }
cp "$extension" "$ROOT/src/ffl_p2p/$(basename "$extension")"

dependencies="$(ldd "$extension")"
printf '%s\n' "$dependencies"
if grep -Eiq '(libjuice|libplum)\.so' <<<"$dependencies"; then
    echo "The extension has dynamically linked third-party dependencies." >&2
    exit 1
fi

echo "[PASS] Native Linux build completed: $ROOT/src/ffl_p2p/$(basename "$extension")"

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

for command in cmake git ldd pkg-config "$PYTHON"; do
    command -v "$command" >/dev/null 2>&1 || { echo "Missing required command: $command" >&2; exit 1; }
done

GNUTLS_ROOT="${FFL_P2P_GNUTLS_ROOT:-${CONDA_PREFIX:-}}"
if [[ -n "$GNUTLS_ROOT" && -f "$GNUTLS_ROOT/lib/pkgconfig/gnutls.pc" ]]; then
    export PKG_CONFIG_PATH="$GNUTLS_ROOT/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    echo "Using GnuTLS pkg-config metadata: $GNUTLS_ROOT/lib/pkgconfig/gnutls.pc"
fi

if ! pkg-config --atleast-version=3.7.5 gnutls; then
    cat >&2 <<'EOF'
ffl-p2p requires GnuTLS 3.7.5 or newer for QUIC support.
Install a newer system GnuTLS development package, or install GnuTLS in the
active Conda environment and rerun this command:

  conda install -c conda-forge "gnutls>=3.7.5" pkg-config

Set FFL_P2P_GNUTLS_ROOT=/path/to/prefix when the desired gnutls.pc is outside
the active Conda environment.
EOF
    exit 1
fi

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

#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${PYTHON:-python3}"
OUT="$ROOT/out/native-linux"
BUILD="$OUT/build"
RAW_WHEEL="$OUT/raw-wheel"
WHEEL_DIR="$OUT/wheel"
WHEEL_EXTRACT="$OUT/wheel-extract"
FAKE_PLUM=0

for argument in "$@"; do
    case "$argument" in
        --fake-plum) FAKE_PLUM=1 ;;
        --clean) : ;; # Kept for compatibility; every Linux build is clean.
        *) echo "Unknown argument: $argument" >&2; exit 2 ;;
    esac
done

for command in cmake git ldd pkg-config "$PYTHON"; do
    command -v "$command" >/dev/null 2>&1 || { echo "Missing required command: $command" >&2; exit 1; }
done

detect_manylinux_plat() {
    local platform="${MANYLINUX_PLAT:-${AUDITWHEEL_PLAT:-}}"
    local arch glibc candidate supported

    command -v auditwheel >/dev/null 2>&1 || return 1
    if [[ "$platform" == manylinux_* ]]; then
        printf '%s\n' "$platform"
        return 0
    fi

    arch="$(uname -m)"
    glibc="$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}' || true)"
    if [[ "$glibc" =~ ^([0-9]+)\.([0-9]+)$ ]]; then
        candidate="manylinux_${BASH_REMATCH[1]}_${BASH_REMATCH[2]}_${arch}"
        supported="$(auditwheel repair --help 2>&1 || true)"
        if grep -Fq "$candidate" <<<"$supported"; then
            printf '%s\n' "$candidate"
            return 0
        fi
    fi

    return 1
}

validate_manylinux_build_host() {
    local target="$1"
    local hostGlibc targetMajor targetMinor hostMajor hostMinor

    [[ "$target" =~ ^manylinux_([0-9]+)_([0-9]+)_ ]] || return 0
    targetMajor="${BASH_REMATCH[1]}"
    targetMinor="${BASH_REMATCH[2]}"
    hostGlibc="$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}' || true)"
    [[ "$hostGlibc" =~ ^([0-9]+)\.([0-9]+)$ ]] || return 0
    hostMajor="${BASH_REMATCH[1]}"
    hostMinor="${BASH_REMATCH[2]}"

    if (( hostMajor > targetMajor || (hostMajor == targetMajor && hostMinor > targetMinor) )); then
        cat >&2 <<EOF
Cannot build $target on this host: its glibc is $hostGlibc.
Build inside a $target-compatible manylinux image instead. auditwheel can bundle
third-party libraries, but it cannot lower GLIBC or libstdc++ symbol versions.
EOF
        exit 1
    fi
}

REQUESTED_MANYLINUX="${MANYLINUX_PLAT:-${AUDITWHEEL_PLAT:-}}"
if [[ -n "$REQUESTED_MANYLINUX" ]] && ! command -v auditwheel >/dev/null 2>&1; then
    echo "auditwheel is required for requested target: $REQUESTED_MANYLINUX" >&2
    exit 1
fi

MANYLINUX=""
if MANYLINUX="$(detect_manylinux_plat)"; then
    echo "manylinux     : enabled ($MANYLINUX)"
    validate_manylinux_build_host "$MANYLINUX"
else
    echo "manylinux     : not detected; building a native Linux wheel"
fi

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

# Native extensions and wheels must always be rebuilt together. In particular,
# never package an extension left by a different Python ABI or CMake cache.
rm -rf "$OUT"

if ! "$PYTHON" -c 'import build' >/dev/null 2>&1; then
    "$PYTHON" -m pip install --disable-pip-version-check build
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

rm -rf "$RAW_WHEEL" "$WHEEL_DIR" "$WHEEL_EXTRACT"
mkdir -p "$RAW_WHEEL" "$WHEEL_DIR"
"$PYTHON" -m build --wheel --no-isolation --outdir "$RAW_WHEEL" "$ROOT"

rawWheels=("$RAW_WHEEL"/*.whl)
[[ -f "${rawWheels[0]}" && ${#rawWheels[@]} -eq 1 ]] || {
    echo "Expected one raw wheel." >&2
    exit 1
}
[[ "${rawWheels[0]}" != *-none-any.whl ]] || {
    echo "The raw wheel is incorrectly tagged as pure Python. Ensure setup.py is present." >&2
    exit 1
}

if [[ -n "$MANYLINUX" ]]; then
    auditwheel show "${rawWheels[0]}"
    auditwheel repair --plat "$MANYLINUX" --wheel-dir "$WHEEL_DIR" "${rawWheels[0]}"
else
    cp "${rawWheels[0]}" "$WHEEL_DIR/"
fi

wheels=("$WHEEL_DIR"/*.whl)
[[ -f "${wheels[0]}" && ${#wheels[@]} -eq 1 ]] || {
    echo "Expected one final wheel." >&2
    exit 1
}

"$PYTHON" - "$WHEEL_EXTRACT" "${wheels[0]}" <<'PY'
import shutil
import sys
import zipfile
from pathlib import Path

destination, wheel = map(Path, sys.argv[1:])
shutil.rmtree(destination, ignore_errors=True)
with zipfile.ZipFile(wheel) as archive:
    archive.extractall(destination)
extensions = list(destination.glob('ffl_p2p/_ffl_p2p*.so'))
if len(extensions) != 1:
    raise SystemExit(f'Expected one native extension in the final wheel, found {len(extensions)}')
print(extensions[0])
PY

echo "[PASS] Native Linux wheel build completed: ${wheels[0]}"

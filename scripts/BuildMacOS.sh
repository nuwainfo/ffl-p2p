#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${PYTHON:-}" ]]; then
    PYTHON="$PYTHON"
elif command -v python >/dev/null 2>&1; then
    PYTHON=python
else
    PYTHON=python3
fi
ARCH="${ARCH:-$(uname -m)}"
OUT="$ROOT/out/native-macos"
BUILD="$OUT/build"
RAW_WHEEL="$OUT/raw-wheel"
WHEEL_DIR="$OUT/wheel"
WHEEL_EXTRACT="$OUT/wheel-extract"
TOOLS_VENV="$OUT/build-tools"
FAKE_PLUM=0

for argument in "$@"; do
    case "$argument" in
        --fake-plum) FAKE_PLUM=1 ;;
        --clean) : ;; # Kept for compatibility; every macOS build is clean.
        *) echo "Unknown argument: $argument" >&2; exit 2 ;;
    esac
done

for command in cmake git otool pkg-config "$PYTHON"; do
    command -v "$command" >/dev/null 2>&1 || { echo "Missing required command: $command" >&2; exit 1; }
done

GNUTLS_ROOT="${FFL_P2P_GNUTLS_ROOT:-}"
if [[ -z "$GNUTLS_ROOT" ]] && command -v brew >/dev/null 2>&1; then
    GNUTLS_ROOT="$(brew --prefix gnutls 2>/dev/null || true)"
fi

for pkgConfigDirectory in "$GNUTLS_ROOT/lib/pkgconfig" "$GNUTLS_ROOT/share/pkgconfig"; do
    if [[ -f "$pkgConfigDirectory/gnutls.pc" ]]; then
        export PKG_CONFIG_PATH="$pkgConfigDirectory${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
        echo "Using GnuTLS pkg-config metadata: $pkgConfigDirectory/gnutls.pc"
        break
    fi
done

if ! pkg-config --atleast-version=3.7.5 gnutls; then
    cat >&2 <<'EOF'
ffl-p2p requires GnuTLS 3.7.5 or newer for QUIC support.

Install the macOS prerequisites and rerun the build:

  brew install cmake git pkg-config gnutls python

Set FFL_P2P_GNUTLS_ROOT=/path/to/prefix when GnuTLS is installed outside
Homebrew's default prefix.
EOF
    exit 1
fi

# Native extensions and wheels must always be rebuilt together. In particular,
# never package an extension left by a different Python ABI or CMake cache.
rm -rf "$OUT" "$ROOT/build" "$ROOT/src/ffl_p2p.egg-info"

BUILD_PYTHON="$PYTHON"
if ! "$PYTHON" -c 'import build, delocate, setuptools' >/dev/null 2>&1; then
    "$PYTHON" -m venv "$TOOLS_VENV" || {
        echo "Unable to create a build-tools virtual environment with $PYTHON." >&2
        exit 1
    }

    BUILD_PYTHON="$TOOLS_VENV/bin/python"
    "$BUILD_PYTHON" -m pip install --disable-pip-version-check \
        'setuptools>=68' build delocate
fi

cmake_args=(
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_OSX_ARCHITECTURES="$ARCH"
    -DPython3_EXECUTABLE="$PYTHON"
)
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

find "$ROOT/src/ffl_p2p" -maxdepth 1 -type f -name '_ffl_p2p*.so' -delete
cp "$extension" "$ROOT/src/ffl_p2p/$(basename "$extension")"

dependencies="$(otool -L "$extension")"
printf '%s\n' "$dependencies"
if grep -Eiq '(libjuice|libplum)\.(dylib|so)' <<<"$dependencies"; then
    echo "The extension has dynamically linked third-party dependencies." >&2
    exit 1
fi

mkdir -p "$RAW_WHEEL" "$WHEEL_DIR"
"$BUILD_PYTHON" -m build --wheel --no-isolation --outdir "$RAW_WHEEL" "$ROOT"

rawWheels=("$RAW_WHEEL"/*.whl)
[[ -f "${rawWheels[0]}" && ${#rawWheels[@]} -eq 1 ]] || {
    echo "Expected one raw wheel." >&2
    exit 1
}
[[ "${rawWheels[0]}" != *-none-any.whl ]] || {
    echo "The raw wheel is incorrectly tagged as pure Python. Ensure setup.py is present." >&2
    exit 1
}

"$BUILD_PYTHON" -m delocate.cmd.delocate_wheel -w "$WHEEL_DIR" "${rawWheels[0]}"

wheels=("$WHEEL_DIR"/*.whl)
[[ -f "${wheels[0]}" && ${#wheels[@]} -eq 1 ]] || {
    echo "Expected one repaired wheel." >&2
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

libraries = list(destination.glob('ffl_p2p/.dylibs/libgnutls*.dylib'))
if len(libraries) != 1:
    raise SystemExit(f'Expected one bundled GnuTLS library, found {len(libraries)}')

print(extensions[0])
PY

wheelExtension="$(find "$WHEEL_EXTRACT/ffl_p2p" -maxdepth 1 -type f -name '_ffl_p2p*.so' -print -quit)"
wheelDependencies="$(otool -L "$wheelExtension")"
printf '%s\n' "$wheelDependencies"
if grep -Eq '/(opt/homebrew|usr/local)/(Cellar|opt)/' <<<"$wheelDependencies"; then
    echo "The repaired wheel still references a Homebrew library path." >&2
    exit 1
fi

echo "[PASS] Native macOS wheel build completed: ${wheels[0]}"

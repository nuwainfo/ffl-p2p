param(
    [switch]$Clean,
    [switch]$FakePlum,
    [switch]$Diagnostics,
    [string]$VcpkgRoot
)

$ErrorActionPreference = "Stop"

# VsDevCmd.bat can define VCPKG_ROOT to Visual Studio's bundled, manifest-only
# vcpkg distribution. Reuse an environment root only when it already contains
# this project's static GnuTLS installation.
$explicitVcpkgRoot = $VcpkgRoot
if (-not $explicitVcpkgRoot -and $env:VCPKG_ROOT) {
    $environmentGnuTLS = Join-Path $env:VCPKG_ROOT "installed\x64-windows-static-md\lib\gnutls.lib"
    if (Test-Path $environmentGnuTLS) {
        $explicitVcpkgRoot = $env:VCPKG_ROOT
    }
}

function Import-VSDeveloperEnvironment {
    if (
        $env:VSCMD_VER -and
        $env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and
        $env:VSCMD_ARG_HOST_ARCH -eq 'x64' -and
        (Get-Command cl.exe -ErrorAction SilentlyContinue)
    ) {
        Write-Host "Using existing Visual Studio x64 developer environment"
        return
    }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "Visual Studio Installer was not found: $vswhere"
    }

    $installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installationPath) {
        throw "Visual Studio with C++ build tools was not found"
    }

    $vsDevCmd = Join-Path $installationPath "Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path $vsDevCmd)) {
        throw "VsDevCmd.bat was not found: $vsDevCmd"
    }

    cmd.exe /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && set" |
        ForEach-Object {
            $name, $value = $_ -split '=', 2
            if ($name -and $null -ne $value) {
                Set-Item -Path "Env:$name" -Value $value
            }
        }
}

function Assert-StaticThirdPartyLinkage([string]$extensionPath) {
    $dependencies = & dumpbin.exe /DEPENDENTS $extensionPath
    $dependencies
    $unexpectedDependencies = @('juice.dll', 'libjuice.dll', 'plum.dll', 'libplum.dll') | Where-Object {
        $dependencies -match [regex]::Escape($_)
    }
    if ($unexpectedDependencies) {
        throw "The extension has dynamically linked third-party dependencies: $($unexpectedDependencies -join ', ')"
    }
}

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$outDirectory = Join-Path $root "out\native-windows"
$buildDirectory = Join-Path $outDirectory "build"

Import-VSDeveloperEnvironment

$VcpkgRoot = $explicitVcpkgRoot

if ($Clean -and (Test-Path $outDirectory)) {
    Remove-Item -LiteralPath $outDirectory -Recurse -Force
}

$bootstrapArguments = @(Join-Path $root "scripts\Bootstrap.py")
$cmakeArguments = @("-S", $root, "-B", $buildDirectory)

# Visual Studio's bundled VCPKG_ROOT is deliberately ignored above because it
# is manifest-only and cannot contain this project's GnuTLS dependency.
if (-not $VcpkgRoot) {
    Write-Host "=== Provisioning pinned Windows vcpkg GnuTLS toolchain ==="
    & python (Join-Path $root "scripts\Bootstrap.py") --windows-vcpkg
    if ($LASTEXITCODE -ne 0) {
        throw "Windows vcpkg GnuTLS provisioning failed"
    }
    $VcpkgRoot = Join-Path $root "thirdparty\vcpkg"
}

# ngtcp2 is built from the source fetched by Bootstrap.py. GnuTLS is supplied
# through a static-md vcpkg installation, either provisioned above or selected
# explicitly by the caller.
if ($VcpkgRoot) {
    $vcpkgMarker = Join-Path $VcpkgRoot ".vcpkg-root"
    if (-not (Test-Path $vcpkgMarker)) {
        New-Item -ItemType File -Path $vcpkgMarker -Force | Out-Null
    }
    $vcpkgToolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    if (-not (Test-Path $vcpkgToolchain)) {
        throw "vcpkg toolchain was not found: $vcpkgToolchain"
    }
    $cmakeArguments += "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain"
    $cmakeArguments += "-DVCPKG_TARGET_TRIPLET=x64-windows-static-md"
    # The extension statically links GnuTLS and its closure. Avoid vcpkg's
    # app-local post-build command, which requires vcpkg.exe even when the
    # supplied toolchain is a portable installed-package tree.
    $cmakeArguments += "-DVCPKG_APPLOCAL_DEPS=OFF"

    $vcpkgInstalled = Join-Path $VcpkgRoot "installed\x64-windows-static-md"
    $gnutlsLibrary = Join-Path $vcpkgInstalled "lib\gnutls.lib"
    if (-not (Test-Path $gnutlsLibrary)) {
        throw "Static GnuTLS library was not found: $gnutlsLibrary"
    }
    $pkgconf = Get-ChildItem -Path (Join-Path $VcpkgRoot "installed") -Filter "pkgconf.exe" -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $pkgconf) {
        throw "pkgconf.exe was not found under $VcpkgRoot\installed"
    }
    $pkgconfigDirectories = @(
        (Join-Path $vcpkgInstalled "lib\pkgconfig"),
        (Join-Path $vcpkgInstalled "share\pkgconfig")
    ) | Where-Object { Test-Path $_ }
    if (-not $pkgconfigDirectories) {
        throw "GnuTLS pkg-config metadata was not found under $vcpkgInstalled"
    }
    $env:PKG_CONFIG_PATH = $pkgconfigDirectories -join ';'
    $cmakeArguments += "-DPKG_CONFIG_EXECUTABLE=$pkgconf"
}
if ($FakePlum) {
    $bootstrapArguments += "--fake-plum"
    $cmakeArguments += "-DFFL_P2P_FAKE_PLUM=ON"
}
if ($Diagnostics) {
    $cmakeArguments += "-DFFL_P2P_ENABLE_DIAGNOSTICS=ON"
} else {
    $cmakeArguments += "-DFFL_P2P_ENABLE_DIAGNOSTICS=OFF"
}

Write-Host "=== 1/4 Bootstrap pinned native dependencies ==="
& python @bootstrapArguments
if ($LASTEXITCODE -ne 0) {
    throw "Native dependency bootstrap failed"
}

Write-Host "=== 2/4 Configure and build native extension ==="
& cmake @cmakeArguments
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed"
}
& cmake --build $buildDirectory --config Release --target _ffl_p2p --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Native build failed"
}

$extension = Get-ChildItem -LiteralPath $buildDirectory -Recurse -Filter '_ffl_p2p*.pyd' |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $extension) {
    throw "Built _ffl_p2p extension was not found"
}
$target = Join-Path $root "src\ffl_p2p\$($extension.Name)"
Copy-Item -LiteralPath $extension.FullName -Destination $target -Force

Write-Host "=== 3/4 Verify static third-party linkage ==="
Assert-StaticThirdPartyLinkage $extension.FullName
Write-Host "Native extension ready: $target"

Write-Host "=== 4/4 Build wheel ==="
$wheelDirectory = Join-Path $outDirectory "wheel"
$wheelExtractDirectory = Join-Path $outDirectory "wheel-extract"
New-Item -ItemType Directory -Force -Path $wheelDirectory | Out-Null

& python -c "import build"
if ($LASTEXITCODE -ne 0) {
    & python -m pip install --disable-pip-version-check build
    if ($LASTEXITCODE -ne 0) {
        throw "Wheel build requirement installation failed"
    }
}

# setup.py packages whatever extension is already sitting in src\ffl_p2p
# (just copied above), matching the same python -m build step already used
# in scripts/BuildLinux.sh and scripts/BuildMacOS.sh -- it does not invoke
# CMake itself.
& python -m build --wheel --no-isolation --outdir $wheelDirectory $root
if ($LASTEXITCODE -ne 0) {
    throw "Wheel build failed"
}

$wheel = Get-ChildItem -LiteralPath $wheelDirectory -Filter 'ffl_p2p-*.whl' |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $wheel) {
    throw "Wheel build completed without producing an ffl_p2p wheel"
}
if ($wheel.Name -like '*-none-any.whl') {
    throw "The wheel is incorrectly tagged as pure Python; the native extension was not packaged"
}

if (Test-Path $wheelExtractDirectory) {
    Remove-Item -LiteralPath $wheelExtractDirectory -Recurse -Force
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($wheel.FullName, $wheelExtractDirectory)
$wheelExtension = Get-ChildItem -LiteralPath $wheelExtractDirectory -Recurse -Filter '_ffl_p2p*.pyd' |
    Select-Object -First 1
if (-not $wheelExtension) {
    throw "Built wheel does not contain the _ffl_p2p extension"
}
Assert-StaticThirdPartyLinkage $wheelExtension.FullName

Write-Host "[PASS] Native Windows wheel build completed: $($wheel.FullName)"

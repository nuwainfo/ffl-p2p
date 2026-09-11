param(
    [switch]$Clean,
    [switch]$FakePlum,
    [switch]$Diagnostics,
    [string]$VcpkgRoot
)

$ErrorActionPreference = "Stop"

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

if ($Clean -and (Test-Path $outDirectory)) {
    Remove-Item -LiteralPath $outDirectory -Recurse -Force
}

$bootstrapArguments = @(Join-Path $root "scripts\Bootstrap.py")
$cmakeArguments = @("-S", $root, "-B", $buildDirectory)

# ngtcp2 is built from the source fetched by Bootstrap.py.  GnuTLS itself is
# expected from the native toolchain.  When a vcpkg root is supplied (or
# VCPKG_ROOT is set), use its toolchain so the static-md GnuTLS setup from the
# supplied smoke test can be reused without vendoring GnuTLS into ffl-p2p.
if (-not $VcpkgRoot -and $env:VCPKG_ROOT) {
    $VcpkgRoot = $env:VCPKG_ROOT
}
if ($VcpkgRoot) {
    $vcpkgToolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    if (-not (Test-Path $vcpkgToolchain)) {
        throw "vcpkg toolchain was not found: $vcpkgToolchain"
    }
    $cmakeArguments += "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain"
    $cmakeArguments += "-DVCPKG_TARGET_TRIPLET=x64-windows-static-md"

    $vcpkgInstalled = Join-Path $VcpkgRoot "installed\x64-windows-static-md"
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

Write-Host "=== 1/3 Bootstrap pinned native dependencies ==="
& python @bootstrapArguments
if ($LASTEXITCODE -ne 0) {
    throw "Native dependency bootstrap failed"
}

Write-Host "=== 2/3 Configure and build native extension ==="
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

Write-Host "=== 3/3 Verify static third-party linkage ==="
Assert-StaticThirdPartyLinkage $extension.FullName
Write-Host "[PASS] Native Windows build completed: $target"

# ffl-p2p

Native peer-to-peer transports for FastFileLink.

`ffl-p2p` establishes direct paths between a publisher and downloader while
leaving FastFileLink responsible for share URLs, authentication, encryption,
resume, checksums, progress, and HTTP fallback. It provides:

- direct TCP downloads using the publisher's existing HTTP server;
- reliable direct UDP transfer over QUIC;
- ICE/STUN candidate gathering through libjuice;
- optional PCP, NAT-PMP, and UPnP port mapping through libplum.

The share URL remains the signaling and authorization channel. Bulk data uses a
direct TCP or UDP path when one can be established.

## Prerequisites

Install Python 3.10 or newer, Git, CMake, a C/C++ compiler, `pkg-config`, and
the GnuTLS development package. QUIC is a required transport in this package,
so GnuTLS 3.7.5 or newer is required to build `ffl-p2p`; activating a Python or
Conda environment alone does not provide its native headers and libraries.

### Ubuntu or Debian

```bash
sudo apt update
sudo apt install -y build-essential cmake git pkg-config libgnutls28-dev python3-dev
```

### RHEL, Rocky Linux, Fedora, or compatible systems

```bash
sudo dnf install -y gcc gcc-c++ cmake git pkgconf-pkg-config gnutls-devel python3-devel
```

Run the command without `sudo` when building as `root` in a container.

### macOS

```bash
brew install cmake git pkg-config gnutls python
```

`BuildMacOS.sh` discovers Homebrew's GnuTLS prefix automatically. If GnuTLS is
installed elsewhere, set `FFL_P2P_GNUTLS_ROOT` to that prefix before building.
When needed, the script creates an isolated temporary venv for its `build` and
`delocate` tooling; it does not modify the Homebrew Python environment.

Install Xcode Command Line Tools if needed:

```bash
xcode-select --install
```

### Windows

Install Visual Studio 2022 with the **Desktop development with C++** workload,
Python 3.10 or newer, Git, and CMake. No manual vcpkg download or copied
dependency directory is required: the Windows build fetches its vcpkg checkout
into the ignored `thirdparty/vcpkg/` directory and builds the required static
GnuTLS closure on its first run. It needs network access, Visual Studio's C++
tools, and substantial disk space; later builds reuse that installation.

This project includes `vcpkg-overlays/shiftmedia-libgnutls`, the historical
MSVC GnuTLS port verified by the native smoke build. The overlay is deliberately
paired with vcpkg commit `5812244ec0caf8f5ab9f71cac42d98aea6cc53b8`, whose
Nettle 3.10 port the GnuTLS project requires. Current upstream vcpkg no longer
supports its maintained `libgnutls` port on MSVC and has delisted this older
port, so using an arbitrary current vcpkg checkout will not reproduce this
build. Restoring a supported upstream MSVC GnuTLS port remains the long-term
solution; the overlay plus pinned baseline is the reproducible interim one.

Build scripts fetch pinned libjuice and ngtcp2 sources plus the configured
libplum ref automatically. libplum defaults to upstream `master` for development;
release builders should set `FFL_P2P_LIBPLUM_REF` to a tested commit. No separate
third-party fetch step is required.

## Build and test

Run commands from the repository root.

### Windows

Build normally; on a fresh clone the first command provisions the pinned vcpkg
toolchain automatically:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\BuildNative.ps1 -Clean
python scripts\Test.py
```

To reuse a compatible toolchain outside the checkout, pass it explicitly
instead:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\BuildNative.ps1 -Clean `
  -VcpkgRoot C:\path\to\prepared-vcpkg
```

### Linux

```bash
./scripts/BuildLinux.sh --clean
python scripts/Test.py
```

### macOS

```bash
./scripts/BuildMacOS.sh --clean
python scripts/Test.py
```

The Linux and macOS scripts produce wheels under `out/native-*/wheel/`. The
macOS wheel bundles its GnuTLS libraries, so users installing that wheel do not
need Homebrew or a separate GnuTLS installation. Each build checks that
libjuice and libplum are statically linked. For deterministic tests without a
production libplum, pass `-FakePlum` on Windows or `--fake-plum` on Linux/macOS.

## Integrating with FastFileLink

Create one publisher beside the existing FastFileLink HTTP server, send its
offer through your authenticated signaling endpoint, then connect the downloader
with the same base URL:

```python
from ffl_p2p import ICEConfiguration, ICEServer, P2PConnector, P2PPublisher

ice = ICEConfiguration(
    iceServers=[ICEServer(urls='stun:stun.example.net:3478')]
)

# Publisher: expose the offer through the application's /p2p/offer endpoint.
publisher = P2PPublisher(httpPort=httpServer.port, configuration=ice)
offer = publisher.createOffer()

# Downloader: HTTPSignalingClient obtains the offer and posts the answer.
connection = P2PConnector(configuration=ice).connect(shareURL, preference='auto')
if connection is None:
    # Let the application continue with its normal fallback policy.
    raise RuntimeError('No direct path is available')
```

`preference='auto'` tries direct TCP first and then UDP/QUIC. Use `'tcp'` or
`'udp'` to require one transport. `P2PDownloadMixin` is available for a
Downloader integration that keeps HTTP fallback in the application layer.

Use `QUICFileServer` and `QUICFileClient` when the application owns file-stream
transfer directly. `QUICStream` is available for stream-oriented integrations.

## ICE configuration

Pass an `ICEConfiguration` to both peers. `ffl-p2p` accepts `stun:` URLs; TURN
is intentionally not implemented. The configuration is list-shaped for
compatibility with common ICE configuration formats, but the native ICE engine
uses the first STUN URL.

```python
ICEConfiguration(
    iceServers=[ICEServer(urls=[
        'stun:stun.example.net:3478',
        'stun:backup.example.net:3478',
    ])],
    connectivityTimeout=3.0,
)
```

## APE deployment

`ffl-p2p` can be bundled with a Cosmopolitan APE-based FastFileLink deployment.
Build the extension for the target package, place it beside the Python package
as `ffl_p2p/_ffl_p2p`, and deploy the matching Python package and extension to
both peers.

## Runtime environment

Runtime tuning and diagnostics can be selected before starting the process:

```text
FFL_P2P_NATIVE_LOGGING_LEVEL=ERROR
FFL_P2P_QUIC_DISABLE_BATCH=1
FFL_P2P_QUIC_WRITE_BUFFER_MIB=8
FFL_P2P_QUIC_WORKERS=2
FFL_P2P_QUIC_DISABLE_GSO=1
FFL_P2P_QUIC_DISABLE_RECV_BATCH=1
FFL_P2P_QUIC_DISABLE_RECV_COALESCING=1
```

`FFL_P2P_NATIVE_LOGGING_LEVEL` accepts `DEBUG`, `INFO`, `WARNING`, `ERROR`,
`CRITICAL`, or `NONE`. `FFL_P2P_QUIC_WRITE_BUFFER_MIB` must be a positive
integer. `FFL_P2P_QUIC_WORKERS` accepts `1` through `16`; the default is `2`.
The disable switches accept `1`, `true`, `yes`, or `on`.

## Performance

On the Mars-to-Eris 1 Gbit/s UDP test path, the direct native QUIC control
(generated memory to null sink) sustained approximately **110-120 MiB/s**.
The same path measured about **994 Mbit/s** with `iperf3 -u`; the QUIC result is
therefore close to the available network capacity.

These transport-control measurements deliberately exclude disk I/O, checksum,
progress reporting, and application processing. End-to-end FastFileLink speed
depends on those costs, the selected direct path, NAT behavior, and the host
network. Reproduce local comparisons with:

```bash
python scripts/PerformanceTest.py --size-mib 64 --iterations 3
```

The native QUIC runtime uses a small Worker pool. The default is two Workers,
which provides protocol-processing parallelism for multiple receivers without
scaling thread and receive-pool memory to the host CPU count. Connections are
assigned to Workers round-robin and remain owned by that Worker for their entire
lifetime.

Set `FFL_P2P_QUIC_WORKERS` before the first QUIC connection to override the pool
size for profiling or debugging. Valid values are `1` through `16`; invalid
values fail fast instead of silently falling back. The value is read once per
process when the runtime Worker pool is initialized, so changing the environment
later does not resize a live pool. `1` preserves the previous serialized protocol
execution model, while `2` and `4` are useful A/B values for small multi-receiver
workloads.

For an APE no-disk transfer, stream generated input with stdin caching disabled
and receive to `/dev/null`:

```bash
dd if=/dev/zero bs=1M count=4096 status=progress | \
  ./ffl.com - --stdin-cache off --name perf.bin --max-downloads 1
```

## Testing

```bash
python scripts/Test.py
python -m compileall -q src tests scripts
```

The suite covers ICE signaling, direct TCP and UDP paths, QUIC transfer and
resume behavior, port mapping, and supported compatibility paths.

## License

Apache-2.0. See [LICENSE](LICENSE).

## Version updates

Use `python scripts/UpdateVersion.py X.Y.Z` to update the project version in
`pyproject.toml`, the Python package, and the native build marker. Native QUIC
control scripts read the current project version automatically.

## Native logging

`FFL_P2P_NATIVE_LOGGING_LEVEL` sets the initial process-wide native logging
level. Applications can change it at runtime with
`ffl_p2p.setNativeLoggingLevel(...)`. Native libjuice/libplum diagnostics are
written to stderr so stdout remains safe for application payloads.

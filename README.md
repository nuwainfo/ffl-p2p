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

## Requirements

- Python 3.10 or newer
- Git and CMake
- A C/C++ compiler: MSVC on Windows, or Clang/GCC on Linux and macOS
- GnuTLS development headers and libraries for QUIC support

Build scripts fetch pinned libjuice, libplum, and ngtcp2 sources automatically.
No separate third-party fetch step is required.

## Build and test

Run commands from the repository root.

### Windows

Install GnuTLS through vcpkg, then build with its root when it is not already
discoverable by CMake:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\BuildNative.ps1 -Clean `
  -VcpkgRoot C:\path\to\vcpkg
python scripts\Test.py
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

The built `_ffl_p2p` extension is copied into `src/ffl_p2p/`. Each build also
checks that libjuice and libplum are statically linked. For deterministic tests
without a production libplum, pass `-FakePlum` on Windows or `--fake-plum` on
Linux/macOS.

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

Linux compatibility switches are available when a network or platform requires
a conservative UDP path:

```text
FFL_P2P_QUIC_DISABLE_GSO=1
FFL_P2P_QUIC_DISABLE_RECV_BATCH=1
FFL_P2P_QUIC_DISABLE_RECV_COALESCING=1
```

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

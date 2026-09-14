# thirdparty

This directory is populated automatically by the platform build scripts through
`scripts/Bootstrap.py`. Dependency source trees are not part of the ffl-p2p
distribution/overlay.

Dependencies are real Git checkouts so patches are applied inside the dependency
repository rather than the enclosing FFL tree.

- `libjuice/`: pinned `v1.7.2`, then the incremental FFL patch series
  `0001-FFLNative.patch` through `0006-DatapathSendHook.patch`.
- `libplum/`: upstream `master` by default, plus
  `0001-ExposeLogLevelSetter.patch` so ffl-p2p can update the native log level
  after libplum initialization. Release builders should set
  `FFL_P2P_LIBPLUM_REF` to a tested commit so a release is reproducible.
- `ngtcp2/`: pinned `v1.25.0`; fetched exactly like libjuice/libplum. It is not
  pre-vendored in this project.

GnuTLS is a toolchain/system dependency rather than a vendored source tree. On
Windows, `scripts/BuildNative.ps1 -VcpkgRoot <path>` can reuse the static-md
vcpkg/GnuTLS environment from the ngtcp2+GnuTLS smoke project.

Fetched dependency checkouts and native build output are intentionally ignored.

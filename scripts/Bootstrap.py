#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess


class DependencyPatch:
    def __init__(self, repository: Path, patchPath: Path, expectedSHA256: str):
        self.repository = repository
        self.patchPath = patchPath
        self.expectedSHA256 = expectedSHA256

    def apply(self):
        self._verifyChecksum()
        self.applyVerified()

    def applyVerified(self):
        if self._check(reverse=True):
            print(f'Patch already applied: {self.patchPath}')
            return
        if not self._check():
            subprocess.run(
                [
                    'git', '-C', str(self.repository), 'apply', '--ignore-space-change',
                    '--check', str(self.patchPath)
                ],
                check=False,
            )
            raise RuntimeError(f'Dependency patch does not apply cleanly: {self.patchPath}')
        subprocess.run([
            'git', '-C', str(self.repository), 'apply', '--ignore-space-change', str(self.patchPath)
        ], check=True)
        print(f'Patch applied: {self.patchPath}')

    def _verifyChecksum(self):
        digest = hashlib.sha256(self.patchPath.read_bytes()).hexdigest()
        if digest != self.expectedSHA256:
            raise RuntimeError(
                f'Patch SHA-256 mismatch for {self.patchPath}: '
                f'expected {self.expectedSHA256}, got {digest}'
            )

    def _check(self, reverse: bool = False) -> bool:
        command = ['git', '-C', str(self.repository), 'apply', '--ignore-space-change']
        if reverse:
            command.append('--reverse')
        command.extend(['--check', str(self.patchPath)])
        result = subprocess.run(
            command,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        return result.returncode == 0


class GitDependency:
    def __init__(self, url: str, destination: Path, ref: str):
        self.url = url
        self.destination = destination
        self.ref = ref

    def fetch(self, force: bool = False):
        if force:
            shutil.rmtree(self.destination, ignore_errors=True)
        if self.destination.exists():
            if not (self.destination / '.git').exists():
                raise RuntimeError(f'Dependency directory exists but is not a Git checkout: {self.destination}')
            print(f'Using existing dependency checkout: {self.destination}')
            return

        self.destination.parent.mkdir(parents=True, exist_ok=True)
        if self._isBranchOrTagRef():
            subprocess.run(
                ['git', 'clone', '--depth', '1', '--branch', self.ref, self.url, str(self.destination)],
                check=True,
            )
            return

        subprocess.run(
            ['git', 'clone', '--filter=blob:none', '--no-checkout', self.url, str(self.destination)],
            check=True,
        )
        subprocess.run(
            ['git', '-C', str(self.destination), 'fetch', '--depth', '1', 'origin', self.ref],
            check=True,
        )
        subprocess.run(
            ['git', '-C', str(self.destination), 'checkout', '--detach', 'FETCH_HEAD'],
            check=True,
        )

    def _isBranchOrTagRef(self) -> bool:
        if self.ref in {'main', 'master'}:
            return True
        return self.ref.startswith('v') or self.ref.startswith('release-')


class Bootstrapper:
    LIBJUICE_REF = 'v1.7.2'
    LIBJUICE_URL = 'https://github.com/paullouisageneau/libjuice.git'
    LIBPLUM_DEFAULT_REF = 'master'
    LIBPLUM_URL = 'https://github.com/paullouisageneau/libplum.git'
    NGTCP2_REF = 'v1.25.0'
    NGTCP2_URL = 'https://github.com/ngtcp2/ngtcp2.git'
    LIBJUICE_PATCH_SHA256 = '953669385890bd8ae10170ceaedbae3a4af312d415a8c276332e6aeccbf4d556'
    LIBJUICE_UDP_FAMILY_CACHE_PATCH_SHA256 = '33bbc912db3d0eae9a6859ef6f797bc275a7f997f89240637c0cb1e2fe029ae6'
    LIBJUICE_UDP_BATCH_PATCH_SHA256 = 'b4a6b5d940114085e16c06d120940401b67d9f8512b581e7bd0581721f827fdb'
    LIBJUICE_UDP_RECV_BATCH_HOOK_PATCH_SHA256 = '5586683137be8a1d0e9d433536d716dea5a89ed8342610a4ec63d30f865084de'
    LIBJUICE_AGGREGATE_BACKPRESSURE_PATCH_SHA256 = 'bc9af7f51fc8c782e9a09112f49ffc3874c6a1cfcf376414187383465eaaa9f1'
    LIBJUICE_DATAPATH_SEND_HOOK_PATCH_SHA256 = '2d6c4b1e691be6dad315261b6578976d208dc13a4e8898ac866aa0ab1f9426d2'

    def __init__(self, root: Path):
        self.root = root
        self.thirdparty = root / 'thirdparty'
        self.libjuice = GitDependency(
            os.environ.get('FFL_P2P_LIBJUICE_URL', self.LIBJUICE_URL),
            self.thirdparty / 'libjuice',
            os.environ.get('FFL_P2P_LIBJUICE_REF', self.LIBJUICE_REF),
        )
        self.libplum = GitDependency(
            os.environ.get('FFL_P2P_LIBPLUM_URL', self.LIBPLUM_URL),
            self.thirdparty / 'libplum',
            os.environ.get('FFL_P2P_LIBPLUM_REF', self.LIBPLUM_DEFAULT_REF),
        )
        self.ngtcp2 = GitDependency(
            os.environ.get('FFL_P2P_NGTCP2_URL', self.NGTCP2_URL),
            self.thirdparty / 'ngtcp2',
            os.environ.get('FFL_P2P_NGTCP2_REF', self.NGTCP2_REF),
        )
        self.libjuicePatches = [
            DependencyPatch(
                self.thirdparty / 'libjuice',
                root / 'patches' / 'libjuice' / '0001-FFLNative.patch',
                self.LIBJUICE_PATCH_SHA256,
            ),
            DependencyPatch(
                self.thirdparty / 'libjuice',
                root / 'patches' / 'libjuice' / '0002-CacheUDPSocketFamily.patch',
                self.LIBJUICE_UDP_FAMILY_CACHE_PATCH_SHA256,
            ),
            DependencyPatch(
                self.thirdparty / 'libjuice',
                root / 'patches' / 'libjuice' / '0003-UDPBatchSend.patch',
                self.LIBJUICE_UDP_BATCH_PATCH_SHA256,
            ),
            DependencyPatch(
                self.thirdparty / 'libjuice',
                root / 'patches' / 'libjuice' / '0004-UDPReceiveBatchHook.patch',
                self.LIBJUICE_UDP_RECV_BATCH_HOOK_PATCH_SHA256,
            ),
            DependencyPatch(
                self.thirdparty / 'libjuice',
                root / 'patches' / 'libjuice' / '0005-AggregateSendBackpressure.patch',
                self.LIBJUICE_AGGREGATE_BACKPRESSURE_PATCH_SHA256,
            ),
            DependencyPatch(
                self.thirdparty / 'libjuice',
                root / 'patches' / 'libjuice' / '0006-DatapathSendHook.patch',
                self.LIBJUICE_DATAPATH_SEND_HOOK_PATCH_SHA256,
            ),
        ]

    def run(self, force: bool = False, fakePlum: bool = False):
        self.thirdparty.mkdir(exist_ok=True)
        self.libjuice.fetch(force=force)
        self._applyPatchSeries(self.libjuicePatches)
        if not fakePlum:
            self.libplum.fetch(force=force)
        self.ngtcp2.fetch(force=force)
        print('thirdparty ready')

    @staticmethod
    def _applyPatchSeries(patches: list[DependencyPatch]):
        """Apply an ordered patch series without undoing later-patch context.

        A later patch may modify lines introduced by an earlier patch.  In that
        valid state, ``git apply --reverse --check`` for the earlier patch no
        longer succeeds even though it is already present.  A reversible later
        patch proves the earlier patch is part of the checked-out series, so do
        not try to apply the earlier patch again.
        """
        for patch in patches:
            patch._verifyChecksum()

        for index, patch in enumerate(patches):
            if patch._check(reverse=True):
                print(f'Patch already applied: {patch.patchPath}')
                continue

            laterPatchApplied = any(
                laterPatch._check(reverse=True)
                for laterPatch in patches[index + 1:]
            )
            if laterPatchApplied:
                print(f'Patch already applied through later patch: {patch.patchPath}')
                continue

            patch.applyVerified()


def main():
    parser = argparse.ArgumentParser(description='Bootstrap pinned ffl-p2p native dependencies')
    parser.add_argument('--force', action='store_true')
    parser.add_argument('--fake-plum', action='store_true', dest='fakePlum')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    Bootstrapper(root).run(force=args.force, fakePlum=args.fakePlum)


if __name__ == '__main__':
    main()

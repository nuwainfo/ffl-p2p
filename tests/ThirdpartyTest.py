#!/usr/bin/env python
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
#
# FastFileLink CLI - Fast, no-fuss file sharing
# Copyright (C) 2025-2026 FastFileLink contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from pathlib import Path
import hashlib
import subprocess
import tempfile
import unittest

from scripts.Bootstrap import Bootstrapper, DependencyPatch, GitDependency


class ThirdpartyTest(unittest.TestCase):
    def testWindowsGnuTLSOverlayMatchesPinnedVcpkgBaseline(self):
        root = Path(__file__).resolve().parents[1]
        overlay = root / 'vcpkg-overlays' / 'shiftmedia-libgnutls'

        self.assertTrue((overlay / 'portfile.cmake').is_file())
        self.assertTrue((overlay / 'vcpkg.json').is_file())
        self.assertIn('pkgconfig.patch', (overlay / 'portfile.cmake').read_text())
        self.assertIn('"nettle"', (overlay / 'vcpkg.json').read_text())
        self.assertEqual('5812244ec0caf8f5ab9f71cac42d98aea6cc53b8', Bootstrapper.VCPKG_REF)

    def testDependencyPatchStaysInsideDependencyRepositoryAndIsIdempotent(self):
        with tempfile.TemporaryDirectory() as temporaryDirectory:
            root = Path(temporaryDirectory)
            outerRepository = root / 'outer'
            dependencyRepository = outerRepository / 'thirdparty' / 'dependency'
            dependencyRepository.mkdir(parents=True)
            self._initializeRepository(outerRepository)
            self._initializeRepository(dependencyRepository)

            sourceFile = dependencyRepository / 'Source.c'
            sourceFile.write_text('before\n')
            self._commitAll(dependencyRepository, 'dependency baseline')

            sourceFile.write_text('after\n')
            patchPath = root / 'Dependency.patch'
            patchPath.write_bytes(subprocess.check_output(['git', '-C', str(dependencyRepository), 'diff']))
            subprocess.run(['git', '-C', str(dependencyRepository), 'checkout', '--', 'Source.c'], check=True)

            expectedSHA256 = hashlib.sha256(patchPath.read_bytes()).hexdigest()

            outerStatusBefore = subprocess.check_output(
                ['git', '-C', str(outerRepository), 'status', '--short'], text=True
            )

            patch = DependencyPatch(dependencyRepository, patchPath, expectedSHA256)
            patch.apply()
            self.assertEqual('after\n', sourceFile.read_text())

            patch.apply()
            self.assertEqual('after\n', sourceFile.read_text())

            outerStatusAfter = subprocess.check_output(
                ['git', '-C', str(outerRepository), 'status', '--short'], text=True
            )
            self.assertEqual(outerStatusBefore, outerStatusAfter)

    def testExistingDependencyMustBeGitCheckout(self):
        with tempfile.TemporaryDirectory() as temporaryDirectory:
            destination = Path(temporaryDirectory) / 'dependency'
            destination.mkdir()
            dependency = GitDependency('unused', destination, 'v1.0.0')
            with self.assertRaisesRegex(RuntimeError, 'not a Git checkout'):
                dependency.fetch()

    def _initializeRepository(self, repository: Path):
        repository.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', '-C', str(repository), 'init', '-q'], check=True)

        subprocess.run(['git', '-C', str(repository), 'config', 'user.email', 'test@example.com'], check=True)
        subprocess.run(['git', '-C', str(repository), 'config', 'user.name', 'Test'], check=True)

    def _commitAll(self, repository: Path, message: str):
        subprocess.run(['git', '-C', str(repository), 'add', '.'], check=True)
        subprocess.run(['git', '-C', str(repository), 'commit', '-qm', message], check=True)


if __name__ == '__main__':
    unittest.main()

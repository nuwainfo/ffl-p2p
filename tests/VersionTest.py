#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0

from pathlib import Path

import importlib.util
import shutil
import subprocess
import sys
import tempfile
import types
import unittest

from unittest import mock


class VersionTest(unittest.TestCase):
    PROJECT_ROOT = Path(__file__).resolve().parents[1]
    SCRIPTS_ROOT = PROJECT_ROOT / 'scripts'
    UPDATE_VERSION = SCRIPTS_ROOT / 'UpdateVersion.py'
    VERSION_FILES = (
        Path('pyproject.toml'),
        Path('src/ffl_p2p/__init__.py'),
        Path('native/P2P.c'),
    )

    @classmethod
    def _loadModule(cls, path: Path, moduleName: str):
        specification = importlib.util.spec_from_file_location(moduleName, path)
        module = importlib.util.module_from_spec(specification)

        with mock.patch.dict(sys.modules, {moduleName: module}):
            specification.loader.exec_module(module)

        return module

    @classmethod
    def _loadUpdateVersion(cls):
        return cls._loadModule(cls.UPDATE_VERSION, 'UpdateVersionForTest')

    @classmethod
    def _loadControlScript(cls, scriptName: str):
        updateVersion = cls._loadUpdateVersion()
        remoteTestSupport = types.ModuleType('RemoteTestSupport')
        remoteTestSupport.RemoteTestHost = object
        remoteTestSupport.formatEnvironment = None
        remoteTestSupport.paramiko = None
        remoteTestSupport.runAPEPreflight = None

        with mock.patch.dict(
            sys.modules,
            {
                'RemoteTestSupport': remoteTestSupport,
                'UpdateVersion': updateVersion,
            },
        ):
            return cls._loadModule(
                cls.SCRIPTS_ROOT / scriptName,
                f'{Path(scriptName).stem}ForVersionTest',
            )

    @classmethod
    def _copyVersionFiles(cls, root: Path):
        for relativePath in cls.VERSION_FILES:
            destination = root / relativePath
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(cls.PROJECT_ROOT / relativePath, destination)

    @classmethod
    def _readVersionFiles(cls, root: Path):
        return {
            relativePath: (root / relativePath).read_bytes()
            for relativePath in cls.VERSION_FILES
        }

    def testUpdateVersionSynchronizesAllVersionMarkers(self):
        updateVersion = self._loadUpdateVersion()

        with tempfile.TemporaryDirectory() as temporaryDirectory:
            root = Path(temporaryDirectory)
            self._copyVersionFiles(root)

            updateVersion.updateProjectVersion('9.8.7', root)

            self.assertEqual('9.8.7', updateVersion.getProjectVersion(root))
            self.assertIn(
                '__version__ = "9.8.7"',
                (root / 'src/ffl_p2p/__init__.py').read_text(encoding='utf-8'),
            )
            self.assertIn(
                '"9.8.7");',
                (root / 'native/P2P.c').read_text(encoding='utf-8'),
            )

    def testUpdateVersionRejectsInconsistentSourceWithoutPartialChanges(self):
        updateVersion = self._loadUpdateVersion()

        with tempfile.TemporaryDirectory() as temporaryDirectory:
            root = Path(temporaryDirectory)
            self._copyVersionFiles(root)

            initPath = root / 'src' / 'ffl_p2p' / '__init__.py'
            currentVersion = updateVersion.getProjectVersion(root)
            initContent = initPath.read_text(encoding='utf-8')
            initPath.write_text(
                initContent.replace(
                    f'__version__ = "{currentVersion}"',
                    '__version__ = "0.0.0-inconsistent"',
                ),
                encoding='utf-8',
            )
            expectedFiles = self._readVersionFiles(root)

            with self.assertRaises(RuntimeError):
                updateVersion.updateProjectVersion('9.8.7', root)

            self.assertEqual(expectedFiles, self._readVersionFiles(root))

    def testUpdateVersionRollsBackWhenWriteFails(self):
        updateVersion = self._loadUpdateVersion()

        with tempfile.TemporaryDirectory() as temporaryDirectory:
            root = Path(temporaryDirectory)
            self._copyVersionFiles(root)
            expectedFiles = self._readVersionFiles(root)
            realWriteBytes = Path.write_bytes
            failed = False

            def writeBytes(path, data):
                nonlocal failed

                isFailingWrite = (
                    path.name == '__init__.py'
                    and b'9.8.7' in data
                    and not failed
                )
                if isFailingWrite:
                    failed = True
                    raise OSError('forced version write failure')

                return realWriteBytes(path, data)

            with mock.patch.object(Path, 'write_bytes', new=writeBytes):
                with self.assertRaises(OSError):
                    updateVersion.updateProjectVersion('9.8.7', root)

            self.assertTrue(failed)
            self.assertEqual(expectedFiles, self._readVersionFiles(root))

    def testUpdateVersionRejectsInvalidVersionWithoutChanges(self):
        updateVersion = self._loadUpdateVersion()

        with tempfile.TemporaryDirectory() as temporaryDirectory:
            root = Path(temporaryDirectory)
            self._copyVersionFiles(root)
            expectedFiles = self._readVersionFiles(root)

            with self.assertRaises(ValueError):
                updateVersion.updateProjectVersion('not-a-version', root)

            self.assertEqual(expectedFiles, self._readVersionFiles(root))

    def testUpdateVersionCLI(self):
        with tempfile.TemporaryDirectory() as temporaryDirectory:
            root = Path(temporaryDirectory)
            self._copyVersionFiles(root)
            scripts = root / 'scripts'
            scripts.mkdir()
            shutil.copy2(self.UPDATE_VERSION, scripts / 'UpdateVersion.py')

            subprocess.run(
                [sys.executable, str(scripts / 'UpdateVersion.py'), '9.8.7'],
                cwd=root,
                check=True,
                stdout=subprocess.DEVNULL,
            )

            self.assertIn(
                'version = "9.8.7"',
                (root / 'pyproject.toml').read_text(encoding='utf-8'),
            )

    def testControlScriptsReadBothDefaultVersionsDynamically(self):
        for scriptName in (
            'RunNativeQUICControl.py',
            'RunAPENativeQUICControl.py',
        ):
            with self.subTest(scriptName=scriptName):
                module = self._loadControlScript(scriptName)

                with mock.patch.object(
                    module,
                    'getProjectVersion',
                    return_value='9.8.7',
                ):
                    args = module.createParser().parse_args([
                        '--run-name',
                        'version-test',
                    ])

                self.assertEqual('9.8.7', args.expectedNativeMarker)
                self.assertEqual('9.8.7', args.expectedPythonVersion)


if __name__ == '__main__':
    unittest.main()

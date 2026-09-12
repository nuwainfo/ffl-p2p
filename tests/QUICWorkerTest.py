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

import os
import subprocess
import sys
import unittest

from pathlib import Path

class QUICWorkerTest(unittest.TestCase):
    root = Path(__file__).resolve().parents[1]
    scenario = root / 'tests' / 'QUICWorkerScenario.py'

    def _runScenario(
        self,
        expectedWorkerCount: int,
        configuredWorkerCount=None,
        transfers: int = 0,
        payloadMiB: int = 2,
    ):
        environment = os.environ.copy()
        environment['PYTHONPATH'] = (
            str(self.root / 'src') + os.pathsep +
            str(self.root) + os.pathsep +
            str(self.root / 'tests')
        )
        environment.pop('FFL_P2P_QUIC_WORKERS', None)
        if configuredWorkerCount is not None:
            environment['FFL_P2P_QUIC_WORKERS'] = str(configuredWorkerCount)

        command = [
            sys.executable,
            str(self.scenario),
            '--expected-workers', str(expectedWorkerCount),
            '--sessions', str(max(expectedWorkerCount * 2, 4)),
        ]
        if transfers:
            command.extend([
                '--transfers', str(transfers),
                '--payload-mib', str(payloadMiB),
            ])

        return subprocess.run(
            command,
            cwd=self.root,
            env=environment,
            capture_output=True,
            text=True,
            timeout=120,
        )

    def testDefaultQUICWorkerCountIsTwo(self):
        result = self._runScenario(expectedWorkerCount=2)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def testConfiguredQUICWorkerCountsUseRoundRobinAssignment(self):
        for workerCount in (1, 2, 4):
            with self.subTest(workerCount=workerCount):
                result = self._runScenario(
                    expectedWorkerCount=workerCount,
                    configuredWorkerCount=workerCount,
                )
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def testInvalidQUICWorkerCountsFailFast(self):
        for workerCount in ('0', '17', 'invalid'):
            with self.subTest(workerCount=workerCount):
                result = self._runScenario(
                    expectedWorkerCount=2,
                    configuredWorkerCount=workerCount,
                )
                self.assertNotEqual(0, result.returncode)
                self.assertIn(
                    'FFL_P2P_QUIC_WORKERS must be an integer between 1 and 16',
                    result.stdout + result.stderr,
                )

    def testFourWorkersHandleConcurrentTransfers(self):
        result = self._runScenario(
            expectedWorkerCount=4,
            configuredWorkerCount=4,
            transfers=4,
            payloadMiB=2,
        )
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()

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
import tempfile
import unittest

from pathlib import Path


@unittest.skipUnless(sys.platform == 'linux', 'Requires Linux socket fault injection')
class QUICSegmentationTest(unittest.TestCase):
    root = Path(__file__).resolve().parents[1]

    def testQUICDownloadWhenKernelIgnoresUDPSegmentation(self):
        """Regression: Linux 4.9 accepted UDP_SEGMENT but sent unsegmented datagrams."""
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory) / 'IgnoreUDPSegmentation.so'
            subprocess.run(
                [
                    'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                    str(self.root / 'tests/native/IgnoreUDPSegmentation.c'),
                    '-ldl', '-o', str(library),
                ],
                check=True,
            )

            environment = os.environ.copy()
            environment.pop('FFL_P2P_QUIC_DISABLE_GSO', None)
            environment.pop('FFL_P2P_QUIC_DISABLE_BATCH', None)
            environment['LD_PRELOAD'] = os.pathsep.join(
                path for path in (str(library), environment.get('LD_PRELOAD')) if path
            )
            environment['PYTHONPATH'] = os.pathsep.join(
                (str(self.root / 'src'), str(self.root / 'tests'), str(self.root))
            )

            result = subprocess.run(
                [
                    sys.executable, '-c',
                    'from QUICTransferHarness import QUICTransferHarness; '
                    'payload = (bytes(range(251)) * 9424)[:2365294]; '
                    'QUICTransferHarness().transfer(payload, timeout=10)',
                ],
                cwd=self.root, env=environment, capture_output=True, text=True, timeout=60,
            )

            output = result.stdout + result.stderr
            self.assertIn('UDP_SEGMENT ignored by test kernel', output,
                          'The kernel fault must actually be exercised')
            self.assertEqual(0, result.returncode,
                             f'QUIC must deliver the complete file without GSO:\n{output}')


if __name__ == '__main__':
    unittest.main()

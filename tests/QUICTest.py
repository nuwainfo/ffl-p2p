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
import unittest
from unittest import mock

from ffl_p2p.QUIC import QUICStream

from QUICTransferHarness import QUICTransferHarness


class QUICTest(unittest.TestCase):
    def _transfer(self, payload, offset=0, timeout=45, chunkSize=256 * 1024):
        return QUICTransferHarness().transfer(
            payload, offset=offset, timeout=timeout, chunkSize=chunkSize)

    def testICEUDPQUICLargeStream(self):
        # Native initial per-stream receive credit is 16 MiB. 24 MiB forces
        # MAX_STREAM_DATA extension and also exercises bounded ACK buffering.
        payload = bytes(range(256)) * (24 * 1024 * 1024 // 256)
        self._transfer(payload)

    def testLinuxDatapathBatchingCanBeDisabled(self):
        payload = bytes(range(241)) * (2 * 1024 * 1024 // 241)
        with mock.patch.dict(
            os.environ,
            {
                'FFL_P2P_QUIC_DISABLE_GSO': '1',
                'FFL_P2P_QUIC_DISABLE_RECV_BATCH': '1',
            },
        ):
            self._transfer(payload, timeout=20)

    def testICEUDPQUICBatchCanBeDisabled(self):
        payload = bytes(range(239)) * (2 * 1024 * 1024 // 239)
        with mock.patch.dict(os.environ, {'FFL_P2P_QUIC_DISABLE_BATCH': '1'}):
            self._transfer(payload, timeout=20)

    def testIterReceiveDrainsBufferedTailAfterPeerFinished(self):
        class FinishedSession:
            def __init__(self):
                self._chunks = [b'first', b'last', b'']
                self.peerFinished = True

            def read(self):
                return self._chunks.pop(0)

            def wait(self, timeout):
                raise AssertionError('buffered data must not require a wait')

        stream = object.__new__(QUICStream)
        stream.session = FinishedSession()
        self.assertEqual(b'firstlast', b''.join(stream.iterReceive(timeout=0.1)))

    def testICEUDPQUICResumeOffset(self):
        payload = bytes(range(251)) * (6 * 1024 * 1024 // 251)
        # Deliberately non-chunk-aligned: the ffl-file protocol carries the
        # logical byte offset without imposing transport-level framing.
        offset = 1024 * 1024 + 123
        self._transfer(payload, offset=offset, timeout=30)


if __name__ == '__main__':
    unittest.main()

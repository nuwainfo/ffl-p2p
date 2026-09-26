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

import hashlib
import threading
import time
import unittest

from QUICTransferHarness import QUICTransferSession

MIB = 1024 * 1024


class PatternPayload:
    """A large payload that is never held in memory: byte i is i % 251."""

    PERIOD = bytes(range(251))

    def __init__(self, size):
        self.size = size

    def __len__(self):
        return self.size

    def __getitem__(self, item):
        start, stop, _step = item.indices(self.size)
        length = max(0, stop - start)
        phase = start % len(self.PERIOD)
        repeats = (phase + length) // len(self.PERIOD) + 1
        return (self.PERIOD * repeats)[phase:phase + length]


class StalledReaderSession(QUICTransferSession):
    """The receiving application reads the first chunk, then stops reading for a while.

    It records how far the sender's producer got meanwhile: with QUIC flow control the sender must stop at about the
    receive window, whatever the payload size, instead of queueing everything in the receiver's memory.
    """

    def __init__(self, payload, stallSeconds, **kwargs):
        super().__init__(payload, offset=0, observer=None, **kwargs)
        self.stallSeconds = stallSeconds
        self.producedBytes = 0
        self.producedWhileStalled = None
        self.lock = threading.Lock()
        self.digest = hashlib.sha256()

    def _iterPayloadChunks(self, requestedOffset):
        for position in range(requestedOffset, len(self.payload), self.chunkSize):
            chunk = self.payload[position:position + self.chunkSize]
            with self.lock:
                self.producedBytes = position + len(chunk)
            yield chunk

    def _receivePayload(self):
        receivedBytes = 0
        stalled = False
        for received in self.client.iterDownload(offset=0, timeout=self.timeout):
            self.digest.update(received)
            receivedBytes += len(received)
            if not stalled:
                stalled = True
                time.sleep(self.stallSeconds)
                with self.lock:
                    self.producedWhileStalled = self.producedBytes

        if receivedBytes != len(self.payload):
            raise AssertionError(f'received {receivedBytes} of {len(self.payload)} bytes')


class QUICFlowControlTest(unittest.TestCase):

    def testSenderStopsAtTheReceiveWindowWhileTheReaderStalls(self):
        # A fast source (a borg export of a registry) and a slow consumer: the receiver was OOM killed at 24.6 GB
        # because its credit followed arrival instead of consumption.
        payload = PatternPayload(512 * MIB)
        session = StalledReaderSession(payload, stallSeconds=4, timeout=120, chunkSize=256 * 1024)

        session.run()

        # Stream receive window (16 MiB) plus what the sender itself buffers (Python write pipeline, native queue).
        self.assertLess(
            session.producedWhileStalled, 96 * MIB,
            f'the sender produced {session.producedWhileStalled / MIB:.0f} MiB while the reader read nothing',
        )

    def testEverythingStillArrivesAfterTheStall(self):
        payload = PatternPayload(64 * MIB)
        session = StalledReaderSession(payload, stallSeconds=2, timeout=60, chunkSize=256 * 1024)

        session.run()

        self.assertEqual(hashlib.sha256(payload[0:len(payload)]).hexdigest(), session.digest.hexdigest())


if __name__ == '__main__':
    unittest.main()

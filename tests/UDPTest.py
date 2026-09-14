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

import socket
import struct
import threading
import unittest

from unittest.mock import patch

from ffl_p2p import ICEConfiguration, ICEServer
from ffl_p2p.UDP import UDPConnector


class FakeSTUNServer:
    MAGIC_COOKIE = 0x2112A442

    def __init__(self, mappedAddress: str, mappedPort: int):
        self.mappedAddress = mappedAddress
        self.mappedPort = mappedPort
        self.requestCount = 0
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.bind(('127.0.0.1', 0))
        self._socket.settimeout(0.05)
        self.port = self._socket.getsockname()[1]
        self._closed = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        while not self._closed.is_set():
            try:
                request, sender = self._socket.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return

            if len(request) < 20 or request[4:8] != struct.pack('!I', self.MAGIC_COOKIE):
                continue

            self.requestCount += 1
            transactionID = request[8:20]
            xorPort = self.mappedPort ^ (self.MAGIC_COOKIE >> 16)
            address = socket.inet_aton(self.mappedAddress)
            cookie = struct.pack('!I', self.MAGIC_COOKIE)
            xorAddress = bytes(left ^ right for left, right in zip(address, cookie))
            attribute = struct.pack('!HHBBH4s', 0x0020, 8, 0, 1, xorPort, xorAddress)
            response = struct.pack(
                '!HHI12s', 0x0101, len(attribute), self.MAGIC_COOKIE, transactionID
            ) + attribute
            self._socket.sendto(response, sender)

    def close(self):
        self._closed.set()
        self._socket.close()

        self._thread.join(timeout=1)


class UDPTest(unittest.TestCase):
    def testOfferAnswerRolesExchangeDatagrams(self):
        offerer = UDPConnector(usePortMapping=False)
        answerer = UDPConnector(usePortMapping=False)

        try:
            offerSDP = offerer.gather()
            self.assertEqual('controlling', offerer.iceRole)

            # Match WebRTC/aioice offer-answer ordering: the answerer learns the
            # remote description before it starts gathering. libjuice therefore
            # enters CONTROLLED instead of defaulting both peers to CONTROLLING.
            answerer.setRemoteDescription(offerSDP)
            self.assertEqual('controlled', answerer.iceRole)
            answerSDP = answerer.gather()
            offerer.setRemoteDescription(answerSDP)

            offerTransport = offerer.connect()
            answerTransport = answerer.connect()
            self.assertIsNotNone(offerTransport)
            self.assertIsNotNone(answerTransport)
            self.assertEqual('controlling', offerTransport.iceRole)
            self.assertEqual('controlled', answerTransport.iceRole)

            offerer.agent.send(b'left-to-right')
            self.assertEqual(b'left-to-right', answerer.agent.waitForEvent('receive', 2)['value'])
            answerer.agent.send(b'right-to-left')
            self.assertEqual(b'right-to-left', offerer.agent.waitForEvent('receive', 2)['value'])

        finally:
            offerer.close()
            answerer.close()

    def testDeterministicOfferAnswerRolesAreStableAcrossRepeatedConnections(self):
        for attempt in range(12):
            with self.subTest(attempt=attempt):
                offerer = UDPConnector(usePortMapping=False)
                answerer = UDPConnector(usePortMapping=False)

                try:
                    offerSDP = offerer.gather()
                    answerer.setRemoteDescription(offerSDP)
                    answerSDP = answerer.gather()
                    offerer.setRemoteDescription(answerSDP)

                    self.assertIsNotNone(offerer.connect(timeout=2.0))
                    self.assertIsNotNone(answerer.connect(timeout=2.0))
                    self.assertEqual('controlling', offerer.iceRole)
                    self.assertEqual('controlled', answerer.iceRole)
                finally:
                    offerer.close()
                    answerer.close()

    def testPortMappingFailureStillCompletesRealICEConnection(self):
        offerer = UDPConnector(usePortMapping=True)
        answerer = UDPConnector(usePortMapping=True)

        try:
            with patch(
                'ffl_p2p.UDP.NativePortMapping',
                side_effect=RuntimeError('forced mapping failure'),
            ):
                offerSDP = offerer.gather()
                answerer.setRemoteDescription(offerSDP)
                answerSDP = answerer.gather()

            offerer.setRemoteDescription(answerSDP)

            self.assertIsNotNone(offerer.connect(timeout=2.0))
            self.assertIsNotNone(answerer.connect(timeout=2.0))

            offerer.agent.send(b'port-mapping-fallback')
            received = answerer.agent.waitForEvent('receive', 2)
            self.assertEqual(b'port-mapping-fallback', received['value'])
        finally:
            offerer.close()
            answerer.close()

    def testConfiguredSTUNUsesOnlyFirstServer(self):
        firstServer = FakeSTUNServer('203.0.113.31', 45631)
        secondServer = FakeSTUNServer('203.0.113.32', 45632)
        connector = UDPConnector(
            ICEConfiguration(
                iceServers=[
                    ICEServer(urls=[
                        f'stun:127.0.0.1:{firstServer.port}',
                        f'stun:127.0.0.1:{secondServer.port}',
                    ])
                ],
                gatheringTimeout=1.5,
            ),
            usePortMapping=False,
        )

        try:
            sdp = connector.gather()
            self.assertGreater(firstServer.requestCount, 0)
            self.assertEqual(0, secondServer.requestCount)
            self.assertIn('203.0.113.31', sdp)
            self.assertIn('45631', sdp)
            self.assertIn('typ srflx', sdp)
        finally:
            connector.close()
            firstServer.close()
            secondServer.close()

    def testPortMappingCandidateInjection(self):
        connector = UDPConnector(usePortMapping=True)

        try:
            sdp = connector.gather()
            if connector._mapping is None:
                self.assertTrue(sdp)
                return

            info = connector._mapping.query()
            self.assertEqual('success', info.state)
            self.assertIn(info.externalHost, sdp)
            self.assertIn(str(info.externalPort), sdp)
            self.assertIn('typ srflx', sdp)
        finally:
            connector.close()


if __name__ == '__main__':
    unittest.main()

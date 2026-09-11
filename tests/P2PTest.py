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

import threading
import time
import unittest
from unittest.mock import patch

from ffl_p2p import ICEConfiguration
from ffl_p2p.P2P import P2PConnectivityTimeout, P2PConnector, P2PPublisher
from ffl_p2p.Signaling import P2POffer

from TestHTTPServer import P2PTestHTTPServer


class P2PTest(unittest.TestCase):
    def _createServer(self, payload):
        server = P2PTestHTTPServer(payload)
        publisher = P2PPublisher(server.port, useUDPPortMapping=True, useTCPPortMapping=False, tcpLocalHosts=['127.0.0.1'])

        server.setPublisher(publisher)
        server.start()
        return publisher, server

    def testAnswererSetsRemoteBeforeGatherAndKeepsFullConnectivityTimeout(self):
        events = []

        class FakeTransport:
            name = 'udp'

            def close(self):
                return

        fakeTransport = FakeTransport()

        class FakeSignaling:
            def __init__(self, baseURL, headers=None):
                self.baseURL = baseURL
                self.headers = headers

            def getOffer(self):
                events.append('getOffer')
                return P2POffer('session', 'offer-sdp', [], None)

            def sendAnswer(self, answer):
                events.append(('sendAnswer', answer.udpDescription))

        class FakeUDPConnector:
            def __init__(self, configuration, usePortMapping=True):
                self.configuration = configuration
                self.usePortMapping = usePortMapping

            def setRemoteDescription(self, sdp):
                events.append(('setRemoteDescription', sdp))

            def gather(self):
                events.append('gather')
                time.sleep(0.02)
                return 'answer-sdp'

            def connect(self, timeout):
                events.append(('connect', timeout))
                return fakeTransport

            def close(self):
                events.append('close')

        configuration = ICEConfiguration(connectivityTimeout=4.5)

        with patch('ffl_p2p.P2P.HTTPSignalingClient', FakeSignaling), \
             patch('ffl_p2p.P2P.UDPConnector', FakeUDPConnector):
            connection = P2PConnector(configuration, useUDPPortMapping=False).connect(
                'https://example.invalid', preference='udp'
            )

        self.assertIsNotNone(connection)
        self.assertEqual(
            [
                'getOffer',
                ('setRemoteDescription', 'offer-sdp'),
                'gather',
                ('sendAnswer', 'answer-sdp'),
                ('connect', 4.5),
            ],
            events,
        )

    def testUDPConnectivityTimeoutIsReportedSeparately(self):
        class FakeSignaling:
            def __init__(self, baseURL, headers=None):
                return

            def getOffer(self):
                return P2POffer('session', 'offer-sdp', [], None)

            def sendAnswer(self, answer):
                return

        class FakeUDPConnector:
            def __init__(self, configuration, usePortMapping=True):
                self.closed = False

            def setRemoteDescription(self, sdp):
                return

            def gather(self):
                return 'answer-sdp'

            def connect(self, timeout):
                return None

            def close(self):
                self.closed = True

        with patch('ffl_p2p.P2P.HTTPSignalingClient', FakeSignaling), \
             patch('ffl_p2p.P2P.UDPConnector', FakeUDPConnector):
            with self.assertRaises(P2PConnectivityTimeout):
                P2PConnector(useUDPPortMapping=False).connect(
                    'https://example.invalid', preference='udp'
                )

    def testHTTPBootstrapThenTCPDirectTransfer(self):
        payload = b'FFL HTTP bootstrap -> direct TCP -> existing HTTP server'
        publisher, server = self._createServer(payload)

        try:
            connection = P2PConnector(useUDPPortMapping=True).connect(server.baseURL, preference='tcp')
            self.assertIsNotNone(connection)
            self.assertEqual('tcp', connection.transportName)

            self.assertEqual(payload, connection.transport.read('/file'))
            connection.close()
        finally:
            server.close()
            publisher.close()

    def testHTTPBootstrapThenUDPDirect(self):
        payload = b'unused'
        publisher, server = self._createServer(payload)
        senderResult = {}

        def waitSender():
            senderResult['transport'] = publisher.waitForUDP(4)

        senderThread = threading.Thread(target=waitSender)
        senderThread.start()

        try:
            connection = P2PConnector(useUDPPortMapping=True).connect(server.baseURL, preference='udp', timeout=4)
            senderThread.join(timeout=5)

            self.assertIsNotNone(connection)
            self.assertEqual('udp', connection.transportName)
            self.assertEqual('controlled', connection.transport.iceRole)
            self.assertIsNotNone(senderResult.get('transport'))
            self.assertEqual('controlling', senderResult['transport'].iceRole)

            connection.transport.send(b'ffl-native-p2p')
            self.assertEqual(b'ffl-native-p2p', senderResult['transport'].receive(2))
            senderResult['transport'].send(b'ack')
            self.assertEqual(b'ack', connection.transport.receive(2))

            connection.close()
        finally:
            senderThread.join(timeout=1)
            server.close()
            publisher.close()


if __name__ == '__main__':
    unittest.main()

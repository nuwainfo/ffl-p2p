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

import unittest

from ffl_p2p.P2P import P2PPublisher
from ffl_p2p.TCP import TCPConnector

from TestHTTPServer import P2PTestHTTPServer


class TCPTest(unittest.TestCase):
    def testPublisherKeepsSharePathInTCPEndpoint(self):
        publisher = P2PPublisher(
            8080, useUDPPortMapping=False, useTCPPortMapping=False,
            tcpLocalHosts=['127.0.0.1'], tcpPath='/share-id',
        )

        try:
            self.assertEqual(['http://127.0.0.1:8080/share-id'], publisher._tcp.endpoints())
        finally:
            publisher.close()

    def testDirectHTTPUsesExistingServer(self):
        payload = b'ffl-p2p tcp direct payload'
        server = P2PTestHTTPServer(payload)
        publisher = P2PPublisher(server.port, useUDPPortMapping=False, useTCPPortMapping=False, tcpLocalHosts=['127.0.0.1'])
        server.setPublisher(publisher)
        server.start()

        try:
            transport = TCPConnector(publisher._tcp.endpoints()).connect()
            self.assertIsNotNone(transport)
            self.assertEqual(payload, transport.read('/file'))
        finally:
            server.close()
            publisher.close()


if __name__ == '__main__':
    unittest.main()

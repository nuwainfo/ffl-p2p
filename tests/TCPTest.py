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
from ffl_p2p.Transport import HTTPTransport

from TestHTTPServer import P2PTestHTTPServer, TLSTestIdentity


class TCPTest(unittest.TestCase):
    SECRET_HEADERS = {'Authorization': 'Basic c2VjcmV0OnNlY3JldA==', 'X-FFL-Pickup': '123456'}

    def _startServer(self, payload, tls):
        server = P2PTestHTTPServer(payload, tls=tls)
        publisher = P2PPublisher(
            server.port, useUDPPortMapping=False, useTCPPortMapping=False, tcpLocalHosts=['127.0.0.1'],
            tcpCertificateFingerprint=TLSTestIdentity.fingerprint() if tls else None,
        )
        server.setPublisher(publisher)
        server.start()
        self.addCleanup(publisher.close)
        self.addCleanup(server.close)
        return publisher, server

    def testPublisherAdvertisesTLSEndpointsWithTheirCertificateFingerprint(self):
        fingerprint = TLSTestIdentity.fingerprint()
        publisher = P2PPublisher(
            8080, useUDPPortMapping=False, useTCPPortMapping=False,
            tcpLocalHosts=['127.0.0.1'], tcpPath='/share-id', tcpCertificateFingerprint=fingerprint,
        )

        try:
            offer = publisher.createOffer()
            self.assertEqual(['https://127.0.0.1:8080/share-id'], offer.tcpEndpoints)
            self.assertEqual(fingerprint, offer.tcpCertificateFingerprint)
        finally:
            publisher.close()

    def testDirectTLSPinsTheCertificateTheOfferAdvertised(self):
        payload = b'ffl-p2p pinned tls payload'
        publisher, server = self._startServer(payload, tls=True)
        offer = publisher.createOffer()

        transport = TCPConnector(offer.tcpEndpoints, certificateFingerprint=offer.tcpCertificateFingerprint).connect(
            headers=self.SECRET_HEADERS
        )

        self.assertIsNotNone(transport)
        self.assertEqual(offer.tcpCertificateFingerprint, transport.certificateFingerprint)
        self.assertEqual(payload, transport.read('/file'))
        self.assertTrue(server.requests)
        self.assertTrue(all(request.secure for request in server.requests), server.requests)

    def testDirectTLSSendsNoRequestToAnUnpinnedCertificate(self):
        publisher, server = self._startServer(b'unused', tls=True)

        transport = TCPConnector(publisher.createOffer().tcpEndpoints, certificateFingerprint='00' * 32).connect(
            headers=self.SECRET_HEADERS
        )

        self.assertIsNone(transport)
        self.assertEqual([], server.requests, 'Headers were sent to a server whose certificate does not match the pin')

    def testRequiredTLSNeverProbesAPlaintextEndpoint(self):
        publisher, server = self._startServer(b'unused', tls=False)

        transport = TCPConnector(publisher.createOffer().tcpEndpoints, requireTLS=True).connect(
            headers=self.SECRET_HEADERS
        )

        self.assertIsNone(transport)
        self.assertEqual([], server.requests, 'Headers were sent in plaintext although TLS is required')

    def testTLSEndpointWithoutAFingerprintIsRejected(self):
        with self.assertRaises(ValueError):
            HTTPTransport('https://127.0.0.1:8080/share-id')

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

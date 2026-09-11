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

from dataclasses import dataclass

from ffl_p2p.P2P import P2PConnector, P2PPublisher
from ffl_p2p.QUIC import QUICFileClient, QUICFileServer

from TestHTTPServer import P2PTestHTTPServer


@dataclass(frozen=True)
class QUICTransferResult:
    requestResult: dict


class QUICTransferSession:
    """Own one loopback QUIC transfer and its test-only resources."""

    def __init__(self, payload, offset, timeout, chunkSize, observer):
        self.payload = payload
        self.offset = offset
        self.timeout = timeout
        self.chunkSize = chunkSize
        self.observer = observer

        self.server = P2PTestHTTPServer(b'control-plane-only')
        self.publisher = None
        self.connection = None
        self.client = None
        self.senderErrors = []
        self.requestResults = []

    @staticmethod
    def _forbidPythonDatagramPath(*args, **kwargs):
        raise AssertionError('QUIC packet crossed the Python UDP send/receive path')

    def run(self):
        self.publisher = self._createPublisher()
        self.server.setPublisher(self.publisher)
        self.server.start()

        try:
            self._connectClient()
            self._receivePayload()
            self._waitForSender()
            return QUICTransferResult(requestResult=self.requestResults[0])
        finally:
            self._close()

    def _createPublisher(self):
        return P2PPublisher(
            self.server.port,
            useUDPPortMapping=False,
            useTCPPortMapping=False,
            tcpLocalHosts=['127.0.0.1'],
            udpHandler=self._serveQUIC,
        )

    def _serveQUIC(self, udpTransport):
        self._forceNativeDatapath(udpTransport)
        quic = QUICFileServer(udpTransport)

        try:
            requestResult = quic.serve(
                self._iterPayloadChunks, timeout=self.timeout, closeTimeout=10)
            if not requestResult['cleanClose']:
                raise AssertionError('QUIC transfer did not close cleanly')

            self.requestResults.append(requestResult)
            if self.observer is not None:
                self.observer.onServerComplete(quic.stream)
        except Exception as error:
            self.senderErrors.append(error)
            raise
        finally:
            quic.close()

    def _iterPayloadChunks(self, requestedOffset):
        for position in range(requestedOffset, len(self.payload), self.chunkSize):
            yield self.payload[position:position + self.chunkSize]

    def _connectClient(self):
        offer = self.publisher.createOffer()
        if 'BEGIN CERTIFICATE' not in offer.quicCertificate:
            raise AssertionError('QUIC offer did not contain a certificate')

        self.connection = P2PConnector(useUDPPortMapping=False).connect(
            self.server.baseURL, preference='udp', timeout=5)
        if self.connection is None or self.connection.transportName != 'udp':
            raise AssertionError('native QUIC transfer did not select UDP')

        self._forceNativeDatapath(self.connection.transport)
        self.client = QUICFileClient(self.connection.transport)

    def _receivePayload(self):
        receivedOffset = self.offset
        for received in self.client.iterDownload(offset=self.offset, timeout=self.timeout):
            receivedEnd = receivedOffset + len(received)
            if received != self.payload[receivedOffset:receivedEnd]:
                raise AssertionError('native QUIC payload mismatch')
            receivedOffset = receivedEnd

        if receivedOffset != len(self.payload):
            raise AssertionError('native QUIC payload length mismatch')

        if self.observer is not None:
            self.observer.onClientComplete(self.client.stream)

    def _waitForSender(self):
        if self.publisher._udpHandlerThread:
            self.publisher._udpHandlerThread.join(timeout=10)

        if self.senderErrors:
            raise self.senderErrors[0]
        if self.publisher.udpHandlerError is not None:
            raise self.publisher.udpHandlerError
        if not self.requestResults or self.requestResults[0]['offset'] != self.offset:
            raise AssertionError('QUIC resume offset mismatch')

    def _forceNativeDatapath(self, udpTransport):
        udpTransport.send = self._forbidPythonDatagramPath
        udpTransport.receive = self._forbidPythonDatagramPath

    def _close(self):
        if self.client:
            self.client.close()
        if self.connection:
            self.connection.close()

        self.server.close()
        if self.publisher:
            self.publisher.close()


class QUICTransferHarness:
    """Create loopback native-QUIC sessions for tests and benchmarks."""

    def transfer(self, payload, offset=0, timeout=45, chunkSize=256 * 1024, observer=None):
        return QUICTransferSession(payload, offset, timeout, chunkSize, observer).run()

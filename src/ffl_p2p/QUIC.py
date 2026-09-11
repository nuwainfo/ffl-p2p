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

import json
import time

from typing import Optional

from .Native import NativeQUICSession
from .QUICFlow import (
    QUICWriteFlowConfiguration, QUICWriteFlowController, environmentEnabled,
)


# Historical Python QUIC diagnostics/profiling helpers are intentionally absent from
# production. Recover them from commit b8ef8777560e7e45ca539ceb39f1152e1c10248a.


class QUICUnavailableError(RuntimeError):
    pass


class QUICStream:
    """One reliable bidirectional stream driven by the native QUIC transport.

    Python owns file-level flow only.  ICE receive callbacks, QUIC timers,
    packet generation and ACK/retransmit progress are queued as native
    operations and executed by the connection's single owning Worker.
    """

    def __init__(self, udpTransport, role: str, credentials=None, certificate: Optional[str] = None):
        if role not in {'client', 'server'}:
            raise ValueError("role must be 'client' or 'server'")

        self.udpTransport = udpTransport
        self.role = role
        self.credentials = credentials
        self.certificate = certificate
        self.session = None
        self.runtimeStarted = False
        self.aggregatePackets = not environmentEnabled('FFL_P2P_QUIC_DISABLE_BATCH')
        self.writeFlow = QUICWriteFlowController(
            QUICWriteFlowConfiguration.fromEnvironment())

    @classmethod
    def client(cls, udpTransport):
        certificate = getattr(udpTransport, 'quicCertificate', None)
        if not certificate:
            raise QUICUnavailableError('peer did not advertise a pinned QUIC certificate')

        return cls(udpTransport, 'client', certificate=certificate)

    @classmethod
    def server(cls, udpTransport):
        credentials = getattr(udpTransport, 'quicCredentials', None)
        if credentials is None:
            raise QUICUnavailableError('publisher has no QUIC credentials')

        return cls(udpTransport, 'server', credentials=credentials)

    def _ensureSession(self):
        if self.session is not None:
            return

        if self.role == 'client':
            self.session = NativeQUICSession.client(self.certificate)
        else:
            self.session = NativeQUICSession.server(self.credentials)

    def _ensureRuntimeStarted(self, timeout: float = 5.0):
        self._ensureSession()
        if self.runtimeStarted:
            return

        if not self.session.supportsRuntimeV2:
            raise QUICUnavailableError('ffl-p2p native QUIC support is required')

        self.session.start(self.udpTransport.nativeAgent, aggregate=self.aggregatePackets, timeout=timeout)
        self.runtimeStarted = True

    def _raiseRuntimeError(self):
        if self.session is not None:
            self.session.checkError()

    def connect(self, timeout: float = 5.0):
        self._ensureRuntimeStarted(timeout=min(timeout, 5.0))

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.session.handshakeComplete:
                return self

            remaining = max(0.0, deadline - time.monotonic())
            self.session.wait(timeout=min(0.05, remaining))
            self._raiseRuntimeError()

        raise TimeoutError('QUIC/TLS handshake timed out')

    def send(self, data: bytes, fin: bool = False, timeout: float = 60.0):
        if self.session is None or not self.session.handshakeComplete:
            raise RuntimeError('QUIC stream is not connected')

        deadline = time.monotonic() + timeout
        self.writeFlow.queue(
            self.session, data, fin, deadline,
            waitForChange=lambda waitTimeout: self.session.wait(timeout=waitTimeout),
            raiseRuntimeError=self._raiseRuntimeError,
        )

    def receive(self, timeout: float = 60.0):
        if self.session is None:
            raise RuntimeError('QUIC stream is not connected')

        data = self.session.read()
        if data:
            return data

        if self.session.peerFinished:
            return b''

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.session.wait(timeout=min(0.05, max(0.0, deadline - time.monotonic())))
            self._raiseRuntimeError()
            data = self.session.read()
            if data or self.session.peerFinished:
                return data

        raise TimeoutError('QUIC stream receive timed out')

    def iterReceive(self, timeout: float = 60.0):
        # A peer FIN means no *future* stream bytes will arrive, but it does not
        # mean the application receive buffer has already been drained.  The
        # worker can append the final data and publish peerFinished between this
        # generator yielding one chunk and its next iteration.  Only an empty
        # receive() result is therefore a terminal condition.
        while True:
            data = self.receive(timeout)
            if not data:
                return

            yield data

    def waitForWriteAcknowledged(self, timeout: float = 10.0):
        if self.session is None:
            return False

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.session.writeAcknowledged:
                return True

            self.session.wait(timeout=min(0.05, max(0.0, deadline - time.monotonic())))
            self._raiseRuntimeError()

        return self.session.writeAcknowledged

    def waitForCleanClose(self, timeout: float = 10.0):
        if self.session is None:
            return False

        deadline = time.monotonic() + timeout
        closedAt = None
        while time.monotonic() < deadline:
            if self.session.streamClosed:
                if closedAt is None:
                    closedAt = time.monotonic()

                if time.monotonic() - closedAt >= 0.05:
                    return True

            self.session.wait(timeout=min(0.05, max(0.0, deadline - time.monotonic())))
            self._raiseRuntimeError()

        return self.session.streamClosed

    def close(self):
        if self.session is not None:
            self.session.close()
            self.session = None

        self.runtimeStarted = False


class QUICFileClient:
    PROTOCOL = 'ffl-file/1'

    def __init__(self, udpTransport):
        self.stream = QUICStream.client(udpTransport)

    def iterDownload(self, offset: int = 0, timeout: float = 60.0):
        if offset < 0:
            raise ValueError('offset must be non-negative')
        self.stream.connect(timeout=min(timeout, 10.0))
        request = json.dumps(
            {'protocol': self.PROTOCOL, 'offset': int(offset)},
            separators=(',', ':'),
        ).encode('utf-8') + b'\n'

        # Keep the client->server direction open. After the file has been fully
        # consumed by the caller, send an application-level completion ACK and
        # FIN. This is stronger than relying on transport ACKs alone: the sender
        # only counts the download complete after Python has consumed every byte.
        self.stream.send(request, fin=False, timeout=timeout)
        receivedBytes = 0

        for data in self.stream.iterReceive(timeout=timeout):
            receivedBytes += len(data)
            yield data

        completion = json.dumps(
            {
                'protocol': self.PROTOCOL,
                'status': 'complete',
                'receivedBytes': receivedBytes,
            },
            separators=(',', ':'),
        ).encode('utf-8') + b'\n'

        self.stream.send(completion, fin=False, timeout=timeout)

        # Send FIN as an empty STREAM frame. ngtcp2 then reports its transport
        # acknowledgement explicitly through acked_stream_data_offset(..., 0),
        # so the native transport can distinguish data ACK from FIN ACK.
        self.stream.send(b'', fin=True, timeout=timeout)
        if not self.stream.waitForWriteAcknowledged(timeout=min(timeout, 10.0)):
            raise ConnectionError('QUIC completion ACK was not acknowledged')

        if not self.stream.waitForCleanClose(timeout=min(timeout, 10.0)):
            raise ConnectionError('QUIC file stream did not close cleanly')

    def close(self):
        self.stream.close()


class QUICFileServer:
    PROTOCOL = QUICFileClient.PROTOCOL
    MAX_REQUEST_SIZE = 64 * 1024

    def __init__(self, udpTransport):
        self.stream = QUICStream.server(udpTransport)
        self._clientBuffer = bytearray()

    def _receiveClientLine(self, timeout: float):
        deadline = time.monotonic() + timeout
        while True:
            newline = self._clientBuffer.find(b'\n')
            if newline >= 0:
                line = bytes(self._clientBuffer[:newline])
                del self._clientBuffer[:newline + 1]
                return line
            if len(self._clientBuffer) > self.MAX_REQUEST_SIZE:
                raise ValueError('QUIC file control message is too large')
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('QUIC file control message timed out')
            data = self.stream.receive(min(timeout, remaining))
            if data:
                self._clientBuffer.extend(data)
                continue
            if self.stream.session.peerFinished:
                raise ConnectionError('peer closed QUIC request stream before control message')

    def _parseClientJSON(self, line: bytes, description: str):
        try:
            payload = json.loads(line.decode('utf-8'))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ValueError(f'invalid QUIC file {description}') from error
        if payload.get('protocol') != self.PROTOCOL:
            raise ValueError('unsupported QUIC file protocol')
        return payload

    def _receiveRequest(self, timeout: float):
        payload = self._parseClientJSON(self._receiveClientLine(timeout), 'request')
        offset = int(payload.get('offset', 0))
        if offset < 0:
            raise ValueError('invalid QUIC file offset')
        return offset, payload

    def _receiveCompletion(self, timeout: float, sentBytes: int):
        payload = self._parseClientJSON(self._receiveClientLine(timeout), 'completion')
        if payload.get('status') != 'complete':
            raise ValueError('QUIC file completion status is not complete')
        receivedBytes = int(payload.get('receivedBytes', -1))
        if receivedBytes != sentBytes:
            raise ValueError(
                f'QUIC file completion byte mismatch: client={receivedBytes}, server={sentBytes}'
            )
        return payload

    def serve(self, chunkIterator, timeout: float = 60.0, closeTimeout: float = 10.0):
        """Serve one file request.

        ``chunkIterator(offset)`` must return an iterable of bytes beginning at
        the requested logical offset.  ffl-specific encryption, checksum and
        reader semantics deliberately stay above this package.
        """
        self.stream.connect(timeout=min(timeout, 10.0))
        offset, request = self._receiveRequest(timeout)
        sentBytes = 0
        for chunk in chunkIterator(offset):
            if not chunk:
                continue
            chunk = bytes(chunk)
            self.stream.send(chunk, timeout=timeout)
            sentBytes += len(chunk)
        self.stream.send(b'', fin=True, timeout=timeout)

        # Do not report success merely because QUIC transport ACKed the final
        # packet. The client sends this only after its application consumed the
        # complete byte stream (and, in FFL, after decrypt/write processing).
        completion = self._receiveCompletion(timeout, sentBytes)
        clean = self.stream.waitForCleanClose(closeTimeout)
        return {
            'offset': offset,
            'request': request,
            'completion': completion,
            'sentBytes': sentBytes,
            'cleanClose': clean,
        }

    def close(self):
        self.stream.close()

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

from dataclasses import asdict, dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import hashlib
import json
import socket
import ssl
import threading

from ffl_p2p.Signaling import P2PAnswer


class TLSTestIdentity:
    """A static self-signed identity for pinned direct TLS tests; peers pin
    its fingerprint and never trust it as a CA."""

    PATH = Path(__file__).with_name('TLSTestIdentity.pem')
    CERTIFICATE_END = '-----END CERTIFICATE-----'

    @classmethod
    def fingerprint(cls) -> str:
        identityPEM = cls.PATH.read_text(encoding='ascii')
        certificatePEM = identityPEM[:identityPEM.index(cls.CERTIFICATE_END) + len(cls.CERTIFICATE_END)]
        return hashlib.sha256(ssl.PEM_cert_to_DER_cert(certificatePEM)).hexdigest()

    @classmethod
    def createServerContext(cls) -> ssl.SSLContext:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cls.PATH)
        return context


@dataclass(frozen=True)
class RecordedRequest:
    method: str
    path: str
    secure: bool


class DualProtocolHTTPServer(ThreadingHTTPServer):
    """Serve TLS and plain HTTP on one port, as FFL shares do: a connection
    is TLS when its first byte opens a TLS handshake record."""

    TLS_HANDSHAKE_RECORD = b'\x16'

    def __init__(self, serverAddress, handlerClass, tlsContext=None):
        super().__init__(serverAddress, handlerClass)
        self.tlsContext = tlsContext

    def finish_request(self, request, client_address):
        if self.tlsContext is None or request.recv(1, socket.MSG_PEEK) != self.TLS_HANDSHAKE_RECORD:
            super().finish_request(request, client_address)
            return

        with self.tlsContext.wrap_socket(request, server_side=True) as tlsRequest:
            super().finish_request(tlsRequest, client_address)


class P2PTestHTTPServer:
    def __init__(self, payload: bytes, tls: bool = False):
        self.publisher = None
        self.payload = payload
        self.requests = []

        self.server = DualProtocolHTTPServer(
            ('127.0.0.1', 0), self._createHandler(),
            tlsContext=TLSTestIdentity.createServerContext() if tls else None,
        )
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def port(self):
        return self.server.server_address[1]

    @property
    def baseURL(self):
        return f'http://127.0.0.1:{self.port}'

    def _createHandler(self):
        owner = self

        class Handler(BaseHTTPRequestHandler):
            # BaseHTTPRequestHandler requires these snake_case hook names.
            def do_GET(self):
                self._record()
                if self.path == '/p2p/offer':
                    self._writeJSON(asdict(owner.publisher.createOffer()))
                    return
                if self.path == '/p2p/ping':
                    self.send_response(200)
                    self.send_header('Content-Length', '2')
                    self.end_headers()
                    self.wfile.write(b'OK')
                    return
                if self.path == '/file':
                    self.send_response(200)
                    self.send_header('Content-Length', str(len(owner.payload)))
                    self.end_headers()
                    self.wfile.write(owner.payload)
                    return
                self.send_error(404)

            def do_POST(self):
                self._record()
                if self.path != '/p2p/answer':
                    self.send_error(404)
                    return
                size = int(self.headers.get('Content-Length', '0'))
                answer = P2PAnswer.fromDict(json.loads(self.rfile.read(size).decode('utf-8')))
                owner.publisher.acceptAnswer(answer)
                self._writeJSON({'accepted': True})

            def _record(self):
                owner.requests.append(RecordedRequest(
                    self.command, self.path, isinstance(self.connection, ssl.SSLSocket)
                ))

            def _writeJSON(self, data):
                body = json.dumps(data).encode('utf-8')

                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, format, *args):
                return

        return Handler

    def setPublisher(self, publisher):
        self.publisher = publisher

    def start(self):
        if self.publisher is None:
            raise RuntimeError('publisher must be set before server start')
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()

        self.thread.join(timeout=2)

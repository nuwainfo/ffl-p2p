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

from dataclasses import asdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading

from ffl_p2p.Signaling import P2PAnswer


class P2PTestHTTPServer:
    def __init__(self, payload: bytes):
        self.publisher = None
        self.payload = payload

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), self._createHandler())
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
                if self.path != '/p2p/answer':
                    self.send_error(404)
                    return
                size = int(self.headers.get('Content-Length', '0'))
                answer = P2PAnswer.fromDict(json.loads(self.rfile.read(size).decode('utf-8')))
                owner.publisher.acceptAnswer(answer)
                self._writeJSON({'accepted': True})

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

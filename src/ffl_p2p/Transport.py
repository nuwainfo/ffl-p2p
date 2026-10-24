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

from abc import ABC, abstractmethod
from typing import Optional
from urllib.parse import urlsplit

import hashlib
import http.client
import ssl


class TLSCertificateMismatchError(ConnectionError):
    """A direct endpoint presented a certificate other than the pinned one."""


class Transport(ABC):
    @property
    @abstractmethod
    def name(self) -> str:
        raise NotImplementedError

    @abstractmethod
    def close(self):
        raise NotImplementedError


class HTTPTransport(Transport):
    """HTTP to a direct endpoint.

    No CA can vouch for an ad-hoc LAN or port-mapped address, so an https
    endpoint is authenticated by the SHA-256 fingerprint of its DER
    certificate, which the publisher delivers over the signaling channel. The
    pin is checked right after the handshake, before any request byte is sent.
    """

    def __init__(self, baseURL: str, certificateFingerprint: Optional[str] = None):
        self.baseURL = baseURL.rstrip('/')
        self._url = urlsplit(self.baseURL)
        if self.secure and not certificateFingerprint:
            raise ValueError(f'{self.baseURL} has no certificate fingerprint to pin')

        self.certificateFingerprint = certificateFingerprint.lower() if self.secure else None

    @staticmethod
    def _createPinningContext() -> ssl.SSLContext:
        # The pin, not a CA chain or hostname, authenticates the endpoint.
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        return context

    @property
    def name(self) -> str:
        return 'tcp'

    @property
    def secure(self) -> bool:
        return self._url.scheme == 'https'

    def _connect(self, timeout: float) -> http.client.HTTPConnection:
        if not self.secure:
            return http.client.HTTPConnection(self._url.hostname, self._url.port, timeout=timeout)

        connection = http.client.HTTPSConnection(
            self._url.hostname, self._url.port, timeout=timeout, context=self._createPinningContext()
        )
        connection.connect()

        presentedFingerprint = hashlib.sha256(connection.sock.getpeercert(binary_form=True)).hexdigest()
        if presentedFingerprint != self.certificateFingerprint:
            connection.close()
            raise TLSCertificateMismatchError(
                f'{self.baseURL} presented certificate {presentedFingerprint}, '
                f'expected {self.certificateFingerprint}'
            )

        return connection

    def get(self, path: str, headers=None, timeout: float = 30) -> tuple[int, bytes]:
        connection = self._connect(timeout)
        try:
            connection.request('GET', self._url.path + '/' + path.lstrip('/'), headers=headers or {})
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    def ping(self, headers=None, timeout: float = 0.25) -> bool:
        status = self.get('/p2p/ping', headers=headers, timeout=timeout)[0]
        return status == 200

    def read(self, path: str = '/file', timeout: float = 30) -> bytes:
        status, body = self.get(path, timeout=timeout)
        if status != 200:
            raise ConnectionError(f'GET {path} failed: HTTP {status}')

        return body

    def close(self):
        return


class UDPTransport(Transport):
    def __init__(self, agent, iceRole: str, stunURL=None):
        self._agent = agent
        self.iceRole = iceRole
        self.stunURL = stunURL

    @property
    def name(self) -> str:
        return 'udp'

    def send(self, data: bytes):
        self._agent.send(data)

    def receive(self, timeout: float = 3.0):
        event = self._agent.waitForEvent('receive', timeout)
        return None if event is None else event['value']

    @property
    def nativeAgent(self):
        return self._agent

    @property
    def selectedAddresses(self):
        return self._agent.selectedAddresses

    def close(self):
        self._agent.close()

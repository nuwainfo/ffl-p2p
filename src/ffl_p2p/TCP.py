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

import http.client
import ipaddress
import logging
import socket
import time

from typing import Iterable, Optional

from .Native import NativePortMapping, NativeUnavailableError
from .Transport import HTTPTransport, TLSCertificateMismatchError


logger = logging.getLogger(__name__)


@dataclass(frozen=True)
class TCPEndpoint:
    host: str
    port: int
    path: str = ''
    secure: bool = False

    @property
    def url(self) -> str:
        scheme = 'https' if self.secure else 'http'
        host = f'[{self.host}]' if ':' in self.host else self.host
        path = '/' + self.path.lstrip('/') if self.path else ''
        return f'{scheme}://{host}:{self.port}{path}'


class TCPPublisher:
    def __init__(self, port: int, portMappingTimeout: float = 0.25, usePortMapping: bool = True,
                 localHosts: Optional[Iterable[str]] = None, path: str = '', useTLS: bool = False):
        self.port = port
        self.portMappingTimeout = portMappingTimeout
        self.usePortMapping = usePortMapping
        self.localHosts = None if localHosts is None else list(localHosts)
        self.path = path
        self.useTLS = useTLS
        self._mapping = None

    def _createEndpoint(self, host: str, port: int) -> TCPEndpoint:
        return TCPEndpoint(host, port, self.path, secure=self.useTLS)

    def endpoints(self) -> list[str]:
        endpoints = self._localEndpoints()
        if self.usePortMapping:
            mappedEndpoint = self._mappedEndpoint()
            if mappedEndpoint:
                endpoints.append(mappedEndpoint)
                
        return list(dict.fromkeys(endpoint.url for endpoint in endpoints))

    def _localEndpoints(self) -> list[TCPEndpoint]:
        if self.localHosts is not None:
            return [self._createEndpoint(host, self.port) for host in self.localHosts]

        hosts = set()
        try:
            addresses = socket.getaddrinfo(socket.gethostname(), None, type=socket.SOCK_STREAM)
        except socket.gaierror:
            addresses = []
            
        for address in addresses:
            host = address[4][0]
            try:
                parsed = ipaddress.ip_address(host.split('%', 1)[0])
            except ValueError:
                continue
                
            if parsed.is_unspecified or parsed.is_loopback or parsed.is_link_local:
                continue
                
            hosts.add(host)
            
        return [self._createEndpoint(host, self.port) for host in sorted(hosts)]

    def _mappedEndpoint(self):
        try:
            self._mapping = NativePortMapping('tcp', self.port)
            info = self._mapping.wait(self.portMappingTimeout)
        except (NativeUnavailableError, RuntimeError) as error:
            logger.debug('TCP port mapping unavailable: %s', error)
            self._closePortMapping()
            return None

        if info.state != 'success':
            logger.debug('TCP port mapping did not succeed: %s', info.state)
            self._closePortMapping()
            return None

        return self._createEndpoint(info.externalHost, info.externalPort)

    def _closePortMapping(self):
        if self._mapping is None:
            return

        self._mapping.close()
        self._mapping = None

    def close(self):
        self._closePortMapping()


class TCPConnector:
    """Probe direct endpoints and return the first that answers.

    The probe carries the caller's (authorizing) headers, so a plaintext
    endpoint is never probed when requireTLS is set, and a TLS endpoint
    receives nothing unless it presents the pinned certificate.
    """

    def __init__(self, endpoints: Iterable[str], certificateFingerprint: Optional[str] = None,
                 requireTLS: bool = False):
        self.endpoints = list(endpoints)
        self.certificateFingerprint = certificateFingerprint
        self.requireTLS = requireTLS

    def connect(self, timeout: float = 0.5, headers=None) -> Optional[HTTPTransport]:
        deadline = time.monotonic() + timeout
        for endpoint in self.endpoints:
            transport = HTTPTransport(endpoint, self.certificateFingerprint)
            if self.requireTLS and not transport.secure:
                logger.debug('Skipping plaintext TCP endpoint %s: TLS is required', endpoint)
                continue

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None

            try:
                if transport.ping(headers, timeout=min(remaining, 0.25)):
                    return transport
            except TLSCertificateMismatchError as error:
                logger.warning('Refusing TCP endpoint: %s', error)
            except (OSError, http.client.HTTPException):
                continue

        return None

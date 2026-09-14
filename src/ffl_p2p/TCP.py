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

import ipaddress
import logging
import socket
import time
import urllib.request

from typing import Iterable, Optional

from .Native import NativePortMapping, NativeUnavailableError
from .Transport import HTTPTransport


logger = logging.getLogger(__name__)


@dataclass(frozen=True)
class TCPEndpoint:
    host: str
    port: int
    path: str = ''

    @property
    def url(self) -> str:
        host = f'[{self.host}]' if ':' in self.host else self.host
        path = '/' + self.path.lstrip('/') if self.path else ''
        return f'http://{host}:{self.port}{path}'


class TCPPublisher:
    def __init__(self, port: int, portMappingTimeout: float = 0.25, usePortMapping: bool = True,
                 localHosts: Optional[Iterable[str]] = None, path: str = ''):
        self.port = port
        self.portMappingTimeout = portMappingTimeout
        self.usePortMapping = usePortMapping
        self.localHosts = None if localHosts is None else list(localHosts)
        self.path = path
        self._mapping = None

    def endpoints(self) -> list[str]:
        endpoints = self._localEndpoints()
        if self.usePortMapping:
            mappedEndpoint = self._mappedEndpoint()
            if mappedEndpoint:
                endpoints.append(mappedEndpoint)
                
        return list(dict.fromkeys(endpoint.url for endpoint in endpoints))

    def _localEndpoints(self) -> list[TCPEndpoint]:
        if self.localHosts is not None:
            return [TCPEndpoint(host, self.port, self.path) for host in self.localHosts]

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
            
        return [TCPEndpoint(host, self.port, self.path) for host in sorted(hosts)]

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

        return TCPEndpoint(info.externalHost, info.externalPort, self.path)

    def _closePortMapping(self):
        if self._mapping is None:
            return

        self._mapping.close()
        self._mapping = None

    def close(self):
        self._closePortMapping()


class TCPConnector:
    def __init__(self, endpoints: Iterable[str]):
        self.endpoints = list(endpoints)

    def connect(self, timeout: float = 0.5, headers=None) -> Optional[HTTPTransport]:
        deadline = time.monotonic() + timeout
        for endpoint in self.endpoints:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
                
            request = urllib.request.Request(endpoint + '/p2p/ping', headers=headers or {}, method='GET')
            
            try:
                with urllib.request.urlopen(request, timeout=min(remaining, 0.25)) as response:
                    if response.status == 200:
                        return HTTPTransport(endpoint)
            except OSError:
                continue
                
        return None

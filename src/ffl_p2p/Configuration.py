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

from dataclasses import dataclass, field
from typing import Optional


DEFAULT_STUN_PORT = 3478


@dataclass(slots=True)
class ICEServer:
    urls: str | list[str]

    @property
    def _nativeURLs(self) -> list[str]:
        urls = [self.urls] if isinstance(self.urls, str) else list(self.urls)
        for url in urls:
            if not isinstance(url, str):
                raise TypeError('ICE server URLs must be strings')

            scheme = url.split(':', 1)[0].lower()
            if scheme != 'stun':
                raise ValueError('ffl-p2p supports only stun: ICE server URLs')

        return urls


@dataclass(slots=True)
class ICEConfiguration:
    iceServers: list[ICEServer] = field(default_factory=list)
    bindAddress: Optional[str] = None
    portBegin: int = 0
    portEnd: int = 0
    portMappingTimeout: float = 0.25
    gatheringTimeout: float = 2.0
    connectivityTimeout: float = 3.0

    def __post_init__(self):
        if not 0 <= self.portBegin <= 65535 or not 0 <= self.portEnd <= 65535:
            raise ValueError('port range values must be between 0 and 65535')
            
        if self.portBegin and self.portEnd and self.portBegin > self.portEnd:
            raise ValueError('portBegin cannot be greater than portEnd')
            
        for name, value in (
            ('portMappingTimeout', self.portMappingTimeout),
            ('gatheringTimeout', self.gatheringTimeout),
            ('connectivityTimeout', self.connectivityTimeout),
        ):
            if value <= 0:
                raise ValueError(f'{name} must be positive')

    @property
    def _nativeURLs(self) -> list[str]:
        """Return the native ICE URL set using aioice-compatible STUN selection.

        ffl-datachannel intentionally exposes a list-shaped configuration while
        passing only the first STUN URL to its native backend.  ffl-p2p follows
        the same policy so both direct transports behave consistently when FFL
        feeds them the same ICE server configuration.
        """
        nativeURLs = []
        hasSTUNServer = False
        for iceServer in self.iceServers:
            for url in iceServer._nativeURLs:
                if hasSTUNServer:
                    continue

                hasSTUNServer = True
                nativeURLs.append(url)

        return nativeURLs

    @property
    def stunURL(self) -> Optional[str]:
        nativeURLs = self._nativeURLs
        return nativeURLs[0] if nativeURLs else None

    @property
    def stunEndpoint(self) -> Optional[tuple[str, int]]:
        if self.stunURL is None:
            return None
            
        return _parseSTUNURL(self.stunURL)


def _parseSTUNURL(url: str) -> tuple[str, int]:
    if ':' not in url:
        raise ValueError(f'invalid STUN URL: {url!r}')
        
    scheme, remainder = url.split(':', 1)
    if scheme.lower() != 'stun':
        raise ValueError('ffl-p2p supports only stun: ICE server URLs')

    remainder = remainder.removeprefix('//')
    if not remainder or any(marker in remainder for marker in ('/', '?', '#', '@')):
        raise ValueError(f'invalid STUN URL: {url!r}')

    if remainder.startswith('['):
        closingBracket = remainder.find(']')
        if closingBracket <= 1:
            raise ValueError(f'invalid STUN URL: {url!r}')
            
        host = remainder[1:closingBracket]
        suffix = remainder[closingBracket + 1:]
        if not suffix:
            return host, DEFAULT_STUN_PORT
            
        if not suffix.startswith(':') or not suffix[1:]:
            raise ValueError(f'invalid STUN URL: {url!r}')
            
        portText = suffix[1:]
    else:
        colonCount = remainder.count(':')
        if colonCount == 0:
            return remainder, DEFAULT_STUN_PORT
            
        if colonCount != 1:
            raise ValueError('IPv6 STUN hosts must use bracket notation')
            
        host, portText = remainder.rsplit(':', 1)
        if not host or not portText:
            raise ValueError(f'invalid STUN URL: {url!r}')

    try:
        port = int(portText)
    except ValueError as error:
        raise ValueError(f'invalid STUN port in URL: {url!r}') from error
        
    if not 1 <= port <= 65535:
        raise ValueError(f'STUN port out of range in URL: {url!r}')
        
    return host, port

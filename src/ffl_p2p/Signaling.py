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
from dataclasses import asdict, dataclass
from typing import Optional

import json
import urllib.error
import urllib.request


class P2PSignalingError(RuntimeError):
    pass


@dataclass(frozen=True)
class P2POffer:
    sessionId: str
    udpDescription: str
    tcpEndpoints: list[str]
    quicCertificate: Optional[str] = None

    @classmethod
    def fromDict(cls, data):
        return cls(**data)


@dataclass(frozen=True)
class P2PAnswer:
    sessionId: str
    udpDescription: str

    @classmethod
    def fromDict(cls, data):
        return cls(**data)


class SignalingClient(ABC):
    @abstractmethod
    def getOffer(self) -> P2POffer:
        raise NotImplementedError

    @abstractmethod
    def sendAnswer(self, answer: P2PAnswer):
        raise NotImplementedError


class HTTPSignalingClient(SignalingClient):
    def __init__(self, baseURL: str, timeout: float = 10, headers=None):
        self.baseURL = baseURL.rstrip('/')
        self.timeout = timeout
        self.headers = dict(headers or {})

    def _requestJSON(self, path: str, method: str = 'GET', data=None):
        body = None
        headers = {'Accept': 'application/json', **self.headers}
        
        if data is not None:
            body = json.dumps(data).encode('utf-8')
            headers['Content-Type'] = 'application/json'
            
        request = urllib.request.Request(
            self.baseURL + '/' + path.lstrip('/'),
            data=body,
            headers=headers,
            method=method,
        )
        
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                return json.loads(response.read().decode('utf-8'))
        except urllib.error.HTTPError as error:
            raise P2PSignalingError(
                f'P2P signaling {method} {path} failed: '
                f'HTTP {error.code} {error.reason}'
            ) from error
        except urllib.error.URLError as error:
            raise P2PSignalingError(
                f'P2P signaling {method} {path} failed: {error.reason}'
            ) from error

    def getOffer(self) -> P2POffer:
        return P2POffer.fromDict(self._requestJSON('/p2p/offer'))

    def sendAnswer(self, answer: P2PAnswer):
        self._requestJSON('/p2p/answer', 'POST', asdict(answer))

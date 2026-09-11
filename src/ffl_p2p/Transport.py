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

import urllib.request


class Transport(ABC):
    @property
    @abstractmethod
    def name(self) -> str:
        raise NotImplementedError

    @abstractmethod
    def close(self):
        raise NotImplementedError


class HTTPTransport(Transport):
    def __init__(self, baseURL: str):
        self.baseURL = baseURL.rstrip('/')

    @property
    def name(self) -> str:
        return 'tcp'

    def read(self, path: str = '/file', timeout: float = 30) -> bytes:
        request = urllib.request.Request(self.baseURL + '/' + path.lstrip('/'))
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.read()

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

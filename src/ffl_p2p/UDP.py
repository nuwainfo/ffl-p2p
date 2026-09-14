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

import logging

from typing import Optional

from .Configuration import ICEConfiguration
from .Native import NativeAgent, NativePortMapping, NativeUnavailableError
from .Transport import UDPTransport


logger = logging.getLogger(__name__)


class UDPConnector:
    def __init__(self, configuration: Optional[ICEConfiguration] = None, usePortMapping: bool = True):
        self.configuration = configuration or ICEConfiguration()
        self.usePortMapping = usePortMapping
        self._agent = None
        self._mapping = None
        self._gathered = False
        self._remoteDescriptionSet = False
        self._iceRole = 'unknown'

    @property
    def agent(self):
        return self._agent

    @property
    def iceRole(self):
        return self._iceRole

    @property
    def stunURL(self):
        return self.configuration.stunURL

    def _ensureAgent(self):
        if self._agent is not None:
            return
            
        self._agent = NativeAgent(
            stunServer=self.configuration.stunEndpoint,
            bindAddress=self.configuration.bindAddress,
            portBegin=self.configuration.portBegin,
            portEnd=self.configuration.portEnd,
        )

    def gather(self) -> str:
        if self._gathered:
            return self._agent.localDescription

        self._ensureAgent()
        
        if self._iceRole == 'unknown':
            # libjuice assumes CONTROLLING when gathering starts before a
            # remote description.  This is the intentional offerer lifecycle.
            self._iceRole = 'controlling'

        if self.usePortMapping:
            self._agent.holdGathering()

        try:
            self._agent.gather()

            if self.usePortMapping:
                self._tryPortMapping()
        finally:
            if self.usePortMapping:
                self._agent.releaseGathering()

        if self._agent.waitForEvent('gatheringDone', self.configuration.gatheringTimeout) is None:
            raise TimeoutError('libjuice candidate gathering timed out')
            
        self._gathered = True
        
        return self._agent.localDescription

    def _tryPortMapping(self):
        try:
            self._mapping = NativePortMapping('udp', self._agent.localPort)
            info = self._mapping.wait(self.configuration.portMappingTimeout)
        except (NativeUnavailableError, RuntimeError) as error:
            logger.debug('UDP port mapping unavailable: %s', error)
            self._closePortMapping()
            return

        if info.state != 'success':
            logger.debug('UDP port mapping did not succeed: %s', info.state)
            self._closePortMapping()
            return

        try:
            self._agent.addMappedCandidate(
                info.externalHost,
                info.externalPort,
            )
        except (RuntimeError, ValueError) as error:
            logger.debug('UDP mapped candidate rejected: %s', error)
            self._closePortMapping()

    def _closePortMapping(self):
        if self._mapping is None:
            return

        self._mapping.close()
        self._mapping = None

    def setRemoteDescription(self, sdp: str):
        if self._remoteDescriptionSet:
            raise RuntimeError('remote description is already set')
            
        self._ensureAgent()
        
        if self._iceRole == 'unknown':
            # libjuice assumes CONTROLLED when the remote description is set
            # before local gathering.  The answerer deliberately uses this
            # ordering so normal offer/answer never relies on role-conflict
            # repair.
            self._iceRole = 'controlled'
            
        self._agent.setRemoteDescription(sdp)
        self._agent.remoteGatheringDone()
        self._remoteDescriptionSet = True

    def connect(self, timeout: Optional[float] = None) -> Optional[UDPTransport]:
        if not self._gathered:
            raise RuntimeError('local candidate gathering is not complete')
            
        if not self._remoteDescriptionSet:
            raise RuntimeError('remote description is not set')
            
        connectivityTimeout = self.configuration.connectivityTimeout if timeout is None else timeout
        
        if connectivityTimeout <= 0:
            raise ValueError('connectivity timeout must be positive')
            
        if self._agent.waitForConnected(connectivityTimeout):
            return UDPTransport(self._agent, iceRole=self._iceRole, stunURL=self.stunURL)
            
        return None

    def close(self):
        self._closePortMapping()

        if self._agent:
            self._agent.close()
            self._agent = None

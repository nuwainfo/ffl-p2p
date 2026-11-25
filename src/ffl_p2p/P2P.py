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
import threading
import time
import uuid

from typing import Optional
from urllib.parse import urlsplit

from .Connection import Connection
from .Native import NativeQUICCredentials, NativeUnavailableError
from .Signaling import HTTPSignalingClient, P2PAnswer, P2POffer, P2PSignalingError
from .TCP import TCPConnector, TCPPublisher
from .Configuration import ICEConfiguration
from .UDP import UDPConnector


logger = logging.getLogger(__name__)


class P2PConnectivityTimeout(TimeoutError):
    """ICE connectivity checks ran, but no direct UDP candidate pair connected in time."""


class P2PUDPUnavailableError(RuntimeError):
    """Direct UDP could not be set up for this session, so no ICE check was attempted.

    Unlike P2PConnectivityTimeout this says nothing about whether the network
    permits UDP, so applications should keep their other ICE-based fallbacks.
    """


class P2PPublisher:
    def __init__(self, httpPort: int, configuration: Optional[ICEConfiguration] = None,
                 useUDPPortMapping: bool = True, useTCPPortMapping: bool = True,
                 tcpLocalHosts=None, tcpPath: str = '', udpHandler=None,
                 tcpCertificateFingerprint: Optional[str] = None):
        self.sessionId = uuid.uuid4().hex
        self.configuration = configuration or ICEConfiguration()
        self.tcpCertificateFingerprint = tcpCertificateFingerprint
        self._udp = UDPConnector(self.configuration, usePortMapping=useUDPPortMapping)
        self._tcp = TCPPublisher(
            httpPort, usePortMapping=useTCPPortMapping, localHosts=tcpLocalHosts, path=tcpPath,
            useTLS=tcpCertificateFingerprint is not None,
        )
        self._offer = None
        self._answerAccepted = threading.Event()
        
        try:
            self._quicCredentials = NativeQUICCredentials()
        except NativeUnavailableError:
            self._quicCredentials = None
            
        self._udpHandler = udpHandler
        self._udpHandlerThread = None
        self._udpHandlerError = None

    def createOffer(self) -> P2POffer:
        if self._offer is None:
            self._offer = P2POffer(
                sessionId=self.sessionId,
                udpDescription=self._gatherUDP(),
                tcpEndpoints=self._tcp.endpoints(),
                quicCertificate=(self._quicCredentials.certificate if self._quicCredentials else None),
                tcpCertificateFingerprint=self.tcpCertificateFingerprint,
            )
            
        return self._offer

    def _gatherUDP(self) -> Optional[str]:
        try:
            return self._udp.gather()
        except UDPConnector.SETUP_ERRORS as error:
            # The offer still carries the TCP endpoints; udpDescription=None tells
            # the connector that this session has no direct UDP transport.
            logger.warning('Direct UDP is unavailable for P2P session %s: %s', self.sessionId[:8], error)
            self._udp.close()
            return None

    @property
    def _udpUnavailable(self) -> bool:
        return self._offer is not None and self._offer.udpDescription is None

    def acceptAnswer(self, answer: P2PAnswer):
        if answer.sessionId != self.sessionId:
            raise ValueError('P2P answer sessionId does not match offer')

        if self._udpUnavailable:
            raise RuntimeError('direct UDP is unavailable for this P2P session')

        self._udp.setRemoteDescription(answer.udpDescription)
        self._answerAccepted.set()
        if self._udpHandler is not None and self._udpHandlerThread is None:
            self._udpHandlerThread = threading.Thread(
                target=self._runUDPHandler,
                name=f'ffl-p2p-udp-{self.sessionId[:8]}',
                daemon=True,
            )
            self._udpHandlerThread.start()

    def _runUDPHandler(self):
        transport = None
        try:
            transport = self.waitForUDP()
            if transport is not None:
                self._udpHandler(transport)
        except Exception as error:
            self._udpHandlerError = error
        finally:
            if transport is not None:
                transport.close()

    @property
    def udpHandlerError(self):
        return self._udpHandlerError

    def waitForUDP(self, timeout: Optional[float] = None):
        if self._udpUnavailable:
            return None

        connectivityTimeout = self.configuration.connectivityTimeout if timeout is None else timeout
        if not self._answerAccepted.wait(connectivityTimeout):
            return None
            
        transport = self._udp.connect(connectivityTimeout)
        if transport is not None:
            transport.quicCredentials = self._quicCredentials
            
        return transport

    def close(self):
        self._tcp.close()
        self._udp.close()


class P2PConnector:
    def __init__(self, configuration: Optional[ICEConfiguration] = None, useUDPPortMapping: bool = True):
        self.configuration = configuration or ICEConfiguration()
        self.useUDPPortMapping = useUDPPortMapping

    def connect(self, baseURL: str, timeout: Optional[float] = None, preference: str = 'auto', headers=None) -> Optional[Connection]:
        if preference not in {'auto', 'tcp', 'udp'}:
            raise ValueError("preference must be 'auto', 'tcp', or 'udp'")

        connectivityTimeout = self.configuration.connectivityTimeout if timeout is None else timeout
        if connectivityTimeout <= 0:
            raise ValueError('timeout must be positive')

        startedAt = time.monotonic()
        signaling = HTTPSignalingClient(baseURL, headers=headers)
        offer = signaling.getOffer()

        # UDP is negotiated before the TCP probe so the publisher's ICE checks run
        # while TCP is tried, but a UDP failure never prevents the TCP attempt.
        udp = None
        udpError = None
        if preference != 'tcp':
            try:
                udp = self._negotiateUDP(signaling, offer)
            except P2PUDPUnavailableError as error:
                if preference == 'udp':
                    raise

                logger.warning('Direct UDP is unavailable; trying direct TCP only: %s', error)
                udpError = error

        try:
            if preference != 'udp':
                # Headers that authorize HTTPS signaling must not be downgraded to a plaintext probe.
                tcpTransport = TCPConnector(
                    offer.tcpEndpoints,
                    certificateFingerprint=offer.tcpCertificateFingerprint,
                    requireTLS=urlsplit(baseURL).scheme == 'https',
                ).connect(min(connectivityTimeout, 0.75), headers=headers)
                
                if tcpTransport:
                    return Connection(tcpTransport, connectTime=time.monotonic() - startedAt)

            if preference == 'tcp':
                return None

            if udpError is not None:
                raise udpError

            # Signaling and candidate gathering have their own timeouts.  Give
            # ICE connectivity checks the complete configured budget.
            udpTransport = udp.connect(connectivityTimeout)
            if udpTransport is None:
                raise P2PConnectivityTimeout(
                    'direct UDP ICE candidate connectivity timed out'
                )

            udpTransport.quicCertificate = offer.quicCertificate
            udp = None
            
            return Connection(udpTransport, connectTime=time.monotonic() - startedAt)
        finally:
            if udp is not None:
                udp.close()

    def _negotiateUDP(self, signaling: HTTPSignalingClient, offer: P2POffer) -> UDPConnector:
        if offer.udpDescription is None:
            raise P2PUDPUnavailableError('publisher offered no direct UDP transport')

        udp = UDPConnector(self.configuration, usePortMapping=self.useUDPPortMapping)
        try:
            # The answerer sets the remote offer before local gathering.  libjuice
            # therefore enters the CONTROLLED role before gather() instead of
            # defaulting both peers to CONTROLLING and relying on glare repair.
            udp.setRemoteDescription(offer.udpDescription)
            localDescription = udp.gather()
            signaling.sendAnswer(P2PAnswer(offer.sessionId, localDescription))
        except (*UDPConnector.SETUP_ERRORS, P2PSignalingError) as error:
            udp.close()
            raise P2PUDPUnavailableError(f'direct UDP setup failed: {error}') from error

        return udp


class P2PDownloadMixin:
    """Small FFL integration seam; Downloader/HTTP responsibilities stay outside.

    The concrete Downloader owns URL parsing, authentication, progress, resume,
    checksums and HTTP fallback. This mixin only creates a native P2P connection.
    """

    def __init__(self, *args, p2pConfiguration: Optional[ICEConfiguration] = None, **kwargs):
        super().__init__(*args, **kwargs)
        self._p2pConnector = P2PConnector(configuration=p2pConfiguration)

    def connectP2P(self, baseURL: str, timeout: Optional[float] = None, preference: str = 'auto'):
        return self._p2pConnector.connect(baseURL, timeout=timeout, preference=preference)

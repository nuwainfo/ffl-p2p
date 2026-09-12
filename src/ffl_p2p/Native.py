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
import time

from collections import deque
from dataclasses import dataclass
from typing import Optional

from .RuntimeConfiguration import RuntimeConfiguration

try:
    from . import _ffl_p2p as nativeModule
except ImportError:
    # Some embedded builds expose the extension as a top-level CPython builtin.
    import _ffl_p2p as nativeModule


class NativeUnavailableError(RuntimeError):
    pass


_NATIVE_LOG_LEVEL_NAMES = (
    (logging.DEBUG, 'debug'),
    (logging.INFO, 'info'),
    (logging.WARNING, 'warning'),
    (logging.ERROR, 'error'),
    (logging.CRITICAL, 'fatal'),
)

_nativeLoggingConfigured = False


def setLogLevel(level: int):
    """Set the native libjuice/libplum level from a standard ``logging`` level."""
    if not isinstance(level, int):
        raise TypeError('log level must be an integer from the logging module')

    for loggingLevel, nativeLevelName in _NATIVE_LOG_LEVEL_NAMES:
        if level <= loggingLevel:
            nativeModule.setLogLevel(nativeLevelName)
            return

    nativeModule.setLogLevel('none')


def _configureNativeLogging():
    global _nativeLoggingConfigured
    if _nativeLoggingConfigured:
        return

    configuration = RuntimeConfiguration()
    setLogLevel(configuration.nativeLoggingLevel)
    _nativeLoggingConfigured = True


@dataclass(frozen=True)
class PortMappingInfo:
    state: str
    mappingProtocol: str
    internalPort: int
    externalHost: str
    externalPort: int

    @classmethod
    def fromDict(cls, data):
        return cls(**data)


class NativePortMapping:
    def __init__(self, protocol: str, internalPort: int):
        if nativeModule is None:
            raise NativeUnavailableError("_ffl_p2p native module is not built")

        _configureNativeLogging()
        self._mapping = nativeModule.PortMapping(protocol=protocol, internalPort=internalPort)

    def query(self) -> PortMappingInfo:
        return PortMappingInfo.fromDict(self._mapping.query())

    def wait(self, timeout: float = 0.5, interval: float = 0.02) -> PortMappingInfo:
        deadline = time.monotonic() + timeout

        info = self.query()
        while info.state == 'pending' and time.monotonic() < deadline:
            time.sleep(interval)
            info = self.query()

        return info

    def close(self):
        if self._mapping is None:
            return

        self._mapping.close()
        self._mapping = None


class NativeQUICCredentials:
    def __init__(self):
        if nativeModule is None:
            raise NativeUnavailableError("_ffl_p2p native module is not built")

        self._credentials = nativeModule.QUICCredentials()

    @property
    def certificate(self) -> str:
        return self._credentials.certificate

    @property
    def native(self):
        return self._credentials


class NativeQUICSession:
    def __init__(self, session):
        self._session = session

    @classmethod
    def client(cls, certificate: str):
        if nativeModule is None:
            raise NativeUnavailableError("_ffl_p2p native module is not built")

        return cls(nativeModule.QUICSession(role='client', certificate=certificate))

    @classmethod
    def server(cls, credentials: NativeQUICCredentials, initialPacket: Optional[bytes] = None):
        if nativeModule is None:
            raise NativeUnavailableError("_ffl_p2p native module is not built")

        kwargs = {'role': 'server', 'credentials': credentials.native}
        if initialPacket is not None:
            kwargs['initialPacket'] = initialPacket

        return cls(nativeModule.QUICSession(**kwargs))

    @property
    def handshakeComplete(self):
        return self._session.handshakeComplete

    @property
    def handshakeConfirmed(self):
        return self._session.handshakeConfirmed

    @property
    def peerFinished(self):
        return self._session.peerFinished

    @property
    def pendingWrite(self):
        return self._session.pendingWrite

    @property
    def bufferedWriteBytes(self):
        return self._session.bufferedWriteBytes

    @property
    def writeAcknowledged(self):
        return self._session.writeAcknowledged

    @property
    def streamClosed(self):
        return self._session.streamClosed

    @property
    def supportsRuntimeV2(self):
        return bool(getattr(self._session, 'runtimeV2', False))

    @property
    def workerIndex(self) -> int:
        return self._session.workerIndex

    @property
    def workerCount(self) -> int:
        return self._session.workerCount

    def start(self, agent, aggregate: bool = True, timeout: float = 5.0):
        if not self.supportsRuntimeV2:
            raise NativeUnavailableError('native QUIC support is required')

        self._session.start(agent=agent.native, aggregate=aggregate, timeout=timeout)

    def queueAsync(self, data: bytes, fin: bool = False):
        self._session.queueAsync(data, fin=fin)

    def wait(self, timeout: float = 0.01):
        return self._session.wait(timeout=timeout)

    def checkError(self):
        self._session.checkError()

    def read(self):
        return self._session.read()

    def close(self):
        if self._session is not None:
            self._session.close()
            self._session = None


class NativeAgent:
    _CONNECTED_STATES = set()

    def __init__(self, stunServer: Optional[tuple[str, int]] = None,
                 bindAddress: Optional[str] = None, portBegin: int = 0, portEnd: int = 0):
        if nativeModule is None:
            raise NativeUnavailableError("_ffl_p2p native module is not built")

        _configureNativeLogging()
        stunHost = None if stunServer is None else stunServer[0]
        stunPort = 3478 if stunServer is None else stunServer[1]

        self._agent = nativeModule.Agent(
            stunHost=stunHost,
            stunPort=stunPort,
            bindAddress=bindAddress,
            portBegin=portBegin,
            portEnd=portEnd,
        )

        self._pendingEvents = deque()
        if not self._CONNECTED_STATES:
            self._CONNECTED_STATES.update({
                nativeModule.JUICE_STATE_CONNECTED,
                nativeModule.JUICE_STATE_COMPLETED,
            })

    @property
    def native(self):
        if self._agent is None:
            raise RuntimeError('agent is closed')

        return self._agent

    def gather(self):
        self._agent.gather()

    def holdGathering(self):
        self._agent.holdGathering()

    def releaseGathering(self):
        self._agent.releaseGathering()

    @property
    def localPort(self) -> int:
        return self._agent.localPort()

    @property
    def localDescription(self) -> str:
        return self._agent.localDescription()

    @property
    def state(self) -> int:
        return self._agent.state()

    @property
    def connected(self) -> bool:
        return self.state in self._CONNECTED_STATES

    @property
    def selectedAddresses(self):
        return self._agent.selectedAddresses()

    def setRemoteDescription(self, sdp: str):
        self._agent.setRemoteDescription(sdp)

    def addRemoteCandidate(self, candidateSDP: str):
        self._agent.addRemoteCandidate(candidateSDP)

    def remoteGatheringDone(self):
        self._agent.remoteGatheringDone()

    def addMappedCandidate(self, address: str, port: int):
        self._agent.addMappedCandidate(address, port)

    def send(self, data: bytes):
        self._agent.send(data)

    def pollEvents(self, maxEvents: int = 128):
        events = list(self._pendingEvents)
        self._pendingEvents.clear()
        events.extend(self._agent.pollEvents(maxEvents=maxEvents))
        return events

    def waitForEvent(self, eventType: str, timeout: float):
        # Preserve every non-selected event.  The old implementation returned
        # the first matching receive event from a native batch and silently
        # discarded the remaining datagrams, which is fatal for QUIC bursts.
        for index, event in enumerate(self._pendingEvents):
            if event['type'] == eventType:
                del self._pendingEvents[index]
                return event

        deadline = time.monotonic() + max(0.0, timeout)
        firstPoll = True
        while firstPoll or time.monotonic() < deadline:
            firstPoll = False
            remaining = max(0.0, deadline - time.monotonic())
            waitTimeout = remaining if timeout > 0 else 0.0

            # Native waitEvents is event-driven: libjuice's callback thread wakes
            # this consumer immediately.  Do not reintroduce a millisecond-scale
            # polling sleep here; it directly caps QUIC throughput on low-RTT paths.
            if hasattr(self._agent, 'waitEvents'):
                events = self._agent.waitEvents(timeout=waitTimeout, maxEvents=128)
            else:
                events = self._agent.pollEvents(maxEvents=128)

            selected = None
            for event in events:
                if selected is None and event['type'] == eventType:
                    selected = event
                else:
                    self._pendingEvents.append(event)

            if selected is not None:
                return selected

            if timeout <= 0 or time.monotonic() >= deadline:
                break

            if not hasattr(self._agent, 'waitEvents'):
                time.sleep(min(0.005, max(0.0, deadline - time.monotonic())))

        return None

    def waitForConnected(self, timeout: float = 3.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pollEvents()
            if self.connected:
                return True

            time.sleep(0.005)

        return self.connected

    def close(self):
        if self._agent is None:
            return

        self._agent.close()
        self._agent = None

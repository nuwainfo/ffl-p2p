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

import time


@dataclass(frozen=True)
class QUICWriteFlowConfiguration:
    highWatermarkBytes: int
    lowWatermarkBytes: int

    @classmethod
    def fromHighWatermarkBytes(cls, highWatermarkBytes: int):
        return cls(
            highWatermarkBytes=highWatermarkBytes,
            lowWatermarkBytes=max(1, highWatermarkBytes // 2),
        )


class QUICWriteFlowController:
    """Bound producer memory while keeping the native QUIC worker fed."""

    def __init__(self, configuration: QUICWriteFlowConfiguration):
        self.configuration = configuration

    def queue(
        self,
        session,
        data: bytes,
        fin: bool,
        deadline: float,
        waitForChange,
        raiseRuntimeError,
    ):
        if data:
            self._waitForCapacity(
                session,
                len(data),
                deadline,
                waitForChange,
                raiseRuntimeError,
            )

        session.queueAsync(data, fin=fin)

    def _waitForCapacity(
        self,
        session,
        dataSize: int,
        deadline: float,
        waitForChange,
        raiseRuntimeError,
    ):
        bufferedBytes = session.bufferedWriteBytes
        if self._hasCapacity(bufferedBytes, dataSize):
            return

        while bufferedBytes > self.configuration.lowWatermarkBytes:
            self._wait(deadline, waitForChange, raiseRuntimeError)
            bufferedBytes = session.bufferedWriteBytes

    def _hasCapacity(self, bufferedBytes: int, dataSize: int) -> bool:
        if bufferedBytes == 0 and dataSize > self.configuration.highWatermarkBytes:
            return True

        return bufferedBytes + dataSize <= self.configuration.highWatermarkBytes

    @staticmethod
    def _wait(deadline: float, waitForChange, raiseRuntimeError):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('QUIC stream ACK backpressure timed out')

        waitForChange(min(0.05, remaining))
        raiseRuntimeError()

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
import os


_TRUE_VALUES = {'1', 'true', 'yes', 'on'}
_DEFAULT_QUIC_WRITE_BUFFER_BYTES = 8 * 1024 * 1024
_NATIVE_LOG_LEVELS_BY_NAME = {
    'DEBUG': logging.DEBUG,
    'INFO': logging.INFO,
    'WARNING': logging.WARNING,
    'WARN': logging.WARNING,
    'ERROR': logging.ERROR,
    'CRITICAL': logging.CRITICAL,
    'FATAL': logging.CRITICAL,
    'NONE': logging.CRITICAL + 1,
}


class RuntimeConfiguration:
    """Read Python-owned runtime settings from one environment source."""

    def __init__(self, environment=None):
        self._environment = os.environ if environment is None else environment

    @property
    def nativeLoggingLevel(self) -> int:
        configuredName = self._environment.get(
            'FFL_P2P_NATIVE_LOGGING_LEVEL',
            'ERROR',
        ).strip().upper()
        
        configuredLevel = _NATIVE_LOG_LEVELS_BY_NAME.get(configuredName)
        
        if configuredLevel is None:
            raise ValueError(
                'FFL_P2P_NATIVE_LOGGING_LEVEL must be DEBUG, INFO, WARNING, '
                'ERROR, CRITICAL, or NONE'
            )

        return configuredLevel

    @property
    def aggregateQUICPackets(self) -> bool:
        return not self._isEnvironmentEnabled('FFL_P2P_QUIC_DISABLE_BATCH')

    @property
    def quicWriteBufferHighWatermarkBytes(self) -> int:
        configured = self._environment.get(
            'FFL_P2P_QUIC_WRITE_BUFFER_MIB',
            '',
        ).strip()
        
        if not configured:
            return _DEFAULT_QUIC_WRITE_BUFFER_BYTES

        try:
            sizeMiB = int(configured)
        except ValueError as error:
            raise ValueError(
                'FFL_P2P_QUIC_WRITE_BUFFER_MIB must be an integer'
            ) from error

        if sizeMiB <= 0:
            raise ValueError(
                'FFL_P2P_QUIC_WRITE_BUFFER_MIB must be positive'
            )

        return sizeMiB * 1024 * 1024

    def _isEnvironmentEnabled(self, name: str) -> bool:
        configured = self._environment.get(name, '')
        return configured.strip().lower() in _TRUE_VALUES


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
import unittest

from ffl_p2p.RuntimeConfiguration import RuntimeConfiguration


class RuntimeConfigurationTest(unittest.TestCase):
    def testDefaults(self):
        configuration = RuntimeConfiguration({})

        self.assertEqual(logging.ERROR, configuration.nativeLoggingLevel)
        self.assertTrue(configuration.aggregateQUICPackets)
        self.assertEqual(
            8 * 1024 * 1024,
            configuration.quicWriteBufferHighWatermarkBytes,
        )

    def testConfiguredValues(self):
        configuration = RuntimeConfiguration({
            'FFL_P2P_NATIVE_LOGGING_LEVEL': 'debug',
            'FFL_P2P_QUIC_DISABLE_BATCH': 'YES',
            'FFL_P2P_QUIC_WRITE_BUFFER_MIB': '16',
        })

        self.assertEqual(logging.DEBUG, configuration.nativeLoggingLevel)
        self.assertFalse(configuration.aggregateQUICPackets)
        self.assertEqual(
            16 * 1024 * 1024,
            configuration.quicWriteBufferHighWatermarkBytes,
        )

    def testInvalidLoggingLevelFailsFast(self):
        configuration = RuntimeConfiguration({
            'FFL_P2P_NATIVE_LOGGING_LEVEL': 'invalid',
        })

        with self.assertRaises(ValueError):
            configuration.nativeLoggingLevel

    def testInvalidWriteBufferFailsFast(self):
        for value in ('invalid', '0', '-1'):
            with self.subTest(value=value):
                configuration = RuntimeConfiguration({
                    'FFL_P2P_QUIC_WRITE_BUFFER_MIB': value,
                })

                with self.assertRaises(ValueError):
                    configuration.quicWriteBufferHighWatermarkBytes


if __name__ == '__main__':
    unittest.main()

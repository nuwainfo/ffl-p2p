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
import unittest

from concurrent.futures import ThreadPoolExecutor
from unittest import mock

import ffl_p2p.Native as Native
from ffl_p2p.Native import (
    NativeAgent,
    NativePortMapping,
    NativeQUICCredentials,
    setLogLevel,
)


class NativeTest(unittest.TestCase):
    def testNativeLogLevelValidation(self):
        with mock.patch('ffl_p2p.Native.nativeModule') as nativeModule:
            for loggingLevel, nativeLevelName in (
                (logging.DEBUG, 'debug'),
                (logging.INFO, 'info'),
                (logging.WARNING, 'warning'),
                (logging.ERROR, 'error'),
                (logging.CRITICAL, 'fatal'),
                (logging.CRITICAL + 1, 'none'),
            ):
                with self.subTest(loggingLevel=loggingLevel):
                    setLogLevel(loggingLevel)
                    nativeModule.setLogLevel.assert_called_with(nativeLevelName)

        with self.assertRaises(TypeError):
            setLogLevel('error')

    def testNativeLoggingUsesEnvironmentOrErrorDefault(self):
        with mock.patch('ffl_p2p.Native.nativeModule') as nativeModule:
            with mock.patch('ffl_p2p.Native._nativeLoggingConfigured', False):
                with mock.patch.dict(os.environ, {}, clear=True):
                    Native._configureNativeLogging()

            nativeModule.setLogLevel.assert_called_once_with('error')

        with mock.patch('ffl_p2p.Native.nativeModule') as nativeModule:
            with mock.patch('ffl_p2p.Native._nativeLoggingConfigured', False):
                with mock.patch.dict(os.environ, {
                    'FFL_P2P_NATIVE_LOGGING_LEVEL': 'DEBUG',
                }, clear=True):
                    Native._configureNativeLogging()

            nativeModule.setLogLevel.assert_called_once_with('debug')

    def testNativePortMapping(self):
        mapping = NativePortMapping('udp', 40000)

        try:
            info = mapping.wait()
            self.assertIn(info.state, {'pending', 'success', 'failure'})
            if info.state != 'success':
                return

            self.assertIn(info.mappingProtocol, {'pcp', 'nat-pmp', 'upnp', 'direct'})
            self.assertTrue(info.externalHost)
            self.assertGreater(info.externalPort, 0)
        finally:
            mapping.close()

    def testNativeAgentExposesCamelCaseSurface(self):
        agent = NativeAgent()

        try:
            agent.gather()
            self.assertGreater(agent.localPort, 0)
            self.assertTrue(agent.localDescription)
        finally:
            agent.close()

    def testQUICCredentialsSupportConcurrentGeneration(self):
        def generateCredential(taskIndex):
            return taskIndex, NativeQUICCredentials().certificate

        with ThreadPoolExecutor(max_workers=4) as executor:
            results = list(executor.map(generateCredential, range(4)))

        self.assertEqual(4, len(results))
        for taskIndex, certificate in results:
            with self.subTest(taskIndex=taskIndex):
                self.assertIn('BEGIN CERTIFICATE', certificate)


if __name__ == '__main__':
    unittest.main()

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

import inspect
import unittest
from unittest.mock import patch

from ffl_p2p import ICEConfiguration, ICEServer, P2PConnector, P2PPublisher
from ffl_p2p.UDP import UDPConnector


class ConfigurationTest(unittest.TestCase):
    def testFirstSTUNServerMatchesAioiceSelection(self):
        configuration = ICEConfiguration(
            iceServers=[
                ICEServer(urls=['stun:a.example', 'stun:b.example:19302']),
                ICEServer(urls='stun:c.example'),
            ]
        )

        self.assertEqual(['stun:a.example'], configuration._nativeURLs)
        self.assertEqual('stun:a.example', configuration.stunURL)
        self.assertEqual(('a.example', 3478), configuration.stunEndpoint)

    def testSTUNURLParsingSupportsExplicitPortAndIPv6(self):
        explicitPort = ICEConfiguration(iceServers=[ICEServer(urls='stun:stun.example:19302')])
        ipv6 = ICEConfiguration(iceServers=[ICEServer(urls='stun:[2001:db8::1]:3479')])

        self.assertEqual(('stun.example', 19302), explicitPort.stunEndpoint)
        self.assertEqual(('2001:db8::1', 3479), ipv6.stunEndpoint)

    def testTURNIsNotSupported(self):
        configuration = ICEConfiguration(iceServers=[ICEServer(urls='turn:relay.example:3478')])

        with self.assertRaises(ValueError):
            configuration._nativeURLs

    def testUDPConnectorPassesOnlySelectedSTUNServerToNativeAgent(self):
        configuration = ICEConfiguration(
            iceServers=[ICEServer(urls=['stun:first.example:1234', 'stun:second.example:5678'])],
            bindAddress='127.0.0.1',
            portBegin=40000,
            portEnd=40100,
        )

        with patch('ffl_p2p.UDP.NativeAgent') as nativeAgent:
            connector = UDPConnector(configuration, usePortMapping=False)
            connector._ensureAgent()

        nativeAgent.assert_called_once_with(
            stunServer=('first.example', 1234),
            bindAddress='127.0.0.1',
            portBegin=40000,
            portEnd=40100,
        )

    def testPublicP2PAPIUsesConfigurationNotLegacyUDPConfig(self):
        connectorSignature = inspect.signature(P2PConnector)
        publisherSignature = inspect.signature(P2PPublisher)
        configurationSignature = inspect.signature(ICEConfiguration)

        self.assertIn('configuration', connectorSignature.parameters)
        self.assertIn('configuration', publisherSignature.parameters)
        self.assertIn('iceServers', configurationSignature.parameters)

        self.assertNotIn('udpConfig', connectorSignature.parameters)
        self.assertNotIn('udpConfig', publisherSignature.parameters)
        self.assertNotIn('stunHost', configurationSignature.parameters)
        self.assertNotIn('stunPort', configurationSignature.parameters)


if __name__ == '__main__':
    unittest.main()

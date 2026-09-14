#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0

import unittest
from unittest.mock import Mock, patch

from ffl_p2p.Native import PortMappingInfo
from ffl_p2p.TCP import TCPPublisher
from ffl_p2p.UDP import UDPConnector


class FakeAgent:
    def __init__(self):
        self.localPort = 40000
        self.localDescription = 'local-sdp'
        self.releaseCount = 0
        self.addMappedCandidate = Mock()

    def holdGathering(self):
        return

    def releaseGathering(self):
        self.releaseCount += 1

    def gather(self):
        return

    def waitForEvent(self, name, timeout):
        return object()


class PortMappingTest(unittest.TestCase):
    def _gatherWithMapping(self, mappingFactory):
        agent = FakeAgent()
        connector = UDPConnector(usePortMapping=True)

        with patch('ffl_p2p.UDP.NativeAgent', return_value=agent), \
             patch('ffl_p2p.UDP.NativePortMapping', mappingFactory):
            description = connector.gather()

        self.assertEqual('local-sdp', description)
        self.assertEqual(1, agent.releaseCount)
        return agent

    def testUDPContinuesWhenPortMappingCreationFails(self):
        with self.assertLogs('ffl_p2p.UDP', level='DEBUG') as logs:
            self._gatherWithMapping(
                Mock(side_effect=RuntimeError('mapping unavailable'))
            )

        self.assertIn(
            'UDP port mapping unavailable: mapping unavailable',
            '\n'.join(logs.output),
        )

    def testUDPContinuesWhenPortMappingQueryFails(self):
        mapping = Mock()
        mapping.wait.side_effect = RuntimeError('mapping query failed')

        self._gatherWithMapping(Mock(return_value=mapping))
        mapping.close.assert_called_once_with()

    def testUDPContinuesWhenMappedCandidateInjectionFails(self):
        mapping = Mock()
        mapping.wait.return_value = PortMappingInfo(
            'success',
            'pcp',
            40000,
            '203.0.113.1',
            41000,
        )
        agent = FakeAgent()
        agent.addMappedCandidate.side_effect = RuntimeError('candidate injection failed')
        connector = UDPConnector(usePortMapping=True)

        with patch('ffl_p2p.UDP.NativeAgent', return_value=agent), \
             patch('ffl_p2p.UDP.NativePortMapping', return_value=mapping):
            self.assertEqual('local-sdp', connector.gather())

        self.assertEqual(1, agent.releaseCount)
        mapping.close.assert_called_once_with()

    def testUDPContinuesWhenMappedCandidateIsInvalid(self):
        mapping = Mock()
        mapping.wait.return_value = PortMappingInfo(
            'success',
            'pcp',
            40000,
            '203.0.113.1',
            0,
        )
        agent = FakeAgent()
        agent.addMappedCandidate.side_effect = ValueError('port out of range')
        connector = UDPConnector(usePortMapping=True)

        with patch('ffl_p2p.UDP.NativeAgent', return_value=agent), \
             patch('ffl_p2p.UDP.NativePortMapping', return_value=mapping):
            self.assertEqual('local-sdp', connector.gather())

        self.assertEqual(1, agent.releaseCount)
        mapping.close.assert_called_once_with()

    def testTCPKeepsLocalEndpointsWhenPortMappingQueryFails(self):
        mapping = Mock()
        mapping.wait.side_effect = RuntimeError('mapping query failed')
        publisher = TCPPublisher(8080, localHosts=['127.0.0.1'])

        with patch('ffl_p2p.TCP.NativePortMapping', return_value=mapping):
            self.assertEqual(
                ['http://127.0.0.1:8080'],
                publisher.endpoints(),
            )

        mapping.close.assert_called_once_with()


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0

import io
import unittest
import urllib.error
from unittest.mock import patch

from ffl_p2p.Signaling import HTTPSignalingClient, P2PAnswer, P2PSignalingError


class SignalingTest(unittest.TestCase):
    def testOfferHTTPErrorIncludesSignalingStage(self):
        error = urllib.error.HTTPError(
            'https://example.invalid/p2p/offer',
            403,
            'Forbidden',
            {},
            io.BytesIO(),
        )

        with patch('urllib.request.urlopen', side_effect=error):
            with self.assertRaisesRegex(
                P2PSignalingError,
                r'GET /p2p/offer failed: HTTP 403 Forbidden',
            ):
                HTTPSignalingClient('https://example.invalid').getOffer()

    def testAnswerHTTPErrorIncludesSignalingStage(self):
        error = urllib.error.HTTPError(
            'https://example.invalid/p2p/answer',
            403,
            'Forbidden',
            {},
            io.BytesIO(),
        )

        with patch('urllib.request.urlopen', side_effect=error):
            with self.assertRaisesRegex(
                P2PSignalingError,
                r'POST /p2p/answer failed: HTTP 403 Forbidden',
            ):
                HTTPSignalingClient('https://example.invalid').sendAnswer(
                    P2PAnswer('session', 'sdp')
                )


if __name__ == '__main__':
    unittest.main()

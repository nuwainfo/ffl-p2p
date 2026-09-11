#!/usr/bin/env python
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
#
# FastFileLink CLI - Fast, no-fuss file sharing
# Copyright (C) 2025-2026 FastFileLink contributors

import time
import unittest

from ffl_p2p.QUICFlow import QUICWriteFlowConfiguration, QUICWriteFlowController


class FakeSession:
    def __init__(self, bufferedWriteBytes=0):
        self.bufferedWriteBytes = bufferedWriteBytes
        self.queued = []

    def queueAsync(self, data, fin=False):
        self.queued.append((data, fin))


class QUICFlowTest(unittest.TestCase):
    @staticmethod
    def _controller(highWatermark=8 * 1024 * 1024):
        return QUICWriteFlowController(QUICWriteFlowConfiguration(
            highWatermarkBytes=highWatermark,
            lowWatermarkBytes=highWatermark // 2,
        ))

    @staticmethod
    def _unexpectedWait(waitTimeout):
        raise AssertionError(f'write unexpectedly waited for {waitTimeout}')

    @staticmethod
    def _raiseNoError():
        return None

    def testWriteDoesNotWaitForNativeSubmission(self):
        session = FakeSession()
        controller = self._controller()

        controller.queue(
            session, b'payload', False, time.monotonic() + 1,
            self._unexpectedWait, self._raiseNoError)

        self.assertEqual([(b'payload', False)], session.queued)

    def testWritePipelineUsesHighLowWatermarks(self):
        highWatermark = 8 * 1024 * 1024
        session = FakeSession(bufferedWriteBytes=highWatermark - 1)
        controller = self._controller(highWatermark=highWatermark)
        waitCalls = []

        def waitForChange(waitTimeout):
            waitCalls.append(waitTimeout)
            session.bufferedWriteBytes = highWatermark // 2

        controller.queue(
            session, b'x' * 1024, False, time.monotonic() + 1,
            waitForChange, self._raiseNoError)

        self.assertEqual(1, len(waitCalls))
        self.assertEqual(1, len(session.queued))

    def testOversizedWriteCanStartWhenPipelineIsEmpty(self):
        highWatermark = 1024
        session = FakeSession()
        controller = self._controller(highWatermark=highWatermark)

        controller.queue(
            session, b'x' * (highWatermark + 1), False, time.monotonic() + 1,
            self._unexpectedWait, self._raiseNoError)

        self.assertEqual(1, len(session.queued))


if __name__ == '__main__':
    unittest.main()

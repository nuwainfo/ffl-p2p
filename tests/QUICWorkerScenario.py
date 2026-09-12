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

import argparse

from concurrent.futures import ThreadPoolExecutor

from ffl_p2p.Native import NativeQUICCredentials, NativeQUICSession

from QUICTransferHarness import QUICTransferHarness


def verifyWorkerAssignments(expectedWorkerCount: int, sessionCount: int):
    credentials = NativeQUICCredentials()
    sessions = [
        NativeQUICSession.client(credentials.certificate)
        for sessionIndex in range(sessionCount)
    ]

    try:
        actualWorkerCounts = {session.workerCount for session in sessions}
        if actualWorkerCounts != {expectedWorkerCount}:
            raise AssertionError(
                f'worker count mismatch: expected {expectedWorkerCount}, '
                f'got {actualWorkerCounts}'
            )

        expectedWorkerIndexes = [
            sessionIndex % expectedWorkerCount
            for sessionIndex in range(sessionCount)
        ]
    
        actualWorkerIndexes = [session.workerIndex for session in sessions]
        if actualWorkerIndexes != expectedWorkerIndexes:
            raise AssertionError(
                f'worker assignment mismatch: expected {expectedWorkerIndexes}, '
                f'got {actualWorkerIndexes}'
            )
    finally:
        for session in sessions:
            session.close()


def runConcurrentTransfers(transferCount: int, payloadMiB: int):
    payloadSize = payloadMiB * 1024 * 1024

    def transfer(transferIndex: int):
        pattern = bytes(((byteIndex + transferIndex) % 251 for byteIndex in range(251)))
        payload = (pattern * ((payloadSize + len(pattern) - 1) // len(pattern)))[:payloadSize]
        QUICTransferHarness().transfer(payload, timeout=30)

    with ThreadPoolExecutor(max_workers=transferCount) as executor:
        futures = [
            executor.submit(transfer, transferIndex)
            for transferIndex in range(transferCount)
        ]
        for future in futures:
            future.result()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--expected-workers',
        dest='expectedWorkerCount',
        type=int,
        required=True,
    )
    parser.add_argument('--sessions', dest='sessionCount', type=int, default=8)
    parser.add_argument('--transfers', dest='transferCount', type=int, default=0)
    parser.add_argument('--payload-mib', dest='payloadMiB', type=int, default=2)
    arguments = parser.parse_args()

    verifyWorkerAssignments(arguments.expectedWorkerCount, arguments.sessionCount)
    if arguments.transferCount:
        runConcurrentTransfers(arguments.transferCount, arguments.payloadMiB)


if __name__ == '__main__':
    main()

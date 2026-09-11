#!/usr/bin/env python3
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

"""Developer benchmark for supported QUIC datapath and write-window controls.

Every timing run uses the same end-to-end transfer harness as the correctness
suite, so payload integrity and clean close are always checked.
"""

from __future__ import annotations

import argparse
import contextlib
import os
import statistics
import sys
import time

from dataclasses import dataclass
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src'))
sys.path.insert(0, str(ROOT / 'tests'))

from QUICTransferHarness import QUICTransferHarness  # noqa: E402


@dataclass(frozen=True)
class PerformanceCase:
    name: str
    environment: dict[str, str]


class QUICPerformanceBenchmark:
    CONTROL_ENVIRONMENT = (
        'FFL_P2P_QUIC_DISABLE_GSO',
        'FFL_P2P_QUIC_DISABLE_RECV_BATCH',
        'FFL_P2P_QUIC_WRITE_BUFFER_MIB',
    )

    DATAPATH_CASES = (
        PerformanceCase('fastPath', {}),
        PerformanceCase('noGSO', {'FFL_P2P_QUIC_DISABLE_GSO': '1'}),
        PerformanceCase('noRecvBatch', {'FFL_P2P_QUIC_DISABLE_RECV_BATCH': '1'}),
        PerformanceCase(
            'portableDatagramPath',
            {
                'FFL_P2P_QUIC_DISABLE_GSO': '1',
                'FFL_P2P_QUIC_DISABLE_RECV_BATCH': '1',
            },
        ),
    )

    WRITE_WINDOW_CASES = (
        PerformanceCase('writeWindow1MiB', {'FFL_P2P_QUIC_WRITE_BUFFER_MIB': '1'}),
        PerformanceCase('writeWindow4MiB', {'FFL_P2P_QUIC_WRITE_BUFFER_MIB': '4'}),
        PerformanceCase('writeWindow8MiB', {'FFL_P2P_QUIC_WRITE_BUFFER_MIB': '8'}),
        PerformanceCase('writeWindow16MiB', {'FFL_P2P_QUIC_WRITE_BUFFER_MIB': '16'}),
    )

    def __init__(self, sizeMiB: int, iterations: int, suite: str, chunkKiB: int):
        self.sizeMiB = sizeMiB
        self.iterations = iterations
        self.chunkSize = chunkKiB * 1024
        self.cases = self._selectCases(suite)
        pattern = bytes(range(256))
        self.payload = pattern * (sizeMiB * 1024 * 1024 // len(pattern))

    @classmethod
    def _selectCases(cls, suite: str):
        if suite == 'datapath':
            return cls.DATAPATH_CASES
        if suite == 'write-window':
            return cls.WRITE_WINDOW_CASES
        if suite == 'all':
            return cls.DATAPATH_CASES + cls.WRITE_WINDOW_CASES
        raise ValueError(f'unknown benchmark suite: {suite}')

    def run(self):
        results = [self._runCase(case) for case in self.cases]
        self._printSummary(results)
        return results

    def _runCase(self, case: PerformanceCase):
        throughputs = []
        for iteration in range(self.iterations):
            with self._caseEnvironment(case):
                startedAt = time.perf_counter()
                QUICTransferHarness().transfer(
                    self.payload,
                    timeout=120,
                    chunkSize=self.chunkSize,
                )
                duration = time.perf_counter() - startedAt

            throughput = self.sizeMiB / duration
            throughputs.append(throughput)
            print(
                f'{case.name} iteration={iteration + 1} duration={duration:.3f}s '
                f'throughput={throughput:.1f}MiB/s'
            )

        return {
            'name': case.name,
            'medianMiBPerSecond': statistics.median(throughputs),
            'minMiBPerSecond': min(throughputs),
            'maxMiBPerSecond': max(throughputs),
        }

    @contextlib.contextmanager
    def _caseEnvironment(self, case: PerformanceCase):
        with mock.patch.dict(os.environ, {}, clear=False):
            for name in self.CONTROL_ENVIRONMENT:
                os.environ.pop(name, None)
            os.environ.update(case.environment)
            yield

    @staticmethod
    def _printSummary(results):
        print('\nSummary')
        print('case                  median MiB/s   min MiB/s   max MiB/s')
        for result in results:
            print(
                f'{result["name"]:<21} '
                f'{result["medianMiBPerSecond"]:>12.1f} '
                f'{result["minMiBPerSecond"]:>11.1f} '
                f'{result["maxMiBPerSecond"]:>11.1f}'
            )


def main():
    parser = argparse.ArgumentParser(description='Benchmark supported QUIC performance controls')
    parser.add_argument('--size-mib', type=int, default=64, dest='sizeMiB')
    parser.add_argument('--iterations', type=int, default=3)
    parser.add_argument('--chunk-kib', type=int, default=256, dest='chunkKiB')
    parser.add_argument(
        '--suite', choices=('datapath', 'write-window', 'all'), default='datapath')
    args = parser.parse_args()

    if args.sizeMiB <= 0:
        parser.error('--size-mib must be positive')
    if args.iterations <= 0:
        parser.error('--iterations must be positive')
    if args.chunkKiB <= 0:
        parser.error('--chunk-kib must be positive')

    QUICPerformanceBenchmark(
        args.sizeMiB,
        args.iterations,
        suite=args.suite,
        chunkKiB=args.chunkKiB,
    ).run()


if __name__ == '__main__':
    main()

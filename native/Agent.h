/**
SPDX-License-Identifier: Apache-2.0

FastFileLink CLI - Fast, no-fuss file sharing
Copyright (C) 2025-2026 FastFileLink contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef FFL_P2P_AGENT_H
#define FFL_P2P_AGENT_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Native data-plane bridge used by Quic.cpp.
 *
 * These functions deliberately avoid creating Python objects for individual
 * datagrams.  getFFLP2PAgentNativeHandle() is called while the GIL is held; the
 * returned handle remains valid for the duration of the Python method call
 * that owns the Agent argument.  Send/receive may then be called while the GIL
 * is released.
 */
typedef void (*FFLP2PAgentDataSink)(
    void *userPtr,
    const void *data,
    size_t size);

void *getFFLP2PAgentNativeHandle(PyObject *agentObject);

int setFFLP2PAgentNativeDataSink(
    void *agentHandle,
    FFLP2PAgentDataSink sink,
    void *userPtr);

void clearFFLP2PAgentNativeDataSink(
    void *agentHandle,
    FFLP2PAgentDataSink sink,
    void *userPtr);

int sendFFLP2PAgentNative(void *agentHandle, const void *data, size_t size);

int sendFFLP2PAgentNativeAggregateProgress(
    void *agentHandle,
    const void *data,
    size_t size,
    size_t segmentSize,
    size_t *sentSize);

int receiveFFLP2PAgentNative(
    void *agentHandle,
    double timeout,
    unsigned char **data,
    size_t *size);

void freeFFLP2PAgentNativeReceive(unsigned char *data);

#ifdef __cplusplus
}
#endif

#endif

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

#ifndef FFL_P2P_DATAPATH_H
#define FFL_P2P_DATAPATH_H

#include <juice/juice.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FFLP2PDatapath FFLP2PDatapath;

FFLP2PDatapath *createFFLP2PDatapath(void);
void destroyFFLP2PDatapath(FFLP2PDatapath *datapath);
int sendFFLP2PDatapathAggregate(
    uintptr_t socketHandle,
    int socketFamily,
    const uint8_t *destinationAddress,
    size_t destinationAddressSize,
    const char *data,
    size_t size,
    size_t segmentSize,
    size_t *sentSize,
    int *socketError,
    void *userPtr);

int receiveFFLP2PDatapathBatch(
    uintptr_t socketHandle,
    juice_udp_recv_datagram_t *datagrams,
    size_t capacity,
    int *socketError,
    void *userPtr);

#ifdef __cplusplus
}
#endif

#endif

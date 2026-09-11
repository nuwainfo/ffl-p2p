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

#include "datapath/DatapathInternal.h"

#include "platform/Platform.h"

#include <cerrno>
#include <cstring>
#include <new>

#ifdef _WIN32
#include <winsock2.h>
#endif

extern "C" FFLP2PDatapath *createFFLP2PDatapath(void) {
    auto *datapath = new (std::nothrow) FFLP2PDatapath();
    if (!datapath) {
        return nullptr;
    }

    const ffl::datapath::DatapathOptions options{
        ffl::platform::isEnvironmentEnabled("FFL_P2P_QUIC_DISABLE_GSO"),
        ffl::platform::isEnvironmentEnabled("FFL_P2P_QUIC_DISABLE_RECV_BATCH"),
        ffl::platform::isEnvironmentEnabled("FFL_P2P_QUIC_DISABLE_RECV_COALESCING"),
    };

    try {
#if defined(FFL_P2P_DIAGNOSTICS)
        datapath->backend =
            ffl::datapath::createPlatformDatapathBackend(datapath->diagnostics, options);
#else
        datapath->backend = ffl::datapath::createPlatformDatapathBackend(options);
#endif
    } catch (...) {
        delete datapath;
        return nullptr;
    }

    if (!datapath->backend) {
        delete datapath;
        return nullptr;
    }

    return datapath;
}

extern "C" void destroyFFLP2PDatapath(FFLP2PDatapath *datapath) {
    delete datapath;
}

extern "C" int sendFFLP2PDatapathAggregate(
    uintptr_t socketHandle, int socketFamily, const uint8_t *destinationAddress,
    size_t destinationAddressSize, const char *data, size_t size, size_t segmentSize,
    size_t *sentSize, int *socketError, void *userPtr) {
    if (!socketError || !sentSize) {
        return JUICE_UDP_SEND_AGGREGATE_FAILED;
    }

    *socketError = 0;
    *sentSize = 0;

    if (!userPtr || !destinationAddress || destinationAddressSize == 0 || !data ||
        size == 0 || segmentSize == 0 || segmentSize > size) {
            
#ifdef _WIN32
        *socketError = WSAEINVAL;
#else
        *socketError = EINVAL;
#endif
        return JUICE_UDP_SEND_AGGREGATE_FAILED;
    }

    auto *datapath = static_cast<FFLP2PDatapath *>(userPtr);
    
#if defined(FFL_P2P_DIAGNOSTICS)
    datapath->diagnostics.recordSendBatchCall();
#endif

    return datapath->backend->sendAggregate(
        socketHandle, socketFamily, destinationAddress, destinationAddressSize,
        data, size, segmentSize, sentSize, socketError);
}

extern "C" int receiveFFLP2PDatapathBatch(
    uintptr_t socketHandle, juice_udp_recv_datagram_t *datagrams,
    size_t capacity, int *socketError, void *userPtr) {
    if (!socketError) {
        return -1;
    }

    if (!userPtr || !datagrams || capacity == 0) {
        
#ifdef _WIN32
        *socketError = WSAEINVAL;
#else
        *socketError = EINVAL;
#endif
        return -1;
    }

    auto *datapath = static_cast<FFLP2PDatapath *>(userPtr);
    
#if defined(FFL_P2P_DIAGNOSTICS)
    datapath->diagnostics.recordReceiveBatchCall();
#endif

    *socketError = 0;

    return datapath->backend->receiveBatch(socketHandle, datagrams, capacity, socketError);
}

#if defined(FFL_P2P_DIAGNOSTICS)
extern "C" void getFFLP2PDatapathStats(
    FFLP2PDatapath *datapath, FFLP2PDatapathStats *stats) {
    if (!stats) {
        return;
    }

    if (!datapath) {
        std::memset(stats, 0, sizeof(*stats));
        return;
    }

    datapath->diagnostics.getSnapshot(stats);
}
#endif

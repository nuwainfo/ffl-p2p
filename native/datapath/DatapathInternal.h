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

#ifndef FFL_P2P_DATAPATH_INTERNAL_H
#define FFL_P2P_DATAPATH_INTERNAL_H

#include "datapath/Datapath.h"

#if defined(FFL_P2P_DIAGNOSTICS)
#include "datapath/DatapathDiagnostics.h"
#endif

#include <cstddef>
#include <cstdint>
#include <memory>

#if defined(FFL_P2P_DIAGNOSTICS)
#define FFL_P2P_DATAPATH_DIAGNOSTIC(STATEMENT) do { STATEMENT; } while (false)
#else
#define FFL_P2P_DATAPATH_DIAGNOSTIC(STATEMENT) do { } while (false)
#endif

namespace ffl::datapath {

struct DatapathOptions {
    bool disableSendSegmentation{false};
    bool disableReceiveBatching{false};
    bool disableReceiveCoalescing{false};
};

class DatapathBackend {
public:

#if defined(FFL_P2P_DIAGNOSTICS)
    explicit DatapathBackend(DatapathDiagnostics &diagnostics) : diagnostics_(diagnostics) {}
#else
    DatapathBackend() = default;
#endif

    virtual ~DatapathBackend() = default;

    DatapathBackend(const DatapathBackend &) = delete;
    DatapathBackend &operator=(const DatapathBackend &) = delete;

    virtual int sendAggregate(
        uintptr_t socketHandle, int socketFamily, const uint8_t *destinationAddress,
        size_t destinationAddressSize, const char *data, size_t size, size_t segmentSize,
        size_t *sentSize, int *socketError) = 0;
        
    virtual int receiveBatch(
        uintptr_t socketHandle, juice_udp_recv_datagram_t *datagrams,
        size_t capacity, int *socketError) = 0;

#if defined(FFL_P2P_DIAGNOSTICS)
protected:
    DatapathDiagnostics &diagnostics_;
#endif
};

#if defined(FFL_P2P_DIAGNOSTICS)
std::unique_ptr<DatapathBackend> createPlatformDatapathBackend(
    DatapathDiagnostics &diagnostics, const DatapathOptions &options);
#else
std::unique_ptr<DatapathBackend> createPlatformDatapathBackend(
    const DatapathOptions &options);
#endif

} // namespace ffl::datapath

struct FFLP2PDatapath {
#if defined(FFL_P2P_DIAGNOSTICS)
    ffl::datapath::DatapathDiagnostics diagnostics;
#endif
    std::unique_ptr<ffl::datapath::DatapathBackend> backend;
};

#endif

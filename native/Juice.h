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

#ifndef FFL_P2P_JUICE_H
#define FFL_P2P_JUICE_H

#include <juice/juice.h>
#include <stddef.h>
#include <stdint.h>

#include "datapath/Datapath.h"
#if defined(FFL_P2P_DIAGNOSTICS)
#include "datapath/DatapathDiagnostics.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FFLP2PJuice FFLP2PJuice;

void setFFLP2PJuiceLogLevel(juice_log_level_t level);

typedef void (*FFLP2PStateCallback)(void *user, juice_state_t state);
typedef void (*FFLP2PCandidateCallback)(void *user, const char *candidateSDP);
typedef void (*FFLP2PGatheringDoneCallback)(void *user);
typedef void (*FFLP2PReceiveCallback)(void *user, const void *data, size_t size);

typedef struct FFLP2PJuiceConfig {
    const char *stunHost;
    uint16_t stunPort;
    const char *bindAddress;
    uint16_t portBegin;
    uint16_t portEnd;
    void *user;
    FFLP2PStateCallback onState;
    FFLP2PCandidateCallback onCandidate;
    FFLP2PGatheringDoneCallback onGatheringDone;
    FFLP2PReceiveCallback onReceive;
} FFLP2PJuiceConfig;

FFLP2PJuice *createFFLP2PJuice(const FFLP2PJuiceConfig *config);
void destroyFFLP2PJuice(FFLP2PJuice *juice);
int gatherFFLP2PJuice(FFLP2PJuice *juice);
int holdFFLP2PJuiceGathering(FFLP2PJuice *juice);
int releaseFFLP2PJuiceGathering(FFLP2PJuice *juice);
int getFFLP2PJuiceLocalPort(FFLP2PJuice *juice, uint16_t *port);
int addFFLP2PJuiceMappedCandidate(
    FFLP2PJuice *juice,
    const char *address,
    uint16_t port);
int getFFLP2PJuiceLocalDescription(FFLP2PJuice *juice, char *buffer, size_t size);
int setFFLP2PJuiceRemoteDescription(FFLP2PJuice *juice, const char *sdp);
int addFFLP2PJuiceRemoteCandidate(FFLP2PJuice *juice, const char *candidateSDP);
int completeFFLP2PJuiceRemoteGathering(FFLP2PJuice *juice);
int sendFFLP2PJuice(FFLP2PJuice *juice, const void *data, size_t size);
int sendFFLP2PJuiceAggregateProgress(
    FFLP2PJuice *juice,
    const void *data,
    size_t size,
    size_t segmentSize,
    size_t *sentSize);
int getFFLP2PJuiceSelectedAddresses(
    FFLP2PJuice *juice,
    char *local,
    size_t localSize,
    char *remote,
    size_t remoteSize);
juice_state_t getFFLP2PJuiceState(FFLP2PJuice *juice);

#if defined(FFL_P2P_DIAGNOSTICS)
void getFFLP2PJuiceDatapathStats(FFLP2PJuice *juice, FFLP2PDatapathStats *stats);
#endif

#ifdef __cplusplus
}
#endif
#endif

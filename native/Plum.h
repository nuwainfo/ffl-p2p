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

#ifndef FFL_P2P_PLUM_H
#define FFL_P2P_PLUM_H

#include <plum/plum.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FFLP2PPortMapping FFLP2PPortMapping;

typedef struct FFLP2PPortMappingResult {
    plum_state_t state;
    plum_mapping_protocol_t mappingProtocol;
    uint16_t internalPort;
    uint16_t externalPort;
    char externalHost[PLUM_MAX_HOST_LEN];
} FFLP2PPortMappingResult;

int initializeFFLP2PPortMapping(void);
void setFFLP2PPortMappingLogLevel(plum_log_level_t level);
void cleanupFFLP2PPortMapping(void);
FFLP2PPortMapping *createFFLP2PPortMapping(plum_ip_protocol_t protocol, uint16_t internalPort);
int queryFFLP2PPortMapping(FFLP2PPortMapping *mapping, FFLP2PPortMappingResult *result);
void destroyFFLP2PPortMapping(FFLP2PPortMapping *mapping);

#ifdef __cplusplus
}
#endif
#endif

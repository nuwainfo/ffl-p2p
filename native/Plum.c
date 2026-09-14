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

#include "Plum.h"
#include "Log.h"

#include <stdlib.h>
#include <string.h>

struct FFLP2PPortMapping {
    int id;
    int destroyed;
};

static int initialized = 0;
static plum_log_level_t logLevel = PLUM_LOG_LEVEL_WARN;

static void onLog(plum_log_level_t level, const char *message) {
    writeFFLP2PNativeLog((int)level, message);
}

void setFFLP2PPortMappingLogLevel(plum_log_level_t level) {
    logLevel = level;

    if (initialized) {
        plum_set_log_level(level);
    }
}

static void onMapping(int id, plum_state_t state, const plum_mapping_t *mapping) {
    (void)id;
    (void)state;
    (void)mapping;
    /* libplum calls this from its worker thread. Python/user code polls state. */
}

int initializeFFLP2PPortMapping(void) {
    plum_config_t config;
    int result;

    if (initialized)
        return PLUM_ERR_SUCCESS;

    memset(&config, 0, sizeof(config));
    config.log_level = logLevel;
    config.log_callback = onLog;
    config.protocol = PLUM_PROTOCOL_ANY;
    
    result = plum_init(&config);
    if (result == PLUM_ERR_SUCCESS)
        initialized = 1;
    
    return result;
}

void cleanupFFLP2PPortMapping(void) {
    if (!initialized)
        return;
    
    plum_cleanup();
    initialized = 0;
}

FFLP2PPortMapping *createFFLP2PPortMapping(plum_ip_protocol_t protocol, uint16_t internalPort) {
    FFLP2PPortMapping *mapping;
    plum_mapping_t request;
    int id;

    if (!internalPort)
        return NULL;
    
    if (initializeFFLP2PPortMapping() != PLUM_ERR_SUCCESS)
        return NULL;

    mapping = (FFLP2PPortMapping *)calloc(1, sizeof(*mapping));
    if (!mapping)
        return NULL;

    memset(&request, 0, sizeof(request));
    request.protocol = protocol;
    request.internal_port = internalPort;
    
    id = plum_create_mapping(&request, onMapping);
    if (id < 0) {
        free(mapping);
        return NULL;
    }
    
    mapping->id = id;
    
    return mapping;
}

int queryFFLP2PPortMapping(FFLP2PPortMapping *mapping, FFLP2PPortMappingResult *result) {
    plum_state_t state;
    plum_mapping_t current;
    int queryResult;

    if (!mapping || !result || mapping->destroyed)
        return PLUM_ERR_INVALID;

    memset(&current, 0, sizeof(current));
    queryResult = plum_query_mapping(mapping->id, &state, &current);
    if (queryResult != PLUM_ERR_SUCCESS)
        return queryResult;

    memset(result, 0, sizeof(*result));
    
    result->state = state;
    result->mappingProtocol = current.mapping_protocol;
    result->internalPort = current.internal_port;
    result->externalPort = current.external_port;
    memcpy(result->externalHost, current.external_host, sizeof(result->externalHost));
    result->externalHost[sizeof(result->externalHost) - 1] = '\0';
    
    return PLUM_ERR_SUCCESS;
}

void destroyFFLP2PPortMapping(FFLP2PPortMapping *mapping) {
    if (!mapping)
        return;
    
    if (!mapping->destroyed) {
        plum_destroy_mapping(mapping->id);
        mapping->destroyed = 1;
    }
    
    free(mapping);
}

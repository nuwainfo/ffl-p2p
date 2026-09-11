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

#include <plum/plum.h>
#include <stdio.h>
#include <string.h>

#define MAX_MAPPINGS 32

typedef struct MappingSlot {
    int used;
    int id;
    plum_state_t state;
    plum_mapping_t mapping;
} MappingSlot;

static int initialized = 0;
static int nextId = 1;
static MappingSlot mappings[MAX_MAPPINGS];

int plum_init(const plum_config_t *config) {
    (void)config;

    initialized = 1;
    return PLUM_ERR_SUCCESS;
}

int plum_cleanup(void) {
    memset(mappings, 0, sizeof(mappings));

    initialized = 0;
    return PLUM_ERR_SUCCESS;
}

int plum_create_mapping(const plum_mapping_t *mapping, plum_mapping_callback_t callback) {
    int index;

    if (!initialized || !mapping || !callback || !mapping->internal_port)
        return PLUM_ERR_INVALID;

    for (index = 0; index < MAX_MAPPINGS; index++) {
        MappingSlot *slot = &mappings[index];

        if (slot->used)
            continue;

        slot->used = 1;
        slot->id = nextId++;
        slot->state = PLUM_STATE_SUCCESS;
        slot->mapping = *mapping;
        slot->mapping.mapping_protocol = PLUM_MAPPING_PROTOCOL_PCP;
        /* Loopback keeps integration traffic routable while still proving injection. */
        slot->mapping.external_port = mapping->internal_port;
        snprintf(slot->mapping.external_host, sizeof(slot->mapping.external_host), "127.0.0.1");

        callback(slot->id, slot->state, &slot->mapping);
        return slot->id;
    }

    return PLUM_ERR_FAILED;
}

int plum_query_mapping(int id, plum_state_t *state, plum_mapping_t *mapping) {
    int index;

    for (index = 0; index < MAX_MAPPINGS; index++) {
        MappingSlot *slot = &mappings[index];

        if (!slot->used || slot->id != id)
            continue;

        if (state)
            *state = slot->state;
        if (mapping)
            *mapping = slot->mapping;
        return PLUM_ERR_SUCCESS;
    }

    return PLUM_ERR_NOT_AVAIL;
}

int plum_destroy_mapping(int id) {
    int index;

    for (index = 0; index < MAX_MAPPINGS; index++) {
        MappingSlot *slot = &mappings[index];

        if (!slot->used || slot->id != id)
            continue;

        slot->used = 0;
        slot->state = PLUM_STATE_DESTROYED;
        return PLUM_ERR_SUCCESS;
    }

    return PLUM_ERR_NOT_AVAIL;
}

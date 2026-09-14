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

#include "Juice.h"
#include "Log.h"

#include <stdlib.h>
#include <string.h>

struct FFLP2PJuice {
    juice_agent_t *agent;
    FFLP2PJuiceConfig config;
    FFLP2PDatapath *datapath;
};

static void onLog(juice_log_level_t level, const char *message) {
    writeFFLP2PNativeLog((int)level, message);
}

void setFFLP2PJuiceLogLevel(juice_log_level_t level) {
    juice_set_log_handler(onLog);
    juice_set_log_level(level);
}

static void onState(juice_agent_t *agent, juice_state_t state, void *ptr) {
    (void)agent; /* avoid /W4 /WX or -Werror */
    FFLP2PJuice *juice = (FFLP2PJuice *)ptr;
    
    if (juice->config.onState)
        juice->config.onState(juice->config.user, state);
}

static void onCandidate(juice_agent_t *agent, const char *sdp, void *ptr) {
    (void)agent;
    
    FFLP2PJuice *juice = (FFLP2PJuice *)ptr;
    if (juice->config.onCandidate)
        juice->config.onCandidate(juice->config.user, sdp);
}

static void onGatheringDone(juice_agent_t *agent, void *ptr) {
    (void)agent;
    
    FFLP2PJuice *juice = (FFLP2PJuice *)ptr;
    if (juice->config.onGatheringDone)
        juice->config.onGatheringDone(juice->config.user);
}

static void onReceive(juice_agent_t *agent, const char *data, size_t size, void *ptr) {
    (void)agent;
    
    FFLP2PJuice *juice = (FFLP2PJuice *)ptr;
    if (juice->config.onReceive)
        juice->config.onReceive(juice->config.user, data, size);
}

FFLP2PJuice *createFFLP2PJuice(const FFLP2PJuiceConfig *config) {
    FFLP2PJuice *juice;
    juice_config_t juiceConfig;

    if (!config)
        return NULL;

    juice = (FFLP2PJuice *)calloc(1, sizeof(*juice));
    if (!juice)
        return NULL;
    
    juice->config = *config;
    juice->datapath = createFFLP2PDatapath();
    
    if (!juice->datapath) {
        free(juice);
        return NULL;
    }

    memset(&juiceConfig, 0, sizeof(juiceConfig));
    
    juiceConfig.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    juiceConfig.stun_server_host = config->stunHost;
    juiceConfig.stun_server_port = config->stunPort;
    juiceConfig.bind_address = config->bindAddress;
    juiceConfig.local_port_range_begin = config->portBegin;
    juiceConfig.local_port_range_end = config->portEnd;
    juiceConfig.cb_state_changed = onState;
    juiceConfig.cb_candidate = onCandidate;
    juiceConfig.cb_gathering_done = onGatheringDone;
    juiceConfig.cb_recv = onReceive;
    juiceConfig.cb_udp_recv_batch = receiveFFLP2PDatapathBatch;
    juiceConfig.cb_udp_send_aggregate = sendFFLP2PDatapathAggregate;
    juiceConfig.user_ptr = juice;
    juiceConfig.udp_recv_batch_user_ptr = juice->datapath;
    juiceConfig.udp_send_aggregate_user_ptr = juice->datapath;

    juice->agent = juice_create(&juiceConfig);
    if (!juice->agent) {
        destroyFFLP2PDatapath(juice->datapath);
        free(juice);
        return NULL;
    }
    
    return juice;
}

void destroyFFLP2PJuice(FFLP2PJuice *juice) {
    if (!juice)
        return;
    
    if (juice->agent)
        juice_destroy(juice->agent);
    
    destroyFFLP2PDatapath(juice->datapath);
    free(juice);
}

int gatherFFLP2PJuice(FFLP2PJuice *juice) {
    return juice && juice->agent ? juice_gather_candidates(juice->agent) : JUICE_ERR_INVALID;
}

int holdFFLP2PJuiceGathering(FFLP2PJuice *juice) {
    return juice && juice->agent ? juice_hold_gathering(juice->agent) : JUICE_ERR_INVALID;
}

int releaseFFLP2PJuiceGathering(FFLP2PJuice *juice) {
    return juice && juice->agent ? juice_release_gathering(juice->agent) : JUICE_ERR_INVALID;
}

int getFFLP2PJuiceLocalPort(FFLP2PJuice *juice, uint16_t *port) {
    return juice && juice->agent ? juice_get_local_port(juice->agent, port) : JUICE_ERR_INVALID;
}

int addFFLP2PJuiceMappedCandidate(FFLP2PJuice *juice, const char *address, uint16_t port) {
    return juice && juice->agent
        ? juice_add_local_mapped_candidate(juice->agent, address, port)
        : JUICE_ERR_INVALID;
}

int getFFLP2PJuiceLocalDescription(FFLP2PJuice *juice, char *buffer, size_t size) {
    return juice && juice->agent
        ? juice_get_local_description(juice->agent, buffer, size)
        : JUICE_ERR_INVALID;
}

int setFFLP2PJuiceRemoteDescription(FFLP2PJuice *juice, const char *sdp) {
    return juice && juice->agent ? juice_set_remote_description(juice->agent, sdp) : JUICE_ERR_INVALID;
}

int addFFLP2PJuiceRemoteCandidate(FFLP2PJuice *juice, const char *candidateSDP) {
    return juice && juice->agent
        ? juice_add_remote_candidate(juice->agent, candidateSDP)
        : JUICE_ERR_INVALID;
}

int completeFFLP2PJuiceRemoteGathering(FFLP2PJuice *juice) {
    return juice && juice->agent ? juice_set_remote_gathering_done(juice->agent) : JUICE_ERR_INVALID;
}

int sendFFLP2PJuice(FFLP2PJuice *juice, const void *data, size_t size) {
    return juice && juice->agent
        ? juice_send(juice->agent, (const char *)data, size)
        : JUICE_ERR_INVALID;
}

int sendFFLP2PJuiceAggregateProgress(FFLP2PJuice *juice, const void *data, size_t size,
                                     size_t segmentSize, size_t *sentSize) {
    return juice && juice->agent
        ? juice_send_aggregate_progress(juice->agent, (const char *)data, size, segmentSize, sentSize)
        : JUICE_ERR_INVALID;
}

int getFFLP2PJuiceSelectedAddresses(FFLP2PJuice *juice, char *local, size_t localSize,
                                 char *remote, size_t remoteSize) {
    return juice && juice->agent
        ? juice_get_selected_addresses(juice->agent, local, localSize, remote, remoteSize)
        : JUICE_ERR_INVALID;
}

juice_state_t getFFLP2PJuiceState(FFLP2PJuice *juice) {
    return juice && juice->agent ? juice_get_state(juice->agent) : JUICE_STATE_FAILED;
}

#if defined(FFL_P2P_DIAGNOSTICS)
void getFFLP2PJuiceDatapathStats(FFLP2PJuice *juice, FFLP2PDatapathStats *stats) {
    getFFLP2PDatapathStats(juice ? juice->datapath : NULL, stats);
}
#endif

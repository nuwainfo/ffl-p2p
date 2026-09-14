#ifndef PLUM_H
#define PLUM_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#define PLUM_ERR_SUCCESS 0
#define PLUM_ERR_INVALID -1
#define PLUM_ERR_FAILED -2
#define PLUM_ERR_NOT_AVAIL -3
typedef enum { PLUM_LOG_LEVEL_VERBOSE=0, PLUM_LOG_LEVEL_DEBUG=1, PLUM_LOG_LEVEL_INFO=2, PLUM_LOG_LEVEL_WARN=3, PLUM_LOG_LEVEL_ERROR=4, PLUM_LOG_LEVEL_FATAL=5, PLUM_LOG_LEVEL_NONE=6 } plum_log_level_t;
typedef void (*plum_log_callback_t)(plum_log_level_t level, const char *message);
typedef enum { PLUM_PROTOCOL_ANY=0, PLUM_PROTOCOL_PCP=1, PLUM_PROTOCOL_UPNP=2 } plum_protocol_t;
typedef struct { plum_log_level_t log_level; plum_log_callback_t log_callback; const char *dummytls_domain; int discover_timeout; int mapping_timeout; int recheck_period; plum_protocol_t protocol; } plum_config_t;
typedef enum { PLUM_IP_PROTOCOL_TCP=0, PLUM_IP_PROTOCOL_UDP=1 } plum_ip_protocol_t;
typedef enum { PLUM_STATE_DESTROYED=0, PLUM_STATE_PENDING=1, PLUM_STATE_SUCCESS=2, PLUM_STATE_FAILURE=3, PLUM_STATE_DESTROYING=4 } plum_state_t;
typedef enum { PLUM_MAPPING_PROTOCOL_UNKNOWN=0, PLUM_MAPPING_PROTOCOL_PCP=1, PLUM_MAPPING_PROTOCOL_NATPMP=2, PLUM_MAPPING_PROTOCOL_UPNP=3, PLUM_MAPPING_PROTOCOL_DIRECT=4 } plum_mapping_protocol_t;
#define PLUM_MAX_HOST_LEN 256
typedef struct { plum_ip_protocol_t protocol; plum_mapping_protocol_t mapping_protocol; uint16_t internal_port; uint16_t external_port; char external_host[PLUM_MAX_HOST_LEN]; void *user_ptr; } plum_mapping_t;
typedef void (*plum_mapping_callback_t)(int id, plum_state_t state, const plum_mapping_t *mapping);
int plum_init(const plum_config_t *config);
int plum_cleanup(void);
void plum_set_log_level(plum_log_level_t level);
int plum_create_mapping(const plum_mapping_t *mapping, plum_mapping_callback_t callback);
int plum_query_mapping(int id, plum_state_t *state, plum_mapping_t *mapping);
int plum_destroy_mapping(int id);
#ifdef __cplusplus
}
#endif
#endif

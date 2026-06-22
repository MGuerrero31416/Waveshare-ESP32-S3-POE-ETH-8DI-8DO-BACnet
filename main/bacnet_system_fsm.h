#ifndef BACNET_SYSTEM_FSM_H
#define BACNET_SYSTEM_FSM_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	BACNET_FSM_STATE_BOOT = 0,
	BACNET_FSM_STATE_PERIPHERAL_INIT,
	BACNET_FSM_STATE_OBJECTS_INIT,
	BACNET_FSM_STATE_NETWORK_WAIT,
	BACNET_FSM_STATE_TRANSPORT_SELECT,
	BACNET_FSM_STATE_BACNET_ACTIVE,
} bacnet_fsm_state_t;

typedef enum {
	BACNET_FSM_EVENT_START = 0,
	BACNET_FSM_EVENT_ETH_UP,
	BACNET_FSM_EVENT_ETH_DOWN,
	BACNET_FSM_EVENT_WIFI_UP,
	BACNET_FSM_EVENT_WIFI_DOWN,
	BACNET_FSM_EVENT_OBJECTS_READY,
	BACNET_FSM_EVENT_MSTP_READY,
	BACNET_FSM_EVENT_RECONCILE_TICK,
} bacnet_fsm_event_t;

typedef struct {
	bool enable_bacnet_ip_ethernet;
	bool enable_bacnet_ip_wifi;
	bool enable_bacnet_mstp;
} bacnet_fsm_config_t;

typedef struct {
	bool (*init_objects_once)(void);
	bool (*init_mstp_once)(void);
	bool (*is_eth_usable)(void);
	bool (*is_wifi_usable)(void);
	esp_err_t (*start_bip_on_interface)(const char *ifkey, const char *label);
	void (*on_bacnet_active)(void);
} bacnet_fsm_hooks_t;

void bacnet_fsm_init(const bacnet_fsm_config_t *config,
					 const bacnet_fsm_hooks_t *hooks);
void bacnet_fsm_start(void);
void bacnet_fsm_handle_event(bacnet_fsm_event_t event);
bacnet_fsm_state_t bacnet_fsm_get_state(void);
const char *bacnet_fsm_get_active_transport_name(void);

const char *bacnet_fsm_state_name(bacnet_fsm_state_t state);
const char *bacnet_fsm_event_name(bacnet_fsm_event_t event);

#ifdef __cplusplus
}
#endif

#endif /* BACNET_SYSTEM_FSM_H */

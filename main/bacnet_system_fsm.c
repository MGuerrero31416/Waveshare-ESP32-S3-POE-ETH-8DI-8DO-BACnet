#include "bacnet_system_fsm.h"

#include <limits.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "bacnet/basic/services.h"
#include "bacnet/basic/tsm/tsm.h"

#define FSM_EVENT_QUEUE_LEN 32
#define FSM_RECONCILE_PERIOD_MS 500
#define IAM_COOLDOWN_US 3000000LL

static const char *TAG = "bacnet_fsm";

typedef enum {
	BACNET_TRANSPORT_NONE = 0,
	BACNET_TRANSPORT_BIP_ETH,
	BACNET_TRANSPORT_BIP_WIFI,
	BACNET_TRANSPORT_MSTP,
} bacnet_transport_t;

typedef struct {
	bacnet_fsm_state_t state;
	bacnet_fsm_config_t config;
	bacnet_fsm_hooks_t hooks;

	bool started;
	bool objects_ready;
	bool mstp_ready;
	bool eth_up;
	bool wifi_up;

	bacnet_transport_t active_transport;
	int64_t last_iam_us;
	bool iam_deferred;
	bool iam_deferred_logged;
	bool objects_wait_logged;

	QueueHandle_t event_queue;
	TaskHandle_t worker_task;
} bacnet_fsm_ctx_t;

static bacnet_fsm_ctx_t s_fsm;

static void bacnet_fsm_worker_task(void *pvParameters);
static void fsm_process_event(bacnet_fsm_event_t event);
static void fsm_reconcile(bacnet_fsm_event_t trigger);

static const char *transport_name(bacnet_transport_t transport)
{
	switch (transport) {
	case BACNET_TRANSPORT_BIP_ETH:
		return "Ethernet";
	case BACNET_TRANSPORT_BIP_WIFI:
		return "WiFi";
	case BACNET_TRANSPORT_MSTP:
		return "MS/TP";
	default:
		return "NONE";
	}
}

const char *bacnet_fsm_state_name(bacnet_fsm_state_t state)
{
	switch (state) {
	case BACNET_FSM_STATE_BOOT:
		return "BOOT";
	case BACNET_FSM_STATE_PERIPHERAL_INIT:
		return "PERIPHERAL_INIT";
	case BACNET_FSM_STATE_OBJECTS_INIT:
		return "OBJECTS_INIT";
	case BACNET_FSM_STATE_NETWORK_WAIT:
		return "NETWORK_WAIT";
	case BACNET_FSM_STATE_TRANSPORT_SELECT:
		return "TRANSPORT_SELECT";
	case BACNET_FSM_STATE_BACNET_ACTIVE:
		return "BACNET_ACTIVE";
	default:
		return "UNKNOWN";
	}
}

const char *bacnet_fsm_event_name(bacnet_fsm_event_t event)
{
	switch (event) {
	case BACNET_FSM_EVENT_START:
		return "START";
	case BACNET_FSM_EVENT_ETH_UP:
		return "ETH_UP";
	case BACNET_FSM_EVENT_ETH_DOWN:
		return "ETH_DOWN";
	case BACNET_FSM_EVENT_WIFI_UP:
		return "WIFI_UP";
	case BACNET_FSM_EVENT_WIFI_DOWN:
		return "WIFI_DOWN";
	case BACNET_FSM_EVENT_OBJECTS_READY:
		return "OBJECTS_READY";
	case BACNET_FSM_EVENT_MSTP_READY:
		return "MSTP_READY";
	case BACNET_FSM_EVENT_RECONCILE_TICK:
		return "RECONCILE_TICK";
	default:
		return "UNKNOWN_EVENT";
	}
}

static void fsm_set_state(bacnet_fsm_state_t next, bacnet_fsm_event_t trigger)
{
	if (s_fsm.state == next) {
		return;
	}
	ESP_LOGD(TAG, "state transition: %s -> %s (event=%s)",
			 bacnet_fsm_state_name(s_fsm.state),
			 bacnet_fsm_state_name(next),
			 bacnet_fsm_event_name(trigger));
	s_fsm.state = next;
}

static void fsm_try_send_iam(const char *reason)
{
	if (s_fsm.active_transport == BACNET_TRANSPORT_NONE) {
		return;
	}

	int64_t now = esp_timer_get_time();
	if ((now - s_fsm.last_iam_us) < IAM_COOLDOWN_US) {
		s_fsm.iam_deferred = true;
		if (!s_fsm.iam_deferred_logged) {
			ESP_LOGD(TAG, "I-Am deferred (%s): cooldown active", reason);
			s_fsm.iam_deferred_logged = true;
		}
		return;
	}

	Send_I_Am(&Handler_Transmit_Buffer[0]);
	s_fsm.last_iam_us = now;
	s_fsm.iam_deferred = false;
	s_fsm.iam_deferred_logged = false;
	ESP_LOGD(TAG, "I-Am sent (%s)", reason);
}

static void fsm_set_active_transport(bacnet_transport_t next, bacnet_fsm_event_t trigger)
{
	if (s_fsm.active_transport == next) {
		if (next != BACNET_TRANSPORT_NONE &&
			(trigger == BACNET_FSM_EVENT_ETH_UP || trigger == BACNET_FSM_EVENT_WIFI_UP)) {
			/* IP/link refresh on active transport still gets deterministic I-Am. */
			fsm_try_send_iam("link refresh");
		}
		return;
	}

	s_fsm.active_transport = next;
	if (next == BACNET_TRANSPORT_NONE) {
		fsm_set_state(BACNET_FSM_STATE_NETWORK_WAIT, trigger);
		return;
	}

	ESP_LOGI(TAG, "BACnet transport switched: %s", transport_name(next));

	fsm_set_state(BACNET_FSM_STATE_BACNET_ACTIVE, trigger);
	ESP_LOGI(TAG, "BACnet ACTIVE");
	if (s_fsm.hooks.on_bacnet_active) {
		s_fsm.hooks.on_bacnet_active();
	}
	fsm_try_send_iam("active transport change");
}

static void fsm_reconcile(bacnet_fsm_event_t trigger)
{
	if (!s_fsm.started) {
		fsm_set_state(BACNET_FSM_STATE_BOOT, trigger);
		return;
	}

	fsm_set_state(BACNET_FSM_STATE_PERIPHERAL_INIT, trigger);

	if (!s_fsm.objects_ready) {
		fsm_set_state(BACNET_FSM_STATE_OBJECTS_INIT, trigger);
		if (!s_fsm.hooks.init_objects_once || !s_fsm.hooks.init_objects_once()) {
			if (!s_fsm.objects_wait_logged) {
				ESP_LOGW(TAG, "OBJECTS_INIT not ready; will retry on next event/tick");
				s_fsm.objects_wait_logged = true;
			}
			return;
		}
		s_fsm.objects_ready = true;
		s_fsm.objects_wait_logged = false;
		ESP_LOGD(TAG, "OBJECTS_READY latched");
		trigger = BACNET_FSM_EVENT_OBJECTS_READY;
	}

	if (s_fsm.config.enable_bacnet_mstp && !s_fsm.mstp_ready) {
		if (s_fsm.hooks.init_mstp_once && s_fsm.hooks.init_mstp_once()) {
			s_fsm.mstp_ready = true;
			ESP_LOGD(TAG, "MSTP_READY latched");
		}
	}

	fsm_set_state(BACNET_FSM_STATE_TRANSPORT_SELECT, trigger);
	bool eth_usable = s_fsm.hooks.is_eth_usable ? s_fsm.hooks.is_eth_usable() : s_fsm.eth_up;
	bool wifi_usable = s_fsm.hooks.is_wifi_usable ? s_fsm.hooks.is_wifi_usable() : s_fsm.wifi_up;

	bacnet_transport_t selected = BACNET_TRANSPORT_NONE;

	if (s_fsm.objects_ready && s_fsm.config.enable_bacnet_ip_ethernet && eth_usable) {
		selected = BACNET_TRANSPORT_BIP_ETH;
	}

	if (selected == BACNET_TRANSPORT_NONE &&
		s_fsm.objects_ready && s_fsm.config.enable_bacnet_ip_wifi && wifi_usable) {
		selected = BACNET_TRANSPORT_BIP_WIFI;
	}

	if (selected == BACNET_TRANSPORT_BIP_ETH && s_fsm.hooks.start_bip_on_interface) {
		bool need_activate = (s_fsm.active_transport != BACNET_TRANSPORT_BIP_ETH);
		bool need_refresh = (trigger == BACNET_FSM_EVENT_ETH_UP);
		if ((need_activate || need_refresh) &&
			s_fsm.hooks.start_bip_on_interface("ETH_DEF", "Ethernet") != ESP_OK) {
			selected = BACNET_TRANSPORT_NONE;
		}
	}

	if (selected == BACNET_TRANSPORT_NONE &&
		s_fsm.objects_ready && s_fsm.config.enable_bacnet_ip_wifi && wifi_usable) {
		selected = BACNET_TRANSPORT_BIP_WIFI;
	}

	if (selected == BACNET_TRANSPORT_BIP_WIFI && s_fsm.hooks.start_bip_on_interface) {
		bool need_activate = (s_fsm.active_transport != BACNET_TRANSPORT_BIP_WIFI);
		bool need_refresh = (trigger == BACNET_FSM_EVENT_WIFI_UP);
		if ((need_activate || need_refresh) &&
			s_fsm.hooks.start_bip_on_interface("WIFI_STA_DEF", "WiFi") != ESP_OK) {
			selected = BACNET_TRANSPORT_NONE;
		}
	}

	if (selected == BACNET_TRANSPORT_NONE &&
		s_fsm.objects_ready && s_fsm.config.enable_bacnet_mstp && s_fsm.mstp_ready) {
		selected = BACNET_TRANSPORT_MSTP;
	}

	fsm_set_active_transport(selected, trigger);

	if (s_fsm.iam_deferred && s_fsm.active_transport != BACNET_TRANSPORT_NONE) {
		fsm_try_send_iam("deferred resend");
	}
}

static void fsm_process_event(bacnet_fsm_event_t event)
{
	switch (event) {
	case BACNET_FSM_EVENT_START:
		s_fsm.started = true;
		break;
	case BACNET_FSM_EVENT_ETH_UP:
		s_fsm.eth_up = true;
		break;
	case BACNET_FSM_EVENT_ETH_DOWN:
		s_fsm.eth_up = false;
		break;
	case BACNET_FSM_EVENT_WIFI_UP:
		s_fsm.wifi_up = true;
		break;
	case BACNET_FSM_EVENT_WIFI_DOWN:
		s_fsm.wifi_up = false;
		break;
	case BACNET_FSM_EVENT_OBJECTS_READY:
		s_fsm.objects_ready = true;
		break;
	case BACNET_FSM_EVENT_MSTP_READY:
		s_fsm.mstp_ready = true;
		break;
	case BACNET_FSM_EVENT_RECONCILE_TICK:
		break;
	default:
		break;
	}

	fsm_reconcile(event);
}

static void bacnet_fsm_worker_task(void *pvParameters)
{
	(void)pvParameters;
	ESP_LOGI(TAG, "FSM worker started");

	while (1) {
		bacnet_fsm_event_t event = BACNET_FSM_EVENT_RECONCILE_TICK;
		if (xQueueReceive(s_fsm.event_queue, &event,
						  pdMS_TO_TICKS(FSM_RECONCILE_PERIOD_MS)) == pdTRUE) {
			ESP_LOGD(TAG, "event: %s", bacnet_fsm_event_name(event));
			fsm_process_event(event);
		} else {
			fsm_process_event(BACNET_FSM_EVENT_RECONCILE_TICK);
		}
	}
}

void bacnet_fsm_init(const bacnet_fsm_config_t *config,
					 const bacnet_fsm_hooks_t *hooks)
{
	memset(&s_fsm, 0, sizeof(s_fsm));
	s_fsm.state = BACNET_FSM_STATE_BOOT;
	s_fsm.last_iam_us = INT64_MIN;

	if (config) {
		s_fsm.config = *config;
	}
	if (hooks) {
		s_fsm.hooks = *hooks;
	}

	s_fsm.event_queue = xQueueCreate(FSM_EVENT_QUEUE_LEN, sizeof(bacnet_fsm_event_t));
	if (!s_fsm.event_queue) {
		ESP_LOGE(TAG, "Failed to create event queue");
		return;
	}

	BaseType_t ok = xTaskCreate(
		bacnet_fsm_worker_task,
		"bacnet_fsm",
		4096,
		NULL,
		tskIDLE_PRIORITY + 4,
		&s_fsm.worker_task);
	if (ok != pdPASS) {
		ESP_LOGE(TAG, "Failed to create FSM worker task");
		s_fsm.worker_task = NULL;
		return;
	}

	ESP_LOGI(TAG, "FSM initialized: state=%s", bacnet_fsm_state_name(s_fsm.state));
}

void bacnet_fsm_start(void)
{
	bacnet_fsm_handle_event(BACNET_FSM_EVENT_START);
}

void bacnet_fsm_handle_event(bacnet_fsm_event_t event)
{
	if (!s_fsm.event_queue) {
		return;
	}

	/*
	 * Lossless dispatch: block until event is queued.
	 * No event is silently dropped.
	 */
	while (xQueueSend(s_fsm.event_queue, &event, portMAX_DELAY) != pdTRUE) {
	}
}

bacnet_fsm_state_t bacnet_fsm_get_state(void)
{
	return s_fsm.state;
}

const char *bacnet_fsm_get_active_transport_name(void)
{
	return transport_name(s_fsm.active_transport);
}

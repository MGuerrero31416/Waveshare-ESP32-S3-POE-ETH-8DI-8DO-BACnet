/**
 * @file bacnet_gateway.h
 * @brief Event-driven BACnet gateway coordinator.
 *
 * Replaces the startup FSM with a simple condition-flag evaluator.
 *
 * DESIGN PRINCIPLES:
 *   - No state machine.  No ordering requirements between boot events.
 *   - Transport activates when: objects_ready AND any_network_ready.
 *   - I-Am is sent when transport first becomes active, and on IP change.
 *   - All flags are independent.  Any event triggers full re-evaluation.
 *
 * EVENT FLOW DIAGRAM:
 *
 *   app_main                     bacnet_gateway               BACnet Stack
 *   ────────                     ──────────────               ────────────
 *   bacnet_gw_init() ──────────► set cfg/hooks
 *   bacnet_gw_start() ─────────► gw_evaluate()
 *                                  init_objects_once()  ──── Device_Init()
 *                                  init_mstp_once()     ──── dlmstp_init()
 *                                  (no net yet → wait)
 *
 *   ETH driver got IP
 *   bacnet_gw_post_event(ETH_IP_READY)
 *                        ──────────► eth_ip_ready = true
 *                                    gw_evaluate()
 *                                      objects_ready → yes
 *                                      start_bip_on_interface("ETH_DEF")
 *                                        datalink_init()
 *                                        BBMD register
 *                                        start rx task
 *                                      on_transport_active()  ── RGB green
 *                                      Send_I_Am()            ── broadcast
 *
 *   ETH drops, WiFi comes up
 *   bacnet_gw_post_event(ETH_IP_LOST)
 *                        ──────────► eth_ip_ready = false
 *                                    gw_evaluate() → no BIP net, keep mstp
 *   bacnet_gw_post_event(WIFI_IP_READY)
 *                        ──────────► wifi_ip_ready = true
 *                                    gw_evaluate()
 *                                      start_bip_on_interface("WIFI_STA_DEF")
 *                                        update IP/bcast addresses
 *                                      Send_I_Am() (cooldown allows)
 */

#ifndef BACNET_GATEWAY_H
#define BACNET_GATEWAY_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Event type
 * ---------------------------------------------------------------------- */
typedef enum {
    /** Synthetic startup trigger – no flag change, forces initial evaluation */
    BACNET_GW_EVT_STARTUP       = 0,
    /** ETH interface acquired a valid IP address */
    BACNET_GW_EVT_ETH_IP_READY,
    /** WiFi STA interface acquired a valid IP address */
    BACNET_GW_EVT_WIFI_CONNECTED,
    /** MS/TP stack successfully initialized */
    BACNET_GW_EVT_MSTP_READY,
    /** Object layer has finished one-time initialization */
    BACNET_GW_EVT_OBJECTS_READY,
    /** All BACnet/IP network links are currently down */
    BACNET_GW_EVT_NETWORK_DOWN,
    /** ETH interface lost its IP address */
    BACNET_GW_EVT_ETH_IP_LOST,
    /** WiFi STA interface lost its IP address */
    BACNET_GW_EVT_WIFI_IP_LOST,
} bacnet_gw_event_t;

/* -------------------------------------------------------------------------
 * Configuration (mirrors what was in bacnet_fsm_config_t)
 * ---------------------------------------------------------------------- */
typedef struct {
    bool enable_bacnet_ip_ethernet; /**< True if ETH BACnet/IP is desired   */
    bool enable_bacnet_ip_wifi;     /**< True if WiFi BACnet/IP is desired   */
    bool enable_bacnet_mstp;        /**< True if MS/TP is desired            */
} bacnet_gw_config_t;

/* -------------------------------------------------------------------------
 * Hook table
 * ---------------------------------------------------------------------- */
typedef struct {
    /**
     * Initialize the BACnet object layer (Device, AV, BV, AI, BI, BO, COV).
     * Called once from gw_evaluate(), independent of network state.
     * Must return true on success.  Must be idempotent.
     */
    bool (*init_objects_once)(void);

    /**
     * Initialize the MS/TP stack and start its receive task.
     * Called once from gw_evaluate() after objects are ready.
     * Must return true on success.  Must be idempotent.
     */
    bool (*init_mstp_once)(void);

    /**
     * Configure BACnet/IP on the specified interface.
     * Called every time the best available IP interface changes.
     * Must be idempotent (safe to call multiple times on same ifkey).
     * On first call: opens UDP socket, registers BBMD, starts rx task.
     * On subsequent calls: updates IP/broadcast addresses only.
     * Returns ESP_OK on success.
     */
    esp_err_t (*start_bip_on_interface)(const char *ifkey, const char *label);

    /**
     * Optional. Called each time the gateway transitions to active transport.
     * Use for LED/RGB status updates.  I-Am is sent automatically after this.
     * May be NULL.
     */
    void (*on_transport_active)(void);
} bacnet_gw_hooks_t;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * Initialize the gateway coordinator.
 * Must be called once from app_main before any events are posted.
 */
void bacnet_gw_init(const bacnet_gw_config_t *cfg, const bacnet_gw_hooks_t *hooks);

/**
 * Start the gateway: trigger initial object initialization and transport
 * activation if any network interface is already available.
 *
 * Call once from app_main after hardware (IO, BLE) is initialized.
 * Safe to call before any network is ready; transport will activate
 * automatically when the first ETH/WiFi event arrives.
 */
void bacnet_gw_start(void);

/**
 * Post a network condition event.
 * Updates the corresponding condition flag and triggers synchronous
 * re-evaluation in the calling task context.
 * Safe to call from any FreeRTOS task.
 */
void bacnet_gw_post_event(bacnet_gw_event_t event);

/**
 * Explicitly request an I-Am broadcast.
 * Subject to a 3-second cooldown to prevent spam.
 * Must only be called when transport is up.
 */
void bacnet_gw_send_iam(void);

/** Returns true if the object layer has been successfully initialized. */
bool bacnet_gw_objects_are_ready(void);

/** Returns true if at least one BACnet transport is currently active. */
bool bacnet_gw_transport_is_up(void);

#ifdef __cplusplus
}
#endif
#endif /* BACNET_GATEWAY_H */

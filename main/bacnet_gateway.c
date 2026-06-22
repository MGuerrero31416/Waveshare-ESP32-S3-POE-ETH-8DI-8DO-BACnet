/*
 * bacnet_gateway.c – Event-driven BACnet gateway coordinator.
 *
 * Replaces bacnet_system_fsm.c.  No state machine.  A single evaluate
 * function runs after every condition flag change and determines whether
 * the transport can be activated.
 *
 * THREADING MODEL:
 *   - s_mutex (non-recursive) protects the entire bacnet_gw_ctx_t.
 *   - gw_evaluate() runs synchronously inside the caller's task context
 *     while holding s_mutex.
 *   - Hooks (init_objects_once, start_bip_on_interface) are called under
 *     s_mutex.  They must not themselves call bacnet_gw_post_event().
 *   - Send_I_Am() does not acquire s_mutex; it is safe to call under it.
 */

#include "bacnet_gateway.h"
#include "rgb_status.h"
#include "User_Settings.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* BACnet stack – needed for Send_I_Am and Handler_Transmit_Buffer */
#include "bacnet/basic/services.h"
#include "bacnet/datalink/datalink.h"
#include "bacnet/basic/tsm/tsm.h"

#include <string.h>

static const char *TAG = "bacnet_gw";

/* 3-second cooldown between consecutive I-Am broadcasts */
#define IAM_COOLDOWN_US  (3000000LL)

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */
typedef struct {
    bacnet_gw_config_t cfg;
    bacnet_gw_hooks_t  hooks;

    /*
     * Condition flags.
     * Network flags (eth/wifi) can be cleared on disconnect.
     * Object/MSTP flags are permanent once true.
     */
    bool objects_ready;  /**< Object layer initialized                      */
    bool eth_ip_ready;   /**< ETH interface has a valid IP                  */
    bool wifi_ip_ready;  /**< WiFi interface has a valid IP                 */
    bool mstp_ready;     /**< MS/TP stack initialized                       */

    /* Transport lifecycle */
    bool bip_initialized; /**< BIP UDP socket opened (never re-opened)      */
    bool transport_up;    /**< Any transport currently active               */

    /* I-Am rate limiting */
    int64_t last_iam_us;  /**< esp_timer_get_time() at last I-Am            */
} bacnet_gw_ctx_t;

static bacnet_gw_ctx_t s_gw;
static SemaphoreHandle_t s_mutex;

/* -------------------------------------------------------------------------
 * Lock helpers
 * ---------------------------------------------------------------------- */
static bool gw_lock(void)
{
    if (!s_mutex) {
        return false;
    }
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) == pdTRUE;
}

static void gw_unlock(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

/* -------------------------------------------------------------------------
 * Diagnostics
 * ---------------------------------------------------------------------- */
static const char *gw_event_name(bacnet_gw_event_t e)
{
    switch (e) {
    case BACNET_GW_EVT_STARTUP:       return "STARTUP";
    case BACNET_GW_EVT_ETH_IP_READY:  return "ETH_IP_READY";
    case BACNET_GW_EVT_WIFI_CONNECTED:return "WIFI_CONNECTED";
    case BACNET_GW_EVT_MSTP_READY:    return "MSTP_READY";
    case BACNET_GW_EVT_OBJECTS_READY: return "OBJECTS_READY";
    case BACNET_GW_EVT_NETWORK_DOWN:  return "NETWORK_DOWN";
    case BACNET_GW_EVT_ETH_IP_LOST:   return "ETH_IP_LOST";
    case BACNET_GW_EVT_WIFI_IP_LOST:  return "WIFI_IP_LOST";
    default:                           return "UNKNOWN";
    }
}

/* -------------------------------------------------------------------------
 * I-Am broadcast (called under s_mutex; Send_I_Am is lock-free)
 * ---------------------------------------------------------------------- */
void bacnet_gw_send_iam(void)
{
    int64_t now = esp_timer_get_time();
    if (now - s_gw.last_iam_us < IAM_COOLDOWN_US) {
        ESP_LOGD(TAG, "I-Am suppressed by cooldown");
        return;
    }
    s_gw.last_iam_us = now;
    Send_I_Am(&Handler_Transmit_Buffer[0]);
    ESP_LOGI(TAG, "I-Am broadcast sent");
}

/* -------------------------------------------------------------------------
 * Core evaluate function – the single decision point.
 *
 * Called under s_mutex after every flag change (or on startup).
 * Idempotent: correct regardless of invocation order or call count.
 *
 * Step 1 – Object layer:   initialize once, independent of network state.
 * Step 2 – MS/TP:          initialize once after objects are ready.
 * Step 3 – BIP transport:  configure on best available IP interface.
 * Step 4 – I-Am:           send when transport first activates or changes.
 * ---------------------------------------------------------------------- */
static void gw_evaluate(bacnet_gw_event_t trigger)
{
    /* ------------------------------------------------------------------
     * Step 1: Initialize BACnet object layer exactly once.
     * Intentionally NOT gated on any network flag.  This ensures objects
     * are available regardless of when (or whether) IP arrives.
     * ------------------------------------------------------------------ */
    if (!s_gw.objects_ready) {
        if (!s_gw.hooks.init_objects_once) {
            ESP_LOGE(TAG, "init_objects_once hook is NULL");
            return;
        }
        ESP_LOGI(TAG, "[GW] Initializing BACnet object layer");
        if (!s_gw.hooks.init_objects_once()) {
            ESP_LOGE(TAG, "[GW] Object init FAILED – gateway halted");
            return;
        }
        s_gw.objects_ready = true;
        ESP_LOGI(TAG, "[GW] Object layer ready");
        trigger = BACNET_GW_EVT_OBJECTS_READY;
    }

    /* ------------------------------------------------------------------
     * Step 2: Initialize MS/TP once (requires objects ready).
     * ------------------------------------------------------------------ */
    if (s_gw.cfg.enable_bacnet_mstp && !s_gw.mstp_ready) {
        if (s_gw.hooks.init_mstp_once && s_gw.hooks.init_mstp_once()) {
            s_gw.mstp_ready = true;
            ESP_LOGI(TAG, "[GW] MS/TP transport ready");
        }
    }

    /* ------------------------------------------------------------------
     * Step 3: Configure BIP transport on best available interface.
     * ETH is preferred over WiFi.  start_bip_on_interface() is idempotent:
     * first call opens the socket; subsequent calls update addresses only.
     * ------------------------------------------------------------------ */
    bool bip_net_available = (s_gw.cfg.enable_bacnet_ip_ethernet && s_gw.eth_ip_ready) ||
                             (s_gw.cfg.enable_bacnet_ip_wifi    && s_gw.wifi_ip_ready);

    if (bip_net_available && s_gw.hooks.start_bip_on_interface) {
        const char *ifkey = NULL;
        const char *label = NULL;

        if (s_gw.cfg.enable_bacnet_ip_ethernet && s_gw.eth_ip_ready) {
            ifkey = "ETH_DEF";
            label = "Ethernet";
        } else if (s_gw.cfg.enable_bacnet_ip_wifi && s_gw.wifi_ip_ready) {
            ifkey = "WIFI_STA_DEF";
            label = "WiFi";
        }

        if (ifkey) {
            esp_err_t err = s_gw.hooks.start_bip_on_interface(ifkey, label);
            if (err == ESP_OK) {
                if (!s_gw.bip_initialized) {
                    ESP_LOGI(TAG, "[GW] BIP transport initialized on %s", label);
                    s_gw.bip_initialized = true;
                } else {
                    ESP_LOGI(TAG, "[GW] BIP reconfigured on %s (trigger=%s)",
                             label, gw_event_name(trigger));
                }
            } else {
                ESP_LOGW(TAG, "[GW] start_bip_on_interface(%s) failed: %d", label, err);
            }
        }
    }

    /* ------------------------------------------------------------------
     * Step 4: Evaluate composite transport state; send I-Am on change.
     * ------------------------------------------------------------------ */
    bool bip_up  = s_gw.bip_initialized;
    bool mstp_up = s_gw.cfg.enable_bacnet_mstp && s_gw.mstp_ready;
    bool new_transport_up = bip_up || mstp_up;

    bool became_active     = new_transport_up && !s_gw.transport_up;
    bool interface_changed = s_gw.transport_up && new_transport_up &&
                             (trigger == BACNET_GW_EVT_ETH_IP_READY ||
                              trigger == BACNET_GW_EVT_WIFI_CONNECTED);

    if (s_gw.transport_up && !new_transport_up) {
        s_gw.transport_up = false;
        ESP_LOGW(TAG, "[GW] All BACnet transports down");
        return;
    }

    s_gw.transport_up = new_transport_up;

    if (became_active || interface_changed) {
        ESP_LOGI(TAG, "[GW] Transport %s (trigger=%s)",
                 became_active ? "became active" : "interface changed",
                 gw_event_name(trigger));
        if (s_gw.hooks.on_transport_active) {
            s_gw.hooks.on_transport_active();
        }
        bacnet_gw_send_iam();
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void bacnet_gw_init(const bacnet_gw_config_t *cfg, const bacnet_gw_hooks_t *hooks)
{
    memset(&s_gw, 0, sizeof(s_gw));
    s_gw.cfg   = *cfg;
    s_gw.hooks = *hooks;

    /* Force I-Am to fire immediately on first transport activation */
    s_gw.last_iam_us = INT64_MIN;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create gateway mutex");
    }

    ESP_LOGI(TAG, "[GW] Initialized: eth=%d wifi=%d mstp=%d",
             (int)cfg->enable_bacnet_ip_ethernet,
             (int)cfg->enable_bacnet_ip_wifi,
             (int)cfg->enable_bacnet_mstp);
}

void bacnet_gw_start(void)
{
    /*
     * Trigger initial evaluation:
     *   - Initializes BACnet objects unconditionally (no network required).
     *   - Activates transport if any network is already available.
     *
     * It is safe to call this before any network comes up.  Transport will
     * activate automatically when the first ETH/WiFi event arrives.
     */
    ESP_LOGI(TAG, "[GW] start(): triggering initial evaluation");
    if (!gw_lock()) {
        ESP_LOGE(TAG, "[GW] start(): failed to acquire lock");
        return;
    }
    gw_evaluate(BACNET_GW_EVT_STARTUP);
    gw_unlock();
}

void bacnet_gw_post_event(bacnet_gw_event_t event)
{
    ESP_LOGI(TAG, "[GW] Event: %s", gw_event_name(event));

    if (!gw_lock()) {
        ESP_LOGW(TAG, "[GW] post_event(%s): lock timeout – event dropped",
                 gw_event_name(event));
        return;
    }

    /* Update the appropriate condition flag */
    switch (event) {
    case BACNET_GW_EVT_ETH_IP_READY:
        s_gw.eth_ip_ready  = true;
        break;
    case BACNET_GW_EVT_WIFI_CONNECTED:
        s_gw.wifi_ip_ready = true;
        break;
    case BACNET_GW_EVT_MSTP_READY:
        s_gw.mstp_ready    = true;
        break;
    case BACNET_GW_EVT_OBJECTS_READY:
        s_gw.objects_ready = true;
        break;
    case BACNET_GW_EVT_NETWORK_DOWN:
        s_gw.eth_ip_ready = false;
        s_gw.wifi_ip_ready = false;
        break;
    case BACNET_GW_EVT_ETH_IP_LOST:
        s_gw.eth_ip_ready  = false;
        break;
    case BACNET_GW_EVT_WIFI_IP_LOST:
        s_gw.wifi_ip_ready = false;
        break;
    case BACNET_GW_EVT_STARTUP:
        /* No flag change – used as a synthetic startup trigger */
        break;
    }

    gw_evaluate(event);
    gw_unlock();
}

bool bacnet_gw_objects_are_ready(void)
{
    return s_gw.objects_ready;
}

bool bacnet_gw_transport_is_up(void)
{
    return s_gw.transport_up;
}

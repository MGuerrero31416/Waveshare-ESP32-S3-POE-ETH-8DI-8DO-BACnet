#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "User_Settings.h"

#if USER_ENABLE_RGB_STATUS

/* Initialize RGB status subsystem. Non-blocking. */
void rgb_status_init(void);

/* Base states */
void rgb_status_set_booting(void);
void rgb_status_set_waiting_for_ip(void);
void rgb_status_set_bacnet_ready(void);
void rgb_status_set_mstp_ready(void);
void rgb_status_set_error(void);
void rgb_status_bbmd_registered(void);

#else
/* When disabled, provide stubs so callers need not be guarded. */
static inline void rgb_status_init(void) {}
static inline void rgb_status_set_booting(void) {}
static inline void rgb_status_set_waiting_for_ip(void) {}
static inline void rgb_status_set_bacnet_ready(void) {}
static inline void rgb_status_set_error(void) {}
static inline void rgb_status_bbmd_registered(void) {}
static inline void rgb_status_set_mstp_ready(void) {}
#endif

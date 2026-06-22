#ifndef BACNET_NETWORK_H
#define BACNET_NETWORK_H

#include <stdint.h>

#define BACNET_NETWORK_EVT_ETH_GOT_IP   (1UL << 0)
#define BACNET_NETWORK_EVT_ETH_LOST_IP  (1UL << 1)
#define BACNET_NETWORK_EVT_WIFI_GOT_IP  (1UL << 2)
#define BACNET_NETWORK_EVT_WIFI_LOST_IP (1UL << 3)

void bacnet_network_notify(uint32_t event_bits);

#endif /* BACNET_NETWORK_H */

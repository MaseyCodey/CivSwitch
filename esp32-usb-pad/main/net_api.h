#ifndef NET_API_H
#define NET_API_H

#include "esp_err.h"

/* Starts Wi-Fi (STA from NVS/compile-time creds, else setup AP) and the
 * HTTP + raw TCP command servers. */
esp_err_t net_api_start(void);

#endif /* NET_API_H */

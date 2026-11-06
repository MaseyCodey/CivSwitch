#ifndef CIV7_H
#define CIV7_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void civ7_set_net(const uint8_t local_ip[4], const uint8_t host_ip[4],
                  const uint8_t host_mac[6], const uint8_t sta_mac[6]);
void civ7_rx_udp(const uint8_t *payload, uint16_t len);
int civ7_udp_send(uint16_t port, const uint8_t *data, size_t len);
void civ7_get_local_ip(uint8_t out[4]);
void civ7_run(void);
bool civ7_done(void);

#endif

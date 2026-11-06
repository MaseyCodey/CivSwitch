#ifndef INSTALLER_H
#define INSTALLER_H
#include <stdint.h>
#include <stddef.h>
void installer_run(void);
void installer_udp_run(void);
void installer_udp_rx(const uint8_t *data, size_t len);
#endif

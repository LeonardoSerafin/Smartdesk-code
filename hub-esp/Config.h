#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

#define RXD2 16
#define TXD2 17

static const uint8_t ESPNOW_CHANNEL = 1;
static const size_t UART_MAX = 4096;
static const uint32_t TABLE_ACK_TIMEOUT_MS = 2000;
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct TablePeer {
  int id;
  uint8_t mac[6];
};

#endif // CONFIG_H
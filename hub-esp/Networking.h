#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include "HubGlobals.h"

const uint8_t* findPeerMac(int tableId);
void flushPendingBookingResToTable();
bool waitSendDone(uint32_t timeoutMs);
bool sendTimeBroadcast(uint32_t msgId, uint32_t epochUtc, uint32_t &sendUs);
bool sendCurrentChunk();
void setupEspNow();

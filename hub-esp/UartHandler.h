#pragma once

#include <Arduino.h>
#include "HubGlobals.h"

void uartReplyOk(uint32_t msgId, int tableId, uint32_t uartUs, uint32_t sendUs, uint32_t ackUs, bool skipped);
void uartReplyErr(uint32_t msgId, const char* reason);
bool extractTableId(const char* json, int &tableId);
UartFrameType parseUartFrame(char* line, uint32_t &msgId, char* jsonOut, size_t jsonOutSize, uint32_t &epochOut);
uint8_t bookingReasonFromText(const char *reason);
bool parseBookResFrame(char *line, BookingResPacket &out);
void forwardBookingReqToUart(const BookingReqPacket &req);
void flushPendingBookingReqToUart();
void onSerial2Rx();

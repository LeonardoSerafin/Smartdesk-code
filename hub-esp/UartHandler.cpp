#include "UartHandler.h"
#include <ArduinoJson.h>

void uartReplyOk(uint32_t msgId, int tableId, uint32_t uartUs, uint32_t sendUs, uint32_t ackUs, bool skipped) {
  Serial2.printf("ACK|%lu|OK|table=%d|uart_us=%lu|send_us=%lu|ack_us=%lu|skipped=%d\n",
                 (unsigned long)msgId, tableId,
                 (unsigned long)uartUs,
                 (unsigned long)sendUs,
                 (unsigned long)ackUs,
                 skipped ? 1 : 0);
}

void uartReplyErr(uint32_t msgId, const char* reason) {
  Serial2.printf("ERR|%lu|%s\n", (unsigned long)msgId, reason);
  Serial.print("ERR msg=");
  Serial.print((unsigned long)msgId);
  Serial.print(" reason=");
  Serial.println(reason);
}

bool extractTableId(const char* json, int &tableId) {
  StaticJsonDocument<32> filter;
  filter["id"] = true;

  DynamicJsonDocument doc(128);
  DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(filter));
  if (err) return false;

  tableId = doc["id"] | -1;
  return tableId >= 0;
}

UartFrameType parseUartFrame(char* line, uint32_t &msgId, char* jsonOut, size_t jsonOutSize, uint32_t &epochOut) {
  if (strncmp(line, "MSG|", 4) == 0) {
    char* p1 = strchr(line, '|');
    if (!p1) return FRAME_INVALID;
    char* p2 = strchr(p1 + 1, '|');
    if (!p2) return FRAME_INVALID;

    *p2 = '\0';
    msgId = (uint32_t)strtoul(p1 + 1, nullptr, 10);

    const char* json = p2 + 1;
    size_t len = strlen(json);
    if (len == 0 || len >= jsonOutSize) return FRAME_INVALID;

    memcpy(jsonOut, json, len + 1);
    return FRAME_MSG;
  }

  if (strncmp(line, "TIME|", 5) == 0) {
    char* p1 = strchr(line, '|');
    if (!p1) return FRAME_INVALID;
    char* p2 = strchr(p1 + 1, '|');
    if (!p2) return FRAME_INVALID;

    *p2 = '\0';
    msgId = (uint32_t)strtoul(p1 + 1, nullptr, 10);

    const char* epochStr = p2 + 1;
    if (*epochStr == '\0') return FRAME_INVALID;

    epochOut = (uint32_t)strtoul(epochStr, nullptr, 10);
    if (epochOut < 1700000000UL) return FRAME_INVALID; // sanity check
    return FRAME_TIME;
  }

  return FRAME_INVALID;
}

uint8_t bookingReasonFromText(const char *reason) {
  if (strcmp(reason, "CONFLICT") == 0) return 1;
  if (strcmp(reason, "SERVER_TIMEOUT") == 0) return 2;
  if (strcmp(reason, "SERVER_ERROR") == 0) return 3;
  if (strcmp(reason, "BAD_REQUEST") == 0) return 4;
  return 3;
}

bool parseBookResFrame(char *line, BookingResPacket &out) {
  if (strncmp(line, "BOOKRES|", 8) != 0) return false;

  char *save = nullptr;
  char *tok = strtok_r(line, "|", &save); // BOOKRES
  if (!tok) return false;

  tok = strtok_r(nullptr, "|", &save); // msgId
  if (!tok) return false;
  uint32_t msgId = (uint32_t)strtoul(tok, nullptr, 10);

  tok = strtok_r(nullptr, "|", &save); // tableId
  if (!tok) return false;
  uint16_t tableId = (uint16_t)strtoul(tok, nullptr, 10);

  tok = strtok_r(nullptr, "|", &save); // action
  if (!tok) return false;
  uint8_t action = (uint8_t)strtoul(tok, nullptr, 10);

  tok = strtok_r(nullptr, "|", &save); // ok
  if (!tok) return false;
  uint8_t ok = (uint8_t)strtoul(tok, nullptr, 10);

  tok = strtok_r(nullptr, "|", &save); // reason
  if (!tok) return false;
  uint8_t reason = bookingReasonFromText(tok);

  tok = strtok_r(nullptr, "|", &save); // reservationId
  if (!tok) return false;
  uint64_t reservationId = strtoull(tok, nullptr, 10);

  out = {};
  out.type = (uint8_t)'R';
  out.action = action;
  out.msgId = msgId;
  out.tableId = tableId;
  out.ok = ok;
  out.reason = reason;
  out.reservationId = reservationId;
  return true;
}

void forwardBookingReqToUart(const BookingReqPacket &req) {
  Serial2.print("BOOKREQ|");
  Serial2.print((unsigned long)req.msgId);
  Serial2.print("|");
  Serial2.print((unsigned int)req.tableId);
  Serial2.print("|");
  Serial2.print((unsigned int)req.action);
  Serial2.print("|");
  Serial2.print((unsigned long long)req.reservationId);
  Serial2.print("|");
  Serial2.print((unsigned long)req.oraInizio);
  Serial2.print("|");
  Serial2.print((unsigned long)req.oraFine);
  Serial2.print("|");
  Serial2.println(req.uid);

  Serial.print("BOOKREQ msg=");
  Serial.print((unsigned long)req.msgId);
  Serial.print(" table=");
  Serial.print((unsigned int)req.tableId);
  Serial.print(" action=");
  Serial.println((unsigned int)req.action);
}

void flushPendingBookingReqToUart() {
  BookingReqPacket req = {};
  bool ready = false;

  portENTER_CRITICAL(&bookingReqMux);
  if (bookingReqReady) {
    req = bookingReqPending;
    bookingReqReady = false;
    ready = true;
  }
  portEXIT_CRITICAL(&bookingReqMux);

  if (ready) {
    forwardBookingReqToUart(req);
  }
}

// UART RX interrupt: assembles incoming bytes into uartBuf one line at a time.
// On newline the line is null-terminated and uartLineReady is raised for loop()
// to consume; further bytes are held until the flag is cleared. Lines longer
// than the buffer set uartOverflow and are discarded. tUartStart/tUartEnd
// timestamp the line for the latency metrics in the ACK reply.
void onSerial2Rx() {
  while (Serial2.available() && !uartLineReady) {
    char c = (char)Serial2.read();

    if (uartIdx == 0) tUartStart = micros();
    if (c == '\r') continue;

    if (c == '\n') {
      if (!uartOverflow && uartIdx < UART_MAX) {
        uartBuf[uartIdx] = '\0';
        tUartEnd = micros();
        uartLineReady = true;
      }
      uartIdx = 0;
      uartOverflow = false;
      return;
    }

    if (uartIdx < UART_MAX - 1) {
      uartBuf[uartIdx++] = c;
    } else {
      uartOverflow = true;
    }
  }
}

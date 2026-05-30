#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "Config.h"
#include "Protocol.h"
#include "HubGlobals.h"
#include "UartHandler.h"
#include "Networking.h"

#include "HubFunctions.h"

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, RXD2, TXD2);
  Serial2.onReceive(onSerial2Rx);

  setupEspNow();

  Serial.println("Hub pronto");
  Serial.print("Hub MAC: ");
  Serial.println(WiFi.macAddress());
}

void loop() {
  flushPendingBookingReqToUart();
  if (state == ST_IDLE) {
    flushPendingBookingResToTable();
  }

  if (state == ST_IDLE && uartLineReady) {
    noInterrupts();
    char line[UART_MAX];
    strncpy(line, uartBuf, sizeof(line));
    line[sizeof(line) - 1] = '\0';
    uartLineReady = false;
    interrupts();

    if (line[0] == '\0') {
      return;
    }

    BookingResPacket bookRes = {};
    if (parseBookResFrame(line, bookRes)) {
      bookingResPending = bookRes;
      bookingResPendingReady = true;
      return;
    }

    uint32_t msgId = 0;
    uint32_t epochUtc = 0;
    UartFrameType frameType = parseUartFrame(line, msgId, workJson, sizeof(workJson), epochUtc);
    if (frameType == FRAME_INVALID) {
      uartReplyErr(msgId, "BAD_FRAME");
      return;
    }

    uint32_t uartUs = tUartEnd - tUartStart;

    if (frameType == FRAME_TIME) {
      uint32_t sendUs = 0;
      bool ok = sendTimeBroadcast(msgId, epochUtc, sendUs);
      if (ok) {
        uartReplyOk(msgId, -1, uartUs, sendUs, 0, false);
      } else {
        uartReplyErr(msgId, "TIME_BROADCAST_FAIL");
      }
      return;
    }

    int tableId = -1;
    if (!extractTableId(workJson, tableId)) {
      uartReplyErr(msgId, "BAD_JSON_OR_ID");
      return;
    }

    const uint8_t* mac = findPeerMac(tableId);
    if (!mac) {
      uartReplyOk(msgId, tableId, uartUs, 0, 0, true);
      Serial.print("Skip msg=");
      Serial.print(msgId);
      Serial.print(" table=");
      Serial.print(tableId);
      Serial.println(" (no peer)");
      return;
    }

    workMsgId = msgId;
    workTableId = tableId;
    workMac = mac;

    workLen = strlen(workJson);
    if (workLen == 0) {
      uartReplyErr(msgId, "EMPTY_JSON");
      return;
    }

    const size_t chunkPayload = sizeof(((ChunkPacket*)0)->payload);
    workChunkCount = (uint16_t)((workLen + chunkPayload - 1) / chunkPayload);
    workChunkIndex = 0;

    tableAckReceived = false;
    tableAckOk = 0;

    tSendStartUs = micros();

    if (!sendCurrentChunk()) {
      uartReplyErr(workMsgId, "ESPNOW_SEND_CALL_FAIL");
      return;
    }

    state = ST_WAIT_SEND_CB;
    return;
  }

  if (state == ST_WAIT_SEND_CB) {
    if (!sendDoneFlag) return;

    sendDoneFlag = false;
    if (sendStatus != ESP_NOW_SEND_SUCCESS) {
      uartReplyErr(workMsgId, "ESPNOW_SEND_FAIL");
      state = ST_IDLE;
      return;
    }

    workChunkIndex++;
    if (workChunkIndex < workChunkCount) {
      if (!sendCurrentChunk()) {
        uartReplyErr(workMsgId, "ESPNOW_SEND_CALL_FAIL");
        state = ST_IDLE;
      }
      return;
    }

    tSendEndUs = micros();
    tAckWaitStartUs = micros();
    ackDeadlineMs = millis() + TABLE_ACK_TIMEOUT_MS;
    state = ST_WAIT_TABLE_ACK;
    return;
  }

  if (state == ST_WAIT_TABLE_ACK) {
    if (tableAckReceived) {
      tableAckReceived = false;

      if (tableAckMsgId == workMsgId &&
          tableAckTableId == (uint16_t)workTableId &&
          tableAckOk == 1) {

        uint32_t uartUs = tUartEnd - tUartStart;
        uint32_t sendUs = tSendEndUs - tSendStartUs;
        uint32_t ackUs = micros() - tAckWaitStartUs;

        uartReplyOk(workMsgId, workTableId, uartUs, sendUs, ackUs, false);

        Serial.print("OK msg=");
        Serial.print(workMsgId);
        Serial.print(" table=");
        Serial.print(workTableId);
        Serial.print(" size=");
        Serial.println(workLen);
      } else {
        uartReplyErr(workMsgId, "TABLE_ACK_MISMATCH");
      }

      state = ST_IDLE;
      return;
    }

    if ((int32_t)(millis() - ackDeadlineMs) >= 0) {
      uartReplyErr(workMsgId, "TABLE_ACK_TIMEOUT");
      state = ST_IDLE;
    }
  }
}
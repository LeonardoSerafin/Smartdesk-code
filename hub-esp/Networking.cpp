#include "Networking.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>

const uint8_t* findPeerMac(int tableId) {
  for (size_t i = 0; i < sizeof(peers) / sizeof(peers[0]); i++) {
    if (peers[i].id == tableId) return peers[i].mac;
  }
  return nullptr;
}

void flushPendingBookingResToTable() {
  if (!bookingResPendingReady) return;

  const uint8_t *mac = findPeerMac((int)bookingResPending.tableId);
  if (!mac) {
    bookingResPendingReady = false;
    return;
  }

  esp_now_send(mac, reinterpret_cast<const uint8_t *>(&bookingResPending), sizeof(bookingResPending));
  Serial.print("BOOKRES msg=");
  Serial.print((unsigned long)bookingResPending.msgId);
  Serial.print(" table=");
  Serial.print((unsigned int)bookingResPending.tableId);
  Serial.print(" ok=");
  Serial.println((unsigned int)bookingResPending.ok);
  bookingResPendingReady = false;
}

bool waitSendDone(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (!sendDoneFlag && (millis() - t0 < timeoutMs)) {
    delay(1);
  }
  if (!sendDoneFlag) return false;
  bool ok = (sendStatus == ESP_NOW_SEND_SUCCESS);
  sendDoneFlag = false;
  return ok;
}

// Broadcasts a time-sync packet to all tables. ESP-NOW broadcasts are not
// acknowledged, so the packet is sent twice (with a short gap) to improve the
// odds of delivery. Each send still waits for the local send callback.
bool sendTimeBroadcast(uint32_t msgId, uint32_t epochUtc, uint32_t &sendUs) {
  TimePacket p = {};
  p.type = (uint8_t)'T';
  p.msgId = msgId;
  p.epochUtc = epochUtc;

  uint32_t t0 = micros();

  for (int i = 0; i < 2; i++) {
    sendDoneFlag = false;
    if (esp_now_send(BROADCAST_MAC, (const uint8_t*)&p, sizeof(p)) != ESP_OK) {
      return false;
    }
    if (!waitSendDone(120)) {
      return false;
    }
    delay(120);
  }

  sendUs = micros() - t0;
  return true;
}

bool sendCurrentChunk() {
  const size_t chunkPayload = sizeof(((ChunkPacket*)0)->payload);

  ChunkPacket p = {};
  p.msgId = workMsgId;
  p.tableId = (uint16_t)workTableId;
  p.chunkIndex = workChunkIndex;
  p.chunkCount = workChunkCount;

  size_t off = (size_t)workChunkIndex * chunkPayload;
  size_t rem = workLen - off;
  p.payloadLen = (uint16_t)((rem > chunkPayload) ? chunkPayload : rem);
  memcpy(p.payload, workJson + off, p.payloadLen);

  sendDoneFlag = false;
  esp_err_t err = esp_now_send(workMac, (const uint8_t*)&p, sizeof(p));
  return err == ESP_OK;
}

// ESP-NOW send callback. The signature changed in Arduino-ESP32 core 3.x, so
// both variants are provided; both just record the status for the state machine.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  (void)tx_info;
  sendStatus = status;
  sendDoneFlag = true;
}
#else
void onEspNowSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  (void)mac_addr;
  sendStatus = status;
  sendDoneFlag = true;
}
#endif

// ESP-NOW receive callback. Dispatches by packet size: an AckPacket confirms a
// chunk transfer to a table; a BookingReqPacket (type 'B') is queued for
// forwarding to the Raspberry over UART.
void onEspNowRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  (void)recv_info;
  if (len == (int)sizeof(AckPacket)) {
    AckPacket ack;
    memcpy(&ack, data, sizeof(ack));

    tableAckMsgId = ack.msgId;
    tableAckTableId = ack.tableId;
    tableAckOk = ack.ok;
    tableAckReceived = true;
    return;
  }

  if (len == (int)sizeof(BookingReqPacket)) {
    BookingReqPacket req;
    memcpy(&req, data, sizeof(req));
    if (req.type != (uint8_t)'B') return;

    portENTER_CRITICAL_ISR(&bookingReqMux);
    bookingReqPending = req;
    bookingReqReady = true;
    portEXIT_CRITICAL_ISR(&bookingReqMux);
  }
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    while (true) { delay(1000); }
  }

  esp_now_register_send_cb(onEspNowSent);
  esp_now_register_recv_cb(onEspNowRecv);

  for (size_t i = 0; i < sizeof(peers) / sizeof(peers[0]); i++) {
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, peers[i].mac, 6);
    p.channel = ESPNOW_CHANNEL;
    p.encrypt = false;
    esp_now_add_peer(&p);
  }

  esp_now_peer_info_t b = {};
  memcpy(b.peer_addr, BROADCAST_MAC, 6);
  b.channel = ESPNOW_CHANNEL;
  b.encrypt = false;
  esp_now_add_peer(&b);
}

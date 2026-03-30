#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>

#define RXD2 16
#define TXD2 17

static const uint8_t ESPNOW_CHANNEL = 1;
static const size_t UART_MAX = 4096;
static const uint32_t TABLE_ACK_TIMEOUT_MS = 2000;
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#pragma pack(push, 1)
struct ChunkPacket {
  uint32_t msgId;
  uint16_t tableId;
  uint16_t chunkIndex;
  uint16_t chunkCount;
  uint16_t payloadLen;
  char payload[200];
};

struct AckPacket {
  uint32_t msgId;
  uint16_t tableId;
  uint8_t ok;
};

struct TimePacket {
  uint8_t type;        // 'T'
  uint32_t msgId;
  uint32_t epochUtc;   // secondi
};

struct BookingReqPacket {
  uint8_t type;      // 'B'
  uint8_t action;    // 1=create, 2=cancel
  uint32_t msgId;
  uint16_t tableId;
  uint64_t reservationId;
  uint32_t oraInizio;
  uint32_t oraFine;
  char uid[24];
};

struct BookingResPacket {
  uint8_t type;      // 'R'
  uint8_t action;    // 1=create, 2=cancel
  uint32_t msgId;
  uint16_t tableId;
  uint8_t ok;
  uint8_t reason;
  uint64_t reservationId;
};
#pragma pack(pop)

struct TablePeer {
  int id;
  uint8_t mac[6];
};

TablePeer peers[] = {
  {1, {0x28, 0x05, 0xA5, 0x0F, 0xDD, 0xEC}},
  // {2, {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF}},
};

volatile bool uartLineReady = false;
volatile bool uartOverflow = false;
volatile size_t uartIdx = 0;
char uartBuf[UART_MAX];

uint32_t tUartStart = 0;
uint32_t tUartEnd = 0;

volatile bool sendDoneFlag = false;
volatile esp_now_send_status_t sendStatus = ESP_NOW_SEND_FAIL;

volatile bool tableAckReceived = false;
volatile uint32_t tableAckMsgId = 0;
volatile uint16_t tableAckTableId = 0;
volatile uint8_t tableAckOk = 0;

portMUX_TYPE bookingReqMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool bookingReqReady = false;
BookingReqPacket bookingReqPending = {};

bool bookingResPendingReady = false;
BookingResPacket bookingResPending = {};

enum ProcState {
  ST_IDLE,
  ST_WAIT_SEND_CB,
  ST_WAIT_TABLE_ACK
};

ProcState state = ST_IDLE;

char workJson[UART_MAX];
uint32_t workMsgId = 0;
int workTableId = -1;
const uint8_t* workMac = nullptr;

size_t workLen = 0;
uint16_t workChunkCount = 0;
uint16_t workChunkIndex = 0;

uint32_t tSendStartUs = 0;
uint32_t tSendEndUs = 0;
uint32_t tAckWaitStartUs = 0;
uint32_t ackDeadlineMs = 0;

enum UartFrameType {
  FRAME_INVALID,
  FRAME_MSG,
  FRAME_TIME
};

const uint8_t* findPeerMac(int tableId) {
  for (size_t i = 0; i < sizeof(peers) / sizeof(peers[0]); i++) {
    if (peers[i].id == tableId) return peers[i].mac;
  }
  return nullptr;
}

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
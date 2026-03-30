#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include <Adafruit_NeoPixel.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_PN532.h>

// -------------------- ESP-NOW protocol (identico a table-esp.ino) --------------------
static const uint8_t ESPNOW_CHANNEL = 1;
static const uint32_t CLOCK_PRINT_INTERVAL_MS = 5000;

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
  uint8_t type;      // 'T'
  uint32_t msgId;
  uint32_t epochUtc; // secondi UTC
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
  uint8_t reason;    // 0=none, 1=conflict, 2=timeout, 3=server, 4=bad request
  uint64_t reservationId;
};
#pragma pack(pop)

// -------------------- Hardware config --------------------
#define TABLE_ID 1
// radar
#define SENSOR_RX 4
#define SENSOR_TX 16
// nfc
#define SDA_NFC 32
#define SCL_NFC 33
// display
#define TFT_MISO 19
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   5  // Chip select control pin
#define TFT_DC    2  // Data Command control pin
#define TFT_RST   17  // Reset pin (could connect to RST pin)
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH
// led
#define LED_PIN 15
#define NUM_LEDS 35

// Touch outputs (adatta i pin alla tua scheda)
#define TOUCH_OUT1_PIN 13
#define TOUCH_OUT2_PIN 12
#define TOUCH_OUT3_PIN 14
#define TOUCH_OUT4_PIN 27

// Radar timing (confermato)
#define DELAY_OCCUPATO 3000
#define TIMEOUT_LIBERO 8000

// Automazioni tavolo
static const uint32_t TIMEOUT_CANCELLAZIONE = 30000;
static const uint32_t TIMEOUT_AUTOBOOK = 20000;
static const uint32_t LONG_PRESS_CONFIRM_MS = 2000;
static const uint32_t BOOK_REQUEST_TIMEOUT_MS = 7000;
static const uint32_t BOOK_ERROR_DISPLAY_MS = 5000;
static const uint32_t BOOK_START_OFFSET_S = 2;
static const uint32_t PRESENCE_INVITE_DELAY_MS = 30000;
static const uint32_t ABSENCE_CANCEL_TIMEOUT_MS = 120000;
static const uint32_t ABSENCE_CANCEL_WINDOW_S = 600;
static const int PRESENCE_PROGRESS_BLOCKS = 10;

const uint8_t MASTER_UID_SUFFIX = 0x0B;

// -------------------- Stato sistema --------------------
enum StatoSistema { VIEW, BOOK_WAIT_NFC, BOOK_DETAILS };
StatoSistema statoAttuale = VIEW;

TFT_eSPI tft = TFT_eSPI();
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_PN532 nfc(SDA_NFC, SCL_NFC);

// -------------------- Stato prenotazioni --------------------
struct Reservation {
  int64_t id;
  String nome;
  uint32_t oraInizio;
  uint32_t oraFine;
  bool localOnly;
};

static const int MAX_RESERVATIONS = 24;
Reservation reservations[MAX_RESERVATIONS];
int reservationsCount = 0;
int64_t nextLocalId = -1;

// -------------------- Stato ESP-NOW RX --------------------
String reassembled;
uint32_t currentMsgId = 0;
uint16_t currentTableId = 0;
uint16_t expectedChunks = 0;
uint16_t receivedChunks = 0;
bool started = false;

// -------------------- Stato tempo --------------------
bool timeSynced = false;
uint32_t lastClockPrintMs = 0;

// JSON ricevuto da ESP-NOW, da processare nel loop (fuori callback radio).
String pendingReservationsJson;
bool pendingReservationsReady = false;
portMUX_TYPE pendingReservationsMux = portMUX_INITIALIZER_UNLOCKED;

uint8_t knownHubMac[6] = {0};
bool hubMacKnown = false;

enum BookingAction : uint8_t {
  BOOK_ACTION_NONE = 0,
  BOOK_ACTION_CREATE = 1,
  BOOK_ACTION_CANCEL = 2
};

enum BookingReason : uint8_t {
  BOOK_REASON_NONE = 0,
  BOOK_REASON_CONFLICT = 1,
  BOOK_REASON_SERVER_TIMEOUT = 2,
  BOOK_REASON_SERVER_ERROR = 3,
  BOOK_REASON_BAD_REQUEST = 4
};

bool bookingPending = false;
uint8_t bookingPendingAction = BOOK_ACTION_NONE;
uint32_t bookingPendingMsgId = 0;
uint32_t bookingPendingStartMs = 0;
uint32_t bookingPendingOraInizio = 0;
uint32_t bookingPendingOraFine = 0;
uint64_t bookingPendingReservationId = 0;
String bookingPendingUid;
uint32_t nextBookingMsgId = 1;

bool bookingErrorVisible = false;
unsigned long bookingErrorUntilMs = 0;
bool bookingErrorReturnToBooking = false;
String currentBookingUidHex;

bool bookingResultReady = false;
BookingResPacket bookingResult;
portMUX_TYPE bookingResultMux = portMUX_INITIALIZER_UNLOCKED;

// -------------------- Stato radar --------------------
bool presenzaConfermata = false;
bool inAttesaConferma = false;
bool ultimoStatoOn = false;
unsigned long ultimoOn = 0;
unsigned long primaPresenza = 0;
String rigaRadar;

// -------------------- Stato UI --------------------
int sceltaGiorno = 0; // 0=oggi, 1=domani
int sceltaOra = 9;    // 07..22
int sottomenu = 0;    // 0 giorno, 1 ora

bool bloccoLeds = false;
unsigned long startAssenza = 0;
unsigned long startPresenza = 0;
unsigned long lastDisplayRefreshMs = 0;
bool presenceInviteVisible = false;

// -------------------- Stato touch --------------------
struct TouchButtonState {
  uint8_t pin;
  bool lastRaw;
  bool stableRaw;
  bool pressedLatch;
  unsigned long lastChangeMs;
};

TouchButtonState btnOut1 = {TOUCH_OUT1_PIN, false, false, false, 0};
TouchButtonState btnOut2 = {TOUCH_OUT2_PIN, false, false, false, 0};
TouchButtonState btnOut3 = {TOUCH_OUT3_PIN, false, false, false, 0};
TouchButtonState btnOut4 = {TOUCH_OUT4_PIN, false, false, false, 0};

unsigned long bookingConfirmHoldStartMs = 0;

// -------------------- Forward declarations --------------------
bool ensurePeer(const uint8_t *mac);
void sendAck(const uint8_t *hubMac, uint32_t msgId, uint16_t tableId, uint8_t ok);
void onDataRecv(const esp_now_recv_info_t *recvInfo, const uint8_t *data, int len);

void applyTimeSync(uint32_t epochUtc);
String formatEpochLocal(uint32_t ts);
void printCurrentClockIfDue();

void parseReservationsFromJson(const String &json);
void printReservationsSummary();
void processPendingReservationsFromEspNow();
void processPendingBookingResponse();
void processBookingTimeout();

void inviaComando(const uint8_t *payload, size_t len);
void configuraSensoreRadar();
void processRadarInput();
void updateRadarState();

void setAllLeds(uint32_t color);
void displayTimeRemaining(time_t startTs, time_t endTs, time_t nowTs);
void writeOnLcd(const String &title, const String &sub, uint16_t color);

bool touchPressed(TouchButtonState &btn);
bool touchIsPressed(TouchButtonState &btn);
void handleButtons();
void handleNfc();
void drawBookingDetails();
void drawBookingConfirmProgress(uint32_t holdMs);
void renderView(bool force = false);
void drawPresenceProgressBlocks(uint32_t elapsedMs);
void showPresenceInviteScreen();
void showBookingError(const String &msg);
void showInvalidEndTimeError();
String bookingReasonToText(uint8_t reason);
String uidToHex(const uint8_t *uid, uint8_t uidLen);

int findActiveReservationIndex(time_t nowTs);
int findReservationIndexById(int64_t id);
void clearReservations();
void addReservation(int64_t id, const String &nome, uint32_t oraInizio, uint32_t oraFine, bool localOnly);
void removeReservationAt(int idx);
bool startCreateBookingRequest(const String &uidHex, uint32_t oraInizio, uint32_t oraFine);
bool startCancelBookingRequest(const Reservation &r);
void autoCancelActiveReservation();
void createAutoBooking();
void createManualBooking();
void runPresenceAutomations();

// -------------------- ESP-NOW --------------------
bool ensurePeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return true;

  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = ESPNOW_CHANNEL;
  p.encrypt = false;
  return esp_now_add_peer(&p) == ESP_OK;
}

void sendAck(const uint8_t *hubMac, uint32_t msgId, uint16_t tableId, uint8_t ok) {
  if (!ensurePeer(hubMac)) return;
  AckPacket ack = {msgId, tableId, ok};
  esp_now_send(hubMac, reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));
}

void onDataRecv(const esp_now_recv_info_t *recvInfo, const uint8_t *data, int len) {
  memcpy(knownHubMac, recvInfo->src_addr, 6);
  hubMacKnown = true;

  if (len == static_cast<int>(sizeof(TimePacket))) {
    TimePacket tp;
    memcpy(&tp, data, sizeof(tp));
    if (tp.type == static_cast<uint8_t>('T')) {
      applyTimeSync(tp.epochUtc);
      Serial.print("[TIME] sync ricevuto, epoch=");
      Serial.println(tp.epochUtc);
    }
    return;
  }

  if (len == static_cast<int>(sizeof(BookingResPacket))) {
    BookingResPacket res;
    memcpy(&res, data, sizeof(res));
    if (res.type == static_cast<uint8_t>('R') && res.tableId == TABLE_ID) {
      portENTER_CRITICAL(&bookingResultMux);
      bookingResult = res;
      bookingResultReady = true;
      portEXIT_CRITICAL(&bookingResultMux);
    }
    return;
  }

  if (len != static_cast<int>(sizeof(ChunkPacket))) return;

  ChunkPacket p;
  memcpy(&p, data, sizeof(p));

  if (!started || p.msgId != currentMsgId) {
    started = true;
    currentMsgId = p.msgId;
    currentTableId = p.tableId;
    expectedChunks = p.chunkCount;
    receivedChunks = 0;
    reassembled = "";
    reassembled.reserve(static_cast<size_t>(p.chunkCount) * 200);
  }

  if (p.chunkIndex != receivedChunks || p.payloadLen > sizeof(p.payload)) {
    sendAck(recvInfo->src_addr, currentMsgId, currentTableId, 0);
    started = false;
    return;
  }

  char tmp[201];
  memcpy(tmp, p.payload, p.payloadLen);
  tmp[p.payloadLen] = '\0';
  reassembled += tmp;
  receivedChunks++;

  if (receivedChunks == expectedChunks) {
    Serial.print("[ESPNOW] JSON tavolo ricevuto, size=");
    Serial.println(reassembled.length());

    bool queued = false;
    portENTER_CRITICAL(&pendingReservationsMux);
    if (!pendingReservationsReady) {
      pendingReservationsJson = reassembled;
      pendingReservationsReady = true;
      queued = true;
    }
    portEXIT_CRITICAL(&pendingReservationsMux);

    if (!queued) {
      Serial.println("[ESPNOW] JSON scartato: parser occupato");
    }

    sendAck(recvInfo->src_addr, currentMsgId, currentTableId, 1);
    started = false;
  }
}

void processPendingReservationsFromEspNow() {
  String json;

  portENTER_CRITICAL(&pendingReservationsMux);
  if (pendingReservationsReady) {
    json = pendingReservationsJson;
    pendingReservationsReady = false;
  }
  portEXIT_CRITICAL(&pendingReservationsMux);

  if (json.length() == 0) return;

  parseReservationsFromJson(json);
  if (statoAttuale == VIEW) {
    renderView(true);
  }
}

String uidToHex(const uint8_t *uid, uint8_t uidLen) {
  static const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(static_cast<size_t>(uidLen) * 2);
  for (uint8_t i = 0; i < uidLen; i++) {
    out += hex[(uid[i] >> 4) & 0x0F];
    out += hex[uid[i] & 0x0F];
  }
  return out;
}

String bookingReasonToText(uint8_t reason) {
  switch (reason) {
    case BOOK_REASON_CONFLICT: return String("Conflitto prenotazione");
    case BOOK_REASON_SERVER_TIMEOUT: return String("Server non raggiungibile");
    case BOOK_REASON_SERVER_ERROR: return String("Errore server");
    case BOOK_REASON_BAD_REQUEST: return String("Richiesta non valida");
    default: return String("Operazione fallita");
  }
}

void showBookingError(const String &msg) {
  bookingErrorVisible = true;
  bookingErrorUntilMs = millis() + BOOK_ERROR_DISPLAY_MS;
  bookingErrorReturnToBooking = false;

  if (msg.indexOf("Conflitto") >= 0) {
    tft.fillScreen(TFT_RED);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    tft.setCursor(10, 35);
    tft.println("CONFLITTO");
    tft.setCursor(10, 65);
    tft.println("PRENOTAZIONE");

    tft.setTextSize(1);
    tft.setCursor(10, 105);
    tft.println("Fascia oraria gia occupata.");
    tft.setCursor(10, 125);
    tft.println("Seleziona una nuova ora fine.");
    tft.setCursor(10, 155);
    tft.println("Ritorno automatico...");
    return;
  }

  writeOnLcd("PRENOTAZIONE", msg, TFT_RED);
}

void showInvalidEndTimeError() {
  bookingErrorVisible = true;
  bookingErrorUntilMs = millis() + BOOK_ERROR_DISPLAY_MS;
  bookingErrorReturnToBooking = true;

  tft.fillScreen(TFT_YELLOW);
  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 35);
  tft.println("ORARIO");
  tft.setCursor(10, 65);
  tft.println("NON VALIDO");

  tft.setTextSize(1);
  tft.setCursor(10, 105);
  tft.println("L'ora di fine deve essere");
  tft.setCursor(10, 125);
  tft.println("successiva all'istante attuale.");
  tft.setCursor(10, 155);
  tft.println("Ritorno a prenotazione...");
}

bool startCreateBookingRequest(const String &uidHex, uint32_t oraInizio, uint32_t oraFine) {
  if (bookingPending) return false;
  if (!hubMacKnown) {
    showBookingError("Hub non disponibile");
    return false;
  }

  BookingReqPacket req = {};
  req.type = static_cast<uint8_t>('B');
  req.action = BOOK_ACTION_CREATE;
  req.msgId = nextBookingMsgId++;
  req.tableId = TABLE_ID;
  req.reservationId = 0;
  req.oraInizio = oraInizio;
  req.oraFine = oraFine;

  String safeUid = uidHex;
  if (safeUid.length() == 0) safeUid = "UNKNOWN";
  strlcpy(req.uid, safeUid.c_str(), sizeof(req.uid));

  if (!ensurePeer(knownHubMac)) {
    showBookingError("Peer hub non valido");
    return false;
  }

  if (esp_now_send(knownHubMac, reinterpret_cast<const uint8_t *>(&req), sizeof(req)) != ESP_OK) {
    showBookingError("Invio richiesta fallito");
    return false;
  }

  bookingPending = true;
  bookingPendingAction = BOOK_ACTION_CREATE;
  bookingPendingMsgId = req.msgId;
  bookingPendingStartMs = millis();
  bookingPendingOraInizio = oraInizio;
  bookingPendingOraFine = oraFine;
  bookingPendingReservationId = 0;
  bookingPendingUid = safeUid;

  writeOnLcd("PRENOTAZIONE", "Verifica disponibilita...", TFT_CYAN);
  return true;
}

bool startCancelBookingRequest(const Reservation &r) {
  if (bookingPending) return false;
  if (!hubMacKnown) {
    showBookingError("Hub non disponibile");
    return false;
  }
  if (r.id <= 0) {
    showBookingError("ID prenotazione non valido");
    return false;
  }

  BookingReqPacket req = {};
  req.type = static_cast<uint8_t>('B');
  req.action = BOOK_ACTION_CANCEL;
  req.msgId = nextBookingMsgId++;
  req.tableId = TABLE_ID;
  req.reservationId = static_cast<uint64_t>(r.id);
  req.oraInizio = r.oraInizio;
  req.oraFine = r.oraFine;
  strlcpy(req.uid, r.nome.c_str(), sizeof(req.uid));

  if (!ensurePeer(knownHubMac)) {
    showBookingError("Peer hub non valido");
    return false;
  }

  if (esp_now_send(knownHubMac, reinterpret_cast<const uint8_t *>(&req), sizeof(req)) != ESP_OK) {
    showBookingError("Invio cancellazione fallito");
    return false;
  }

  bookingPending = true;
  bookingPendingAction = BOOK_ACTION_CANCEL;
  bookingPendingMsgId = req.msgId;
  bookingPendingStartMs = millis();
  bookingPendingReservationId = req.reservationId;
  bookingPendingUid = r.nome;

  writeOnLcd("PRENOTAZIONE", "Cancellazione in corso...", TFT_YELLOW);
  return true;
}

void processPendingBookingResponse() {
  BookingResPacket res = {};
  bool hasResponse = false;

  portENTER_CRITICAL(&bookingResultMux);
  if (bookingResultReady) {
    res = bookingResult;
    bookingResultReady = false;
    hasResponse = true;
  }
  portEXIT_CRITICAL(&bookingResultMux);

  if (!hasResponse) return;
  if (!bookingPending) return;
  if (res.msgId != bookingPendingMsgId || res.tableId != TABLE_ID || res.action != bookingPendingAction) {
    return;
  }

  bookingPending = false;

  if (res.ok == 1) {
    if (res.action == BOOK_ACTION_CREATE) {
      addReservation(static_cast<int64_t>(res.reservationId),
                     bookingPendingUid,
                     bookingPendingOraInizio,
                     bookingPendingOraFine,
                     false);
      Serial.print("[BOOK] Confermata dal server, id=");
      Serial.println(static_cast<unsigned long long>(res.reservationId));
      printReservationsSummary();
    } else if (res.action == BOOK_ACTION_CANCEL) {
      int idx = findReservationIndexById(static_cast<int64_t>(bookingPendingReservationId));
      if (idx >= 0) {
        removeReservationAt(idx);
        Serial.print("[BOOK] Cancellazione confermata, id=");
        Serial.println(static_cast<unsigned long long>(bookingPendingReservationId));
        printReservationsSummary();
      }
    }
    statoAttuale = VIEW;
    renderView(true);
    return;
  }

  showBookingError(bookingReasonToText(res.reason));
  statoAttuale = VIEW;
}

void processBookingTimeout() {
  if (!bookingPending) return;
  if (millis() - bookingPendingStartMs < BOOK_REQUEST_TIMEOUT_MS) return;

  bookingPending = false;
  showBookingError("Risposta server assente");
  statoAttuale = VIEW;
}

// -------------------- Tempo --------------------
void applyTimeSync(uint32_t epochUtc) {
  struct timeval tv;
  tv.tv_sec = static_cast<time_t>(epochUtc);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  timeSynced = true;
}

String formatEpochLocal(uint32_t ts) {
  time_t raw = static_cast<time_t>(ts);
  struct tm tinfo;
  if (localtime_r(&raw, &tinfo) == nullptr) return String("invalid");

  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tinfo);
  return String(buf);
}

void printCurrentClockIfDue() {
  uint32_t nowMs = millis();
  if (nowMs - lastClockPrintMs < CLOCK_PRINT_INTERVAL_MS) return;
  lastClockPrintMs = nowMs;

  if (!timeSynced) {
    Serial.println("[CLOCK] non sincronizzato");
    return;
  }

  time_t nowEpoch = time(nullptr);
  Serial.print("[CLOCK] Ora locale: ");
  Serial.println(formatEpochLocal(static_cast<uint32_t>(nowEpoch)));
}

// -------------------- Prenotazioni --------------------
void clearReservations() {
  reservationsCount = 0;
}

void addReservation(int64_t id, const String &nome, uint32_t oraInizio, uint32_t oraFine, bool localOnly) {
  if (oraFine <= oraInizio) return;

  if (reservationsCount >= MAX_RESERVATIONS) {
    // Mantieni le prenotazioni piu recenti.
    for (int i = 1; i < reservationsCount; i++) {
      reservations[i - 1] = reservations[i];
    }
    reservationsCount = MAX_RESERVATIONS - 1;
  }

  reservations[reservationsCount].id = id;
  reservations[reservationsCount].nome = nome;
  reservations[reservationsCount].oraInizio = oraInizio;
  reservations[reservationsCount].oraFine = oraFine;
  reservations[reservationsCount].localOnly = localOnly;
  reservationsCount++;
}

void removeReservationAt(int idx) {
  if (idx < 0 || idx >= reservationsCount) return;
  for (int i = idx + 1; i < reservationsCount; i++) {
    reservations[i - 1] = reservations[i];
  }
  reservationsCount--;
}

int findActiveReservationIndex(time_t nowTs) {
  for (int i = 0; i < reservationsCount; i++) {
    if (nowTs >= static_cast<time_t>(reservations[i].oraInizio) &&
        nowTs < static_cast<time_t>(reservations[i].oraFine)) {
      return i;
    }
  }
  return -1;
}

int findReservationIndexById(int64_t id) {
  for (int i = 0; i < reservationsCount; i++) {
    if (reservations[i].id == id) return i;
  }
  return -1;
}

void parseReservationsFromJson(const String &json) {
  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.print("[JSON] Errore parsing: ");
    Serial.println(err.c_str());
    return;
  }

  int tableId = doc["id"] | -1;
  if (tableId != TABLE_ID) {
    Serial.print("[JSON] Ignorato: tableId=");
    Serial.println(tableId);
    return;
  }

  JsonArray arr = doc["reservations"].as<JsonArray>();
  clearReservations();

  for (JsonObject r : arr) {
    int64_t rid = r["id"].as<int64_t>();
    const char *nome = r["nome"] | "";
    uint32_t oraInizio = r["oraInizio"] | 0;
    uint32_t oraFine = r["oraFine"] | 0;
    addReservation(rid, String(nome), oraInizio, oraFine, false);
  }

  printReservationsSummary();
}

void printReservationsSummary() {
  time_t nowEpoch = time(nullptr);

  Serial.println("----- TAVOLO -----");
  Serial.print("ID tavolo: ");
  Serial.println(TABLE_ID);
  Serial.print("Numero prenotazioni: ");
  Serial.println(reservationsCount);
  Serial.print("Clock synced: ");
  Serial.println(timeSynced ? "SI" : "NO");
  Serial.print("Ora corrente: ");
  Serial.println(formatEpochLocal(static_cast<uint32_t>(nowEpoch)));

  for (int i = 0; i < reservationsCount; i++) {
    const Reservation &r = reservations[i];
    const char *stato = "FUTURA";
    if (nowEpoch >= static_cast<time_t>(r.oraInizio) && nowEpoch < static_cast<time_t>(r.oraFine)) {
      stato = "ATTIVA";
    } else if (nowEpoch >= static_cast<time_t>(r.oraFine)) {
      stato = "PASSATA";
    }

    Serial.println("Prenotazione:");
    Serial.print("  id: ");
    Serial.println(static_cast<long long>(r.id));
    Serial.print("  nome: ");
    Serial.println(r.nome);
    Serial.print("  inizio: ");
    Serial.println(formatEpochLocal(r.oraInizio));
    Serial.print("  fine:   ");
    Serial.println(formatEpochLocal(r.oraFine));
    Serial.print("  stato:  ");
    Serial.println(stato);
  }

  Serial.println("------------------");
}

// -------------------- Radar --------------------
void inviaComando(const uint8_t *payload, size_t len) {
  const uint8_t header[] = {0xFD, 0xFC, 0xFB, 0xFA};
  const uint8_t footer[] = {0x04, 0x03, 0x02, 0x01};
  const uint8_t lenBytes[] = {static_cast<uint8_t>(len & 0xFF), static_cast<uint8_t>(len >> 8)};

  Serial2.write(header, 4);
  Serial2.write(lenBytes, 2);
  Serial2.write(payload, len);
  Serial2.write(footer, 4);
  delay(300);
}

void configuraSensoreRadar() {
  Serial.println("[RADAR] Configurazione...");

  const uint8_t apri[] = {0xFF, 0x00, 0x01, 0x00};
  inviaComando(apri, sizeof(apri));

  const uint8_t setGate[] = {0x07, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
  inviaComando(setGate, sizeof(setGate));

  const uint8_t setDelay[] = {0x07, 0x00, 0x04, 0x00, 0x03, 0x00, 0x00, 0x00};
  inviaComando(setDelay, sizeof(setDelay));

  const uint8_t chiudi[] = {0xFE, 0x00};
  inviaComando(chiudi, sizeof(chiudi));

  Serial.println("[RADAR] Configurazione completata");
}

void processRadarInput() {
  while (Serial2.available()) {
    char c = static_cast<char>(Serial2.read());
    if (c == '\n') {
      rigaRadar.trim();

      if (rigaRadar == "ON") {
        ultimoOn = millis();
        ultimoStatoOn = true;
        if (!inAttesaConferma && !presenzaConfermata) {
          inAttesaConferma = true;
          primaPresenza = millis();
          Serial.printf("[RADAR] ON, conferma tra %lus\n", DELAY_OCCUPATO / 1000UL);
        }
      } else if (rigaRadar == "OFF") {
        ultimoStatoOn = false;
      }

      rigaRadar = "";
    } else if (c != '\r') {
      rigaRadar += c;
    }
  }
}

void updateRadarState() {
  bool nuovaPresenza = presenzaConfermata;

  if (!presenzaConfermata && inAttesaConferma) {
    unsigned long tempoAttesa = millis() - primaPresenza;
    bool segnaleAttivo = (millis() - ultimoOn) < 1000;
    if (!segnaleAttivo) {
      inAttesaConferma = false;
      Serial.println("[RADAR] Passaggio breve, annullato");
    } else if (tempoAttesa >= DELAY_OCCUPATO) {
      nuovaPresenza = true;
      inAttesaConferma = false;
    }
  }

  if (presenzaConfermata) {
    unsigned long tempoAssenza = millis() - ultimoOn;
    if (tempoAssenza >= TIMEOUT_LIBERO) {
      nuovaPresenza = false;
      inAttesaConferma = false;
    }
  }

  if (nuovaPresenza != presenzaConfermata) {
    presenzaConfermata = nuovaPresenza;
    Serial.println(presenzaConfermata ? "[PRESENZA] OCCUPATO" : "[PRESENZA] LIBERO");
  }
}

// -------------------- UI / Display --------------------
void setAllLeds(uint32_t color) {
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, color);
  strip.show();
}

void displayTimeRemaining(time_t startTs, time_t endTs, time_t nowTs) {
  if (bloccoLeds) return;

  long duration = static_cast<long>(endTs - startTs);
  long elapsed = static_cast<long>(nowTs - startTs);
  if (duration <= 0) return;

  int ledsElapsed = static_cast<int>((static_cast<float>(elapsed) / duration) * NUM_LEDS);
  if (ledsElapsed < 0) ledsElapsed = 0;
  if (ledsElapsed > NUM_LEDS) ledsElapsed = NUM_LEDS;

  strip.clear();
  for (int i = 0; i < ledsElapsed; i++) strip.setPixelColor(i, strip.Color(0, 50, 0));
  for (int i = ledsElapsed; i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(50, 0, 0));
  strip.show();

  int minRimanenti = static_cast<int>((endTs - nowTs) / 60);
  tft.fillRect(10, 100, 220, 20, TFT_BLACK);
  tft.setCursor(10, 105);
  tft.setTextColor(TFT_LIGHTGREY);
  tft.setTextSize(1);
  if (minRimanenti > 0) tft.printf("Scadenza tra: %d min", minRimanenti);
}

void writeOnLcd(const String &title, const String &sub, uint16_t color) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(color);
  tft.setTextSize(2);
  tft.setCursor(10, 60);
  tft.println(title);

  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(10, 90);
  tft.println(sub);
}

void renderView(bool force) {
  const unsigned long refreshPeriod = 800;
  if (!force && millis() - lastDisplayRefreshMs < refreshPeriod) return;
  lastDisplayRefreshMs = millis();

  time_t nowTs = time(nullptr);
  int activeIdx = findActiveReservationIndex(nowTs);

  tft.fillScreen(TFT_BLACK);
  tft.drawFastHLine(0, 32, 240, TFT_DARKGREY);

  tft.setCursor(10, 10);
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);
  if (timeSynced) {
    struct tm ti;
    localtime_r(&nowTs, &ti);
    char b[8];
    strftime(b, sizeof(b), "%H:%M", &ti);
    tft.println(b);
  } else {
    tft.println("--:--");
  }

  tft.setCursor(130, 10);
  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY);
  tft.printf("Radar: %s", presenzaConfermata ? "ON" : "OFF");

  tft.setCursor(10, 50);
  if (activeIdx >= 0) {
    const Reservation &r = reservations[activeIdx];
    tft.setTextColor(TFT_RED);
    tft.setTextSize(2);
    tft.println("TAVOLO 1: OCCUPATO");

    tft.setCursor(10, 80);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.printf("Utente: %s", r.nome.c_str());

    displayTimeRemaining(static_cast<time_t>(r.oraInizio), static_cast<time_t>(r.oraFine), nowTs);
  } else {
    tft.setTextColor(TFT_GREEN);
    tft.setTextSize(2);
    tft.println("TAVOLO 1: LIBERO");

    tft.setCursor(10, 80);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.println("Pronto per prenotazione");

    if (!bloccoLeds) setAllLeds(strip.Color(0, 50, 0));
  }
}

void drawPresenceProgressBlocks(uint32_t elapsedMs) {
  int filled = static_cast<int>((static_cast<float>(elapsedMs) / PRESENCE_INVITE_DELAY_MS) * PRESENCE_PROGRESS_BLOCKS);
  if (filled < 0) filled = 0;
  if (filled > PRESENCE_PROGRESS_BLOCKS) filled = PRESENCE_PROGRESS_BLOCKS;

  const int x = 10;
  const int y = 170;
  const int w = 220;
  const int h = 14;
  const int spacing = 2;
  const int blockW = (w - (PRESENCE_PROGRESS_BLOCKS - 1) * spacing) / PRESENCE_PROGRESS_BLOCKS;

  tft.fillRect(x, y, w, h, TFT_BLACK);
  for (int i = 0; i < PRESENCE_PROGRESS_BLOCKS; i++) {
    int bx = x + i * (blockW + spacing);
    uint16_t color = (i < filled) ? TFT_MAGENTA : TFT_DARKGREY;
    tft.fillRect(bx, y, blockW, h, color);
  }
}

void showPresenceInviteScreen() {
  presenceInviteVisible = true;
  bloccoLeds = true;
  setAllLeds(strip.Color(90, 0, 90));

  tft.fillScreen(TFT_MAGENTA);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 35);
  tft.println("TAVOLO LIBERO");
  tft.setCursor(10, 65);
  tft.println("PRENOTATI");

  tft.setTextSize(1);
  tft.setCursor(10, 105);
  tft.println("Se ti sei seduto, effettua");
  tft.setCursor(10, 123);
  tft.println("la prenotazione con tessera.");
}

// -------------------- Touch + NFC + booking --------------------
bool touchPressed(TouchButtonState &btn) {
  bool stable = touchIsPressed(btn);
  if (stable && !btn.pressedLatch) {
    btn.pressedLatch = true;
    return true;
  }
  if (!stable) {
    btn.pressedLatch = false;
  }
  return false;
}

bool touchIsPressed(TouchButtonState &btn) {
  const unsigned long debounceMs = 50;
  bool raw = digitalRead(btn.pin) == HIGH;

  if (raw != btn.lastRaw) {
    btn.lastRaw = raw;
    btn.lastChangeMs = millis();
  }

  if ((millis() - btn.lastChangeMs) >= debounceMs) {
    btn.stableRaw = raw;
  }

  return btn.stableRaw;
}

void handleButtons() {
  if (bookingPending || bookingErrorVisible) return;

  bool out1Pressed = touchPressed(btnOut1);
  bool out2Pressed = touchPressed(btnOut2);
  bool out3Pressed = touchPressed(btnOut3);
  bool out4Pressed = touchPressed(btnOut4);
  bool anyPressed = out1Pressed || out2Pressed || out3Pressed || out4Pressed;

  if (statoAttuale == VIEW) {
    if (anyPressed) {
      statoAttuale = BOOK_WAIT_NFC;
      
      tft.fillScreen(TFT_BLUE);
      tft.setTextColor(TFT_BLACK);
      tft.setTextSize(2);
      tft.setCursor(10, 100);
      tft.println("PRENOTAZIONE");
      tft.setCursor(10, 130);
      tft.println("Avvicina la tessera...");
    }
    return;
  }

  if (statoAttuale == BOOK_WAIT_NFC) {
    if (anyPressed) {
      if (presenceInviteVisible) {
        // In modalita invito resta sempre sulla schermata viola.
        showPresenceInviteScreen();
      } else {
        statoAttuale = VIEW;
        renderView(true);
      }
    }
    return;
  }

  if (statoAttuale != BOOK_DETAILS) return;

  if (out1Pressed) {
    bookingConfirmHoldStartMs = 0;
    if (presenceInviteVisible) {
      // Se si arriva da invito, tornare sempre alla schermata viola.
      statoAttuale = BOOK_WAIT_NFC;
      showPresenceInviteScreen();
    } else {
      statoAttuale = VIEW;
      renderView(true);
    }
    return;
  }

  if (out2Pressed) {
    if (sottomenu == 0) {
      sceltaGiorno = (sceltaGiorno == 0) ? 1 : 0;
    } else if (sottomenu == 1) {
      sceltaOra--;
      if (sceltaOra < 7) sceltaOra = 22;
    }
    tft.fillScreen(TFT_BLACK);
  }

  if (out3Pressed) {
    if (sottomenu == 0) {
      sceltaGiorno = (sceltaGiorno == 0) ? 1 : 0;
    } else if (sottomenu == 1) {
      sceltaOra++;
      if (sceltaOra > 22) sceltaOra = 7;
    }
    tft.fillScreen(TFT_BLACK);
  }

  if (out4Pressed) {
    sottomenu = (sottomenu + 1) % 2;
    tft.fillScreen(TFT_BLACK);
  }

  bool out4Down = touchIsPressed(btnOut4);
  if (out4Down) {
    if (bookingConfirmHoldStartMs == 0) {
      bookingConfirmHoldStartMs = millis();
    }

    uint32_t heldMs = millis() - bookingConfirmHoldStartMs;
    drawBookingConfirmProgress(heldMs);

    if (heldMs >= LONG_PRESS_CONFIRM_MS) {
      createManualBooking();
      bookingConfirmHoldStartMs = 0;
      if (!bookingErrorVisible) {
        statoAttuale = VIEW;
      }
    }
  } else if (bookingConfirmHoldStartMs != 0) {
    bookingConfirmHoldStartMs = 0;
    drawBookingConfirmProgress(0);
  }
}

void handleNfc() {
  if (bookingPending || bookingErrorVisible) return;
  if (statoAttuale != BOOK_WAIT_NFC && statoAttuale != VIEW) return;

  uint8_t uid[7] = {0};
  uint8_t uidLen = 0;
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50)) return;

  bool isMaster = (uidLen > 0) && (uid[uidLen - 1] == MASTER_UID_SUFFIX);

  if (isMaster) {
    currentBookingUidHex = uidToHex(uid, uidLen);
    statoAttuale = BOOK_DETAILS;
    sottomenu = 0;
    bookingConfirmHoldStartMs = 0;
    tft.fillScreen(TFT_BLACK);
  } else if (statoAttuale == BOOK_WAIT_NFC) {
    if (presenceInviteVisible) {
      // In modalita invito ignoriamo tessere non valide per evitare escape.
      writeOnLcd("PRENOTAZIONE", "Tessera non valida", TFT_RED);
      delay(700);
      showPresenceInviteScreen();
    } else {
      autoCancelActiveReservation();
      statoAttuale = VIEW;
      if (!bookingPending) {
        renderView(true);
      }
    }
  }
}

void drawBookingDetails() {
  if (statoAttuale != BOOK_DETAILS || bookingErrorVisible) return;

  tft.setCursor(10, 20);
  tft.setTextColor(TFT_YELLOW);
  tft.setTextSize(2);
  tft.println("CONFIGURA:");

  tft.setCursor(10, 50);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.printf("UID: %s", currentBookingUidHex.length() == 0 ? "N/A" : currentBookingUidHex.c_str());

  tft.setCursor(10, 85);
  tft.setTextColor(sottomenu == 0 ? TFT_CYAN : TFT_WHITE);
  tft.setTextSize(2);
  tft.printf("D: %s", sceltaGiorno == 0 ? "OGGI" : "DOMANI");

  tft.setCursor(10, 125);
  tft.setTextColor(sottomenu == 1 ? TFT_CYAN : TFT_WHITE);
  tft.printf("FINE: %02d:00", sceltaOra);

  tft.setCursor(10, 165);
  tft.setTextColor(TFT_LIGHTGREY);
  tft.setTextSize(1);
  tft.println("A=Indietro B=Selezione campo");

  tft.setCursor(10, 182);
  tft.setTextColor(TFT_GREEN);
  tft.println("Tieni premuto B per confermare");

  drawBookingConfirmProgress(bookingConfirmHoldStartMs == 0 ? 0 : (millis() - bookingConfirmHoldStartMs));
}

void drawBookingConfirmProgress(uint32_t holdMs) {
  const int x = 10;
  const int y = 205;
  const int w = 220;
  const int h = 12;

  if (holdMs > LONG_PRESS_CONFIRM_MS) holdMs = LONG_PRESS_CONFIRM_MS;
  int fillW = static_cast<int>((static_cast<float>(holdMs) / LONG_PRESS_CONFIRM_MS) * w);

  tft.drawRect(x, y, w, h, TFT_DARKGREY);
  tft.fillRect(x + 1, y + 1, w - 2, h - 2, TFT_BLACK);
  if (fillW > 2) {
    tft.fillRect(x + 1, y + 1, fillW - 2, h - 2, TFT_GREEN);
  }
}

void createManualBooking() {
  time_t nowTs = time(nullptr);
  if (nowTs <= 0) {
    Serial.println("[BOOK] Tempo non disponibile, prenotazione manuale annullata");
    return;
  }

  struct tm ti;
  if (!getLocalTime(&ti)) {
    Serial.println("[BOOK] Tempo non disponibile, prenotazione manuale annullata");
    return;
  }

  ti.tm_hour = sceltaOra;
  ti.tm_min = 0;
  ti.tm_sec = 0;
  if (sceltaGiorno == 1) ti.tm_mday += 1;

  time_t tEnd = mktime(&ti);
  if (tEnd <= 0) return;

  if (tEnd <= nowTs + 60) {
    showInvalidEndTimeError();
    Serial.println("[BOOK] Ora fine selezionata non valida rispetto all'ora corrente");
    return;
  }

  uint32_t startTs = static_cast<uint32_t>(nowTs + BOOK_START_OFFSET_S);
  Serial.print("[BOOK] start offset sec=");
  Serial.println((unsigned long)BOOK_START_OFFSET_S);

  if (!startCreateBookingRequest(currentBookingUidHex,
                                 startTs,
                                 static_cast<uint32_t>(tEnd))) {
    Serial.println("[BOOK] Invio prenotazione manuale fallito");
  }
}

void autoCancelActiveReservation() {
  if (bookingPending) return;

  time_t nowTs = time(nullptr);
  int idx = findActiveReservationIndex(nowTs);
  if (idx < 0) return;

  if (startCancelBookingRequest(reservations[idx])) {
    Serial.print("[AUTO] Richiesta cancellazione inviata id=");
    Serial.println(static_cast<long long>(reservations[idx].id));
  }
}

void createAutoBooking() {
  if (bookingPending) return;

  time_t nowTs = time(nullptr);
  if (nowTs <= 0) return;

  uint32_t startTs = static_cast<uint32_t>(nowTs + BOOK_START_OFFSET_S);

  if (!startCreateBookingRequest("AUTO", startTs, static_cast<uint32_t>(nowTs + 3600))) {
    Serial.println("[AUTO] Invio auto-book fallito");
  }
}

void runPresenceAutomations() {
  if (bookingPending) return;
  if (bookingErrorVisible) return;
  if (statoAttuale != VIEW) return;

  time_t nowTs = time(nullptr);
  int activeIdx = findActiveReservationIndex(nowTs);

  if (activeIdx >= 0) {
    const Reservation &r = reservations[activeIdx];
    bool withinCancelWindow = (nowTs >= static_cast<time_t>(r.oraInizio) &&
                               nowTs <= static_cast<time_t>(r.oraInizio + ABSENCE_CANCEL_WINDOW_S));

    if (presenceInviteVisible) {
      presenceInviteVisible = false;
      bloccoLeds = false;
      renderView(true);
    }

    if (!presenzaConfermata) {
      if (withinCancelWindow) {
        if (startAssenza == 0) startAssenza = millis();

        unsigned long trascorso = millis() - startAssenza;
        if (trascorso < ABSENCE_CANCEL_TIMEOUT_MS) {
          int rim = static_cast<int>((ABSENCE_CANCEL_TIMEOUT_MS - trascorso) / 1000);
          tft.fillRect(0, 130, 240, 40, TFT_BLACK);
          tft.setCursor(10, 140);
          tft.setTextColor(TFT_YELLOW);
          tft.setTextSize(1);
          tft.printf("Assente: cancellazione in %ds", rim);
        } else {
          autoCancelActiveReservation();
          startAssenza = 0;
          renderView(true);
        }
      } else if (startAssenza != 0) {
        startAssenza = 0;
        renderView(true);
      }
    } else if (startAssenza != 0) {
      startAssenza = 0;
      renderView(true);
    }

    startPresenza = 0;
    bloccoLeds = false;
  } else {
    if (presenzaConfermata) {
      if (startPresenza == 0) {
        startPresenza = millis();
      }

      unsigned long trascorso = millis() - startPresenza;
      if (trascorso < PRESENCE_INVITE_DELAY_MS) {
        //int rim = static_cast<int>((PRESENCE_INVITE_DELAY_MS - trascorso) / 1000);
        tft.fillRect(0, 130, 240, 35, TFT_BLACK);
        tft.setCursor(10, 140);
        tft.setTextColor(TFT_MAGENTA);
        tft.setTextSize(1);
        //tft.printf("Prenota il tavolo tra: %ds", rim);
        tft.printf("Prenota il tavolo tra: ");
        drawPresenceProgressBlocks(static_cast<uint32_t>(trascorso));
      } else {
        if (!presenceInviteVisible) {
          showPresenceInviteScreen();
        }
      }
    } else if (startPresenza != 0) {
      startPresenza = 0;
      presenceInviteVisible = false;
      bloccoLeds = false;
      renderView(true);
    }

    startAssenza = 0;
  }
}

// -------------------- Setup/Loop --------------------
void setup() {
  Serial.begin(115200);

  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
  tzset();

  pinMode(TOUCH_OUT1_PIN, INPUT);
  pinMode(TOUCH_OUT2_PIN, INPUT);
  pinMode(TOUCH_OUT3_PIN, INPUT);
  pinMode(TOUCH_OUT4_PIN, INPUT);

  strip.begin();
  strip.setBrightness(50);
  strip.show();

  tft.init();
  tft.setRotation(1);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  Wire.begin(SDA_NFC, SCL_NFC);
  nfc.begin();
  if (nfc.getFirmwareVersion()) {
    nfc.SAMConfig();
    Serial.println("[NFC] pronto");
  } else {
    Serial.println("[NFC] non rilevato");
  }

  Serial2.begin(115200, SERIAL_8N1, SENSOR_RX, SENSOR_TX);
  delay(1000);
  configuraSensoreRadar();

  WiFi.mode(WIFI_STA);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] init failed");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onDataRecv);

  Serial.print("[BOOT] Table MAC: ");
  Serial.println(WiFi.macAddress());

  writeOnLcd("TAVOLO AVVIATO", "In attesa dati ESP-NOW", TFT_GREEN);
  delay(1000);
  renderView(true);
}

void loop() {
  processRadarInput();
  updateRadarState();
  processPendingReservationsFromEspNow();
  processPendingBookingResponse();
  processBookingTimeout();

  if (bookingErrorVisible && millis() >= bookingErrorUntilMs) {
    bookingErrorVisible = false;
    if (bookingErrorReturnToBooking) {
      bookingErrorReturnToBooking = false;
      statoAttuale = BOOK_DETAILS;
      bookingConfirmHoldStartMs = 0;
      tft.fillScreen(TFT_BLACK);
    } else {
      renderView(true);
    }
  }

  handleButtons();
  handleNfc();
  drawBookingDetails();

  runPresenceAutomations();

  if (statoAttuale == VIEW && !bookingPending && !bookingErrorVisible && !presenceInviteVisible) {
    renderView(false);
  }

  printCurrentClockIfDue();
}
#pragma once

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

#include "Config.h"
#include "Protocol.h"

// -------------------- Stato sistema --------------------
enum StatoSistema { VIEW, BOOK_WAIT_NFC, BOOK_DETAILS };
extern StatoSistema statoAttuale;

extern TFT_eSPI tft;
extern Adafruit_NeoPixel strip;
extern Adafruit_PN532 nfc;

// -------------------- Stato prenotazioni --------------------
struct Reservation {
  int64_t id;
  String nome;
  uint32_t oraInizio;
  uint32_t oraFine;
  bool localOnly;
  bool checkedIn;
  bool bookedFromTable;
};

extern Reservation reservations[MAX_RESERVATIONS];
extern int reservationsCount;
extern int64_t nextLocalId;

// -------------------- Stato ESP-NOW RX --------------------
extern String reassembled;
extern uint32_t currentMsgId;
extern uint16_t currentTableId;
extern uint16_t expectedChunks;
extern uint16_t receivedChunks;
extern bool started;

// -------------------- Stato tempo --------------------
extern bool timeSynced;
extern uint32_t lastClockPrintMs;

// JSON ricevuto da ESP-NOW, da processare nel loop (fuori callback radio).
extern String pendingReservationsJson;
extern bool pendingReservationsReady;
extern portMUX_TYPE pendingReservationsMux;

extern uint8_t knownHubMac[6];
extern bool hubMacKnown;

extern bool bookingPending;
extern uint8_t bookingPendingAction;
extern uint32_t bookingPendingMsgId;
extern uint32_t bookingPendingStartMs;
extern uint32_t bookingPendingOraInizio;
extern uint32_t bookingPendingOraFine;
extern uint64_t bookingPendingReservationId;
extern String bookingPendingUid;
extern bool bookingPendingFromTable;
extern uint32_t nextBookingMsgId;

extern bool bookingErrorVisible;
extern unsigned long bookingErrorUntilMs;
extern bool bookingErrorReturnToBooking;
extern String currentBookingUidHex;

extern bool bookingResultReady;
extern BookingResPacket bookingResult;
extern portMUX_TYPE bookingResultMux;

// -------------------- Stato radar --------------------
extern bool presenzaConfermata;
extern bool inAttesaConferma;
extern bool ultimoStatoOn;
extern unsigned long ultimoOn;
extern unsigned long primaPresenza;
extern String rigaRadar;

// -------------------- Stato UI --------------------
extern int bookingEndMinutes;

extern bool bloccoLeds;
extern unsigned long startAssenza;
extern unsigned long checkInPresenceStartMs;
extern int64_t checkInTrackingReservationId;
extern unsigned long startPresenza;
extern unsigned long lastDisplayRefreshMs;
extern bool presenceInviteVisible;

// -------------------- Stato touch --------------------
struct TouchButtonState {
  uint8_t pin;
  bool lastRaw;
  bool stableRaw;
  bool pressedLatch;
  unsigned long lastChangeMs;
};

extern TouchButtonState btnOut1;
extern TouchButtonState btnOut2;
extern TouchButtonState btnOut3;
extern TouchButtonState btnOut4;

extern unsigned long bookingConfirmHoldStartMs;
extern unsigned long bookingMinusHoldStartMs;
extern unsigned long bookingMinusLastRepeatMs;
extern unsigned long bookingPlusHoldStartMs;
extern unsigned long bookingPlusLastRepeatMs;


#include <Arduino.h>
#include "TableGlobals.h"
#include "TableFunctions.h"

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
  tft.setRotation(0);
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
      // If the presence-invite (purple) screen was visible, restore it.
      if (presenceInviteVisible) {
        showPresenceInviteScreen();
      } else {
        statoAttuale = BOOK_DETAILS;
        bookingConfirmHoldStartMs = 0;
        bookingMinusHoldStartMs = 0;
        bookingMinusLastRepeatMs = 0;
        bookingPlusHoldStartMs = 0;
        bookingPlusLastRepeatMs = 0;
        tft.fillScreen(TFT_BLACK);
      }
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
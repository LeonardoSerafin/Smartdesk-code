#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// -------------------- ESP-NOW protocol --------------------
static const uint8_t ESPNOW_CHANNEL = 1;
static const uint32_t CLOCK_PRINT_INTERVAL_MS = 5000;

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
#define TFT_CS   5
#define TFT_DC    2
#define TFT_RST   17
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH
// led
#define LED_PIN 15
#define NUM_LEDS 35

// Touch outputs
#define TOUCH_OUT1_PIN 13
#define TOUCH_OUT2_PIN 12
#define TOUCH_OUT3_PIN 14
#define TOUCH_OUT4_PIN 27

// Radar timing
#define DELAY_OCCUPATO 3000
#define TIMEOUT_LIBERO 8000

// Automazioni tavolo
static const uint32_t TIMEOUT_CANCELLAZIONE = 30000;
static const uint32_t TIMEOUT_AUTOBOOK = 20000;
static const uint32_t LONG_PRESS_CONFIRM_MS = 2000;
static const uint32_t BOOK_REQUEST_TIMEOUT_MS = 7000;
static const uint32_t BOOK_ERROR_DISPLAY_MS = 5000;
static const uint32_t BOOK_START_OFFSET_S = 1;
static const uint32_t PRESENCE_INVITE_DELAY_MS = 30000;
static const uint32_t ABSENCE_CANCEL_TIMEOUT_MS = 600000;
static const uint32_t ABSENCE_CANCEL_WINDOW_S = 600;
static const uint32_t CHECKIN_CONFIRM_OCCUPIED_MS = 30000;
static const int PRESENCE_PROGRESS_BLOCKS = 10;
static const int BOOK_TIME_STEP_MIN = 15;
static const uint32_t BOOK_REPEAT_INITIAL_DELAY_MS = 450;
static const uint32_t BOOK_REPEAT_INTERVAL_MS = 140;

const uint8_t MASTER_UID_SUFFIX = 0x0B;

static const int MAX_RESERVATIONS = 24;

#endif // CONFIG_H

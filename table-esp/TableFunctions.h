#pragma once
#include "TableGlobals.h"

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
void resetBookingEndSelection();
void adjustBookingEndMinutes(int steps);
void processBookingTimeHold(bool minusDown, bool plusDown);
void renderView(bool force = false);
void drawPresenceProgressBlocks(uint32_t elapsedMs);
void showPresenceInviteScreen();
void showBookingError(const String &msg);
void showInvalidCardError();
void showInvalidEndTimeError();
String bookingReasonToText(uint8_t reason);
String uidToHex(const uint8_t *uid, uint8_t uidLen);

int findActiveReservationIndex(time_t nowTs);
int findReservationIndexById(int64_t id);
void clearReservations();
void addReservation(int64_t id, const String &nome, uint32_t oraInizio, uint32_t oraFine, bool localOnly, bool checkedIn, bool bookedFromTable);
void removeReservationAt(int idx);
bool startCreateBookingRequest(const String &uidHex, uint32_t oraInizio, uint32_t oraFine, bool fromTable);
bool startCancelBookingRequest(const Reservation &r);
void autoCancelActiveReservation();
void createAutoBooking();
void createManualBooking();
void runPresenceAutomations();


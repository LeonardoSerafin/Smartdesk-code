#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <Arduino.h>

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

#endif // PROTOCOL_H

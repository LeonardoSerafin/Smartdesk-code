#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include "Config.h"
#include "Protocol.h"

extern TablePeer peers[];
extern volatile bool uartLineReady;
extern volatile bool uartOverflow;
extern volatile size_t uartIdx;
extern char uartBuf[UART_MAX];

extern uint32_t tUartStart;
extern uint32_t tUartEnd;

extern volatile bool sendDoneFlag;
extern volatile esp_now_send_status_t sendStatus;

extern volatile bool tableAckReceived;
extern volatile uint32_t tableAckMsgId;
extern volatile uint16_t tableAckTableId;
extern volatile uint8_t tableAckOk;

extern portMUX_TYPE bookingReqMux;
extern volatile bool bookingReqReady;
extern BookingReqPacket bookingReqPending;

extern bool bookingResPendingReady;
extern BookingResPacket bookingResPending;

enum ProcState {
  ST_IDLE,
  ST_WAIT_SEND_CB,
  ST_WAIT_TABLE_ACK
};

extern ProcState state;

extern char workJson[UART_MAX];
extern uint32_t workMsgId;
extern int workTableId;
extern const uint8_t* workMac;

extern size_t workLen;
extern uint16_t workChunkCount;
extern uint16_t workChunkIndex;

extern uint32_t tSendStartUs;
extern uint32_t tSendEndUs;
extern uint32_t tAckWaitStartUs;
extern uint32_t ackDeadlineMs;

enum UartFrameType {
  FRAME_INVALID,
  FRAME_MSG,
  FRAME_TIME
};

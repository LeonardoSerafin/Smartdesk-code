#include "HubGlobals.h"

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

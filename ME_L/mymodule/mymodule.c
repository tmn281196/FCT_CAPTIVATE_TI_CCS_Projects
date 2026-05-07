/* --COPYRIGHT--,BSD
 * Copyright (c) 2017, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * --/COPYRIGHT--*/
// #############################################################################
//
//! \file   Backchannel_UART_demo.c
//
//  Group:          MSP
//  Target Devices: MSP430FR2633
//
//  (C) Copyright 2018, Texas Instruments, Inc.
// #############################################################################
// TI Release: 1.83.00.05
// Release Date: May 15, 2020
// #############################################################################

//*****************************************************************************
// Includes
//*****************************************************************************

#include "mymodule.h"

//*****************************************************************************
// Definitions
//*****************************************************************************

//*****************************************************************************
// Global Variables
//*****************************************************************************

//*****************************************************************************
// Function Prototypes
//*****************************************************************************

//*****************************************************************************
// Function Implementations
//*****************************************************************************
#define NUM_BUTTONS 4
#define FRAMES_PER_BTN 6 // giả sử max cycles = 6, điều chỉnh theo thực tế
#define ELEMENTS_PER_CYCLE 2
#define FRAME_SIZE                                                             \
  (2 * ELEMENTS_PER_CYCLE +                                                    \
   6) // 2*ELEMENTS_PER_CYCLE  + 6 (STX + BTN_ID + CYCLE_ID + CRC16 + ETX)

static uint8_t tx_buf[NUM_BUTTONS][FRAMES_PER_BTN][FRAME_SIZE];
static uint8_t tx_frame_counts[NUM_BUTTONS] = {0}; // đếm riêng cho từng button
static volatile bool tx_pending_flags[NUM_BUTTONS] = {false};

#define RX_BUFFER_SIZE 128
#define RX_RING_SIZE 256
static uint8_t rx_buffer[RX_BUFFER_SIZE];
static uint8_t rx_index = 0;
static volatile uint8_t rx_ring[RX_RING_SIZE];
static volatile uint8_t rx_head = 0; // write index (ISR)
static volatile uint8_t rx_tail = 0; // read index (main)

#define STX 0xAA
#define ETX 0x55

/* Bien global parse */
static uint8_t g_btn_id = 0;
static uint8_t g_cycle_id = 0;
static uint8_t g_elem_id = 0;

/* Bien global */
static uint8_t g_tuning_field = 0; /* X=Offset, Y=Coarse, Z=Fine, W=LTA */
static uint16_t g_set_offset = 0;
static uint8_t g_set_coarse = 0;
static uint8_t g_set_fine = 0;
static uint16_t g_set_lta = 0;

extern volatile bool isFrozen;

// Hàm tính CRC-16 CCITT (poly 0x1021, init 0xFFFF)
static uint16_t calculate_crc16(const uint8_t *data, uint8_t len) {
  uint16_t crc = 0xFFFF;
  uint8_t i, j;

  for (i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8; // XOR byte vào MSB của CRC

    for (j = 0; j < 8; j++) {
      if (crc & 0x8000) {
        crc = (crc << 1) ^ 0x1021;
      } else {
        crc <<= 1;
      }
      crc &= 0xFFFF; // Giữ trong 16 bit
    }
  }
  return crc;
}

static void prepare_tx_for_button(tSensor *pSensor, uint8_t btn_idx,
                                  uint8_t button_id) {
  uint8_t *count_ptr = &tx_frame_counts[btn_idx];
  *count_ptr = 0;
  uint8_t iCycle;

  for (iCycle = 0; iCycle < pSensor->ui8NrOfCycles; iCycle++) {
    if (*count_ptr >= FRAMES_PER_BTN)
      break;
    uint8_t *frame = tx_buf[btn_idx][*count_ptr];

    frame[0] = STX;
    frame[1] = button_id;
    frame[2] = iCycle;

    uint8_t iElement;
    for (iElement = 0; iElement < ELEMENTS_PER_CYCLE; iElement++) {
      if (iElement < pSensor->pCycle[iCycle]->ui8NrOfElements) {
        uint16_t fc = pSensor->pCycle[iCycle]
                          ->pElements[iElement]
                          ->filterCount.ui16Natural;
        uint16_t lta =
            pSensor->pCycle[iCycle]->pElements[iElement]->LTA.ui16Natural;
        uint16_t delta = (fc > lta) ? (fc - lta) : (lta - fc);
        frame[3 + iElement * 2] = (uint8_t)(delta >> 8);   // MSB
        frame[4 + iElement * 2] = (uint8_t)(delta & 0xFF); // LSB
      } else {
        frame[3 + iElement * 2] = 0;
        frame[4 + iElement * 2] = 0;
      }
    }

    // === Chèn CRC-16 trước ETX ===
    // Tính CRC trên toàn bộ frame từ STX đến byte cuối data (không tính CRC và
    // ETX)
    uint8_t crc_pos_start = FRAME_SIZE - 3; // Vị trí bắt đầu của 2 bytes CRC
    uint8_t data_len = crc_pos_start; // Độ dài data để tính CRC (từ frame[0]
                                      // đến frame[crc_pos_start-1])

    uint16_t crc = calculate_crc16(frame, data_len);

    frame[crc_pos_start] = (uint8_t)(crc >> 8);       // CRC MSB (high byte)
    frame[crc_pos_start + 1] = (uint8_t)(crc & 0xFF); // CRC LSB (low byte)
    frame[FRAME_SIZE - 1] = ETX;                      // ETX ở cuối cùng

    (*count_ptr)++;
  }

  tx_pending_flags[btn_idx] = (*count_ptr > 0);
}

// callback functions for buttons
void btn00_callback(tSensor *pSensor) {
  prepare_tx_for_button(pSensor, 0, 0x00);
}
void btn01_callback(tSensor *pSensor) {
  prepare_tx_for_button(pSensor, 1, 0x01);
}
void btn02_callback(tSensor *pSensor) {
  prepare_tx_for_button(pSensor, 2, 0x02);
}
void btn03_callback(tSensor *pSensor) {
  prepare_tx_for_button(pSensor, 3, 0x03);
}

void transmit_pending_data(void) {
  uint8_t btn;
  for (btn = 0; btn < NUM_BUTTONS; btn++) {
    if (!tx_pending_flags[btn])
      continue;
    uint8_t i;
    for (i = 0; i < tx_frame_counts[btn]; i++) {
      UART_transmitBuffer(tx_buf[btn][i], FRAME_SIZE);
      // __delay_cycles(40000);
      while (!(UCA0IFG & UCTXIFG))
        ;
      // Thêm delay nhỏ để shift register hoàn thành byte cuối
      __delay_cycles(25000); // ~12.5ms @ 2MHz, đủ cho 1 frame 9600 baud
    }

    tx_frame_counts[btn] = 0;
    tx_pending_flags[btn] = false;
  }
}
void disable_handling(void) {
  isFrozen = true;

  static uint8_t frame[FRAME_SIZE];
  frame[0] = STX;
  frame[1] = 'D';
  frame[2] = 'I';
  frame[3] = 'S';

  frame[FRAME_SIZE - 1] = ETX;
  uint16_t crc = calculate_crc16(frame, FRAME_SIZE - 3);

  frame[FRAME_SIZE - 3] = (uint8_t)(crc >> 8);   // MSB
  frame[FRAME_SIZE - 2] = (uint8_t)(crc & 0xFF); // LSB

  UART_transmitBuffer(frame, FRAME_SIZE);
}

void enable_handling(void) {
  isFrozen = false;

  static uint8_t frame[FRAME_SIZE];
  frame[0] = STX;
  frame[1] = 'E';
  frame[2] = 'N';
  frame[3] = 'A';

  frame[FRAME_SIZE - 1] = ETX;
  uint16_t crc = calculate_crc16(frame, FRAME_SIZE - 3);

  frame[FRAME_SIZE - 3] = (uint8_t)(crc >> 8);   // MSB
  frame[FRAME_SIZE - 2] = (uint8_t)(crc & 0xFF); // LSB

  UART_transmitBuffer(frame, FRAME_SIZE);
}

void recalib_handling(void) {
  // Recalibrate toàn bộ UI
  CAPT_calibrateUI(&g_uiApp);

  static uint8_t frame[FRAME_SIZE];
  frame[0] = STX;
  frame[1] = 'C';
  frame[2] = 'A';
  frame[3] = 'L';

  frame[FRAME_SIZE - 1] = ETX;
  uint16_t crc = calculate_crc16(frame, FRAME_SIZE - 3);

  frame[FRAME_SIZE - 3] = (uint8_t)(crc >> 8);   // MSB
  frame[FRAME_SIZE - 2] = (uint8_t)(crc & 0xFF); // LSB

  UART_transmitBuffer(frame, FRAME_SIZE);
}

void verify_handling(void) {
  static uint8_t frame[FRAME_SIZE];
  frame[0] = STX;
  frame[1] = 'F';
  frame[2] = 'W';
  frame[3] = 'M';

  frame[FRAME_SIZE - 1] = ETX;
  uint16_t crc = calculate_crc16(frame, FRAME_SIZE - 3);

  frame[FRAME_SIZE - 3] = (uint8_t)(crc >> 8);   // MSB
  frame[FRAME_SIZE - 2] = (uint8_t)(crc & 0xFF); // LSB

  UART_transmitBuffer(frame, FRAME_SIZE);
}

void unknown_handling(void) {
  static uint8_t frame[FRAME_SIZE];
  frame[0] = STX;
  frame[1] = 'U';
  frame[2] = 'N';
  frame[3] = 'K';

  frame[FRAME_SIZE - 1] = ETX;
  uint16_t crc = calculate_crc16(frame, FRAME_SIZE - 3);

  frame[FRAME_SIZE - 3] = (uint8_t)(crc >> 8);   // MSB
  frame[FRAME_SIZE - 2] = (uint8_t)(crc & 0xFF); // LSB

  UART_transmitBuffer(frame, FRAME_SIZE);
}

void get_tuning_handling(uint8_t btn_id, uint8_t cycle_id, uint8_t elem_id,
                         uint8_t field) {
  tSensor *pSensor = NULL;
  if (btn_id == 0x00)
    pSensor = &BTN04;
  if (btn_id == 0x01)
    pSensor = &BTN03;
  if (btn_id == 0x02)
    pSensor = &BTN05;
  if (btn_id == 0x03)
    pSensor = &BTN06;

  if (pSensor == NULL) {
    unknown_handling();
    return;
  }
  if (cycle_id >= pSensor->ui8NrOfCycles) {
    unknown_handling();
    return;
  }
  if (elem_id >= pSensor->pCycle[cycle_id]->ui8NrOfElements) {
    unknown_handling();
    return;
  }

  tCaptivateElementTuning *pTuning =
      pSensor->pCycle[cycle_id]->pElements[elem_id]->pTuning;

  static uint8_t frame[FRAME_SIZE];
  frame[0] = STX;
  frame[1] = (uint8_t)field;
  frame[2] = btn_id;
  frame[3] = cycle_id;
  frame[4] = elem_id;

  switch (field) {
  case 'X': /* Offset */
    frame[5] = (uint8_t)(pTuning->ui16OffsetTap >> 8);
    frame[6] = (uint8_t)(pTuning->ui16OffsetTap & 0xFF);
    break;
  case 'Y': /* Coarse */
    frame[5] = pTuning->ui8CoarseGainRatio;
    frame[6] = 0;
    break;
  case 'Z': /* Fine */
    frame[5] = pTuning->ui8FineGainRatio;
    frame[6] = 0;
    break;
  case 'W': { /* LTA */
    uint16_t lta =
        pSensor->pCycle[cycle_id]->pElements[elem_id]->LTA.ui16Natural;
    frame[5] = (uint8_t)(lta >> 8);
    frame[6] = (uint8_t)(lta & 0xFF);
    break;
  }
  default:
    unknown_handling();
    return;
  }

  frame[7] = 0;
  frame[FRAME_SIZE - 1] = ETX;
  uint16_t crc = calculate_crc16(frame, FRAME_SIZE - 3);
  frame[FRAME_SIZE - 3] = (uint8_t)(crc >> 8);
  frame[FRAME_SIZE - 2] = (uint8_t)(crc & 0xFF);
  UART_transmitBuffer(frame, FRAME_SIZE);
}

static void send_ack_frame(uint8_t btn_id, uint8_t cycle_id, uint8_t elem_id,
                           uint8_t field) {
  static uint8_t frame[FRAME_SIZE];
  frame[0] = STX;
  frame[1] = 'S';
  frame[2] = btn_id;
  frame[3] = cycle_id;
  frame[4] = elem_id;
  frame[5] = field;
  frame[6] = 0;
  frame[7] = 0;
  frame[FRAME_SIZE - 1] = ETX;
  uint16_t crc = calculate_crc16(frame, FRAME_SIZE - 3);
  frame[FRAME_SIZE - 3] = (uint8_t)(crc >> 8);
  frame[FRAME_SIZE - 2] = (uint8_t)(crc & 0xFF);
  UART_transmitBuffer(frame, FRAME_SIZE);
}

void set_tuning_handling(uint8_t btn_id, uint8_t cycle_id, uint8_t elem_id,
                         uint8_t field) {
  tSensor *pSensor = NULL;
  if (btn_id == 0x00)
    pSensor = &BTN04;
  if (btn_id == 0x01)
    pSensor = &BTN03;
  if (btn_id == 0x02)
    pSensor = &BTN05;
  if (btn_id == 0x03)
    pSensor = &BTN06;
  if (pSensor == NULL) {
    unknown_handling();
    return;
  }
  if (cycle_id >= pSensor->ui8NrOfCycles) {
    unknown_handling();
    return;
  }
  if (elem_id >= pSensor->pCycle[cycle_id]->ui8NrOfElements) {
    unknown_handling();
    return;
  }

  tCaptivateElementTuning *pTuning =
      pSensor->pCycle[cycle_id]->pElements[elem_id]->pTuning;

  switch (field) {
  case 'X':
    pTuning->ui16OffsetTap = g_set_offset;
    break;
  case 'Y':
    pTuning->ui8CoarseGainRatio = g_set_coarse & 0x07;
    break;
  case 'Z':
    pTuning->ui8FineGainRatio = g_set_fine & 0x1F;
    break;
  case 'W':
    pSensor->pCycle[cycle_id]->pElements[elem_id]->LTA.ui16Natural = g_set_lta;
        /* Reset filter ve LTA moi */
    pSensor->pCycle[cycle_id]->pElements[elem_id]->bReseed      = true;
    pSensor->pCycle[cycle_id]->pElements[elem_id]->bElementHalt = false;
    break;
  default:
    unknown_handling();
    return;
  }

  /* Reset filter ve LTA moi */
  pSensor->pCycle[cycle_id]->pElements[elem_id]->bReseed = true;
  pSensor->pCycle[cycle_id]->pElements[elem_id]->bElementHalt = false;

  send_ack_frame(btn_id, cycle_id, elem_id, field);
}

typedef enum {
  CMD_UNKNOWN = 0,
  CMD_RESET,
  CMD_VERIFY,
  CMD_DISABLE,
  CMD_ENABLE,
  CMD_GET_TUNING,
  CMD_SET_TUNING
} command_t;

command_t parse_command(char *cmd) {
  if (strcmp(cmd, "RESET") == 0)
    return CMD_RESET;

  if (strcmp(cmd, "DISABLE") == 0)
    return CMD_DISABLE;
  if (strcmp(cmd, "ENABLE") == 0)
    return CMD_ENABLE;
  if (strcmp(cmd, "ID0001") == 0)
    return CMD_VERIFY;

  /* Parse GET_TUNING:<btn>:<cycle>:<elem>:<field>
   * Vi du: GET_TUNING:0:0:0:O\r */
  if (strncmp(cmd, "GET_TUNING:", 11) == 0) {
    char *p = cmd + 11;
    g_btn_id = (uint8_t)strtoul(p, &p, 10);
    if (*p == ':')
      p++;
    g_cycle_id = (uint8_t)strtoul(p, &p, 10);
    if (*p == ':')
      p++;
    g_elem_id = (uint8_t)strtoul(p, &p, 10);
    if (*p == ':')
      p++;
    g_tuning_field = (uint8_t)*p;
    return CMD_GET_TUNING;
  }

  /* Parse SET_TUNING:<btn>:<cycle>:<elem>:<field>:<value>
   * Vi du: SET_TUNING:0:0:0:O:1234\r
   *        SET_TUNING:0:0:0:C:5\r
   *        SET_TUNING:0:0:0:F:12\r  */
  if (strncmp(cmd, "SET_TUNING:", 11) == 0) {
    char *p = cmd + 11;
    g_btn_id = (uint8_t)strtoul(p, &p, 10);
    if (*p == ':')
      p++;
    g_cycle_id = (uint8_t)strtoul(p, &p, 10);
    if (*p == ':')
      p++;
    g_elem_id = (uint8_t)strtoul(p, &p, 10);
    if (*p == ':')
      p++;
    g_tuning_field = (uint8_t)*p;
    p++; /* skip field char 'O'/'C'/'F' */
    if (*p == ':')
      p++; /* skip ':' */
    if (g_tuning_field == 'X')
      g_set_offset = (uint16_t)strtoul(p, NULL, 10);
    else if (g_tuning_field == 'Y')
      g_set_coarse = (uint8_t)strtoul(p, NULL, 10);
    else if (g_tuning_field == 'Z')
      g_set_fine = (uint8_t)strtoul(p, NULL, 10);
    else if (g_tuning_field == 'W')
      g_set_lta = (uint16_t)strtoul(p, NULL, 10);
    return CMD_SET_TUNING;
  }
  return CMD_UNKNOWN;
}

void process_command(char *cmd) {
  command_t command = parse_command(cmd);

  switch (command) {
  case CMD_RESET:
    recalib_handling();
    break;

  case CMD_DISABLE:
    disable_handling();
    break;
  case CMD_ENABLE:
    enable_handling();
    break;

  case CMD_VERIFY:
    verify_handling();
    break;

  case CMD_GET_TUNING:
    get_tuning_handling(g_btn_id, g_cycle_id, g_elem_id, g_tuning_field);
    break;
  case CMD_SET_TUNING:
    set_tuning_handling(g_btn_id, g_cycle_id, g_elem_id, g_tuning_field);
    break;
  case CMD_UNKNOWN:
  default:
    unknown_handling();
    break;
  }
}

void uart_rx_callback(uint8_t data_byte) { // Giữ tên nếu driver yêu cầu
  uint8_t next_head = (rx_head + 1) & (RX_RING_SIZE - 1);
  if (next_head != rx_tail) { // Không full
    rx_ring[rx_head] = data_byte;
    rx_head = next_head;
  } // else overrun → có thể flag error
}

uint8_t uart_available(void) {
  return (rx_head - rx_tail) & (RX_RING_SIZE - 1);
}

int16_t uart_read(void) {
  if (rx_head == rx_tail)
    return -1; // không có data

  uint8_t data = rx_ring[rx_tail];
  rx_tail = (rx_tail + 1) & (RX_RING_SIZE - 1);

  return data;
}

void process_rx(void) {
  while (uart_available()) {
    int16_t data = uart_read();
    if (data < 0)
      break;

    uint8_t byte = (uint8_t)data;

    // Bỏ qua LF để tránh double trigger CRLF
    if (byte == '\n')
      continue;

    if (byte == '\r') {
      rx_buffer[rx_index] = '\0';
      process_command((char *)rx_buffer);
      rx_index = 0;
    } else if (rx_index < RX_BUFFER_SIZE - 1) {
      rx_buffer[rx_index++] = byte;
    } else {
      // Chuỗi quá dài → reset tránh lỗi
      rx_index = 0;
    }
  }
}
// SMCLK = 2 MHz, Baud = 9600
const tUARTPort UARTPort = {
    .pbReceiveCallback = &uart_rx_callback,
    .pbErrorCallback = NULL,

    .peripheralParameters.selectClockSource = EUSCI_A_UART_CLOCKSOURCE_SMCLK,

    // UCBRx = 13
    .peripheralParameters.clockPrescalar = 13,

    // UCBRFx = 5
    .peripheralParameters.firstModReg = 5,

    // UCBRSx = 0x49 (đúng)
    .peripheralParameters.secondModReg = 0x49,

    .peripheralParameters.parity = EUSCI_A_UART_NO_PARITY,

    .peripheralParameters.msborLsbFirst = EUSCI_A_UART_LSB_FIRST,

    .peripheralParameters.numberofStopBits = EUSCI_A_UART_ONE_STOP_BIT,

    .peripheralParameters.uartMode = EUSCI_A_UART_MODE,

    .peripheralParameters.overSampling =
        EUSCI_A_UART_OVERSAMPLING_BAUDRATE_GENERATION};

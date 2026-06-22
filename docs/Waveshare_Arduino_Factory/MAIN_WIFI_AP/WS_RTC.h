#pragma once

#include "WS_PCF85063.h"
#include "WS_Dout.h"
#include "WS_GPIO.h"
#include "WS_RS485.h"
#include "WS_CAN.h"

#define Timing_events_Number_MAX    10              // Indicates the number of timers that can be set

typedef enum {
  RTC_Mode0 = 0,        // Mode0  ----  Dout
  RTC_Mode1 = 1,        // Mode1  ----  Serial
  RTC_Mode2 = 2,        // retain
  RTC_Mode3 = 3,        // retain
} RTC_mode;
typedef struct {
  bool Enable_Flag;                   // The timer event enabled flag.
  uint8_t Event_Number;               // Current event sequence number
  RTC_mode RTC_Mode = RTC_Mode0;              // The timer event enabled flag.
  Status_adjustment Dout_CH1 = STATE_Retain;                       // The CH1 status is changed periodically
  Status_adjustment Dout_CH2 = STATE_Retain;                       // The CH2 status is changed periodically
  Status_adjustment Dout_CH3 = STATE_Retain;                       // The CH3 status is changed periodically
  Status_adjustment Dout_CH4 = STATE_Retain;                       // The CH4 status is changed periodically
  Status_adjustment Dout_CH5 = STATE_Retain;                       // The CH5 status is changed periodically
  Status_adjustment Dout_CH6 = STATE_Retain;                       // The CH6 status is changed periodically
  Status_adjustment Dout_CH7 = STATE_Retain;                       // The CH7 status is changed periodically
  Status_adjustment Dout_CH8 = STATE_Retain;                       // The CH8 status is changed periodically
  datetime_t Time;                    // Execution date
  Web_Receive Web_Data;               // Execution content
  Repetition_event repetition_State;  // Periodic execution
} Timing_RTC;

extern uint8_t Timing_events_Num;
extern Timing_RTC CHx_State[Timing_events_Number_MAX];
extern char Event_str[Timing_events_Number_MAX][1000];

void RTCTask(void *parameter);
void Continuous_Task(void *parameter);
void TimerEvent_handling(Timing_RTC event);
void TimerEvent_printf(Timing_RTC event);
void TimerEvent_Del(Timing_RTC event);

void RTC_Init(void);
void TimerEvent_CHx_Set(datetime_t time,uint8_t CHx, bool State, Repetition_event Repetition);
void TimerEvent_CHxs_Set(datetime_t time,uint8_t PinState, Repetition_event Repetition);
void TimerEvent_CHxn_Set(datetime_t time,Status_adjustment *Dout_n, uint32_t *repetition_Time, Repetition_event Repetition);

void TimerEvent_Serial_Set(datetime_t time, Web_Receive* SerialData, Repetition_event Repetition);

void TimerEvent_printf_ALL(void);
void TimerEvent_Del_Number(uint8_t Event_Number);
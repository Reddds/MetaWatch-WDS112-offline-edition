//==============================================================================
//  Copyright 2011 Meta Watch Ltd. - http://www.MetaWatch.org/
//
//  Licensed under the Meta Watch License, Version 1.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.MetaWatch.org/licenses/license-1.0.html
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//==============================================================================

/******************************************************************************/
/*! \file Background.c
*
* The background task handles everything that does not belong with a specialized
* task.
*/
/******************************************************************************/

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "Messages.h"
#include "MessageQueues.h"

#include "hal_board_type.h"
#include "hal_rtc.h"
#include "hal_analog_display.h"
#include "hal_battery.h"
#include "hal_miscellaneous.h"
#include "hal_clock_control.h"
#include "hal_lpm.h"
#include "hal_crystal_timers.h"

#include "Buttons.h"
#include "Background.h"
#include "DebugUart.h"
#include "Utilities.h"
#include "Wrapper.h"
#include "Adc.h"
#include "OneSecondTimers.h"
#include "Vibration.h"
#include "Statistics.h"
#include "OSAL_Nv.h"
#include "NvIds.h"
#include "Display.h"
#include "LcdDisplay.h"
#include "OledDriver.h"
#include "OledDisplay.h"
#include "Accelerometer.h"
#include "Calendar.h"

static void BackgroundTask(void *pvParameters);

static void BackgroundMessageHandler(tMessage* pMsg);

static void AdvanceWatchHandsHandler(tMessage* pMsg);
static void EnableButtonMsgHandler(tMessage* pMsg);
static void DisableButtonMsgHandler(tMessage* pMsg);
static void ReadButtonConfigHandler(tMessage* pMsg);
static void ReadBatteryVoltageHandler(void);
static void ReadLightSensorHandler(void);
static void NvalOperationHandler(tMessage* pMsg);
static void SoftwareResetHandler(tMessage* pMsg);
static void SetCallbackTimerHandler(tMessage* pMsg);

#define BACKGROUND_MSG_QUEUE_LEN   8
#define BACKGROUND_STACK_SIZE	   (configMINIMAL_STACK_SIZE + 100)
#define BACKGROUND_TASK_PRIORITY   (tskIDLE_PRIORITY + 1)

xTaskHandle xBkgTaskHandle;

static tMessage BackgroundMsg;

static tTimerId BatteryMonitorTimerId;
static void InitializeBatteryMonitorInterval(void);

static unsigned int nvBatteryMonitorIntervalInSeconds;

static unsigned char LedOn;
static tTimerId LedTimerId;
static void LedChangeHandler(tMessage* pMsg);
static tTimerId CallbackTimerId;

/******************************************************************************/

/* externed with hal_lpm */
unsigned char nvRstNmiConfiguration;
static void InitializeRstNmiConfiguration(void);

/******************************************************************************/

static void NvUpdater(unsigned int NvId);

/******************************************************************************/

#ifdef RAM_TEST

static tTimerId RamTestTimerId;

#endif

/******************************************************************************/

#ifdef RATE_TEST

static unsigned char RateTestCallback(void);

#define RATE_TEST_INTERVAL_MS ( 1000 )

#endif

// Alarm -----------------------------------------------------------------------
static tTimerId AlarmTimerId;

unsigned char nvAlarmOn[10];
unsigned char currentAlarm = 0;
unsigned char nvAlarmMinutes[10]; // 10 будильников
unsigned char nvAlarmHours[10]; // 10 будильников

void GenerateAlarm(void);

#ifdef DIARY

// Diary -----------------------------------------------------------------------

//  DiaryIsEmptyRecord = 0xbc,
//  Получение статуса ячейки


//  DiaryWriteRecord = 0xbd,
// Options = 0 (3-8 бит - номер ячейки)
// Запись первой части данных в ячейку (статус и настройки)
// 0 - Статус
// 1 - вид
// 2,3,4 год(от 12 года), мес, день
// 5, 6 - время

//  DiaryWriteRecord = 0xbd,
// Options = 1 (3-8 бит - номер ячейки)
// Запись второй части данных (текст)


// NUMBER_OF_DIARY_RECORDS записей по TEXT_LENGTH_OF_DIARY_RECORDS символов
// 0 байт - статус (4)  0 - пустая ячейка 1 - одноразово 2 - каждый год
//                      3 - каждый месяц 4 - каждую неделю
//                      5 - каждый день
//   вид напоминания (4) 0 - без напоминания
//                      1 - вибро
// 3,4 - дата - [год(4бита от 12 года), мес(4 бита)], день
// 5,6 - время
typedef struct
{
  unsigned char Status;
  unsigned char Alarm;
  unsigned char Year;
  unsigned char Month;
  unsigned char Day;
  unsigned char DayOfWeek;
  unsigned char Hour;
  unsigned char Minute;

} tDiaryRecord;


#define NUMBER_OF_DIARY_RECORDS 10//14
#define TEXT_LENGTH_OF_DIARY_RECORDS 20
unsigned char diary[NUMBER_OF_DIARY_RECORDS][(5 + TEXT_LENGTH_OF_DIARY_RECORDS)];
signed char shownId = -1; // Отображаемое первое событие
unsigned char alarmRecord = 0; // Текщее событие сейчас!
unsigned char updateDiaryOnScreen = 0; // Перерисовать на экране

unsigned char eventIndexes[3]; // 3 индекса событий, которые отображаются на экране
unsigned char diaryEventStatus[4]; // 0 - текущий, Остальные для 3х событий на экране
unsigned int  diaryEventYear[4];
unsigned char diaryEventMonth[4];
unsigned char diaryEventDay[4];
unsigned char diaryEventDow[4];
unsigned char diaryEventHour[4];
unsigned char diaryEventMin[4];
unsigned char *diaryEventText[4];

/*
unsigned char diaryEventStatus = diary[shownId][0] >> 4;
unsigned int  diaryEventYear = 2012 + (diary[shownId][1] >> 4);
unsigned char diaryEventMonth = diary[shownId][1] & 0xf;
unsigned char diaryEventDay = (diary[shownId][2] >> 3);
unsigned char diaryEventDow = (diary[shownId][2] & 0x7);
unsigned char diaryEventHour = diary[shownId][3];
unsigned char diaryEventMin = diary[shownId][4];*/

unsigned char distanceToEvent[NUMBER_OF_DIARY_RECORDS]; // условный временный интервал до события  

void ShowDiary();
static void DiaryWriteRecordHandler(tMessage* pMsg);
static void DiaryWriteEndHandler();
static unsigned char  LoadDiaryEvent(unsigned char eventIndex, unsigned int year, 
                           unsigned char month, unsigned char weekday,
                           unsigned char day,
                           unsigned char hour, unsigned char min);
static void SortDiaryEventsOnDate(unsigned int year, unsigned char month, 
                                  unsigned char day, unsigned char hour,
                                  unsigned char min);

#endif


// Time ------------------------------------------------------------------------
// Количество секунд, которые набегают за 10 дней
signed char nvCorrectionValue = 0;
/******************************************************************************/

/*! Does the initialization and allocates the resources for the background task
 *
 */
void InitializeBackgroundTask( void )
{
  // This is a Rx message queue, messages come from Serial IO or button presses
  QueueHandles[BACKGROUND_QINDEX] =
    xQueueCreate( BACKGROUND_MSG_QUEUE_LEN, MESSAGE_QUEUE_ITEM_SIZE );

  // prams are: task function, task name, stack len , task params, priority, task handle
  xTaskCreate(BackgroundTask,
              (const signed char *)"BACKGROUND",
              BACKGROUND_STACK_SIZE,
              NULL,
              BACKGROUND_TASK_PRIORITY,
              &xBkgTaskHandle);

}


/*! Function to implement the BackgroundTask loop
 *
 * \param pvParameters not used
 *
 */
static void BackgroundTask(void *pvParameters)
{
  if ( QueueHandles[BACKGROUND_QINDEX] == 0 )
  {
    PrintString("Background Queue not created!\r\n");
  }

  PrintString(SPP_DEVICE_NAME);
  PrintString2("\r\nSoftware Version ",VERSION_STRING);
  PrintString("\r\n\r\n");

  InitializeRstNmiConfiguration();

  /*
   * check on the battery
   */
  ConfigureBatteryPins();
  BatteryChargingControl();
  BatterySenseCycle();

  /*
   * now set up a timer that will cause the battery to be checked at
   * a regular frequency.
   */
  BatteryMonitorTimerId = AllocateOneSecondTimer();

  InitializeBatteryMonitorInterval();

  SetupOneSecondTimer(BatteryMonitorTimerId,
                      nvBatteryMonitorIntervalInSeconds,
                      REPEAT_FOREVER,
                      BACKGROUND_QINDEX,
                      BatteryChargeControl,
                      NO_MSG_OPTIONS);

  StartOneSecondTimer(BatteryMonitorTimerId);

  /*
   * Setup a timer to use with the LED for the LCD.
   */
  LedTimerId = AllocateOneSecondTimer();

  SetupOneSecondTimer(LedTimerId,
                      ONE_SECOND*3,
                      NO_REPEAT,
                      BACKGROUND_QINDEX,
                      LedChange,
                      LED_OFF_OPTION);

  // Alarm ---------------------------------------------------------------------
  
  AlarmTimerId = AllocateOneSecondTimer();

  //InitializeBatteryMonitorInterval();

  SetupOneSecondTimer(AlarmTimerId,
                      ONE_SECOND*5,
                      REPEAT_FOREVER,
                      BACKGROUND_QINDEX,
                      AlarmControl,
                      NO_MSG_OPTIONS);

  StartOneSecondTimer(AlarmTimerId);
  // Allocate a timer for wake-up iOS background BLE app
  CallbackTimerId = AllocateOneSecondTimer();

  /****************************************************************************/

#ifdef RAM_TEST

  RamTestTimerId = AllocateOneSecondTimer();

  SetupOneSecondTimer(RamTestTimerId,
                      ONE_SECOND*20,
                      NO_REPEAT,
                      DISPLAY_QINDEX,
                      RamTestMsg,
                      NO_MSG_OPTIONS);

  StartOneSecondTimer(RamTestTimerId);

#endif

  /****************************************************************************/

  InitializeAccelerometer();

#ifdef ACCELEROMETER_DEBUG

  SetupMessageAndAllocateBuffer(&BackgroundMsg,
                                AccelerometerSetupMsg,
                                ACCELEROMETER_SETUP_INTERRUPT_CONTROL_OPTION);

  BackgroundMsg.pBuffer[0] = INTERRUPT_CONTROL_ENABLE_INTERRUPT;
  BackgroundMsg.Length = 1;
  RouteMsg(&BackgroundMsg);

  /* don't call AccelerometerEnable() directly use a message*/
  SetupMessage(&BackgroundMsg,AccelerometerEnableMsg,NO_MSG_OPTIONS);
  RouteMsg(&BackgroundMsg);

#endif

  /****************************************************************************/

#ifdef RATE_TEST

  StartCrystalTimer(CRYSTAL_TIMER_ID3,RateTestCallback,RATE_TEST_INTERVAL_MS);

#endif

  /****************************************************************************/

  for(;;)
  {
    if( pdTRUE == xQueueReceive(QueueHandles[BACKGROUND_QINDEX],
                                &BackgroundMsg, portMAX_DELAY ) )
    {
      PrintMessageType(&BackgroundMsg);

      BackgroundMessageHandler(&BackgroundMsg);

      SendToFreeQueue(&BackgroundMsg);

      CheckStackUsage(xBkgTaskHandle,"Background Task");

      CheckQueueUsage(QueueHandles[BACKGROUND_QINDEX]);

    }

  }

}

/*! Handle the messages routed to the background task */
static void BackgroundMessageHandler(tMessage* pMsg)
{
  tMessage OutgoingMsg;
      
  unsigned char doAlarm = 0; // Звонить? Чтобы не звонить 10 раз, если будильники на одно время

  switch(pMsg->Type)
  {
    case SetCallbackTimerMsg:
      SetCallbackTimerHandler(pMsg);
      break;

  case GetDeviceType:

    SetupMessageAndAllocateBuffer(&OutgoingMsg,
                                  GetDeviceTypeResponse,
                                  NO_MSG_OPTIONS);

    OutgoingMsg.pBuffer[0] = BOARD_TYPE;
    OutgoingMsg.Length = 1;
    RouteMsg(&OutgoingMsg);

    break;

  case AdvanceWatchHandsMsg:
    AdvanceWatchHandsHandler(pMsg);
    break;

  case SetVibrateMode:
    SetVibrateModeHandler(pMsg);
    break;

  case SetRealTimeClock:
    halRtcSet((tRtcHostMsgPayload*)pMsg->pBuffer);

#ifdef DIGITAL
    SetupMessage(&OutgoingMsg,IdleUpdate,NO_MSG_OPTIONS);
    RouteMsg(&OutgoingMsg);
#endif
    break;

  case GetRealTimeClock:

    SetupMessageAndAllocateBuffer(&OutgoingMsg,
                                  GetRealTimeClockResponse,
                                  NO_MSG_OPTIONS);

    halRtcGet((tRtcHostMsgPayload*)OutgoingMsg.pBuffer);
    OutgoingMsg.Length = sizeof(tRtcHostMsgPayload);
    RouteMsg(&OutgoingMsg);
    break;

  case EnableButtonMsg:
    EnableButtonMsgHandler(pMsg);
    break;

  case DisableButtonMsg:
    DisableButtonMsgHandler(pMsg);
    break;

  case ReadButtonConfigMsg:
    ReadButtonConfigHandler(pMsg);
    break;

  case BatteryChargeControl:

#ifdef DIGITAL
    /* update the screen if there has been a change in charging status */
    if ( BatteryChargingControl() )
    {
      SetupMessage(&OutgoingMsg,IdleUpdate,NO_MSG_OPTIONS);
      RouteMsg(&OutgoingMsg);
    }
#endif

    BatterySenseCycle();
    LowBatteryMonitor();

#ifdef TASK_DEBUG
    UTL_FreeRtosTaskStackCheck();
#endif

#if 0
    LightSenseCycle();
#endif

    break;

  case LedChange:
    LedChangeHandler(pMsg);
    break;

  case BatteryConfigMsg:
    SetBatteryLevels(pMsg->pBuffer);
    break;

  case ReadBatteryVoltageMsg:
    ReadBatteryVoltageHandler();
    break;

  case ReadLightSensorMsg:
    ReadLightSensorHandler();
    break;

  case SoftwareResetMsg:
    SoftwareResetHandler(pMsg);
    break;

  case NvalOperationMsg:
    NvalOperationHandler(pMsg);
    break;

  case GeneralPurposeWatchMsg:
    /* insert handler here */
    break;

  case ButtonStateMsg:
    ButtonStateHandler();
    break;

  /*
   * Accelerometer Messages
   */
  case AccelerometerEnableMsg:
    AccelerometerEnable();
    break;

  case AccelerometerDisableMsg:
    AccelerometerDisable();
    break;

  case AccelerometerSendDataMsg:
    AccelerometerSendDataHandler();
    break;

  case AccelerometerAccessMsg:
    AccelerometerAccessHandler(pMsg);
    break;

  case AccelerometerSetupMsg:
    AccelerometerSetupHandler(pMsg);
    break;

  /*
   *
   */
  case RateTestMsg:
    SetupMessageAndAllocateBuffer(&OutgoingMsg,DiagnosticLoopback,NO_MSG_OPTIONS);
    /* don't care what data is */
    OutgoingMsg.Length = 10;
    RouteMsg(&OutgoingMsg);
    break;

  case AlarmControl:
    // Проверка событий ежедневника shownId
#ifdef DIARY__
    // Проверка событий ежедневника shownId
    for(unsigned char i = 0; i < NUMBER_OF_DIARY_RECORDS; i++)
    {
      // Если первый показ
      if(shownId < 0)
      {// рисуем события
        ShowDiary();
        shownId = 0;
      }
      else
      {// иначе проверяем, не наступило ли ближайшее из событий
        LoadDiaryEvent(shownId);
        
        // Событие наступило!
        if(diaryEventYear == RTCYEAR && diaryEventMonth == RTCMON && diaryEventDay == RTCDAY
           && diaryEventHour == RTCHOUR && diaryEventMin == RTCMIN)
        {
          alarmRecord = 1;
          ShowDiary();
        }
      }
    }
#endif          
    // проверка будильников
    for(unsigned char i = 0; i < 10; i++)
    {
      if(nvAlarmOn[i] == 0)
        continue;
      
      if(RTCHOUR == nvAlarmHours[i] && RTCMIN == nvAlarmMinutes[i])
      {
        // Выключим будильник
        nvAlarmOn[i] = 0;
        doAlarm = 1;
      }
    }
    if(doAlarm == 1)
      GenerateAlarm();
    break;
#ifdef DIARY
  case DiaryWriteRecord:
    DiaryWriteRecordHandler(pMsg);
    break;
  case DiaryWriteEnd:
    DiaryWriteEndHandler();
    break;
#endif
  /*
   *
   */
  default:
    PrintStringAndHex("<<Unhandled Message>> in Background Task: Type 0x", pMsg->Type);
    break;
  }

}

#ifdef DIARY

void ShowDiary()
{
  SortDiaryEventsOnDate(RTCYEAR, RTCMON, RTCDAY, RTCHOUR, RTCMIN);
  updateDiaryOnScreen = 1;
}

// RTCYEAR RTCMON  RTCDOW RTCDAY
// Если событие в эту дату, возвращается 1
// hour, min - событие должно быть после этогов ремени
static unsigned char  LoadDiaryEvent(unsigned char eventIndex, unsigned int year, 
                           unsigned char month, unsigned char weekday,
                           unsigned char day,
                           unsigned char hour, unsigned char min)
{
  // NUMBER_OF_DIARY_RECORDS записей по 32 символа
        // 0 байт - статус (3)  0 - пустая ячейка 1 - одноразово 2 - каждый год
        //                      3 - День рождения
        //                      4 - каждый месяц 5 - каждую неделю
        //                      6 - каждый день
        //   вид напоминания (2) 0 - без напоминания
        //                      1 - вибро
        //   начало года     (3)
        // 1,2 - дата - [год(4бита от 1900 года), мес(4 бита)], день[(5бит) день недели(3 бита)]
        // 3,4 - время
        diaryEventStatus[0] = diary[eventIndex][0] >> 5;
        if(diaryEventStatus[0] == 0)
        {
          return 0;
        }
        
        int tempYear = 1900 + ((diary[eventIndex][0] & 0x7) << 4) | (diary[eventIndex][1] >> 4);
        diaryEventYear[0] = tempYear;
        diaryEventMonth[0] = diary[eventIndex][1] & 0xf;
        diaryEventDay[0] = (diary[eventIndex][2] >> 3);
        diaryEventDow[0] = (diary[eventIndex][2] & 0x7);
        diaryEventHour[0] = diary[eventIndex][3];
        diaryEventMin[0] = diary[eventIndex][4];
        diaryEventText[0] = &(diary[eventIndex][5]);
        
        // Если повторы, то год текущий ставим
        if(diaryEventStatus[0] >= 2 && diaryEventStatus[0] <= 6)
        {
          diaryEventYear[0] = year;
        }
        // Если повторы чаще, чем раз в год, то месяц текущий
        if(diaryEventStatus[0] >= 4 && diaryEventStatus[0] <= 6)
        {
          diaryEventMonth[0] = month;
        }
        // Если повторы каждую неделю и день недели совпадает, то день текущий
        if(diaryEventStatus[0] == 5)
        {
          if(diaryEventDow[0] == weekday)
            diaryEventDay[0] == day;
        }
        // Если повторы каждый день, то день текущий
        if(diaryEventStatus[0] == 6)
        {
          diaryEventDay[0] == day;
        }
        if(diaryEventYear[0] == year && diaryEventMonth[0] == month
           && diaryEventDay[0] == day && diaryEventHour[0])
        {
          if(diaryEventHour[0] > hour || (diaryEventHour[0] == hour && diaryEventMin[0] >= min))
          {
            if(diaryEventStatus[0] == 3) // Если тип - день рождения
            { // То год ставим рождения
              diaryEventYear[0] = tempYear;
            }
            return 1;
          }
          else
            return 0;
        }
        else return 0;
}


static void SortDiaryEventsOnDate(unsigned int year, unsigned char month, 
                                  unsigned char day, unsigned char hour,
                                  unsigned char min)
{
  // События в пределах 3 месцев
  for(unsigned char i = 0; i < 3; i++)
  {
    diaryEventYear[i + 1] = 0;
    eventIndexes[i] = 0;
  }
  unsigned int curYear = RTCYEAR;
  unsigned char curMonth = RTCMON,
                
                startDay = RTCDAY,
                startHour = hour,
                startMin = min;
  
  for(unsigned char month = 0; month <= 3; month++)
  {
    // Сколько дней в месяце
    unsigned char days = daysInMonth(curMonth, curYear);
    for(unsigned char day = startDay; day <= days; day++)
    {
      // Получаем день недели
      unsigned char curDOW = dayOfWeek2(day, curMonth, curYear);
      for(unsigned char i = 0; i < NUMBER_OF_DIARY_RECORDS; i++)
      {
        if(LoadDiaryEvent(i, curYear, curMonth, curDOW, day, startHour, startMin) == 1)
        {
          for(unsigned char j = 0; j < 3; j++)
          {
            // Если ячейка не занята, занять!
            if(diaryEventYear[j + 1] == 0)
            {
              diaryEventYear[j + 1] = diaryEventYear[0];
              diaryEventMonth[j + 1] = diaryEventMonth[0];
              diaryEventDay[j + 1] = diaryEventDay[0];
              eventIndexes[j] = i;
              break;
            }
            else
            {
              // Если событие раньше, чем записанное, занять ячейку и переместить записанное событие дальше
              if(diaryEventYear[0] < diaryEventYear[j + 1] 
                 || (diaryEventYear[0] == diaryEventYear[j + 1] && diaryEventMonth[0] < diaryEventMonth[j + 1])
                 || (diaryEventYear[0] == diaryEventYear[j + 1] && diaryEventMonth[0] == diaryEventMonth[j + 1] && diaryEventDay[0] < diaryEventDay[j + 1]))
              {
                if(j < 2) // сдвигаем остальные ячейки
                {
                  for(unsigned char k = 2; k > j; k--)
                  {
                    diaryEventYear[k + 1] = diaryEventYear[k + 1 - 1];
                    diaryEventMonth[k + 1] = diaryEventMonth[k + 1 - 1];
                    diaryEventDay[k + 1] = diaryEventDay[k + 1 - 1];
                    eventIndexes[k] = eventIndexes[k - 1];
                  }
                }
                // Записываем событие в освободившуюся ячейку
                diaryEventYear[j + 1] = diaryEventYear[0];
                diaryEventMonth[j + 1] = diaryEventMonth[0];
                diaryEventDay[j + 1] = diaryEventDay[0];
                eventIndexes[j] = i;
              }
            }
          }
        }
                 
      }
      // Если в третьей ячейке записано год больше 0, то заканчиваем
      if(diaryEventYear[3] > 0)
        return;
      
      // После текущего дня ищем события, которые могут быть в любое время
      startHour = 0;
      startMin = 0;
    } // day
    if(curMonth == 12)
    {
      curYear++;
      curMonth = 1;
    }
    else
    {
      curMonth++;
    }
    startDay = 1;
  } // month
    
}

static void DiaryWriteRecordHandler(tMessage* pMsg)
{
  unsigned char recordId = pMsg->Options & 0x3f;
  if(recordId >= NUMBER_OF_DIARY_RECORDS)
    return;
  // Если настройки
  if((pMsg->Options & 0x80) == 0)
  {
         // 0 байт - статус (3)  0 - пустая ячейка 1 - одноразово 2 - каждый год
        //                      3 - День рождения
        //                      4 - каждый месяц 5 - каждую неделю
        //                      6 - каждый день
        //   вид напоминания (2) 0 - без напоминания
        //                      1 - вибро
        //   начало года     (3)
        // 1,2 - дата - [год(4бита от 1900 года), мес(4 бита)], день[(5бит) день недели(3 бита)]
        // 3,4 - время
    
    tDiaryRecord* pDiaryRecord = (tDiaryRecord*)pMsg->pBuffer;
    diary[recordId][0] = (pDiaryRecord->Status << 5) | (pDiaryRecord->Alarm << 3)
                                | (pDiaryRecord->Year >> 4);
    diary[recordId][1] = (pDiaryRecord->Year << 4) | pDiaryRecord->Month;
    diary[recordId][2] = (pDiaryRecord->Day << 3) | pDiaryRecord->DayOfWeek;
    diary[recordId][3] = pDiaryRecord->Hour;
    diary[recordId][4] = pDiaryRecord->Minute;
  }
  else // текст
  {
    for(unsigned char i = 0; i < TEXT_LENGTH_OF_DIARY_RECORDS; i++)
    {
      diary[recordId][5 + i] = pMsg->pBuffer[i];
    }
  }
  updateDiaryOnScreen = 1;
}

static void DiaryWriteEndHandler()
{
  ShowDiary();
}

unsigned char IsUpdateDiary()
{
  return 1;// пока что 
  unsigned char updateDiaryOnScreenValue = updateDiaryOnScreen;
  updateDiaryOnScreen = 0;
  return updateDiaryOnScreenValue;
}

///
/// now - событие происходит сейчас
/// Возврат 1 - событие рисовать
unsigned char GetDiaryDataStrings(unsigned char index, char *string0, char *string1, unsigned char *now)
{
//  char *tudayString = "СЕГОДНЯ";
//  char *nowString = "СЕЙЧАС";
  if(diaryEventYear[index + 1] == 0)
     return 0;
  
  //!!!!eventIndexes[index]
  unsigned char today = LoadDiaryEvent(eventIndexes[index], RTCYEAR, RTCMON, RTCDAY, RTCDOW, RTCHOUR, RTCMIN);
    //LoadDiaryEvent(index);
    //if(diaryEventStatus == 0)
      //return;
  
  
  
    memset(string0, 0x20, 20);
    memset(string1, 0x0, 20);
    
    unsigned char curPos;
    if(today == 1)
    {
      mystrncpy("СЕГОДНЯ", 8, string0); 
      curPos = 8;
    }
    else
    {
      
      itoa(diaryEventDay[0], string0, 2);
      string0[2] = '.';
      itoa(diaryEventMonth[0], string0 + 3, 2);
      string0[5] = '.';
      itoa(diaryEventYear[0], string0 + 6, 4);
      
      curPos = 11;
    }
    
    //if(
    
    unsigned char len;
    len = itoa(diaryEventHour[0], string0 + curPos, 0);
    string0[curPos + len] = ':';
    itoa(diaryEventMin[0], string0 + curPos + len + 1, 2);
    
    mystrncpy(diaryEventText[0], 20, string1);

    return 1;

}
#endif


/*! Handle the AdvanceWatchHands message
 *
 * The AdvanceWatchHands message specifies the hours, min, and sec to advance
 * the analog watch hands.
 *
 */
static void AdvanceWatchHandsHandler(tMessage* pMsg)
{
#ifdef ANALOG
  // overlay a structure pointer on the data section
  tAdvanceWatchHandsPayload* pPayload;
  pPayload = (tAdvanceWatchHandsPayload*) pMsg->pBuffer;

  if ( pPayload->Hours <= 12 )
  {
    // the message values are bytes and we are computing a 16 bit value
    unsigned int numSeconds;
    numSeconds =  (unsigned int) pPayload->Seconds;
    numSeconds += (unsigned int)(pPayload->Minutes) * 60;
    numSeconds += (unsigned int)(pPayload->Hours) * 3600;

    // set the analog watch timer to fast mode to increment the number of
    // extra seconds we specify.  The resolution is 10 seconds.  The update
    // will automatically stop when completed
    AdvanceAnalogDisplay(numSeconds);
  }
#endif
}



/*! Led Change Handler
 *
 * \param tHostMsg* pMsg The message options contain the type of operation that
 * should be performed on the LED outout.
 */
static void LedChangeHandler(tMessage* pMsg)
{
  switch (pMsg->Options)
  {
  case LED_ON_OPTION:
    LedOn = 1;
    ENABLE_LCD_LED();
    StartOneSecondTimer(LedTimerId);
    break;

  case LED_TOGGLE_OPTION:
    if ( LedOn )
    {
      LedOn = 0;
      DISABLE_LCD_LED();
      StopOneSecondTimer(LedTimerId);
    }
    else
    {
      LedOn = 1;
      ENABLE_LCD_LED();
      StartOneSecondTimer(LedTimerId);
    }
    break;

  case LED_START_OFF_TIMER:
    LedOn = 1;
    ENABLE_LCD_LED();
    StartOneSecondTimer(LedTimerId);
    break;

  case LED_OFF_OPTION:
  default:
    LedOn = 0;
    DISABLE_LCD_LED();
    StopOneSecondTimer(LedTimerId);
    break;

  }

}

/*! Attach callback to button press type. Each button press type is associated
 * with a display mode.
 *
 * No error checking
 *
 * \param tHostMsg* pMsg - A message with a tButtonActionPayload payload
 */
static void EnableButtonMsgHandler(tMessage* pMsg)
{
  tButtonActionPayload* pButtonActionPayload =
    (tButtonActionPayload*)pMsg->pBuffer;

  EnableButtonAction(pButtonActionPayload->DisplayMode,
                     pButtonActionPayload->ButtonIndex,
                     pButtonActionPayload->ButtonPressType,
                     pButtonActionPayload->CallbackMsgType,
                     pButtonActionPayload->CallbackMsgOptions);

}

/*! Remove callback for the specified button press type.
 * Each button press type is associated with a display mode.
 *
 * \param tHostMsg* pMsg - A message with a tButtonActionPayload payload
 */
static void DisableButtonMsgHandler(tMessage* pMsg)
{
  tButtonActionPayload* pButtonActionPayload =
    (tButtonActionPayload*)pMsg->pBuffer;

  DisableButtonAction(pButtonActionPayload->DisplayMode,
                      pButtonActionPayload->ButtonIndex,
                      pButtonActionPayload->ButtonPressType);

}

/*! Read configuration of a specified button.  This is used to read the
 * configuration of a button that needs to be restored at a later time
 * by the application.
 *
 * \param tHostMsg* pMsg - A message with a tButtonActionPayload payload
 */
static void ReadButtonConfigHandler(tMessage* pMsg)
{
  /* map incoming message payload to button information */
  tButtonActionPayload* pButtonActionPayload =
    (tButtonActionPayload*)pMsg->pBuffer;

  tMessage OutgoingMsg;
  SetupMessageAndAllocateBuffer(&OutgoingMsg,
                                ReadButtonConfigResponse,
                                NO_MSG_OPTIONS);

  ReadButtonConfiguration(pButtonActionPayload->DisplayMode,
                          pButtonActionPayload->ButtonIndex,
                          pButtonActionPayload->ButtonPressType,
                          OutgoingMsg.pBuffer);

  OutgoingMsg.Length = 5;

  RouteMsg(&OutgoingMsg);

}

/*! Read the voltage of the battery. This provides power good, battery charging,
 * battery voltage, and battery voltage average.
 *
 * \param tHostMsg* pMsg is unused
 *
 */
static void ReadBatteryVoltageHandler(void)
{
  tMessage OutgoingMsg;
  SetupMessageAndAllocateBuffer(&OutgoingMsg,
                                ReadBatteryVoltageResponse,
                                NO_MSG_OPTIONS);

  /* if the battery is not present then these values are meaningless */
  OutgoingMsg.pBuffer[0] = QueryPowerGood();
  OutgoingMsg.pBuffer[1] = QueryBatteryCharging();

  unsigned int bv = ReadBatterySense();
  OutgoingMsg.pBuffer[2] = bv & 0xFF;
  OutgoingMsg.pBuffer[3] = (bv >> 8 ) & 0xFF;

  bv = ReadBatterySenseAverage();
  OutgoingMsg.pBuffer[4] = bv & 0xFF;
  OutgoingMsg.pBuffer[5] = (bv >> 8 ) & 0xFF;

  OutgoingMsg.Length = 6;

  RouteMsg(&OutgoingMsg);

}

/*! Initiate a light sensor cycle.  Then send the instantaneous and average
 * light sense values to the host.
 *
 * \param tHostMsg* pMsg is unused
 *
 */
static void ReadLightSensorHandler(void)
{
  /* start cycle and wait for it to finish */
  LightSenseCycle();

  /* send message to the host */
  tMessage OutgoingMsg;
  SetupMessageAndAllocateBuffer(&OutgoingMsg,
                                ReadLightSensorResponse,
                                NO_MSG_OPTIONS);

  /* instantaneous value */
  unsigned int lv = ReadLightSense();
  OutgoingMsg.pBuffer[0] = lv & 0xFF;
  OutgoingMsg.pBuffer[1] = (lv >> 8 ) & 0xFF;

  /* average value */
  lv = ReadLightSenseAverage();
  OutgoingMsg.pBuffer[2] = lv & 0xFF;
  OutgoingMsg.pBuffer[3] = (lv >> 8 ) & 0xFF;

  OutgoingMsg.Length = 4;

  RouteMsg(&OutgoingMsg);

}

/*! Setup the battery monitor interval - only happens at startup */
static void InitializeBatteryMonitorInterval(void)
{
  nvBatteryMonitorIntervalInSeconds = 8;

  OsalNvItemInit(NVID_BATTERY_SENSE_INTERVAL,
                 sizeof(nvBatteryMonitorIntervalInSeconds),
                 &nvBatteryMonitorIntervalInSeconds);

}

/* choose whether or not to do a master reset (reset non-volatile values) */
static void SoftwareResetHandler(tMessage* pMsg)
{
  if ( pMsg->Options == MASTER_RESET_OPTION )
  {
    WriteMasterResetKey();
  }

  SoftwareReset();

}

static void NvalOperationHandler(tMessage* pMsg)
{
  /* overlay */
  tNvalOperationPayload* pNvPayload = (tNvalOperationPayload*)pMsg->pBuffer;

  /* create the outgoing message */
  tMessage OutgoingMsg;
  SetupMessageAndAllocateBuffer(&OutgoingMsg,
                                NvalOperationResponseMsg,
                                NV_FAILURE);

  /* add identifier to outgoing message */
  tWordByteUnion Identifier;
  Identifier.word = pNvPayload->NvalIdentifier;
  OutgoingMsg.pBuffer[0] = Identifier.Bytes.byte0;
  OutgoingMsg.pBuffer[1] = Identifier.Bytes.byte1;
  OutgoingMsg.Length = 2;

  /* option byte in return message is status */
  switch (pMsg->Options)
  {

  case NVAL_INIT_OPERATION:
    /* may allow access to a specific range of nval ids that
     * the phone can initialize and use
     */
    break;

  case NVAL_READ_OPERATION:

    /* read the value and update the length */
    OutgoingMsg.Options = OsalNvRead(pNvPayload->NvalIdentifier,
                                     NV_ZERO_OFFSET,
                                     pNvPayload->Size,
                                     &OutgoingMsg.pBuffer[2]);

    OutgoingMsg.Length += pNvPayload->Size;

    break;

  case NVAL_WRITE_OPERATION:

    /* check that the size matches (otherwise NV_FAILURE is sent) */
    if ( OsalNvItemLength(pNvPayload->NvalIdentifier) == pNvPayload->Size )
    {
      OutgoingMsg.Options = OsalNvWrite(pNvPayload->NvalIdentifier,
                                        NV_ZERO_OFFSET,
                                        pNvPayload->Size,
                                        (void*)(&pNvPayload->DataStartByte));
    }

    /* update the copy in ram */
    NvUpdater(pNvPayload->NvalIdentifier);
    break;

  default:
    break;
  }

  RouteMsg(&OutgoingMsg);

}


/******************************************************************************/

void InitializeRstNmiConfiguration(void)
{
  nvRstNmiConfiguration = RST_PIN_DISABLED;
  OsalNvItemInit(NVID_RSTNMI_CONFIGURATION,
                 sizeof(nvRstNmiConfiguration),
                 &nvRstNmiConfiguration);

  ConfigureResetPinFunction(nvRstNmiConfiguration);

}


void SaveRstNmiConfiguration(void)
{
  OsalNvWrite(NVID_RSTNMI_CONFIGURATION,
              NV_ZERO_OFFSET,
              sizeof(nvRstNmiConfiguration),
              &nvRstNmiConfiguration);
}




/******************************************************************************/

/* The value in RAM must be updated if the phone writes the value in
 * flash (until the code is changed to read the value from flash)
 */
static void NvUpdater(unsigned int NvId)
{
  switch (NvId)
  {
#ifdef DIGITAL
    case NVID_IDLE_BUFFER_CONFIGURATION:
      InitializeIdleBufferConfig();
      break;
    case NVID_IDLE_BUFFER_INVERT:
      InitializeIdleBufferInvert();
      break;
#endif

    case NVID_IDLE_MODE_TIMEOUT:
    case NVID_APPLICATION_MODE_TIMEOUT:
    case NVID_NOTIFICATION_MODE_TIMEOUT:
    case NVID_RESERVED_MODE_TIMEOUT:
      InitializeModeTimeouts();
      break;

#ifdef ANALOG
    case NVID_IDLE_DISPLAY_TIMEOUT:
    case NVID_APPLICATION_DISPLAY_TIMEOUT:
    case NVID_NOTIFICATION_DISPLAY_TIMEOUT:
    case NVID_RESERVED_DISPLAY_TIMEOUT:
      InitializeDisplayTimeouts();
      break;
#endif

    case NVID_SNIFF_DEBUG:
    case NVID_BATTERY_DEBUG:
    case NVID_CONNECTION_DEBUG:
      InitializeDebugFlags();
      break;

    case NVID_RSTNMI_CONFIGURATION:
      InitializeRstNmiConfiguration();
      break;

    case NVID_MASTER_RESET:
      /* this gets handled on reset */
      break;

    case NVID_LOW_BATTERY_WARNING_LEVEL:
    case NVID_LOW_BATTERY_BTOFF_LEVEL:
      InitializeLowBatteryLevels();
      break;

    case NVID_BATTERY_SENSE_INTERVAL:
      InitializeBatteryMonitorInterval();
      break;

    case NVID_LIGHT_SENSE_INTERVAL:
      break;

    case NVID_SECURE_SIMPLE_PAIRING_ENABLE:
      /* not for phone control - reset watch */
      break;

    case NVID_LINK_ALARM_ENABLE:
      InitializeLinkAlarmEnable();
      break;

    case NVID_LINK_ALARM_DURATION:
      break;

    case NVID_PAIRING_MODE_DURATION:
      /* not for phone control - reset watch */
      break;

    case NVID_TIME_FORMAT:
      InitializeTimeFormat();
      break;

    case NVID_DATE_FORMAT:
      InitializeDateFormat();
      break;

#ifdef DIGITAL
    case NVID_DISPLAY_SECONDS:
      InitializeDisplaySeconds();
      break;
#endif

#ifdef ANALOG
    case NVID_TOP_OLED_CONTRAST_DAY:
    case NVID_BOTTOM_OLED_CONTRAST_DAY:
    case NVID_TOP_OLED_CONTRAST_NIGHT:
    case NVID_BOTTOM_OLED_CONTRAST_NIGHT:
      InitializeContrastValues();
      break;
#endif

  }
}

#ifdef RATE_TEST
static unsigned char RateTestCallback(void)
{
  unsigned char ExitLpm = 0;

  StartCrystalTimer(CRYSTAL_TIMER_ID3,
                    RateTestCallback,
                    RATE_TEST_INTERVAL_MS);

  /* send messages once we are connected and sniff mode */
  if (   QueryConnectionState() == Connected
      && QuerySniffState() == Sniff )
  {

    DEBUG5_PULSE();

    tMessage Msg;
    SetupMessage(&Msg,RateTestMsg,NO_MSG_OPTIONS);
    SendMessageToQueueFromIsr(BACKGROUND_QINDEX,&Msg);
    ExitLpm = 1;

  }

  return ExitLpm;

}
#endif

static void SetCallbackTimerHandler(tMessage* pMsg)
{
  tSetCallbackTimerPayload *pPayload = (tSetCallbackTimerPayload *)(pMsg->pBuffer);

  StopOneSecondTimer(CallbackTimerId);

  if (pPayload->Repeat)
  {
    SetupOneSecondTimer(CallbackTimerId,
                       pPayload->Timeout,
                       pPayload->Repeat,
                       SPP_TASK_QINDEX,
                       CallbackTimeoutMsg,
                       pMsg->Options);

    StartOneSecondTimer(CallbackTimerId);
  }
}

// Alarm -----------------------------------------------------------------------

void InitializeAlarm(void)
{
//  nvAlarmOn = 0;
//  nvAlarmMinutes = 0;
//  nvAlarmHours = 0;
  for(unsigned char i = 0; i < 10; i++)
  {
 
    OsalNvItemInit(NVID_ALARM_ON + i,
                   sizeof(nvAlarmOn[i]),
                   &nvAlarmOn[i]);
    
    OsalNvItemInit(NVID_ALARM_MIN + i,
                   sizeof(nvAlarmMinutes[i]),
                   &nvAlarmMinutes[i]);
      
    OsalNvItemInit(NVID_ALARM_HOUR + i,
                   sizeof(nvAlarmHours[i]),
                   &nvAlarmHours[i]);
  }
}

void SaveAlarm(void)
{
  for(unsigned char i = 0; i < 10; i++)
  {
    OsalNvWrite(NVID_ALARM_ON,
                i,
                sizeof(nvAlarmOn[i]),
                &nvAlarmOn[i]);
    
    OsalNvWrite(NVID_ALARM_MIN,
                i,
                sizeof(nvAlarmMinutes[i]),
                &nvAlarmMinutes[i]);

    OsalNvWrite(NVID_ALARM_HOUR,
                i,
                sizeof(nvAlarmHours[i]),
                &nvAlarmHours[i]);
  }
}
unsigned char GetCurrentAlarm()
{
  return currentAlarm;
}

unsigned char IncCurrentAlarm()
{
  if(currentAlarm == 9)
  {
    currentAlarm = 0;
    return 0;
  }
  currentAlarm++;
  return currentAlarm;
}

unsigned char GetAlarmMinutes()
{
  return nvAlarmMinutes[currentAlarm];
}

unsigned char GetAlarmHours()
{
  return nvAlarmHours[currentAlarm];
}

unsigned char AddAlarmMinute()
{
  if(nvAlarmMinutes[currentAlarm] == 59)
    {
      nvAlarmMinutes[currentAlarm] = 0;
    }
    else
    {
      nvAlarmMinutes[currentAlarm]++;
    }
  return nvAlarmMinutes[currentAlarm];  
}

unsigned char AddAlarmHour()
{
    if(nvAlarmHours[currentAlarm] == 23)
    {
      nvAlarmHours[currentAlarm] = 0;
    }
    else
    {
      nvAlarmHours[currentAlarm]++;
    }
    return nvAlarmHours[currentAlarm];
  
}

unsigned char GetAlarmStatus()
{
  return nvAlarmOn[currentAlarm];
}

void SetAlarmStatus(unsigned char on)
{
  nvAlarmOn[currentAlarm] = on;
}

void GenerateAlarm(void)
{
  tMessage Msg;
  
  SetupMessageAndAllocateBuffer(&Msg,SetVibrateMode,NO_MSG_OPTIONS);
  
  tSetVibrateModePayload* pMsgData;
  pMsgData = (tSetVibrateModePayload*) Msg.pBuffer;
  
  pMsgData->Enable = 1;
  pMsgData->OnDurationLsb = 0x00;
  pMsgData->OnDurationMsb = 0x01;
  pMsgData->OffDurationLsb = 0x00;
  pMsgData->OffDurationMsb = 0x01;
  pMsgData->NumberOfCycles = 10;
  
  RouteMsg(&Msg);
}

signed char GetCorrectionValue()
{
  return nvCorrectionValue;
} 

signed char IncCorrectionValue()
{
  if(nvCorrectionValue < 99)
     nvCorrectionValue++; 
  return nvCorrectionValue;
} 

signed char DecCorrectionValue()
{
  if(nvCorrectionValue > -99)
     nvCorrectionValue--; 
  return nvCorrectionValue;
}

void LoadCorrectionValue()
{
      OsalNvItemInit(NVID_CORRECTION,
                   sizeof(nvCorrectionValue),
                   &nvCorrectionValue);
    

}

void SaveCorrectionValue()
{
    OsalNvWrite(NVID_CORRECTION,
                NV_ZERO_OFFSET,
                sizeof(nvCorrectionValue),
                &nvCorrectionValue);
  
}

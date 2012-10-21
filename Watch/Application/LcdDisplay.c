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
/*! \file LcdDisplay.c
 *
 */
/******************************************************************************/

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "Messages.h"

#include "hal_board_type.h"
#include "hal_rtc.h"
#include "hal_battery.h"
#include "hal_lcd.h"
#include "hal_lpm.h"

#include "DebugUart.h"
#include "Messages.h"
#include "Utilities.h"
#include "LcdDriver.h"
#include "Wrapper.h"
#include "MessageQueues.h"
#include "SerialRam.h"
#include "OneSecondTimers.h"
#include "Adc.h"
#include "Buttons.h"
#include "Statistics.h"
#include "OSAL_Nv.h"
#include "Background.h"
#include "NvIds.h"
#include "Icons.h"
#include "Fonts.h"
#include "Display.h"
#include "Calendar.h"
#include "LcdDisplay.h"


#define DISPLAY_TASK_QUEUE_LENGTH 8
#define DISPLAY_TASK_STACK_SIZE  	(configMINIMAL_STACK_SIZE + 90)
#define DISPLAY_TASK_PRIORITY     (tskIDLE_PRIORITY + 1)

xTaskHandle DisplayHandle;

static void DisplayTask(void *pvParameters);

static void DisplayQueueMessageHandler(tMessage* pMsg);
static void SendMyBufferToLcd(unsigned char TotalRows);

static tMessage DisplayMsg;

static tTimerId DisplayTimerId;
static unsigned char RtcUpdateEnable;

/* Message handlers */

static void IdleUpdateHandler(void);
static void ChangeModeHandler(tMessage* pMsg);
static void ModeTimeoutHandler(tMessage* pMsg);
static void WatchStatusScreenHandler(void);
static void BarCodeHandler(tMessage* pMsg);
static void ShowCalendarHandler(void);
static void ConfigureDisplayHandler(tMessage* pMsg);
static void ConfigureIdleBuferSizeHandler(tMessage* pMsg);
static void ModifyTimeHandler(tMessage* pMsg);
static void MenuModeHandler(unsigned char MsgOptions);
static void MenuButtonHandler(unsigned char MsgOptions);
static void ToggleSecondsHandler(unsigned char MsgOptions);
static void ConnectionStateChangeHandler(void);
static void CalendarHandler(tMessage* pMsg);

/******************************************************************************/
static void DrawIdleScreen(void);
static void InitMyBuffer(void);
static void DisplayStartupScreen(void);
static void SetupSplashScreenTimeout(void);
static void AllocateDisplayTimers(void);
static void StopDisplayTimer(void);
static void DrawVersionInfo(unsigned char RowHeight);

static void DrawMainOptionsPage(void);
static void DrawOptionSettingsPage(void);
//static void DrawMenu3(void);
static void DrawAlarmSettingsPage(void);
static void DrawTimeSettingsPage(void);
static void DrawCommonMenuIcons(void);

static void FillMyBuffer(unsigned char StartingRow,
                         unsigned char NumberOfRows,
                         unsigned char FillValue);

static void PrepareMyBufferForLcd(unsigned char StartingRow,
                                  unsigned char NumberOfRows);


static void CopyRowsIntoMyBuffer(unsigned char const* pImage,
                                 unsigned char StartingRow,
                                 unsigned char NumberOfRows);

static void CopyColumnsIntoMyBuffer(unsigned char const* pImage,
                                    unsigned char StartingRow,
                                    unsigned char NumberOfRows,
                                    unsigned char StartingColumn,
                                    unsigned char NumberOfColumns);

static void WriteIcon4w10h(unsigned char const * pIcon,
                           unsigned char RowOffset,
                           unsigned char ColumnOffset);

static void DisplayAmPm(unsigned char row);
static void DisplayDayOfWeek(void);
static void DisplayDate(void);
static void DisplayDiary(void);


static tLcdLine pMyBuffer[NUM_LCD_ROWS];

/******************************************************************************/

static unsigned char nvIdleBufferConfig;
static unsigned char nvIdleBufferInvert;

static void SaveIdleBufferInvert(void);

/******************************************************************************/

unsigned char nvDisplaySeconds = 0;
static void SaveDisplaySeconds(void);

/******************************************************************************/

/******************************************************************************/

typedef enum
{
  ReservedPage,
  NormalPage,
  /* the next three are only used on power-up */
  RadioOnWithPairingInfoPage,
  RadioOnWithoutPairingInfoPage,
  BluetoothOffPage,
  OptionsMainPage,
  OptionSettingsPage,
  //Menu3Page,
  AlarmSettingsPage, // Настройка будильника
  TimeSettingsPage, // настройка времени
  CalendarPage, // Календарь
  DiaryPage, // Ежедневник 
  WatchStatusPage,
  QrCodePage

} etIdlePageMode;

static etIdlePageMode CurrentIdlePage;
static etIdlePageMode LastIdlePage = ReservedPage;

static unsigned char AllowConnectionStateChangeToUpdateScreen;

static void ConfigureIdleUserInterfaceButtons(void);

static void DontChangeButtonConfiguration(void);
static void DefaultApplicationAndNotificationButtonConfiguration(void);
static void SetupNormalIdleScreenButtons(void);

/******************************************************************************/

//
const unsigned char pBarCodeImage[NUM_LCD_ROWS*NUM_LCD_COL_BYTES];
const unsigned char pMetaWatchSplash[NUM_LCD_ROWS*NUM_LCD_COL_BYTES];
const unsigned char Am[10*4];
const unsigned char Pm[10*4];
//const unsigned char DaysOfWeek[7][10*4];

/******************************************************************************/

static unsigned char LastMode = IDLE_MODE;
static unsigned char CurrentMode = IDLE_MODE;

//static unsigned char ReturnToApplicationMode;

/// Calendar
typedef enum 
{
  CalEditNone,
  CalEditYear,
  CalEditMonth,
  CalEditDay
} calendarEditModes;
static calendarEditModes calendarEditMode = CalEditNone;
static unsigned int curYear;
static unsigned char curMonth;

static void DrawCalendarPage();
/******************************************************************************/

static unsigned char gBitColumnMask;
static unsigned char gColumn;
static unsigned char gRow;

static void WriteFontCharacter(unsigned char Character);
static void WriteFontCharacterSpec(unsigned char Character, unsigned char underline, unsigned char inserse);
static void MyWriteFontCharacter(unsigned char Character, unsigned char colunInPixels, 
                                 unsigned char rectangleWidth, unsigned char inserse);
static void WriteFontString(const tString* pString);
static void WriteFontStringSpec(const tString *pString, unsigned char maxlen, unsigned char underline, unsigned char inverse);


static void FillRect(unsigned char left, unsigned char top, unsigned char width, unsigned char height);
/******************************************************************************/

/*! Initialize the LCD display task
 *
 * Initializes the display driver, clears the display buffer and starts the
 * display task
 *
 * \return none, result is to start the display task
 */
void InitializeDisplayTask(void)
{
  InitMyBuffer();

  QueueHandles[DISPLAY_QINDEX] =
    xQueueCreate( DISPLAY_TASK_QUEUE_LENGTH, MESSAGE_QUEUE_ITEM_SIZE  );

  // task function, task name, stack len , task params, priority, task handle
  xTaskCreate(DisplayTask,
              (const signed char *)"DISPLAY",
              DISPLAY_TASK_STACK_SIZE,
              NULL,
              DISPLAY_TASK_PRIORITY,
              &DisplayHandle);


  ClearShippingModeFlag();

}



/*! LCD Task Main Loop
 *
 * \param pvParameters
 *
 */
static void DisplayTask(void *pvParameters)
{
  if ( QueueHandles[DISPLAY_QINDEX] == 0 )
  {
    PrintString("Display Queue not created!\r\n");
  }

  LcdPeripheralInit();

  DisplayStartupScreen();

  SerialRamInit();

  InitializeIdleBufferConfig();
  InitializeIdleBufferInvert();
  InitializeDisplaySeconds();
  InitializeLinkAlarmEnable();
  InitializeModeTimeouts();
  InitializeTimeFormat();
  InitializeDateFormat();
  AllocateDisplayTimers();
  SetupSplashScreenTimeout();

  DontChangeButtonConfiguration();
  DefaultApplicationAndNotificationButtonConfiguration();
  SetupNormalIdleScreenButtons();

#ifndef ISOLATE_RADIO
  /* turn the radio on; initialize the serial port profile or BLE/GATT */
  tMessage Msg;
  SetupMessage(&Msg,TurnRadioOnMsg,NO_MSG_OPTIONS);
  RouteMsg(&Msg);
#endif

  for(;;)
  {
    if( pdTRUE == xQueueReceive(QueueHandles[DISPLAY_QINDEX],
                                &DisplayMsg, portMAX_DELAY) )
    {
      PrintMessageType(&DisplayMsg);

      DisplayQueueMessageHandler(&DisplayMsg);

      SendToFreeQueue(&DisplayMsg);

      CheckStackUsage(DisplayHandle,"Display");

      CheckQueueUsage(QueueHandles[DISPLAY_QINDEX]);

    }
  }
}

/*! Display the startup image or Splash Screen */
static void DisplayStartupScreen(void)
{
  /* draw metawatch logo */
  CopyRowsIntoMyBuffer(pMetaWatchSplash,STARTING_ROW,NUM_LCD_ROWS);
  PrepareMyBufferForLcd(STARTING_ROW,NUM_LCD_ROWS);
  SendMyBufferToLcd(NUM_LCD_ROWS);
}

/*! Handle the messages routed to the display queue */
static void DisplayQueueMessageHandler(tMessage* pMsg)
{
  unsigned char Type = pMsg->Type;

  switch(Type)
  {

  case WriteBuffer:
    WriteBufferHandler(pMsg);
    break;

  case LoadTemplate:
    LoadTemplateHandler(pMsg);
    break;

  case UpdateDisplay:
    //!!!!!!UpdateDisplayHandler(pMsg);
    break;

  case IdleUpdate:
    IdleUpdateHandler();
    break;

  case ChangeModeMsg:
    ChangeModeHandler(pMsg);
    break;

  case ModeTimeoutMsg:
    ModeTimeoutHandler(pMsg);
    break;

  case WatchStatusMsg:
    WatchStatusScreenHandler();
    break;

  case CalendarMsg:
    CalendarHandler(pMsg);
    break;

  case BarCode:
    BarCodeHandler(pMsg);
    break;

  case ShowCalendarMsg:
    ShowCalendarHandler();
    break;

  case WatchDrawnScreenTimeout:
    IdleUpdateHandler();
    break;

  case ConfigureDisplay:
    ConfigureDisplayHandler(pMsg);
    break;

  case ConfigureIdleBufferSize:
    ConfigureIdleBuferSizeHandler(pMsg);
    break;

  case ConnectionStateChangeMsg:
    ConnectionStateChangeHandler();
    break;

  case ModifyTimeMsg:
    ModifyTimeHandler(pMsg);
    break;

  case MenuModeMsg:
    MenuModeHandler(pMsg->Options);
    break;

  case MenuButtonMsg:
    MenuButtonHandler(pMsg->Options);
    break;

  case ToggleSecondsMsg:
    ToggleSecondsHandler(pMsg->Options);
    break;

  case SplashTimeoutMsg:
    AllowConnectionStateChangeToUpdateScreen = 1;
    IdleUpdateHandler();
    break;

  case LowBatteryWarningMsg:
  case LowBatteryBtOffMsg:
    break;

  case LinkAlarmMsg:
    if ( QueryLinkAlarmEnable() )
    {
      GenerateLinkAlarm();
    }
    break;

  case RamTestMsg:
    RamTestHandler(pMsg);
    break;

  default:
    PrintStringAndHex("<<Unhandled Message>> in Lcd Display Task: Type 0x", Type);
    break;
  }

}

/*! Allocate ids and setup timers for the display modes */
static void AllocateDisplayTimers(void)
{
  DisplayTimerId = AllocateOneSecondTimer();
}

static void SetupSplashScreenTimeout(void)
{
  SetupOneSecondTimer(DisplayTimerId,
                      ONE_SECOND*3,
                      NO_REPEAT,
                      DISPLAY_QINDEX,
                      SplashTimeoutMsg,
                      NO_MSG_OPTIONS);

  StartOneSecondTimer(DisplayTimerId);

  AllowConnectionStateChangeToUpdateScreen = 0;

}

static inline void StopDisplayTimer(void)
{
  RtcUpdateEnable = 0;
  StopOneSecondTimer(DisplayTimerId);
}

/*! Draw the Idle screen and cause the remainder of the display to be updated
 * also
 */
static void IdleUpdateHandler(void)
{
  //ContinueRtc();
  calendarEditMode = CalEditNone;
  StopDisplayTimer();

  /* allow rtc to send IdleUpdate every minute (or second) */
  RtcUpdateEnable = 1;

  /* determine if the bottom of the screen should be drawn by the watch */
    /*
     * immediately update the screen
     */
    if ( nvIdleBufferConfig == WATCH_CONTROLS_TOP )
    {
      /* only draw watch part */
      FillMyBuffer(STARTING_ROW,PHONE_FULL_BUFFER_ROWS,0x00);//WATCH_DRAWN_IDLE_BUFFER_ROWS
      DrawIdleScreen();
      PrepareMyBufferForLcd(STARTING_ROW,PHONE_FULL_BUFFER_ROWS);//WATCH_DRAWN_IDLE_BUFFER_ROWS WATCH_DRAWN_IDLE_BUFFER_ROWS
      SendMyBufferToLcd(PHONE_FULL_BUFFER_ROWS);//WATCH_DRAWN_IDLE_BUFFER_ROWS
    }

    /* now update the remainder of the display */
    /*! make a dirty flag for the idle page drawn by the phone
     * set it whenever watch uses whole screen
     */
    
    tMessage OutgoingMsg;
    SetupMessage(&OutgoingMsg,
                 UpdateDisplay,
                 (IDLE_MODE | DONT_ACTIVATE_DRAW_BUFFER));
    RouteMsg(&OutgoingMsg);

    CurrentIdlePage = NormalPage;
    ConfigureIdleUserInterfaceButtons();
    
}


static void ConnectionStateChangeHandler(void)
{
  if ( AllowConnectionStateChangeToUpdateScreen )
  {
    /* certain pages should not be exited when a change in the
     * connection state has occurred
     */
    switch ( CurrentIdlePage )
    {
    case ReservedPage:
    case NormalPage:
    case RadioOnWithPairingInfoPage:
    case RadioOnWithoutPairingInfoPage:
    case BluetoothOffPage:
      IdleUpdateHandler();
      break;

    case OptionsMainPage:
    //case OptionSettingsPage:
    //case Menu3Page:
      MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
      break;

    /*case CalendarPage:
      ShowCalendarHandler();
      break;*/

    case WatchStatusPage:
      WatchStatusScreenHandler();
      break;

    case QrCodePage:
      break;

    default:
      break;
    }
  }
}

unsigned char QueryButtonMode(void)
{
  unsigned char result;

  switch (CurrentMode)
  {

  case IDLE_MODE:
    if ( CurrentIdlePage == NormalPage )
    {
      result = NORMAL_IDLE_SCREEN_BUTTON_MODE;
    }
    else
    {
      result = WATCH_DRAWN_SCREEN_BUTTON_MODE;
    }
    break;

  case APPLICATION_MODE:
    result = APPLICATION_SCREEN_BUTTON_MODE;
    break;

  case NOTIFICATION_MODE:
    result = NOTIFICATION_BUTTON_MODE;
    break;

  case SCROLL_MODE:
    result = SCROLL_MODE;
    break;

  }

  return result;
}
static void ChangeModeHandler(tMessage* pMsg)
{
  LastMode = CurrentMode;
  CurrentMode = (pMsg->Options & MODE_MASK);

  unsigned int timeout;

  switch ( CurrentMode )
  {

  case IDLE_MODE:

    /* this check is so that the watch apps don't mess up the timer */
    if ( LastMode != CurrentMode )
    {
      /* idle update handler will stop display timer */
      IdleUpdateHandler();
      PrintString("Changing mode to Idle\r\n");
    }
    else
    {
      PrintString("Already in Idle mode\r\n");
    }
    break;

  case APPLICATION_MODE:

    StopDisplayTimer();

    timeout = QueryApplicationModeTimeout();

    /* don't start the timer if the timeout == 0
     * this invites things that look like lock ups...
     * it is preferred to make this a large value
     */
    if ( timeout )
    {
      SetupOneSecondTimer(DisplayTimerId,
                          timeout,
                          NO_REPEAT,
                          DISPLAY_QINDEX,
                          ModeTimeoutMsg,
                          APPLICATION_MODE);

      StartOneSecondTimer(DisplayTimerId);
    }

    PrintString("Changing mode to Application\r\n");
    break;

  case NOTIFICATION_MODE:

    StopDisplayTimer();

    timeout = QueryNotificationModeTimeout();

    if ( timeout )
    {
      SetupOneSecondTimer(DisplayTimerId,
                          timeout,
                          NO_REPEAT,
                          DISPLAY_QINDEX,
                          ModeTimeoutMsg,
                          NOTIFICATION_MODE);

      StartOneSecondTimer(DisplayTimerId);
    }

    PrintString("Changing mode to Notification\r\n");
    break;

  default:
    break;
  }

  /*
   * send a message to the Host indicating buffer update / mode change
   * has completed (don't send message if it is just watch updating time ).
   */
  if ( LastMode != CurrentMode )
  {
    tMessage OutgoingMsg;
    SetupMessageAndAllocateBuffer(&OutgoingMsg,
                                  StatusChangeEvent,
                                  IDLE_MODE);

    OutgoingMsg.pBuffer[0] = (unsigned char)eScUpdateComplete;
    OutgoingMsg.Length = 1;
    RouteMsg(&OutgoingMsg);

//    if ( LastMode == APPLICATION_MODE )
//    {
//      ReturnToApplicationMode = 1;
//    }
//    else
//    {
//      ReturnToApplicationMode = 0;
//    }
  }

}

static void ModeTimeoutHandler(tMessage* pMsg)
{
  switch ( CurrentMode )
  {

  case IDLE_MODE:
    break;

  case APPLICATION_MODE:
  case NOTIFICATION_MODE:
  case SCROLL_MODE:
    /* go back to idle mode */
    IdleUpdateHandler();
    break;

  default:
    break;
  }

  /* send a message to the host indicating that a timeout occurred */
  tMessage OutgoingMsg;
  SetupMessageAndAllocateBuffer(&OutgoingMsg,
                                StatusChangeEvent,
                                CurrentMode);

  OutgoingMsg.pBuffer[0] = (unsigned char)eScModeTimeout;
  OutgoingMsg.Length = 1;
  RouteMsg(&OutgoingMsg);

}


static void WatchStatusScreenHandler(void)
{
  StopDisplayTimer();

  FillMyBuffer(STARTING_ROW,NUM_LCD_ROWS,0x00);

  /*
   * Add Status Icons
   */
  unsigned char const * pIcon;

  if ( QueryBluetoothOn() )
  {
    pIcon = pBluetoothOnStatusScreenIcon;
  }
  else
  {
    pIcon = pBluetoothOffStatusScreenIcon;
  }

  CopyColumnsIntoMyBuffer(pIcon,
                          0,
                          STATUS_ICON_SIZE_IN_ROWS,
                          LEFT_STATUS_ICON_COLUMN,
                          STATUS_ICON_SIZE_IN_COLUMNS);


  if ( QueryPhoneConnected() )
  {
    pIcon = pPhoneConnectedStatusScreenIcon;
  }
  else
  {
    pIcon = pPhoneDisconnectedStatusScreenIcon;
  }

  CopyColumnsIntoMyBuffer(pIcon,
                          0,
                          STATUS_ICON_SIZE_IN_ROWS,
                          CENTER_STATUS_ICON_COLUMN,
                          STATUS_ICON_SIZE_IN_COLUMNS);

  unsigned int bV = ReadBatterySenseAverage();

  if ( QueryBatteryCharging() )
  {
    pIcon = pBatteryChargingStatusScreenIcon;
  }
  else
  {
    if ( bV > 4000 )
    {
      pIcon = pBatteryFullStatusScreenIcon;
    }
    else if ( bV < 3500 )
    {
      pIcon = pBatteryLowStatusScreenIcon;
    }
    else
    {
      pIcon = pBatteryMediumStatusScreenIcon;
    }
  }


  CopyColumnsIntoMyBuffer(pIcon,
                          0,
                          STATUS_ICON_SIZE_IN_ROWS,
                          RIGHT_STATUS_ICON_COLUMN,
                          STATUS_ICON_SIZE_IN_COLUMNS);

  /* display battery voltage */
  unsigned char msd = 0;

  gRow = 27+2;
  gColumn = 8;
  gBitColumnMask = BIT6;
  SetFont(MetaWatch7);


  msd = bV / 1000;
  bV = bV % 1000;
  WriteFontCharacter(msd+'0');
  WriteFontCharacter('.');

  msd = bV / 100;
  bV = bV % 100;
  WriteFontCharacter(msd+'0');

  msd = bV / 10;
  bV = bV % 10;
  WriteFontCharacter(msd+'0');
  WriteFontCharacter(bV+'0');

  /*
   * Add Wavy line
   */
  gRow += 12;
  CopyRowsIntoMyBuffer(pWavyLine,gRow,NUMBER_OF_ROWS_IN_WAVY_LINE);


  /*
   * Add details
   */

  /* add MAC address */
  gRow += NUMBER_OF_ROWS_IN_WAVY_LINE+2;
  gColumn = 0;
  gBitColumnMask = BIT4;
  WriteFontString(GetLocalBluetoothAddressString());

  /* add the firmware version */
  gRow += 12;
  gColumn = 0;
  gBitColumnMask = BIT4;
  DrawVersionInfo(12);

  /* display entire buffer */
  PrepareMyBufferForLcd(STARTING_ROW,NUM_LCD_ROWS);
  SendMyBufferToLcd(NUM_LCD_ROWS);

  CurrentIdlePage = WatchStatusPage;
  ConfigureIdleUserInterfaceButtons();

  /* refresh the status page once a minute */
  SetupOneSecondTimer(DisplayTimerId,
                      ONE_SECOND*60,
                      NO_REPEAT,
                      DISPLAY_QINDEX,
                      WatchStatusMsg,
                      NO_MSG_OPTIONS);

  StartOneSecondTimer(DisplayTimerId);

}

static void DrawVersionInfo(unsigned char RowHeight)
{
  WriteFontString("App ");
  WriteFontString(VERSION_STRING);
  WriteFontString(" Msp430 ");
  WriteFontCharacter(GetMsp430HardwareRevision());

  /* stack version */
  gRow += RowHeight;
  gColumn = 0;
  gBitColumnMask = BIT4;
  tVersion Version = GetWrapperVersion();
  WriteFontString("Stk ");
  WriteFontString(Version.pSwVer);
  WriteFontString(" ");
  WriteFontString(Version.pBtVer);
  WriteFontString(" ");
  WriteFontString(Version.pHwVer);
}

static void DrawCalendar()
{  
  unsigned char RowHeight = 10;
  static tString days[7] = 
  {
    'П',
    'В',
    'С',
    'Ч',
    'П',
    'С',
    'В'
  };
  
  gRow = 26;
  gColumn = 0;
  SetFont(MetaWatch7);
  gBitColumnMask = BIT4;

  unsigned char underline = (calendarEditMode == CalEditDay);
  
  // названия дней
  for(unsigned char weekDay = 0; weekDay < 7; weekDay++)
  {
      unsigned char pixelColumn = weekDay * 13 + 6;
      MyWriteFontCharacter(days[weekDay], pixelColumn, 8, 0);
  }
  
  gRow += RowHeight + 1;
  
  unsigned char dIM = daysInMonth(curMonth, curYear);
  unsigned char dOfW = dayOfWeek1(curMonth, curYear);
  
  unsigned char day = 1;
  for(unsigned char week = 0; day <= dIM && week < 6; week++)
  {
    unsigned char weekDay = 1;
    if(week == 0)
      weekDay = dOfW;
    for(; day <= dIM && weekDay <= 7; weekDay++, day++)
    {
      unsigned char pixelColumn = (weekDay - 1) * 13 + 4;
      unsigned char d1 = day / 10;
      unsigned char d2 = day % 10;
      unsigned char inver;
      if(curYear == RTCYEAR && curMonth == RTCMON && day == RTCDAY)
      {
        inver = 1;
        if(underline)
          FillRect(pixelColumn - 5, gRow - 5, 19, 17);
        else  
          FillRect(pixelColumn - 1, gRow - 1, 11, 9);
        
      }
      else
      {
        inver = 0;
      }
      if(d1 > 0)
        MyWriteFontCharacter(d1+'0', pixelColumn, 4, inver);
      pixelColumn += 5;
      MyWriteFontCharacter(d2+'0', pixelColumn, 4, inver);

    }
    gRow += RowHeight;
    gColumn = 0;
  }
  
}

/* the bar code should remain displayed until the button is pressed again
 * or another mode is started
 */
static void BarCodeHandler(tMessage* pMsg)
{
  StopDisplayTimer();

  FillMyBuffer(STARTING_ROW,NUM_LCD_ROWS,0x00);

  CopyRowsIntoMyBuffer(pBarCodeImage,STARTING_ROW,NUM_LCD_ROWS);

  /* display entire buffer */
  PrepareMyBufferForLcd(STARTING_ROW,NUM_LCD_ROWS);
  SendMyBufferToLcd(NUM_LCD_ROWS);

  CurrentIdlePage = QrCodePage;
  ConfigureIdleUserInterfaceButtons();

}

static void CheckDate()
{
  unsigned char dIM = daysInMonth(RTCMON, RTCYEAR);
  if(RTCDAY > dIM)
    RTCDAY = dIM;
  unsigned char dOfW = dayOfWeek2(RTCDAY, RTCMON, RTCYEAR);
  RTCDOW = dOfW;
}

static void IncrementYear()
{
  RTCYEAR++;
  CheckDate();
}

static void DecrementYear()
{
  RTCYEAR--;
  CheckDate();
}

static void IncrementMonth()
{
  if(RTCMON < 12)
    RTCMON++;
  else
    RTCMON = 1;
}

static void DecrementMonth()
{
  if(RTCMON > 1)
    RTCMON--;
  else
  {
    RTCMON = 12;
  }
}

static void IncrementDay()
{
  unsigned char dIM = daysInMonth(RTCMON, RTCYEAR);
  if(RTCDAY < dIM)
    RTCDAY++;
  else
    RTCDAY = 1;
  unsigned char dOfW = dayOfWeek2(RTCDAY, RTCMON, RTCYEAR);
  RTCDOW = dOfW;

}

static void DecrementDay()
{
  unsigned char dIM = daysInMonth(RTCMON, RTCYEAR);
  if(RTCDAY > 1)
    RTCDAY--;
  else
    RTCDAY = dIM;
  unsigned char dOfW = dayOfWeek2(RTCDAY, RTCMON, RTCYEAR);
  RTCDOW = dOfW;

}


static void CalendarHandler(tMessage* pMsg)
{
  switch (pMsg->Options)
  {
    case CALENDAR_MONTH_PLUS:
      switch(calendarEditMode)
      {
        case CalEditNone:
          if(curMonth == 12)
          {
            curMonth = 1;
            curYear++;
          }
          else
            curMonth++;
          break;
        case CalEditYear:
          IncrementYear();
          curYear = RTCYEAR;
          curMonth = RTCMON;
          break;
        case CalEditMonth:
          IncrementMonth();
          curYear = RTCYEAR;
          curMonth = RTCMON;
          break;
        case CalEditDay:
          IncrementDay();
          break;
      }
      
      break;
    case CALENDAR_MONTH_MINUS:
      switch(calendarEditMode)
      {
        case CalEditNone:
          if(curMonth == 1)
          {
            curMonth = 12;
            curYear--;
          }
          else
            curMonth--;
          break;
        case CalEditYear:
          DecrementYear();
          curYear = RTCYEAR;
          curMonth = RTCMON;
          break;
        case CalEditMonth:
          DecrementMonth();
          curYear = RTCYEAR;
          curMonth = RTCMON;
          break;
        case CalEditDay:
          DecrementDay();
          break;
      }
      break;
    case CALENDAR_EDIT:
      switch(calendarEditMode)
      {
        case CalEditNone:
          calendarEditMode = CalEditYear;
          // Показ актуальных данных
          curYear = RTCYEAR;
          curMonth = RTCMON;
          break;
        case CalEditYear:
          calendarEditMode = CalEditMonth;
          break;
        case CalEditMonth:
          calendarEditMode = CalEditDay;
          break;
        case CalEditDay:
          calendarEditMode = CalEditNone;
          break;
      }
      break;
      
  }
  DrawCalendarPage();
}

static void DrawCalendarPage()
{
    /* draw bottom region */
  FillMyBuffer(STARTING_ROW,NUM_LCD_ROWS,0x00);

  //tString pBluetoothAddress[12+1];
  //tString pBluetoothName[12+1];

  
  
  gRow = 4;
  gColumn = 0;
  gBitColumnMask = BIT4;
  SetFont(MetaWatchTime);
  
  unsigned int year = curYear;
  unsigned char yd = year / 1000;
  unsigned char underline = (calendarEditMode == CalEditYear);
  
  WriteFontCharacterSpec(yd, 0, underline);
  
  year = year % 1000;  
  yd = year / 100;
  
  WriteFontCharacterSpec(yd, 0, underline);
  
  year = year % 100;  
  yd = year / 10;
  
  WriteFontCharacterSpec(yd, 0, underline);

  year = year % 10;  
  WriteFontCharacterSpec(year, 0, underline);

    gRow = 16;
    gColumn = 7;
    gBitColumnMask = BIT4;
    SetFont(MetaWatch7);

  underline = (calendarEditMode == CalEditMonth);
  WriteFontStringSpec(GetMonthName(curMonth), 0, 0, underline);

  
  //WriteFontCharacter(5);
  
  DrawCalendar();
  
  PrepareMyBufferForLcd(STARTING_ROW,NUM_LCD_ROWS);
  SendMyBufferToLcd(NUM_LCD_ROWS);

  CurrentIdlePage = CalendarPage;
  ConfigureIdleUserInterfaceButtons();
}

static void ShowCalendarHandler(void)
{
  StopDisplayTimer();
  curYear = RTCYEAR;
  curMonth = RTCMON;
  DrawCalendarPage();

}

/* change the parameter but don't save it into flash */
static void ConfigureDisplayHandler(tMessage* pMsg)
{
  switch (pMsg->Options)
  {
  case CONFIGURE_DISPLAY_OPTION_DONT_DISPLAY_SECONDS:
    nvDisplaySeconds = 0x00;
    break;
  case CONFIGURE_DISPLAY_OPTION_DISPLAY_SECONDS:
    nvDisplaySeconds = 0x01;
    break;
  case CONFIGURE_DISPLAY_OPTION_DONT_INVERT_DISPLAY:
    nvIdleBufferInvert = 0x00;
    break;
  case CONFIGURE_DISPLAY_OPTION_INVERT_DISPLAY:
    nvIdleBufferInvert = 0x01;
    break;
  }

  if ( CurrentMode == IDLE_MODE )
  {
    IdleUpdateHandler();
  }

}


static void ConfigureIdleBuferSizeHandler(tMessage* pMsg)
{
  nvIdleBufferConfig = pMsg->pBuffer[0] & IDLE_BUFFER_CONFIG_MASK;

  if ( CurrentMode == IDLE_MODE )
  {
    if ( nvIdleBufferConfig == WATCH_CONTROLS_TOP )
    {
      IdleUpdateHandler();
    }
  }

}

static void ModifyTimeHandler(tMessage* pMsg)
{
  int time;
  switch (pMsg->Options)
  {
  case MODIFY_TIME_INCREMENT_HOUR:
    /*! todo - make these functions */
    time = RTCHOUR;
    time++; if ( time == 24 ) time = 0;
    RTCHOUR = time;
    break;
  case MODIFY_TIME_INCREMENT_MINUTE:
    time = RTCMIN;
    time++; if ( time == 60 ) time = 0;
    RTCMIN = time;
    break;
  case MODIFY_TIME_INCREMENT_DOW:
    /* modify the day of the week (not the day of the month) */
    time = RTCDOW;
    time++; if ( time == 7 ) time = 0;
    RTCDOW = time;
    break;
  case MODIFY_TIME_INCREMENT_CORR:
    IncCorrectionValue();
    break;
  case MODIFY_TIME_DECREMENT_CORR:
    DecCorrectionValue();
    break;
  }

  /* now redraw the screen */
  //IdleUpdateHandler();
  MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
}

unsigned char GetIdleBufferConfiguration(void)
{
  return nvIdleBufferConfig;
}


static void SendMyBufferToLcd(unsigned char TotalRows)
{
  UpdateMyDisplay((unsigned char*)pMyBuffer,TotalRows);
}


static void InitMyBuffer(void)
{
  int row;
  int col;

  // Clear the display buffer.  Step through the rows
  for(row = STARTING_ROW; row < NUM_LCD_ROWS; row++)
  {
    // clear a horizontal line
    for(col = 0; col < NUM_LCD_COL_BYTES; col++)
    {
      pMyBuffer[row].Row = row+FIRST_LCD_LINE_OFFSET;
      pMyBuffer[row].Data[col] = 0x00;
      pMyBuffer[row].Dummy = 0x00;

    }
  }


}


static void FillMyBuffer(unsigned char StartingRow,
                         unsigned char NumberOfRows,
                         unsigned char FillValue)
{
  int row = StartingRow;
  int col;

  // Clear the display buffer.  Step through the rows
  for( ; row < NUM_LCD_ROWS && row < StartingRow+NumberOfRows; row++ )
  {
    // clear a horizontal line
    for(col = 0; col < NUM_LCD_COL_BYTES; col++)
    {
      pMyBuffer[row].Data[col] = FillValue;
    }
  }

}

static void PrepareMyBufferForLcd(unsigned char StartingRow,
                                  unsigned char NumberOfRows)
{
  int row = StartingRow;
  int col;

  /*
   * flip the bits before sending to LCD task because it will
   * dma this portion of the screen
  */
  if ( QueryInvertDisplay() == NORMAL_DISPLAY )
  {
    for( ; row < NUM_LCD_ROWS && row < StartingRow+NumberOfRows; row++)
    {
      for(col = 0; col < NUM_LCD_COL_BYTES; col++)
      {
        pMyBuffer[row].Data[col] = ~(pMyBuffer[row].Data[col]);
      }
    }
  }

}



static void CopyRowsIntoMyBuffer(unsigned char const* pImage,
                                 unsigned char StartingRow,
                                 unsigned char NumberOfRows)
{

  unsigned char DestRow = StartingRow;
  unsigned char SourceRow = 0;
  unsigned char col = 0;

  while ( DestRow < NUM_LCD_ROWS && SourceRow < NumberOfRows )
  {
    for(col = 0; col < NUM_LCD_COL_BYTES; col++)
    {
      pMyBuffer[DestRow].Data[col] = pImage[SourceRow*NUM_LCD_COL_BYTES+col];
    }

    DestRow++;
    SourceRow++;

  }

}

static void CopyColumnsIntoMyBuffer(unsigned char const* pImage,
                                    unsigned char StartingRow,
                                    unsigned char NumberOfRows,
                                    unsigned char StartingColumn,
                                    unsigned char NumberOfColumns)
{
  unsigned char DestRow = StartingRow;
  unsigned char RowCounter = 0;
  unsigned char DestColumn = StartingColumn;
  unsigned char ColumnCounter = 0;
  unsigned int SourceIndex = 0;

  /* copy rows into display buffer */
  while ( DestRow < NUM_LCD_ROWS && RowCounter < NumberOfRows )
  {
    DestColumn = StartingColumn;
    ColumnCounter = 0;
    while ( DestColumn < NUM_LCD_COL_BYTES && ColumnCounter < NumberOfColumns )
    {
      pMyBuffer[DestRow].Data[DestColumn] = pImage[SourceIndex];

      DestColumn++;
      ColumnCounter++;
      SourceIndex++;
    }

    DestRow++;
    RowCounter++;
  }

}

static void DrawTimeString(unsigned char row, unsigned char showSeconds)
{
  gRow = row;
  gColumn = 0;
  gBitColumnMask = BIT4;

  SetFont(MetaWatchTime);

  unsigned char msd;
  unsigned char lsd;

  /* display hour */
  int Hour = RTCHOUR;

  /* if required convert to twelve hour format */
  if ( GetTimeFormat() == TWELVE_HOUR )
  {
    if ( Hour == 0 )
    {
      Hour = 12;
    }
    else if ( Hour > 12 )
    {
      Hour -= 12;
    }
  }

  msd = Hour / 10;
  lsd = Hour % 10;
  
  /* if first digit is zero then leave location blank */
  if ( msd == 0 && GetTimeFormat() == TWELVE_HOUR )
  {
    WriteFontCharacter(TIME_CHARACTER_SPACE_INDEX);
  }
  else
  {
    WriteFontCharacter(msd);
  }

  WriteFontCharacter(lsd);

  WriteFontCharacter(TIME_CHARACTER_COLON_INDEX);

  /* display minutes */
  int Minutes = RTCMIN;
  msd = Minutes / 10;
  lsd = Minutes % 10;
  WriteFontCharacter(msd);
  WriteFontCharacter(lsd);

  if ( showSeconds )
  {
    int Seconds = RTCSEC;
    msd = Seconds / 10;
    lsd = Seconds % 10;

    WriteFontCharacter(TIME_CHARACTER_COLON_INDEX);
    WriteFontCharacter(msd);
    WriteFontCharacter(lsd);

  }
  else /* now things starting getting fun....*/
  {
    DisplayAmPm(row);

    /*if ( QueryBluetoothOn() == 0 )
    {
      CopyColumnsIntoMyBuffer(pBluetoothOffIdlePageIcon,
                              IDLE_PAGE_ICON_STARTING_ROW,
                              IDLE_PAGE_ICON_SIZE_IN_ROWS,
                              IDLE_PAGE_ICON_STARTING_COL,
                              IDLE_PAGE_ICON_SIZE_IN_COLS);
    }
    else if ( QueryPhoneConnected() == 0 )
    {
      CopyColumnsIntoMyBuffer(pPhoneDisconnectedIdlePageIcon,
                              IDLE_PAGE_ICON_STARTING_ROW,
                              IDLE_PAGE_ICON_SIZE_IN_ROWS,
                              IDLE_PAGE_ICON_STARTING_COL,
                              IDLE_PAGE_ICON_SIZE_IN_COLS);
    }
    else*/
  }

}

                          
static void DrawIdleScreen(void)
{
  DrawTimeString(6, nvDisplaySeconds);
  
      if ( QueryBatteryCharging() )
      {
        CopyColumnsIntoMyBuffer(pBatteryChargingIdlePageIconType2,
                                IDLE_PAGE_ICON2_STARTING_ROW,
                                IDLE_PAGE_ICON2_SIZE_IN_ROWS,
                                IDLE_PAGE_ICON2_STARTING_COL,
                                IDLE_PAGE_ICON2_SIZE_IN_COLS);
      }
      else
      {
        unsigned int bV = ReadBatterySenseAverage();

        if ( bV < 3500 )
        {
          CopyColumnsIntoMyBuffer(pLowBatteryIdlePageIconType2,
                                  IDLE_PAGE_ICON2_STARTING_ROW,
                                  IDLE_PAGE_ICON2_SIZE_IN_ROWS,
                                  IDLE_PAGE_ICON2_STARTING_COL,
                                  IDLE_PAGE_ICON2_SIZE_IN_COLS);
        }
      }
      DisplayDayOfWeek();
      DisplayDate();
      
#ifdef DIARY
      DisplayDiary();
#endif
}
#ifdef DIARY
static void DisplayDiary(void)
{
  if(!IsUpdateDiary())
    return;
  
  SetFont(MetaWatch7);
  
  char string0[20]; char string1[20];
  unsigned char now;
  
  for(unsigned char i = 0; i < 3; i++)
  {
    if(GetDiaryDataStrings(i, string0, string1, &now) == 0)
      continue;


    gRow = 42 + i*19;
    gColumn = 0;
    gBitColumnMask = BIT4;
    WriteFontStringSpec(string0, 20, 0 ,0);  
    
    gRow += 8;
    gColumn = 0;
    gBitColumnMask = BIT4;  
    WriteFontStringSpec(string1, 20, 0 ,0);

  }  
  
  
}
#endif
static void MenuModeHandler(unsigned char MsgOptions)
{
  StopDisplayTimer();

  /* draw entire region */
  FillMyBuffer(STARTING_ROW, PHONE_FULL_BUFFER_ROWS,0x00);

  switch (MsgOptions)
  {

  case MENU_MODE_OPTION_MAIN_PAGE:
    DrawMainOptionsPage();
    CurrentIdlePage = OptionsMainPage;
    ConfigureIdleUserInterfaceButtons();
    break;

  case MENU_MODE_OPTION_SETTINGS_PAGE:
    DrawOptionSettingsPage();
    CurrentIdlePage = OptionSettingsPage;
    ConfigureIdleUserInterfaceButtons();
    break;

/*  case MENU_MODE_OPTION_PAGE3:
    DrawMenu3();
    CurrentIdlePage = Menu3Page;
    ConfigureIdleUserInterfaceButtons();
    break;*/
    
  case MENU_MODE_OPTION_PAGE_ALARM_SETTINGS:
    DrawAlarmSettingsPage();
    CurrentIdlePage = AlarmSettingsPage;
    ConfigureIdleUserInterfaceButtons();
    break;

  case MENU_MODE_OPTION_PAGE_TIME_SETTINGS:
    //PauseRtc();
    DrawTimeSettingsPage();
    CurrentIdlePage = TimeSettingsPage;
    ConfigureIdleUserInterfaceButtons();
    break;
    

  case MENU_MODE_OPTION_UPDATE_CURRENT_PAGE:

  default:
    switch ( CurrentIdlePage )
    {
    case OptionsMainPage:
      DrawMainOptionsPage();
      break;
    case OptionSettingsPage:
      DrawOptionSettingsPage();
      break;
    /*case Menu3Page:
      DrawMenu3();
      break;*/
    case AlarmSettingsPage:
      DrawAlarmSettingsPage();
      break;
    case TimeSettingsPage:
      DrawTimeSettingsPage();
      break;
     
    default:
      PrintString("Menu Mode Screen Selection Error\r\n");
      break;
    }
    break;
  }

  /* these icons are common to all menus */
  DrawCommonMenuIcons();

  /* only invert the part that was just drawn */
  PrepareMyBufferForLcd(STARTING_ROW,NUM_LCD_ROWS);
  SendMyBufferToLcd(NUM_LCD_ROWS);

  /* MENU MODE DOES NOT TIMEOUT */

}

static void DrawMainOptionsPage(void)
{
  unsigned char const * pIcon;

/*  if ( QueryConnectionState() == Initializing )
  {
    pIcon = pPairableInitIcon;
  }
  else if ( QueryDiscoverable() )
  {
    pIcon = pPairableIcon;
  }
  else
  {
    pIcon = pUnpairableIcon;
  }

  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);
*/
  /***************************************************************************/

  if ( QueryConnectionState() == Initializing )
  {
    pIcon = pBluetoothInitIcon;
  }
  else if ( QueryBluetoothOn() )
  {
    pIcon = pBluetoothOnIcon;
  }
  else
  {
    pIcon = pBluetoothOffIcon;
  }

  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          RIGHT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);
  
  CopyColumnsIntoMyBuffer(pAlarmIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

  CopyColumnsIntoMyBuffer(pSettingsIcon,
                          BUTTON_ICON_B_E_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          RIGHT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);
  
  CopyColumnsIntoMyBuffer(pDiaryIcon,
                          BUTTON_ICON_B_E_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);
  
  /***************************************************************************/
/*
  if ( QueryLinkAlarmEnable() )
  {
    pIcon = pLinkAlarmOnIcon;
  }
  else
  {
    pIcon = pLinkAlarmOffIcon;
  }

  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_B_E_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);*/
}

static void DrawOptionSettingsPage(void)
{
  /* top button is always soft reset */
  CopyColumnsIntoMyBuffer(pResetButtonIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

  unsigned char const * pIcon;

/*  if ( QueryRstPinEnabled() )
  {
    pIcon = pRstPinIcon;
  }
  else
  {
    pIcon = pNmiPinIcon;
  }*/

  CopyColumnsIntoMyBuffer(pNormalDisplayMenuIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          RIGHT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

  /***************************************************************************/

/*  if ( QueryConnectionState() == Initializing )
  {
    pIcon = pSspInitIcon;
  }
  else if ( QuerySecureSimplePairingEnabled() )
  {
    pIcon = pSspEnabledIcon;
  }
  else
  {
    pIcon = pSspDisabledIcon;
  }*/

  CopyColumnsIntoMyBuffer(pTimeIcon,
                          BUTTON_ICON_B_E_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

}

/*static void DrawMenu3(void)
{
  unsigned char const * pIcon;

  pIcon = pNormalDisplayMenuIcon;

  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);
/////////////////////////////////////////////////////////////

#if 0
  // shipping mode was removed for now 
  CopyColumnsIntoMyBuffer(pShippingModeIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          RIGHT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);
#endif
 ///////////////////////////////////////////////////////////////////////

  if ( nvDisplaySeconds )
  {
    pIcon = pSecondsOnMenuIcon;
  }
  else
  {
    pIcon = pSecondsOffMenuIcon;
  }

  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_B_E_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

}
*/
static void DrawAlarmSettingsPage(void)
{
  SetFont(MetaWatchTime);
  gRow = 38; //
  gColumn = 0; //
  gBitColumnMask = BIT4; //

  unsigned char alarmHours = GetAlarmHours();
  
  unsigned char msd = alarmHours / 10;
  unsigned char lsd = alarmHours % 10;
  
  WriteFontCharacter(msd);
  WriteFontCharacter(lsd);
  WriteFontCharacter(TIME_CHARACTER_COLON_INDEX);
  
  unsigned char alarmMinutes = GetAlarmMinutes();
  
  msd = alarmMinutes / 10;
  lsd = alarmMinutes % 10;
  
  WriteFontCharacter(msd);
  WriteFontCharacter(lsd);
  
  /* if first digit is zero then leave location blank 
  if ( msd == 0 && GetTimeFormat() == TWELVE_HOUR )
  {
    WriteFontCharacter(TIME_CHARACTER_SPACE_INDEX);
  }
  else
  {
    WriteFontCharacter(msd);
  }
*/
  
  
  
  
  unsigned char const * pIcon;

  if(GetAlarmStatus() == 1)
  {
    pIcon = pAlarmOnIcon;
  }
  else
  {
    pIcon = pAlarmOffIcon;
  }
  
  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          RIGHT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);
 
  CopyColumnsIntoMyBuffer(pNumIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          3); // Ширина 24
  
  // Номер будильника
  
  gRow = 6; //
  gColumn = 3; //
  gBitColumnMask = BIT0; //

  unsigned char curAlarmNum = GetCurrentAlarm();
  WriteFontCharacter(curAlarmNum);
  
  
  /*
  pIcon = pPlusIcon;
 
  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_B_E_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

    pIcon = pMinusIcon;
 
  CopyColumnsIntoMyBuffer(pIcon,
                          BUTTON_ICON_С_D_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);
*/

}

static void DrawTimeSettingsPage(void)
{
  CopyColumnsIntoMyBuffer(pPlusIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          3);
  CopyColumnsIntoMyBuffer(pMinusIcon,
                          BUTTON_ICON_A_F_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          RIGHT_BUTTON_COLUMN + 3,
                          3);
  
  SetFont(MetaWatchTime);
  gRow = 6; //?
  gColumn = 3; //?
  gBitColumnMask = BIT4; //???

  signed char correctionValue = GetCorrectionValue();
  if(correctionValue == 0)
    WriteFontCharacter(TIME_CHARACTER_SPACE_INDEX);
  else if(correctionValue > 0)
    WriteFontCharacter(TIME_CHARACTER_PLUS_INDEX);
  else
  {
    WriteFontCharacter(TIME_CHARACTER_MINUS_INDEX);
    correctionValue = - correctionValue; 
  }
    
  unsigned char msd = correctionValue / 10;
  unsigned char lsd = correctionValue % 10;
  
  WriteFontCharacter(msd);
  WriteFontCharacter(lsd);
  
  DrawTimeString(38, 0);

}

static void DrawCommonMenuIcons(void)
{
/*  CopyColumnsIntoMyBuffer(pNextIcon,
                          BUTTON_ICON_B_E_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          RIGHT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);*/

  CopyColumnsIntoMyBuffer(pLedIcon,
                          BUTTON_ICON_C_D_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          LEFT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);

  CopyColumnsIntoMyBuffer(pExitIcon,
                          BUTTON_ICON_C_D_ROW,
                          BUTTON_ICON_SIZE_IN_ROWS,
                          RIGHT_BUTTON_COLUMN,
                          BUTTON_ICON_SIZE_IN_COLUMNS);
}

static void MenuButtonHandler(unsigned char MsgOptions)
{
  StopDisplayTimer();

  tMessage OutgoingMsg;

  switch (MsgOptions)
  {
  case MENU_BUTTON_OPTION_TOGGLE_DISCOVERABILITY:

    if ( QueryConnectionState() != Initializing )
    {
      SetupMessage(&OutgoingMsg,PairingControlMsg,NO_MSG_OPTIONS);

      if ( QueryDiscoverable() )
      {
        OutgoingMsg.Options = PAIRING_CONTROL_OPTION_DISABLE_PAIRING;
      }
      else
      {
        OutgoingMsg.Options = PAIRING_CONTROL_OPTION_ENABLE_PAIRING;
      }

      RouteMsg(&OutgoingMsg);
    }
    /* screen will be updated with a message from spp */
    break;

  case MENU_BUTTON_OPTION_TOGGLE_LINK_ALARM:
    ToggleLinkAlarmEnable();
    MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
    break;

  case MENU_BUTTON_OPTION_EXIT:

    /* save all of the non-volatile items */
    SetupMessage(&OutgoingMsg,PairingControlMsg,PAIRING_CONTROL_OPTION_SAVE_SPP);
    RouteMsg(&OutgoingMsg);

    SaveLinkAlarmEnable();
    SaveRstNmiConfiguration();
    SaveIdleBufferInvert();
    SaveDisplaySeconds();
    SaveAlarm();

    /* go back to the normal idle screen */
    SetupMessage(&OutgoingMsg,IdleUpdate,NO_MSG_OPTIONS);
    RouteMsg(&OutgoingMsg);

    break;

  case MENU_BUTTON_OPTION_TOGGLE_BLUETOOTH:

    if ( QueryConnectionState() != Initializing )
    {
      if ( QueryBluetoothOn() )
      {
        SetupMessage(&OutgoingMsg,TurnRadioOffMsg,NO_MSG_OPTIONS);
      }
      else
      {
        SetupMessage(&OutgoingMsg,TurnRadioOnMsg,NO_MSG_OPTIONS);
      }

      RouteMsg(&OutgoingMsg);
    }
    /* screen will be updated with a message from spp */
    break;

  case MENU_BUTTON_OPTION_TOGGLE_SECURE_SIMPLE_PAIRING:
    if ( QueryConnectionState() != Initializing )
    {
      SetupMessage(&OutgoingMsg,PairingControlMsg,PAIRING_CONTROL_OPTION_TOGGLE_SSP);
      RouteMsg(&OutgoingMsg);
    }
    /* screen will be updated with a message from spp */
    break;

  case MENU_BUTTON_OPTION_TOGGLE_RST_NMI_PIN:
    if ( QueryRstPinEnabled() )
    {
      DisableRstPin();
    }
    else
    {
      EnableRstPin();
    }
    MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
    break;

  case MENU_BUTTON_OPTION_DISPLAY_SECONDS:
    ToggleSecondsHandler(TOGGLE_SECONDS_OPTIONS_DONT_UPDATE_IDLE);
    MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
    break;

  case MENU_BUTTON_OPTION_INVERT_DISPLAY:
    if ( nvIdleBufferInvert == 1 )
    {
      nvIdleBufferInvert = 0;
    }
    else
    {
      nvIdleBufferInvert = 1;
    }
    MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
    break;

    
  case MENU_BUTTON_OPTION_ALAM_ON_OFF:
    if(GetAlarmStatus() == 0)
    {
      SetAlarmStatus(1);
    }
    else
    {
      SetAlarmStatus(0);
    }
    MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
    break;
    
  case MENU_BUTTON_OPTION_ALAM_MIN_PLUS:
    AddAlarmMinute();
    MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
    break;
    
  case MENU_BUTTON_OPTION_ALAM_HOUR_PLUS:
    AddAlarmHour();
    MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
    break;
    
  case MENU_BUTTON_OPTION_ALAM_NEXT_ALARM:
    IncCurrentAlarm();
    MenuModeHandler(MENU_MODE_OPTION_UPDATE_CURRENT_PAGE);
    break;
    
    
    
  default:
    break;
  }

}

static void ToggleSecondsHandler(unsigned char Options)
{
  if ( nvDisplaySeconds == 0 )
  {
    nvDisplaySeconds = 1;
  }
  else
  {
    nvDisplaySeconds = 0;
  }

  if ( Options == TOGGLE_SECONDS_OPTIONS_UPDATE_IDLE )
  {
    IdleUpdateHandler();
  }

}

static void DisplayAmPm(unsigned char row)
{
  /* don't display am/pm in 24 hour mode */
  if ( GetTimeFormat() == TWELVE_HOUR )
  {
    int Hour = RTCHOUR;

    unsigned char const *pIcon;

    if ( Hour >= 12 )
    {
      pIcon = Pm;
    }
    else
    {
      pIcon = Am;
    }

    WriteIcon4w10h(pIcon,row,8);
  }

}

static void DisplayDayOfWeek(void)
{
  /* row offset = 0 or 10 , column offset = 8 */
  gRow = 30;
  gColumn = 2;
  gBitColumnMask = BIT0;
  SetFont(MetaWatch7);
  WriteFontString(DaysOfTheWeek[RTCDOW]);
  //WriteIcon4w10h(DaysOfWeek[RTCDOW], 28, 2);
}

static void DisplayDate(void)
{
//  if ( QueryFirstContact() )
  {
    int First;
    int Second;

    /* determine if month or day is displayed first */
    if ( GetDateFormat() == MONTH_FIRST )
    {
      First = RTCMON;
      Second = RTCDAY;
    }
    else
    {
      First = RTCDAY;
      Second = RTCMON;
    }

    /* make it line up with AM/PM and Day of Week */
    gRow = 30;
    gColumn = 5;
    gBitColumnMask = BIT1;
    SetFont(MetaWatch7);


    //gColumn = 5;
    //gBitColumnMask = BIT1;
    
    
    WriteFontCharacter(First/10+'0');
    WriteFontCharacter(First%10+'0');
    WriteFontCharacter('/');
    WriteFontCharacter(Second/10+'0');
    WriteFontCharacter(Second%10+'0');
    WriteFontCharacter(' ');
    
    
    // add year when time is in 24 hour mode 
    //if ( GetTimeFormat() == TWENTY_FOUR_HOUR )
    {
      int year = RTCYEAR;
      WriteFontCharacter(year/1000+'0');
      year %= 1000;
      WriteFontCharacter(year/100+'0');
      year %= 100;
      WriteFontCharacter(year/10+'0');
      year %= 10;
      WriteFontCharacter(year+'0');
      //gRow = 12;
    }
  }
}

/* these items are 4w by 10h */
static void WriteIcon4w10h(unsigned char const * pIcon,
                           unsigned char RowOffset,
                           unsigned char ColumnOffset)
{

  /* copy digit into correct position */
  unsigned char RowNumber;
  unsigned char Column;

  for ( Column = 0; Column < 4; Column++ )
  {
    for ( RowNumber = 0; RowNumber < 10; RowNumber++ )
    {
      pMyBuffer[RowNumber+RowOffset].Data[Column+ColumnOffset] =
        pIcon[RowNumber+(Column*10)];
    }
  }

}

unsigned char* GetTemplatePointer(unsigned char TemplateSelect)
{
  return NULL;
}


const unsigned char pBarCodeImage[NUM_LCD_ROWS*NUM_LCD_COL_BYTES] =
{
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0xE0, 0xE7, 0x13, 0x87, 0x88, 0x62, 0x18, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x24, 0x92, 0x88, 0x88, 0x12, 0x18, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x24, 0x52, 0x90, 0x88, 0x12, 0x2C, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x24, 0x52, 0x90, 0x88, 0x12, 0x24, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x24, 0x72, 0x90, 0x88, 0x0E, 0x24, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x24, 0x52, 0x90, 0x88, 0x12, 0x24, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x24, 0x52, 0x90, 0x88, 0x12, 0x7E, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x24, 0x92, 0x88, 0x88, 0x22, 0x42, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x14, 0x12, 0x87, 0xFF, 0x42, 0x42, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x16, 0xC2, 0xC3, 0x80, 0x0F, 0x2F, 0x04, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x11, 0x23, 0xC0, 0x00, 0x82, 0x28, 0x04, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x11, 0x13, 0x60, 0x01, 0x82, 0x28, 0x04, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x91, 0x12, 0x20, 0x01, 0x82, 0x28, 0x04, 0x00, 0x00, 0x00, 
0x00, 0xE0, 0x50, 0x12, 0x20, 0x01, 0x02, 0xEF, 0x07, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x51, 0x12, 0x20, 0x01, 0x02, 0x29, 0x04, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x31, 0x12, 0xF0, 0x03, 0x02, 0x29, 0x04, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x32, 0x22, 0x10, 0x02, 0x82, 0x28, 0x04, 0x00, 0x00, 0x00, 
0x00, 0x20, 0x14, 0xC2, 0x13, 0x02, 0x42, 0x28, 0x04, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0xFE, 0x7F, 0x00, 0xFC, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0xC0, 0xC1, 0x07, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x43, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x46, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x58, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x70, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x40, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0xC0, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x10, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x20, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x20, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x60, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x40, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x40, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x80, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
};

const unsigned char pMetaWatchSplash[NUM_LCD_ROWS*NUM_LCD_COL_BYTES] =
{
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x30,0x60,0x80,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x30,0x60,0xC0,0x01,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x70,0x70,0xC0,0x01,0xE0,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x70,0xF0,0x40,0xE1,0xFF,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0xD8,0xD8,0x60,0x63,0xE0,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0xD8,0xD8,0x60,0x63,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0xC8,0x58,0x34,0x26,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x8C,0x0D,0x36,0x36,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x0E,0x8C,0x0D,0x36,0x36,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0xFE,0x0F,0x05,0x1E,0x1C,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x0E,0x00,0x07,0x1C,0x1C,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x07,0x0C,0x18,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x02,0x0C,0x18,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x30,0x18,0xFC,0xFC,0x70,0x04,0x00,0x31,0xFC,0xE1,0x83,0x40,
0x30,0x18,0xFC,0xFC,0x70,0x04,0x02,0x31,0x20,0x18,0x8C,0x40,
0x70,0x1C,0x0C,0x30,0x70,0x08,0x82,0x30,0x20,0x04,0x88,0x40,
0x78,0x3C,0x0C,0x30,0xD8,0x08,0x85,0x48,0x20,0x04,0x80,0x40,
0xD8,0x36,0x0C,0x30,0xD8,0x08,0x85,0x48,0x20,0x02,0x80,0x40,
0xD8,0x36,0xFC,0x30,0x8C,0x91,0x48,0xCC,0x20,0x02,0x80,0x7F,
0xDC,0x76,0xFC,0x30,0x8C,0x91,0x48,0x84,0x20,0x02,0x80,0x40,
0x8C,0x63,0x0C,0x30,0xFC,0x91,0x48,0x84,0x20,0x02,0x80,0x40,
0x8C,0x63,0x0C,0x30,0xFE,0xA3,0x28,0xFE,0x21,0x04,0x80,0x40,
0x86,0xC3,0x0C,0x30,0x06,0xA3,0x28,0x02,0x21,0x04,0x88,0x40,
0x06,0xC1,0xFC,0x30,0x03,0x46,0x10,0x01,0x22,0x18,0x8C,0x40,
0x06,0xC1,0xFC,0x30,0x03,0x46,0x10,0x01,0x22,0xE0,0x83,0x40,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};



const unsigned char Am[10*4] =
{
0x00,0x00,0x9C,0xA2,0xA2,0xA2,0xBE,0xA2,0xA2,0x00,
0x00,0x00,0x08,0x0D,0x0A,0x08,0x08,0x08,0x08,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

const unsigned char Pm[10*4] =
{
0x00,0x00,0x9E,0xA2,0xA2,0x9E,0x82,0x82,0x82,0x00,
0x00,0x00,0x08,0x0D,0x0A,0x08,0x08,0x08,0x08,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

/*
const unsigned char DaysOfWeek[7][10*4] =
{
0x00,0x00,0x9C,0xA2,0x82,0x9C,0xA0,0xA2,0x1C,0x00,
0x00,0x00,0x28,0x68,0xA8,0x28,0x28,0x28,0x27,0x00,
0x00,0x00,0x02,0x02,0x02,0x03,0x02,0x02,0x02,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x22,0xB6,0xAA,0xA2,0xA2,0xA2,0x22,0x00,
0x00,0x00,0x27,0x68,0xA8,0x28,0x28,0x28,0x27,0x00,
0x00,0x00,0x02,0x02,0x02,0x03,0x02,0x02,0x02,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0xBE,0x88,0x88,0x88,0x88,0x88,0x08,0x00,
0x00,0x00,0xE8,0x28,0x28,0xE8,0x28,0x28,0xE7,0x00,
0x00,0x00,0x03,0x00,0x00,0x01,0x00,0x00,0x03,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0xA2,0xA2,0xAA,0xAA,0xAA,0xAA,0x94,0x00,
0x00,0x00,0xEF,0x20,0x20,0x27,0x20,0x20,0xEF,0x00,
0x00,0x00,0x01,0x02,0x02,0x02,0x02,0x02,0x01,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0xBE,0x88,0x88,0x88,0x88,0x88,0x88,0x00,
0x00,0x00,0x28,0x28,0x28,0x2F,0x28,0x28,0xC8,0x00,
0x00,0x00,0x7A,0x8A,0x8A,0x7A,0x4A,0x8A,0x89,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0xBE,0x82,0x82,0x9E,0x82,0x82,0x82,0x00,
0x00,0x00,0xC7,0x88,0x88,0x87,0x84,0x88,0xC8,0x00,
0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x01,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x1C,0xA2,0x82,0x9C,0xA0,0xA2,0x9C,0x00,
0x00,0x00,0xE7,0x88,0x88,0x88,0x8F,0x88,0x88,0x00,
0x00,0x00,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
*/

static void DontChangeButtonConfiguration(void)
{
  /* assign LED button to all modes */
  unsigned char i;
  for ( i = 0; i < NUMBER_OF_BUTTON_MODES; i++ )
  {
    /* turn off led 3 seconds after button has been released */
    EnableButtonAction(i,
                       SW_D_INDEX,
                       BUTTON_STATE_PRESSED,
                       LedChange,
                       LED_START_OFF_TIMER);

    /* turn on led immediately when button is pressed */
    EnableButtonAction(i,
                       SW_D_INDEX,
                       BUTTON_STATE_IMMEDIATE,
                       LedChange,
                       LED_ON_OPTION);

    /* software reset is available in all modes */
    EnableButtonAction(i,
                       SW_F_INDEX,
                       BUTTON_STATE_LONG_HOLD,
                       SoftwareResetMsg,
                       MASTER_RESET_OPTION);

  }

}

static void SetupNormalIdleScreenButtons(void)
{
  EnableButtonAction(NORMAL_IDLE_SCREEN_BUTTON_MODE,
                     SW_F_INDEX,
                     BUTTON_STATE_IMMEDIATE,
                     WatchStatusMsg,
                     RESET_DISPLAY_TIMER);

  EnableButtonAction(NORMAL_IDLE_SCREEN_BUTTON_MODE,
                     SW_B_INDEX,
                     BUTTON_STATE_IMMEDIATE,
                     ShowCalendarMsg,
                     NO_MSG_OPTIONS);

  /* led is already assigned */

  EnableButtonAction(NORMAL_IDLE_SCREEN_BUTTON_MODE,
                     SW_E_INDEX,
                     BUTTON_STATE_IMMEDIATE,
                     MenuModeMsg,
                     MENU_MODE_OPTION_MAIN_PAGE);

  EnableButtonAction(NORMAL_IDLE_SCREEN_BUTTON_MODE,
                     SW_A_INDEX,
                     BUTTON_STATE_IMMEDIATE,
                     ToggleSecondsMsg,
                     TOGGLE_SECONDS_OPTIONS_UPDATE_IDLE);

/*  EnableButtonAction(NORMAL_IDLE_SCREEN_BUTTON_MODE,
                     SW_A_INDEX,
                     BUTTON_STATE_IMMEDIATE,
                     BarCode,
                     RESET_DISPLAY_TIMER);*/
}

static void ConfigureIdleUserInterfaceButtons(void)
{
  if ( CurrentIdlePage != LastIdlePage )
  {
    LastIdlePage = CurrentIdlePage;

    /* only allow reset on one of the pages */
    DisableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                        SW_F_INDEX,
                        BUTTON_STATE_PRESSED);

    switch ( CurrentIdlePage )
    {
    case NormalPage:
      /* do nothing */
      break;

    case RadioOnWithPairingInfoPage:

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_F_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         WatchStatusMsg,
                         RESET_DISPLAY_TIMER);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_E_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         ShowCalendarMsg,
                         NO_MSG_OPTIONS);

      /* led is already assigned */

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_C_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuModeMsg,
                         MENU_MODE_OPTION_MAIN_PAGE);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_B_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         ToggleSecondsMsg,
                         TOGGLE_SECONDS_OPTIONS_UPDATE_IDLE);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_A_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         BarCode,
                         RESET_DISPLAY_TIMER);

      break;

    case BluetoothOffPage:
    case RadioOnWithoutPairingInfoPage:

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_F_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         ModifyTimeMsg,
                         MODIFY_TIME_INCREMENT_HOUR);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_E_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         ShowCalendarMsg,
                         NO_MSG_OPTIONS);

      /* led is already assigned */

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_C_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuModeMsg,
                         MENU_MODE_OPTION_MAIN_PAGE);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_B_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         ModifyTimeMsg,
                         MODIFY_TIME_INCREMENT_DOW);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_A_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         ModifyTimeMsg,
                         MODIFY_TIME_INCREMENT_MINUTE);
      break;



    case OptionsMainPage:
      // Открыть окно с настройками времени
      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_F_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuModeMsg,
                         MENU_MODE_OPTION_PAGE_ALARM_SETTINGS);
      
      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_A_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuButtonMsg,
                         MENU_BUTTON_OPTION_TOGGLE_BLUETOOTH);

      /*
      
      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_F_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuButtonMsg,
                         MENU_BUTTON_OPTION_TOGGLE_DISCOVERABILITY);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_E_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuButtonMsg,
                         MENU_BUTTON_OPTION_TOGGLE_LINK_ALARM);
*/
      /* led is already assigned */

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_C_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuButtonMsg,
                         MENU_BUTTON_OPTION_EXIT);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_B_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuModeMsg,
                         MENU_MODE_OPTION_SETTINGS_PAGE);


      break;

    case OptionSettingsPage:

      /* this cannot be immediate because Master Reset is on this button also */
      DisableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                          SW_F_INDEX,
                          BUTTON_STATE_IMMEDIATE);
      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_F_INDEX,
                         BUTTON_STATE_PRESSED,
                         SoftwareResetMsg,
                         NO_MSG_OPTIONS);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_E_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuModeMsg,
                         MENU_MODE_OPTION_PAGE_TIME_SETTINGS);

      /* led is already assigned */

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_C_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuButtonMsg,
                         MENU_BUTTON_OPTION_EXIT);
/*
      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_B_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuModeMsg,
                         MENU_MODE_OPTION_PAGE3);*/
      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_A_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuButtonMsg,
                         MENU_BUTTON_OPTION_INVERT_DISPLAY);

      break;


/*    case Menu3Page:



      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_E_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuButtonMsg,
                         MENU_BUTTON_OPTION_DISPLAY_SECONDS);

     // led is already assigned 

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_C_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuButtonMsg,
                         MENU_BUTTON_OPTION_EXIT);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_B_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuModeMsg,
                         MENU_MODE_OPTION_PAGE_ALARM);


      DisableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                          SW_A_INDEX,
                          BUTTON_STATE_IMMEDIATE);

      break;*/
   case AlarmSettingsPage:

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_F_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuButtonMsg,
                         MENU_BUTTON_OPTION_ALAM_NEXT_ALARM);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_E_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuButtonMsg,
                         MENU_BUTTON_OPTION_ALAM_HOUR_PLUS);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_A_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuButtonMsg,
                         MENU_BUTTON_OPTION_ALAM_ON_OFF);

      
      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_B_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuButtonMsg,
                         MENU_BUTTON_OPTION_ALAM_MIN_PLUS);

      /* led is already assigned */

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_C_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuButtonMsg,
                         MENU_BUTTON_OPTION_EXIT);

      /*EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_B_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuModeMsg,
                         MENU_MODE_OPTION_MAIN_PAGE);*/

/*
      DisableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                          SW_A_INDEX,
                          BUTTON_STATE_IMMEDIATE);
*/
      break;
   case TimeSettingsPage:

     // Добавить секунду коррекции
      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_F_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         ModifyTimeMsg,
                         MODIFY_TIME_INCREMENT_CORR);

      // Отнять секунду коррекции
      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_A_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         ModifyTimeMsg,
                         MODIFY_TIME_DECREMENT_CORR);

      
      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_B_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         ModifyTimeMsg,
                         MODIFY_TIME_INCREMENT_MINUTE);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_E_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         ModifyTimeMsg,
                         MODIFY_TIME_INCREMENT_HOUR);
      
      
      /* led is already assigned */

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_C_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuButtonMsg,
                         MENU_BUTTON_OPTION_EXIT);

/*
      DisableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                          SW_A_INDEX,
                          BUTTON_STATE_IMMEDIATE);
*/
      break;    
    case CalendarPage:

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_A_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         CalendarMsg,
                         CALENDAR_MONTH_MINUS);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_B_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         CalendarMsg,
                         CALENDAR_MONTH_PLUS);
      
      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_F_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         CalendarMsg,
                         CALENDAR_EDIT);
      /* led is already assigned */

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_C_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         IdleUpdate,
                         RESET_DISPLAY_TIMER);
      break;

    case WatchStatusPage:

      /* map this mode's entry button to go back to the idle mode */
      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_F_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         IdleUpdate,
                         RESET_DISPLAY_TIMER);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_E_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         ShowCalendarMsg,
                         NO_MSG_OPTIONS);

      /* led is already assigned */

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_C_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuModeMsg,
                         MENU_MODE_OPTION_MAIN_PAGE);

      DisableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                          SW_B_INDEX,
                          BUTTON_STATE_IMMEDIATE);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_A_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         BarCode,
                         RESET_DISPLAY_TIMER);
      break;

    case QrCodePage:

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_F_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         WatchStatusMsg,
                         RESET_DISPLAY_TIMER);

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_E_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         ShowCalendarMsg,
                         NO_MSG_OPTIONS);

      /* led is already assigned */

      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_C_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         MenuModeMsg,
                         MENU_MODE_OPTION_MAIN_PAGE);

      DisableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                          SW_B_INDEX,
                          BUTTON_STATE_IMMEDIATE);

      /* map this mode's entry button to go back to the idle mode */
      EnableButtonAction(WATCH_DRAWN_SCREEN_BUTTON_MODE,
                         SW_A_INDEX,
                         BUTTON_STATE_IMMEDIATE,
                         IdleUpdate,
                         RESET_DISPLAY_TIMER);

      break;


    }
  }
}

/* the default is for all simple button presses to be sent to the phone */
static void DefaultApplicationAndNotificationButtonConfiguration(void)
{
  unsigned char index = 0;

  /*
   * this will configure the pull switch even though it does not exist
   * on the watch
   */
  for(index = 0; index < NUMBER_OF_BUTTONS; index++)
  {
    if ( index == SW_UNUSED_INDEX )
    {
      index++;
    }

    EnableButtonAction(APPLICATION_SCREEN_BUTTON_MODE,
                       index,
                       BUTTON_STATE_PRESSED,
                       ButtonEventMsg,
                       NO_MSG_OPTIONS);

    EnableButtonAction(NOTIFICATION_BUTTON_MODE,
                       index,
                       BUTTON_STATE_PRESSED,
                       ButtonEventMsg,
                       NO_MSG_OPTIONS);

    EnableButtonAction(SCROLL_BUTTON_MODE,
                       index,
                       BUTTON_STATE_PRESSED,
                       ButtonEventMsg,
                       NO_MSG_OPTIONS);

  }

}


/******************************************************************************/

void InitializeIdleBufferConfig(void)
{
  nvIdleBufferConfig = WATCH_CONTROLS_TOP;
  OsalNvItemInit(NVID_IDLE_BUFFER_CONFIGURATION,
                 sizeof(nvIdleBufferConfig),
                 &nvIdleBufferConfig);
}

void InitializeIdleBufferInvert(void)
{
  nvIdleBufferInvert = 0;
  OsalNvItemInit(NVID_IDLE_BUFFER_INVERT,
                 sizeof(nvIdleBufferInvert),
                 &nvIdleBufferInvert);
}

void InitializeDisplaySeconds(void)
{
  nvDisplaySeconds = 0;
  OsalNvItemInit(NVID_DISPLAY_SECONDS,
                 sizeof(nvDisplaySeconds),
                 &nvDisplaySeconds);
}

static void SaveIdleBufferInvert(void)
{
  OsalNvWrite(NVID_IDLE_BUFFER_INVERT,
              NV_ZERO_OFFSET,
              sizeof(nvIdleBufferInvert),
              &nvIdleBufferInvert);
}

static void SaveDisplaySeconds(void)
{
  OsalNvWrite(NVID_DISPLAY_SECONDS,
              NV_ZERO_OFFSET,
              sizeof(nvDisplaySeconds),
              &nvDisplaySeconds);
}

unsigned char QueryDisplaySeconds(void)
{
  return nvDisplaySeconds;
}

unsigned char QueryInvertDisplay(void)
{
  return nvIdleBufferInvert;
}

/******************************************************************************/

static unsigned int CharacterMask;
static unsigned char CharacterRows;
static unsigned char CharacterWidth;
static unsigned int bitmap[MAX_FONT_ROWS];

/* fonts can be up to 16 bits wide */
static void WriteFontCharacter(unsigned char Character)
{
  WriteFontCharacterSpec(Character, 0, 0);
}

static void WriteFontCharacterSpec(unsigned char Character, unsigned char underline, unsigned char inserse)
{
  CharacterMask = BIT0;
  CharacterRows = GetCharacterHeight();
  CharacterWidth = GetCharacterWidth(Character);
  GetCharacterBitmap(Character,(unsigned int*)&bitmap);

  if ( gRow + CharacterRows > NUM_LCD_ROWS )
  {
    PrintString("Not enough rows to display character\r\n");
    return;
  }

  /* do things bit by bit */
  unsigned char i;
  unsigned char row;

  for (i = 0 ; i < CharacterWidth && gColumn < NUM_LCD_COL_BYTES; i++ )
  {        
    for(row = 0; row < CharacterRows; row++)
    {
      if ( (CharacterMask & bitmap[row]) != 0)
      {
        if(inserse == 0)
          pMyBuffer[gRow+row].Data[gColumn] |= gBitColumnMask;
      }
      else if(inserse == 1)
        pMyBuffer[gRow+row].Data[gColumn] |= gBitColumnMask;
      
    }
    
    if(underline)
    {
      pMyBuffer[gRow + CharacterRows + 2].Data[gColumn] |= gBitColumnMask;
    }
    
    

    /* the shift direction seems backwards... */
    // Извращенцы
    CharacterMask = CharacterMask << 1;
    gBitColumnMask = gBitColumnMask << 1;
    if ( gBitColumnMask == 0 )
    {
      gBitColumnMask = BIT0;
      gColumn++;
    }

  }

  if(inserse == 1)
  {
    for(row = 0; row < CharacterRows; row++)
    {
        pMyBuffer[gRow+row].Data[gColumn] |= gBitColumnMask;
    }
  }
  
  /* add spacing between characters */
  unsigned char FontSpacing = GetFontSpacing();
  for(i = 0; i < FontSpacing; i++)
  {
    gBitColumnMask = gBitColumnMask << 1;
    if ( gBitColumnMask == 0 )
    {
      gBitColumnMask = BIT0;
      gColumn++;
    }
  }
}


static void FillRect(unsigned char left, unsigned char top, unsigned char width, unsigned char height)
{
  /* do things bit by bit */
  unsigned char i;
  //unsigned char row;
  
  unsigned char currentByte = left / 8;
  unsigned char currentBit = BIT0 << (left % 8);
  
  for (i = 0 ; i < width && currentByte < NUM_LCD_COL_BYTES; i++ )
  {
    for(unsigned char row = 0; row < height; row++)
    {
      pMyBuffer[top+row].Data[currentByte] |= currentBit;
    }      
    currentBit = currentBit << 1;
    if ( currentBit == 0 )
    {
      currentBit = BIT0;
      currentByte++;
    }
  

  }
}

// Мой метод вывода без привязки к байтам и выравниманием в пределах заданого прямоугольника
static void MyWriteFontCharacter(unsigned char Character, unsigned char colunInPixels, 
                                 unsigned char rectangleWidth, unsigned char inserse)
{
  CharacterMask = BIT0;
  CharacterRows = GetCharacterHeight();
  CharacterWidth = GetCharacterWidth(Character);
  GetCharacterBitmap(Character,(unsigned int*)&bitmap);

  if ( gRow + CharacterRows > NUM_LCD_ROWS )
  {
    PrintString("Not enough rows to display character\r\n");
    return;
  }

  /* do things bit by bit */
  unsigned char i;
  unsigned char row;

  
  signed char offsetPix = 0;
  if(rectangleWidth > 0)
    offsetPix = rectangleWidth / 2 -  CharacterWidth / 2;
  if(offsetPix < 0)
    offsetPix = 0;
  
  colunInPixels += offsetPix;
  
  unsigned char currentByte = colunInPixels / 8;
  unsigned char currentBit = BIT0 << (colunInPixels % 8);
  
    //Рисуем фон
/*  unsigned char currentBgByte = currentByte;
  unsigned char currentBgBit = currentBit;
  if(inserse == 1)
  {
    for (i = 0 ; i < CharacterWidth && currentBgByte < NUM_LCD_COL_BYTES; i++ )
    {
      for(unsigned char bgRow = 0; bgRow < CharacterRows; bgRow++)
      {
          pMyBuffer[gRow+bgRow].Data[currentBgByte] |= currentBgBit;
      }
  
      currentBgBit = currentBgBit << 1;
      if ( currentBgBit == 0 )
      {
        currentBgBit = BIT0;
        currentBgByte++;
      }
  
    }

  }*/
  
  
  for (i = 0 ; i < CharacterWidth && currentByte < NUM_LCD_COL_BYTES; i++ )
  {
    for(row = 0; row < CharacterRows; row++)
    {
      // получаем бит символа
      // если он =1, то надо добавить в буфер
      if ( (CharacterMask & bitmap[row]) != 0 )
      {
        if(inserse == 1)
        {
          pMyBuffer[gRow+row].Data[currentByte] ^= currentBit;
        }
        else
        {
          pMyBuffer[gRow+row].Data[currentByte] |= currentBit;
        }
      }
    }

    /* the shift direction seems backwards... */
    // Извращенцы
    CharacterMask = CharacterMask << 1;
    currentBit = currentBit << 1;
    if ( currentBit == 0 )
    {
      currentBit = BIT0;
      currentByte++;
    }

  }

  /* add spacing between characters */
  /*unsigned char FontSpacing = GetFontSpacing();
  for(i = 0; i < FontSpacing; i++)
  {
    gBitColumnMask = gBitColumnMask << 1;
    if ( gBitColumnMask == 0 )
    {
      gBitColumnMask = BIT0;
      gColumn++;
    }
  }*/

}
void WriteFontString(const tString *pString)
{
  WriteFontStringSpec(pString, 0, 0, 0);
}

void WriteFontStringSpec(const tString *pString, unsigned char maxlen, unsigned char underline, unsigned char inverse)
{
  unsigned char i = 0;
  signed char len;
  if(maxlen == 0)
    len = 100;
  else
    len = maxlen;
    
  
  while (len >= 0 && pString[i] != 0 && gColumn < NUM_LCD_COL_BYTES)
  {
    WriteFontCharacterSpec(pString[i++], underline, inverse);
    len--;
  }

}

/******************************************************************************/

unsigned char QueryIdlePageNormal(void)
{
  if ( CurrentIdlePage == NormalPage )
  {
    return 1;
  }
  else
  {
    return 0;
  };

}

unsigned char LcdRtcUpdateHandlerIsr(void)
{
  unsigned char ExitLpm = 0;

  unsigned int RtcSeconds = RTCSEC;

  if ( RtcUpdateEnable )
  {
    /* send a message every second or once a minute */
    if (   QueryDisplaySeconds()
        || RtcSeconds == 0 )
    {
      tMessage Msg;
      SetupMessage(&Msg,IdleUpdate,NO_MSG_OPTIONS);
      SendMessageToQueueFromIsr(DISPLAY_QINDEX,&Msg);
      ExitLpm = 1;
    }
  }

  return ExitLpm;

}

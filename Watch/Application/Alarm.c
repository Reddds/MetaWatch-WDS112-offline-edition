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
#include "Alarm.h"
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

static void AlarmTask(void *pvParameters);

static void AlarmMessageHandler(tMessage* pMsg);

void InitializeAlarm(void);
void GenerateAlarm(void);

static tTimerId AlarmTimerId;
//static void InitializeAlarmInterval(void);

static unsigned int nvAlarmIntervalInSeconds = 1;

#define ALARM_MSG_QUEUE_LEN   8
#define ALARM_STACK_SIZE	   (configMINIMAL_STACK_SIZE + 100)
#define ALARM_TASK_PRIORITY   (tskIDLE_PRIORITY + 1)


xTaskHandle xAlrtTaskHandle;

static tMessage AlarmMsg;



// Только что прозвонили
unsigned char alreadyAlarmed = 0;

/*! Does the initialization and allocates the resources for the alert task
 *
 */
void InitializeAlarmTask( void )
{
  // This is a Rx message queue, messages come from Serial IO or button presses
  QueueHandles[ALARM_QINDEX] =
    xQueueCreate( ALARM_MSG_QUEUE_LEN, MESSAGE_QUEUE_ITEM_SIZE );

  // prams are: task function, task name, stack len , task params, priority, task handle
  xTaskCreate(AlarmTask,
              (const signed char *)"ALARM",
              ALARM_STACK_SIZE,
              NULL,
              ALARM_TASK_PRIORITY,
              &xAlrtTaskHandle);

}


/*! Function to implement the AlarmTask loop
 *
 * \param pvParameters not used
 *
 */
static void AlarmTask(void *pvParameters)
{
  if ( QueueHandles[ALARM_QINDEX] == 0 )
  {
    PrintString("Alarm Queue not created!\r\n");
  }

  InitializeAlarm();
  
/*  PrintString(SPP_DEVICE_NAME);
  PrintString2("\r\nSoftware Version ",VERSION_STRING);
  PrintString("\r\n\r\n");

  InitializeRstNmiConfiguration();
*/
  /*
   * check on the battery
   */
/*  ConfigureBatteryPins();
  BatteryChargingControl();
  BatterySenseCycle();
*/
  /*
   * now set up a timer that will cause the 
   * 
   */
  AlarmTimerId = AllocateOneSecondTimer();

  //InitializeBatteryMonitorInterval();

  SetupOneSecondTimer(AlarmTimerId,
                      nvAlarmIntervalInSeconds,
                      REPEAT_FOREVER,
                      ALARM_QINDEX,
                      AlarmControl,
                      NO_MSG_OPTIONS);

  StartOneSecondTimer(AlarmTimerId);

  
  

 

  

}

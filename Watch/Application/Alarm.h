#ifndef ALARM_H
#define ALARM_H

#ifndef MESSAGES_H
  #error "Messages.h must be included before Alarm.h"
#endif

#ifndef QUEUE_H
  #error "queue.h must be included before Alarm.h"
#endif

/*! Setup the queue for the alert task */
void InitializeAlarmTask(void);

//void SaveAlarm(void);
//unsigned char GetAlarmStatus();
//void SetAlarmStatus(unsigned char on);
//unsigned char GetAlarmMinutes();
//unsigned char GetAlarmHours();
//unsigned char AddAlarmMinute();
//unsigned char AddAlarmHour();

/*! Save the RstNmi pin configuration value in non-volatile menu */
//void SaveRstNmiConfiguration(void);


#endif //ALARM_H
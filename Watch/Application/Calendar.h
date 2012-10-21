#ifndef CALENDAR_H
#define CALENDAR_H

unsigned char daysInMonth(unsigned char month, unsigned int year);

unsigned char dayOfWeek1(unsigned char month, unsigned int year);
unsigned char dayOfWeek2(unsigned char day, unsigned char month, unsigned int year);

#endif // CALENDAR_H
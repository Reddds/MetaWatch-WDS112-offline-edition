#include "Calendar.h"

#define StartYear 2000
#define FirstDayOfStartYear 6
#define FirstYearIsLeap 1



static unsigned int day_tab[2][13] =
{   { 0,31,28,31,30,31,30,31,31,30,31,30,31} ,
    { 0,31,29,31,30,31,30,31,31,30,31,30,31}
};

unsigned char isLeap(unsigned int year);
unsigned int dayOfYear(unsigned char day, unsigned char month, unsigned int year);
unsigned char dayOfWeek(unsigned int dayOfYear, unsigned char month, unsigned int year);


unsigned char dayOfWeek1(unsigned char month, unsigned int year)
{
  unsigned int doy = dayOfYear(1, month, year);
  return dayOfWeek(doy, month, year);
}

unsigned char dayOfWeek2(unsigned char day, unsigned char month, unsigned int year)
{
  unsigned int doy = dayOfYear(day, month, year);
  return dayOfWeek(doy, month, year);
}

unsigned char daysInMonth(unsigned char month, unsigned int year)
{
  if(month > 12)
    return 0;
  return day_tab[isLeap(year)][month];
}

unsigned char dayOfWeek(unsigned int dayOfYear, unsigned char month, unsigned int year)
{
      int iNumberOfLeap;
      unsigned char week_day;

      year -= StartYear;
      iNumberOfLeap = year/4 - year/100 + year/400 + FirstYearIsLeap;
      if(isLeap(year))
        iNumberOfLeap--;
      
      week_day = (year + iNumberOfLeap + FirstDayOfStartYear + 
                        (dayOfYear-1)) % 7;
      if (week_day == 0) week_day = 7;


      return week_day;
}

unsigned char isLeap(unsigned int year)
{
      /* reference Ritchie&Kernighan */
   if((year%4 == 0 && year%100 != 0) || year%400 == 0)
    return 1;
 
  return 0;
}

unsigned int dayOfYear(unsigned char day, unsigned char month, unsigned int year)
{
      int dayofyear = 0; 
      unsigned char ii, leap;

      leap = isLeap(year);

      for (ii = 1; ii < month; ii++) 
            dayofyear += day_tab[leap][ii];

      dayofyear += day;

      return dayofyear;
}
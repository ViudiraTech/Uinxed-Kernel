/*-----------------------------------------------------------------------*/
/* FatFs timestamp helper                                                */
/*-----------------------------------------------------------------------*/

#include <cmos.h>
#include <ff.h>

DWORD get_fattime(void)
{
    DWORD year = get_year();
    DWORD mon  = get_mon_hex();
    DWORD day  = get_day_of_month();
    DWORD hour = get_hour_hex();
    DWORD min  = get_min_hex();
    DWORD sec  = get_sec_hex();

    if (year < 1980) year = 1980;

    return ((year - 1980) << 25) | (mon << 21) | (day << 16) | (hour << 11) | (min << 5) | (sec / 2);
}

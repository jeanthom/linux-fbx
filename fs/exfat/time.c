
#include <linux/kernel.h>
#include <linux/fs.h>

#include "exfat.h"
#include "exfat_fs.h"



extern struct timezone sys_tz;

/*
 * The epoch of FAT timestamp is 1980.
 *     :  bits :     value
 * date:  0 -  4: day	(1 -  31)
 * date:  5 -  8: month	(1 -  12)
 * date:  9 - 15: year	(0 - 127) from 1980
 * time:  0 -  4: sec	(0 -  29) 2sec counts
 * time:  5 - 10: min	(0 -  59)
 * time: 11 - 15: hour	(0 -  23)
 */
#define SECS_PER_MIN	60
#define SECS_PER_HOUR	(60 * 60)
#define SECS_PER_DAY	(SECS_PER_HOUR * 24)
/* days between 1.1.70 and 1.1.80 (2 leap days) */
#define DAYS_DELTA	(365 * 10 + 2)
/* 120 (2100 - 1980) isn't leap year */
#define YEAR_2100	120
#define IS_LEAP_YEAR(y)	(!((y) & 3) && (y) != YEAR_2100)

/* Linear day numbers of the respective 1sts in non-leap years. */
static time_t days_in_year[] = {
	/* Jan  Feb  Mar  Apr  May  Jun  Jul  Aug  Sep  Oct  Nov  Dec */
	0,   0,  31,  59,  90, 120, 151, 181, 212, 243, 273, 304, 334, 0, 0, 0,
};

/* Convert a FAT time/date pair to a UNIX date (seconds since 1 1 70). */
void exfat_time_2unix(struct timespec *ts, u32 datetime, u8 time_cs,
		      s8 tz_offset)
{
	u16 date = (datetime >> 16);
	u16 time = (datetime & 0xffff);
	time_t second, day, leap_day, month, year;

	year  = date >> 9;
	month = max(1, (date >> 5) & 0xf);
	day   = max(1, date & 0x1f) - 1;

	if (((tz_offset & (1 << 6)) == 0))
		tz_offset &= ~(1 << 7);

	leap_day = (year + 3) / 4;
	if (year > YEAR_2100)		/* 2100 isn't leap year */
		leap_day--;
	if (IS_LEAP_YEAR(year) && month > 2)
		leap_day++;

	second =  (time & 0x1f) << 1;
	second += ((time >> 5) & 0x3f) * SECS_PER_MIN;
	second += (time >> 11) * SECS_PER_HOUR;
	second += (year * 365 + leap_day
		   + days_in_year[month] + day
		   + DAYS_DELTA) * SECS_PER_DAY;

	second -= tz_offset * 15 * SECS_PER_MIN;

	if (time_cs) {
		ts->tv_sec = second + (time_cs / 100);
		ts->tv_nsec = (time_cs % 100) * 10000000;
	} else {
		ts->tv_sec = second;
		ts->tv_nsec = 0;
	}
}

/* Convert linear UNIX date to a FAT time/date pair. */
void exfat_time_2exfat(struct exfat_sb_info *sbi, struct timespec *ts,
		       u32 *datetime, u8 *time_cs, s8 *tz_offset)
{
	struct tm tm;
	u16 time;
	u16 date;
	int offset;

	if (sbi->options.time_offset_set) {
		offset = -sbi->options.time_offset;
	} else
		offset = sys_tz.tz_minuteswest;

	time_to_tm(ts->tv_sec, -offset * SECS_PER_MIN, &tm);

	/*  FAT can only support year between 1980 to 2107 */
	if (tm.tm_year < 1980 - 1900) {
		time = 0;
		date = cpu_to_le16((0 << 9) | (1 << 5) | 1);
		if (time_cs)
			*time_cs = 0;
		*tz_offset = 0;
		return;
	}
	if (tm.tm_year > 2107 - 1900) {
		time = cpu_to_le16((23 << 11) | (59 << 5) | 29);
		date = cpu_to_le16((127 << 9) | (12 << 5) | 31);
		if (time_cs)
			*time_cs = 199;
		*tz_offset = 0;
		return;
	}

	/* from 1900 -> from 1980 */
	tm.tm_year -= 80;
	/* 0~11 -> 1~12 */
	tm.tm_mon++;
	/* 0~59 -> 0~29(2sec counts) */
	tm.tm_sec >>= 1;

	time = cpu_to_le16(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec);
	date = cpu_to_le16(tm.tm_year << 9 | tm.tm_mon << 5 | tm.tm_mday);

	*datetime = (date << 16) | time;

	if (time_cs)
		*time_cs = (ts->tv_sec & 1) * 100 + ts->tv_nsec / 10000000;
	*tz_offset = -offset / 15;
	*tz_offset |= (1 << 7);
}

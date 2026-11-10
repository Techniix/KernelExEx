/* Shared by w9xrt.c and w9xrt_host.c. Licensed under GPL version 2. */
/* Days from civil date, after H. Hinnant's public-domain algorithm. */
struct tm *w9x_gmtime_r(const long long *tt, struct tm *out)
{
	long long t = *tt;
	long long days = t / 86400;
	long long secs = t % 86400;
	long long z, era, doe, yoe, y, doy, mp, d, m;

	if (secs < 0)
	{
		secs += 86400;
		days -= 1;
	}
	out->tm_hour = (int)(secs / 3600);
	out->tm_min = (int)((secs % 3600) / 60);
	out->tm_sec = (int)(secs % 60);
	out->tm_wday = (int)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday */

	z = days + 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	d = doy - (153 * mp + 2) / 5 + 1;
	m = mp < 10 ? mp + 3 : mp - 9;
	y += (m <= 2);

	out->tm_year = (int)(y - 1900);
	out->tm_mon = (int)(m - 1);
	out->tm_mday = (int)d;
	{
		static const short cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
		int leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
		out->tm_yday = cum[m - 1] + (int)d - 1 + (leap && m > 2);
	}
	out->tm_isdst = 0;
	return out;
}


/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file company_info_math.h Integer helpers of the company window (company_info_gui.cpp): month buckets of the income chart. No OpenTTD types, so a stand-alone program can test them. */

#ifndef COMPANY_INFO_MATH_H
#define COMPANY_INFO_MATH_H

#include <cstdint>

namespace companyinfo {

static constexpr int DAYS_PER_MONTH = 30; ///< Same as GameTimeManager::DAYS_PER_MONTH: the game year has twelve months of 30 days (the last one holds the 5 days left).
static constexpr int MONTHS_PER_YEAR = 12;

/** The number of the game month: game year * 12 + month (0..11) of the day of the year (1..365). */
inline int32_t MonthKey(int year, int day_of_year)
{
	int m = (day_of_year - 1) / DAYS_PER_MONTH;
	if (m < 0) m = 0;
	if (m > MONTHS_PER_YEAR - 1) m = MONTHS_PER_YEAR - 1;
	return static_cast<int32_t>(year) * MONTHS_PER_YEAR + m;
}

/** The month (0..11) of a key from MonthKey. */
inline int MonthOfKey(int32_t key)
{
	return ((key % MONTHS_PER_YEAR) + MONTHS_PER_YEAR) % MONTHS_PER_YEAR;
}

/**
 * What a running total (income or costs of the quarter) gained since it was last read. The total of the game starts again at 0 when a quarter
 * closes: then the new total is the gain. A small fall (a refund) counts as 0.
 */
inline int64_t BucketDelta(int64_t now, int64_t last)
{
	const int64_t d = now - last;
	if (d >= 0) return d;
	return now < last / 2 ? now : 0;
}

/** The top of the chart: the next 1, 2 or 5 times a power of ten that holds the value (at least 1). */
inline int64_t NiceCeil(int64_t v)
{
	if (v <= 1) return 1;
	int64_t p = 1;
	while (p * 10 <= v) p *= 10;
	if (v <= p) return p;
	if (v <= 2 * p) return 2 * p;
	if (v <= 5 * p) return 5 * p;
	return 10 * p;
}

} // namespace companyinfo

#endif /* COMPANY_INFO_MATH_H */

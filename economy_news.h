/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file economy_news.h Economic news: the monthly "Transport Payrolls" report (the counterpart of Non-Farm Payrolls) and its effect on the gold price. */

#ifndef ECONOMY_NEWS_H
#define ECONOMY_NEWS_H

#include <algorithm>
#include <cstdint>
#include <deque>

/*
 * ---- Transport Payrolls ----------------------------------------------------------------------------------------------------
 * Jobs are counted in vehicles: every vehicle of every company counts as PAYROLL_JOBS_PER_VEHICLE jobs. The number of vehicles is sampled
 * once per game day; the average of a period of PAYROLL_PERIOD_DAYS days is the "employment" of the period, so a player cannot move the number
 * by buying vehicles on one day only. At the end of a period the report comes out:
 *   actual   = average of this period - average of the period before (the new jobs),
 *   forecast = mean of the last PAYROLL_FORECAST_PERIODS actual values (0 when there are none),
 *   surprise = actual - forecast.
 * The surprise, relative to the employment of the period before, moves the gold price (a strong economy lowers gold a little, a weak one raises it,
 * like real gold against Non-Farm Payrolls): through the demand pressure of the gold market, at most PAYROLL_GOLD_MAX_PPM. Silver, platinum and diamonds
 * follow gold. The shares are not moved in this version. All maths is integer and uses game state only, so every client gets the same report.
 * The first period only sets the baseline (no report, no effect).
 */
static constexpr int64_t PAYROLL_PERIOD_DAYS = 30;          ///< Days between two reports.
static constexpr int64_t PAYROLL_JOBS_PER_VEHICLE = 1000;   ///< Jobs that one vehicle stands for (display only).
static constexpr int PAYROLL_FORECAST_PERIODS = 3;          ///< The forecast is the mean of this many earlier actual values.
static constexpr int64_t PAYROLL_GOLD_DIV = 10;             ///< The surprise in ppm of the employment is divided by this to give the push on the gold demand (ppm).
static constexpr int64_t PAYROLL_GOLD_MAX_PPM = 50'000;     ///< The push on the gold demand is never more than 5%.
static constexpr size_t PAYROLL_KEEP = 24;                  ///< Reports that are kept and saved (two game years).

/** One report. Numbers in vehicles times 100 (jobs = value * PAYROLL_JOBS_PER_VEHICLE / 100). */
struct PayrollRelease {
	int64_t day = 0;          ///< Game day of the report.
	int64_t avg_x100 = 0;     ///< Average number of vehicles of the period, times 100 (the employment).
	int64_t actual_x100 = 0;  ///< Change of the average against the period before. 0 for the baseline.
	int64_t forecast_x100 = 0;///< The forecast of that change.
	int64_t gold_ppm = 0;     ///< Push on the gold demand that the surprise gave (negative: the gold price is pushed down).
	bool baseline = false;    ///< True for the first period: only the baseline, no report.

	int64_t Surprise() const { return this->actual_x100 - this->forecast_x100; }
};

/** State of the report. Saved in the 'STCK' chunk (SRK_PAYROLL). The sums of the running period are not saved: a load starts the period again. */
struct PayrollNews {
	std::deque<PayrollRelease> releases{}; ///< The reports, the newest in front, at most #PAYROLL_KEEP.
	int64_t sum = 0;                       ///< Sum of the daily vehicle counts of the running period. Not saved.
	int64_t samples = 0;                   ///< Number of daily counts in the sum. Not saved.
	int64_t last_day = -1;                 ///< Game day of the last report (-1 = none). Rebuilt from the reports.

	void Reset() { *this = PayrollNews{}; }
	void Sample();
	void Release(int64_t day);
	/** Average employment now (running period) times 100, 0 when there is no sample yet. */
	int64_t RunningAvgX100() const { return this->samples > 0 ? this->sum * 100 / this->samples : 0; }
	/** Days to the next report, counted from a game day. */
	static int64_t DaysToNext(int64_t day) { return PAYROLL_PERIOD_DAYS - day % PAYROLL_PERIOD_DAYS; }
};

extern PayrollNews _payroll_news;

/** The forecast: mean of the last #PAYROLL_FORECAST_PERIODS actual values of older reports (newest first in the list). */
inline int64_t PayrollForecastX100(const std::deque<PayrollRelease> &older)
{
	int64_t sum = 0;
	int n = 0;
	for (const PayrollRelease &r : older) {
		if (r.baseline) continue;
		sum += r.actual_x100;
		if (++n >= PAYROLL_FORECAST_PERIODS) break;
	}
	return n == 0 ? 0 : sum / n;
}

/** Push on the gold demand (ppm, negative = down) for a surprise: relative to the employment before, scaled, limited. */
inline int64_t PayrollGoldPushPpm(int64_t surprise_x100, int64_t prev_avg_x100)
{
	const int64_t base = std::max<int64_t>(prev_avg_x100, 100);
	const int64_t relative_ppm = surprise_x100 * 1'000'000 / base;
	return -std::clamp<int64_t>(relative_ppm / PAYROLL_GOLD_DIV, -PAYROLL_GOLD_MAX_PPM, PAYROLL_GOLD_MAX_PPM);
}

/** Open the news window (window number 5 of WC_CENTRAL_BANK), or bring it to the front. */
void ShowPayrollNewsWindow();
/** True while the news window is open (for the lowered state of its button). */
bool IsPayrollNewsWindowOpen();

#endif /* ECONOMY_NEWS_H */

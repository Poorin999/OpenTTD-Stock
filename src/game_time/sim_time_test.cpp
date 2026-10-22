/*
 * Test of sim_time.h: checks that the conversions and the candle-closing rules give the proportions
 * 1 sim day = 24 real minutes, 1 sim hour = 1 real minute, and 1h / 4h / 12h candles = 1 / 4 / 12 real minutes.
 *
 * Build and run on its own (this file is not part of the OpenTTD build):
 *   g++ -std=c++17 -Wall -Wextra -O1 sim_time_test.cpp -o sim_time_test && ./sim_time_test
 */

#include "sim_time.h"

#include <iostream>
#include <string>

using namespace sim_time;

namespace {

/** Counts the checks and prints one line for each. */
struct Tester {
	std::ostream &out;
	int passed = 0;
	int failed = 0;

	explicit Tester(std::ostream &out) : out(out) {}

	void Check(const char *what, int64_t expected, int64_t actual)
	{
		const bool ok = expected == actual;
		(ok ? this->passed : this->failed)++;
		this->out << (ok ? "[PASS] " : "[FAIL] ") << what << "  expected=" << expected << " actual=" << actual << '\n';
	}

	void Check(const char *what, bool condition)
	{
		(condition ? this->passed : this->failed)++;
		this->out << (condition ? "[PASS] " : "[FAIL] ") << what << '\n';
	}
};

/** Print the table of the timeframes. */
void PrintScaleTable(std::ostream &out)
{
	out << "Time scale: 1 sim day = " << REAL_MINUTES_PER_SIM_DAY << " real minutes, 1 sim hour = 1 real minute, speed x" << TIME_SCALE << "\n";
	out << "  timeframe | sim length | real length | candles per sim day\n";
	for (size_t i = 0; i < TIMEFRAME_COUNT; i++) {
		const Timeframe tf = static_cast<Timeframe>(i);
		out << "  " << std::setw(9) << GetTimeframeInfo(tf).name << " | "
			<< std::setw(7) << GetTimeframeInfo(tf).sim_hours << " h   | "
			<< std::setw(7) << TimeframeRealMs(tf) / MS_PER_SECOND << " s    | "
			<< CandlesPerSimDay(tf) << "\n";
	}
	out << '\n';
}

} // namespace

/**
 * Verify the time scales and print the result of every check.
 * @param out Where to print.
 * @return True when all checks passed.
 */
bool RunTimeScaleTests(std::ostream &out)
{
	Tester t(out);
	const Timeframe all[] = {Timeframe::H1, Timeframe::H4, Timeframe::H12};
	const int64_t expected_minutes[] = {1, 4, 12};
	const int64_t expected_per_day[] = {24, 6, 2};

	out << "--- 1. Converter between real time and simulation time ---\n";
	t.Check("1 sim day = 24 real minutes (in real ms)", 24 * 60 * 1000, SimMsToRealMs(SIM_MS_PER_DAY));
	t.Check("24 real minutes = 1 sim day (in sim ms)", SIM_MS_PER_DAY, RealMinutesToSimMs(24));
	t.Check("1 real minute = 1 sim hour (in sim ms)", SIM_MS_PER_HOUR, RealMsToSimMs(REAL_MS_PER_MINUTE));
	t.Check("1 sim hour = 1 real minute (in real ms)", REAL_MS_PER_MINUTE, SimHoursToRealMs(1));
	t.Check("1 real second = 1 sim minute", SIM_MS_PER_MINUTE, RealMsToSimMs(MS_PER_SECOND));
	t.Check("time scale is 60", 60, TIME_SCALE);
	t.Check("1 game tick = 1620 sim ms", 1620, TicksToSimMs(1));

	const int64_t round_trip[] = {0, 1, 26, 999, 1000, 60000, 239999, 1440000, 86400000, 123456789012};
	bool round_trip_ok = true;
	for (int64_t real_ms : round_trip) {
		if (SimMsToRealMs(RealMsToSimMs(real_ms)) != real_ms) round_trip_ok = false;
		if (SimMsToRealMsCeil(RealMsToSimMs(real_ms)) != real_ms) round_trip_ok = false;
	}
	t.Check("real -> sim -> real gives the same value (exact, floor and ceil)", round_trip_ok);
	t.Check("sim ms that is not a whole real ms rounds down", 0, SimMsToRealMs(59));
	t.Check("sim ms that is not a whole real ms rounds up with Ceil", 1, SimMsToRealMsCeil(59));

	const SimDateTime split = SplitSimMs(SIM_MS_PER_DAY + 4 * SIM_MS_PER_HOUR + 5 * SIM_MS_PER_MINUTE + 6 * MS_PER_SECOND);
	t.Check("split of 1 day 04:05:06 gives day 1", 1, split.day);
	t.Check("split gives hour 4", 4, split.hour);
	t.Check("split gives minute 5", 5, split.minute);
	t.Check("split gives second 6", 6, split.second);

	out << "\n--- 2. Timeframes: lengths ---\n";
	for (size_t i = 0; i < TIMEFRAME_COUNT; i++) {
		const std::string name = GetTimeframeInfo(all[i]).name;
		t.Check((name + " lasts as many real minutes as sim hours").c_str(), expected_minutes[i], TimeframeRealMs(all[i]) / REAL_MS_PER_MINUTE);
		t.Check((name + " lasts the expected number of real minutes").c_str(), expected_minutes[i] * REAL_MS_PER_MINUTE, TimeframeRealMs(all[i]));
		t.Check((name + " candles per sim day").c_str(), expected_per_day[i], CandlesPerSimDay(all[i]));
		t.Check((name + " candles fit a day exactly").c_str(), 0, SIM_MS_PER_DAY % TimeframeSimMs(all[i]));
	}

	out << "\n--- 3. Candle closing, wall clock stepped one real second at a time for 24 real minutes ---\n";
	{
		SimClock clock;
		int64_t closings[TIMEFRAME_COUNT] = {};
		int64_t first_close_real_ms[TIMEFRAME_COUNT] = {};
		bool time_of_day_ok = true;
		for (int64_t second = 1; second <= REAL_MINUTES_PER_SIM_DAY * 60; second++) {
			const int64_t prev = clock.SimMs();
			clock.AdvanceRealMs(MS_PER_SECOND);
			for (size_t i = 0; i < TIMEFRAME_COUNT; i++) {
				if (!IsCandleClosing(prev, clock.SimMs(), all[i])) continue;
				closings[i]++;
				if (first_close_real_ms[i] == 0) first_close_real_ms[i] = clock.RealMs();
				/* The candle closes exactly when the sim clock shows a multiple of the timeframe. */
				const SimDateTime now = clock.Now();
				if (now.minute != 0 || now.second != 0 || now.hour % GetTimeframeInfo(all[i]).sim_hours != 0) time_of_day_ok = false;
			}
		}
		for (size_t i = 0; i < TIMEFRAME_COUNT; i++) {
			const std::string name = GetTimeframeInfo(all[i]).name;
			t.Check((name + " closes " + std::to_string(expected_per_day[i]) + " times in 24 real minutes").c_str(), expected_per_day[i], closings[i]);
			t.Check((name + " first closes after the expected real time").c_str(), expected_minutes[i] * REAL_MS_PER_MINUTE, first_close_real_ms[i]);
		}
		t.Check("every closing happens at hh:00:00 and the hour is a multiple of the timeframe", time_of_day_ok);
		t.Check("after 24 real minutes the sim clock shows the start of day 1", clock.SimMs() == SIM_MS_PER_DAY && clock.Now().day == 1 && clock.Now().hour == 0);
	}

	out << "\n--- 4. Candle closing, driven by game ticks (27 real ms each) ---\n";
	{
		SimClock clock;
		int64_t closings[TIMEFRAME_COUNT] = {};
		int64_t worst_late_ms[TIMEFRAME_COUNT] = {};
		const int64_t ticks_for_a_day = (REAL_MS_PER_SIM_DAY + REAL_MS_PER_TICK - 1) / REAL_MS_PER_TICK; // Rounded up: the day is not a whole number of ticks.
		for (int64_t tick = 0; tick < ticks_for_a_day; tick++) {
			const int64_t prev = clock.SimMs();
			clock.AdvanceTicks(1);
			for (size_t i = 0; i < TIMEFRAME_COUNT; i++) {
				const int64_t n = CandlesClosed(prev, clock.SimMs(), all[i]);
				if (n == 0) continue;
				/* A tick never skips a candle: it is much shorter than the shortest timeframe. */
				closings[i] += n;
				const int64_t boundary_real_ms = TimeframeRealMs(all[i]) * closings[i];
				worst_late_ms[i] = std::max(worst_late_ms[i], clock.RealMs() - boundary_real_ms);
			}
		}
		for (size_t i = 0; i < TIMEFRAME_COUNT; i++) {
			const std::string name = GetTimeframeInfo(all[i]).name;
			t.Check((name + " closes the right number of times with ticks").c_str(), expected_per_day[i], closings[i]);
			t.Check((name + " is at most one tick (27 ms) late").c_str(), worst_late_ms[i] >= 0 && worst_late_ms[i] < REAL_MS_PER_TICK);
		}
	}

	out << "\n--- 5. Time jumps and the bit mask ---\n";
	t.Check("1h: jump of 13 real minutes closes 13 candles", 13, CandlesClosed(0, RealMinutesToSimMs(13), Timeframe::H1));
	t.Check("4h: jump of 13 real minutes closes 3 candles", 3, CandlesClosed(0, RealMinutesToSimMs(13), Timeframe::H4));
	t.Check("12h: jump of 13 real minutes closes 1 candle", 1, CandlesClosed(0, RealMinutesToSimMs(13), Timeframe::H12));
	t.Check("no time passes: nothing closes", 0, CandlesClosed(RealMinutesToSimMs(4), RealMinutesToSimMs(4), Timeframe::H1));
	t.Check("one ms before the boundary: nothing closes", 0, CandlesClosed(0, RealMsToSimMs(REAL_MS_PER_MINUTE - 1), Timeframe::H1));
	t.Check("exactly at the boundary: the 1h candle closes", 1, CandlesClosed(0, RealMsToSimMs(REAL_MS_PER_MINUTE), Timeframe::H1));

	const int64_t minute_marks[] = {1, 2, 3, 4, 8, 12, 24};
	const int expected_mask[] = {0b001, 0b001, 0b001, 0b011, 0b011, 0b111, 0b111};
	for (size_t i = 0; i < sizeof(minute_marks) / sizeof(minute_marks[0]); i++) {
		const int64_t now = RealMinutesToSimMs(minute_marks[i]);
		const int64_t prev = now - 1;
		const std::string what = "mask of the timeframes that close at real minute " + std::to_string(minute_marks[i]);
		t.Check(what.c_str(), expected_mask[i], TimeframesClosing(prev, now));
	}

	out << "\n--- 6. Aggregation: one price sample per real second (= per sim minute), price = sim minute number ---\n";
	{
		MultiTimeframeCandles candles;
		std::array<size_t, TIMEFRAME_COUNT> total_closed{};
		for (int64_t minute = 0; minute <= HOURS_PER_DAY * MINUTES_PER_HOUR; minute++) { // One sample more, it closes the last candles.
			const std::array<size_t, TIMEFRAME_COUNT> closed = candles.Update(minute * SIM_MS_PER_MINUTE, minute);
			for (size_t i = 0; i < TIMEFRAME_COUNT; i++) total_closed[i] += closed[i];
		}
		for (size_t i = 0; i < TIMEFRAME_COUNT; i++) {
			const std::string name = GetTimeframeInfo(all[i]).name;
			t.Check((name + " closed candles after one sim day").c_str(), expected_per_day[i], static_cast<int64_t>(candles.Get(all[i]).Closed().size()));
			t.Check((name + " closed count reported by Update matches").c_str(), expected_per_day[i], static_cast<int64_t>(total_closed[i]));
		}
		const Candle &h1 = candles.Get(Timeframe::H1).Closed()[1];
		t.Check("1h candle 1: open", 60, h1.open);
		t.Check("1h candle 1: high", 119, h1.high);
		t.Check("1h candle 1: low", 60, h1.low);
		t.Check("1h candle 1: close", 119, h1.close);
		t.Check("1h candle 1: 60 samples", 60, h1.samples);
		const Candle &h4 = candles.Get(Timeframe::H4).Closed()[1];
		t.Check("4h candle 1: open", 240, h4.open);
		t.Check("4h candle 1: close", 479, h4.close);
		t.Check("4h candle 1: 240 samples", 240, h4.samples);
		const Candle &h12 = candles.Get(Timeframe::H12).Closed()[1];
		t.Check("12h candle 1: open", 720, h12.open);
		t.Check("12h candle 1: close", 1439, h12.close);
		t.Check("12h candle 1: 720 samples", 720, h12.samples);
		t.Check("12h candle 1 closes at sim 24:00", SIM_MS_PER_DAY, h12.close_sim_ms);
	}

	out << "\n--- 7. Aggregation when the clock jumps over candles ---\n";
	{
		CandleSeries series(Timeframe::H1);
		series.Update(0, 100);
		const size_t closed = series.Update(3 * SIM_MS_PER_HOUR + 5 * SIM_MS_PER_MINUTE, 105); // 03:05
		t.Check("a sample at 03:05 closes three 1h candles", 3, static_cast<int64_t>(closed));
		t.Check("the skipped candles have no samples", series.Closed()[1].samples == 0 && series.Closed()[2].samples == 0);
		t.Check("the skipped candles are flat at the last close", series.Closed()[1].open == 100 && series.Closed()[1].high == 100
				&& series.Closed()[2].low == 100 && series.Closed()[2].close == 100);
		t.Check("the running candle is number 3", 3, series.Running().index);
		t.Check("the running candle opens with the new price", 105, series.Running().open);
		t.Check("an old sample is ignored", 0, static_cast<int64_t>(series.Update(SIM_MS_PER_HOUR, 1)));
		t.Check("the old sample did not change the running candle", 105, series.Running().close);
	}

	out << "\nResult: " << t.passed << " passed, " << t.failed << " failed\n";
	return t.failed == 0;
}

int main()
{
	PrintScaleTable(std::cout);
	const bool ok = RunTimeScaleTests(std::cout);
	std::cout << (ok ? "ALL TIME SCALES MATCH\n" : "TIME SCALE MISMATCH\n");
	return ok ? 0 : 1;
}

/*
 * Test of the game clock (game_time_manager) and of the timeframe aggregator (game_time_aggregator.h).
 * It needs no part of OpenTTD. The two headers that game_time_manager.cpp takes from OpenTTD (stdafx.h and
 * safeguards.h) are empty here, see the build line:
 *
 *   mkdir -p /tmp/gt/src/game_time && touch /tmp/gt/src/stdafx.h /tmp/gt/src/safeguards.h
 *   cp game_time_manager.* game_time_aggregator.h sim_time.h game_time_test.cpp /tmp/gt/src/game_time/
 *   g++ -std=c++17 -Wall -Wextra -O1 /tmp/gt/src/game_time/game_time_manager.cpp /tmp/gt/src/game_time/game_time_test.cpp -o /tmp/gt/test && /tmp/gt/test
 *
 * sim_time.h (the older, independent model of the same scale) is used as a cross-check for the candles.
 */

#include "game_time_manager.h"
#include "game_time_aggregator.h"
#include "sim_time.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

int _passed = 0;
int _failed = 0;

void Check(const std::string &what, bool ok)
{
	(ok ? _passed : _failed)++;
	if (!ok) std::cout << "[FAIL] " << what << '\n';
}

template <typename A, typename B>
void CheckEq(const std::string &what, const A &expected, const B &actual)
{
	const bool ok = static_cast<int64_t>(expected) == static_cast<int64_t>(actual);
	(ok ? _passed : _failed)++;
	if (!ok) std::cout << "[FAIL] " << what << "  expected=" << expected << " actual=" << actual << '\n';
}

constexpr uint64_t MIN_MS = GameTimeManager::MS_PER_MINUTE;        // 1,000
constexpr uint64_t HOUR_MS = MIN_MS * 60;                          // 60,000
constexpr uint64_t DAY_MS = HOUR_MS * 24;                          // 1,440,000

/** Counts the events of all kinds while it lives. */
struct Counter {
	std::array<uint64_t, static_cast<size_t>(GameTimeEvent::End)> n{};
	std::vector<GameTimeEvent> order;
	GameTimeSubscription m{GameTimeEvent::Minute, [this](const GameTimeSnapshot &) { Hit(GameTimeEvent::Minute); }};
	GameTimeSubscription h{GameTimeEvent::Hour, [this](const GameTimeSnapshot &) { Hit(GameTimeEvent::Hour); }};
	GameTimeSubscription h4{GameTimeEvent::Hour4, [this](const GameTimeSnapshot &) { Hit(GameTimeEvent::Hour4); }};
	GameTimeSubscription h12{GameTimeEvent::Hour12, [this](const GameTimeSnapshot &) { Hit(GameTimeEvent::Hour12); }};
	GameTimeSubscription d{GameTimeEvent::Day, [this](const GameTimeSnapshot &) { Hit(GameTimeEvent::Day); }};
	void Hit(GameTimeEvent e) { n[static_cast<size_t>(e)]++; order.push_back(e); }
	uint64_t Get(GameTimeEvent e) const { return n[static_cast<size_t>(e)]; }
};

GameTimeManager &Clock() { return GameTimeManager::Get(); }

void Fresh()
{
	Clock().Reset();
	Clock().SetStartYear(GameTimeManager::DEFAULT_START_YEAR);
}

void TestScale()
{
	Fresh();
	auto t = Clock().GetSnapshot();
	CheckEq("start: minute", 0, t.minute);
	CheckEq("start: hour", 0, t.hour);
	CheckEq("start: day of year", 1, t.day);
	CheckEq("start: year", 1950, t.year);

	Clock().AdvanceMilliseconds(999);
	CheckEq("999 ms: still minute 0", 0, Clock().GetCurrentGameMinute());
	Clock().AdvanceMilliseconds(1);
	CheckEq("1,000 ms = 1 game minute", 1, Clock().GetCurrentGameMinute());

	Fresh();
	Clock().AdvanceMilliseconds(60 * MIN_MS);
	CheckEq("60 real seconds = 1 game hour", 1, Clock().GetCurrentGameHour());
	CheckEq("... and minute back at 0", 0, Clock().GetCurrentGameMinute());
	CheckEq("total hours", 1, Clock().GetSnapshot().total_hours);

	Fresh();
	Clock().AdvanceMilliseconds(24 * 60 * MIN_MS);
	CheckEq("24 real minutes = 1 game day (day of year 2)", 2, Clock().GetCurrentGameDay());
	CheckEq("... hour 0", 0, Clock().GetCurrentGameHour());
	CheckEq("1,440 game minutes in a day", 1440, GameTimeManager::MINUTES_PER_DAY);

	Fresh();
	Clock().AdvanceMilliseconds(365 * DAY_MS);
	CheckEq("365 days = next year", 1951, Clock().GetCurrentGameYear());
	CheckEq("... day of year 1", 1, Clock().GetCurrentGameDay());

	Fresh();
	Clock().AdvanceMilliseconds(3 * DAY_MS + 5 * HOUR_MS + 7 * MIN_MS + 250);
	t = Clock().GetSnapshot();
	CheckEq("mixed: day", 4, t.day);
	CheckEq("mixed: hour", 5, t.hour);
	CheckEq("mixed: minute", 7, t.minute);
	CheckEq("mixed: total ms keeps the fraction", 3 * DAY_MS + 5 * HOUR_MS + 7 * MIN_MS + 250, Clock().GetTotalMilliseconds());
}

void TestEventCounts()
{
	/* Over one game day, with steps of different sizes, the counts are always the same. */
	for (const uint64_t step : {uint64_t{1}, uint64_t{30}, uint64_t{1000}, uint64_t{7919}, DAY_MS}) {
		Fresh();
		Counter c;
		uint64_t done = 0;
		while (done < DAY_MS) {
			const uint64_t s = std::min(step, DAY_MS - done);
			Clock().AdvanceMilliseconds(s);
			done += s;
		}
		const std::string tag = " (step " + std::to_string(step) + " ms)";
		CheckEq("minute events per day" + tag, 1440, c.Get(GameTimeEvent::Minute));
		CheckEq("hour events per day" + tag, 24, c.Get(GameTimeEvent::Hour));
		CheckEq("4h events per day" + tag, 6, c.Get(GameTimeEvent::Hour4));
		CheckEq("12h events per day" + tag, 2, c.Get(GameTimeEvent::Hour12));
		CheckEq("day events per day" + tag, 1, c.Get(GameTimeEvent::Day));
	}

	/* No event before the boundary, one event on it. */
	Fresh();
	Counter c;
	Clock().AdvanceMilliseconds(HOUR_MS - 1);
	CheckEq("1 ms before the hour: 59 minute events", 59, c.Get(GameTimeEvent::Minute));
	CheckEq("1 ms before the hour: no hour event", 0, c.Get(GameTimeEvent::Hour));
	Clock().AdvanceMilliseconds(1);
	CheckEq("on the hour: hour event", 1, c.Get(GameTimeEvent::Hour));
	CheckEq("on the hour: no 4h event yet", 0, c.Get(GameTimeEvent::Hour4));

	/* Paused game: no ticks, no time. */
	Clock().AdvanceMilliseconds(0);
	CheckEq("advance by 0 changes nothing", 60, c.Get(GameTimeEvent::Minute));
}

void TestBoundariesAndOrder()
{
	Fresh();
	std::vector<std::pair<int, std::string>> log; // hour, event name at each boundary of an hour
	auto name = [](GameTimeEvent e) { return std::string(e == GameTimeEvent::Hour ? "1h" : e == GameTimeEvent::Hour4 ? "4h" : e == GameTimeEvent::Hour12 ? "12h" : e == GameTimeEvent::Day ? "day" : "min"); };
	std::vector<std::unique_ptr<GameTimeSubscription>> subs;
	for (GameTimeEvent e : {GameTimeEvent::Hour, GameTimeEvent::Hour4, GameTimeEvent::Hour12, GameTimeEvent::Day}) {
		subs.push_back(std::make_unique<GameTimeSubscription>(e, [&log, name, e](const GameTimeSnapshot &now) {
			CheckEq("snapshot at an hour boundary has minute 0", 0, now.minute);
			log.push_back({static_cast<int>(now.total_hours), name(e)});
		}));
	}
	Clock().AdvanceMilliseconds(DAY_MS); // One call over many boundaries: all are sent, in time order.

	std::vector<std::pair<int, std::string>> want;
	for (int h = 1; h <= 24; h++) {
		want.push_back({h, "1h"});
		if (h % 4 == 0) want.push_back({h, "4h"});
		if (h % 12 == 0) want.push_back({h, "12h"});
		if (h % 24 == 0) want.push_back({h, "day"});
	}
	Check("one big step sends every boundary in order: 1h, then 4h, then 12h, then day", log == want);
}

void TestPriorityAndReentrancy()
{
	Fresh();
	std::string order;
	GameTimeSubscription a(GameTimeEvent::Minute, [&](const GameTimeSnapshot &) { order += 'a'; }, 10);
	GameTimeSubscription b(GameTimeEvent::Minute, [&](const GameTimeSnapshot &) { order += 'b'; }, -5);
	GameTimeSubscription c(GameTimeEvent::Minute, [&](const GameTimeSnapshot &) { order += 'c'; }, 10);
	Clock().AdvanceMilliseconds(MIN_MS);
	Check("lower priority first, equal priority in order of subscription", order == "bac");

	/* A callback that moves the clock: the time is added after the running events are done. */
	Fresh();
	int calls = 0;
	{
		GameTimeSubscription re(GameTimeEvent::Minute, [&](const GameTimeSnapshot &) {
			if (calls++ == 0) Clock().AdvanceMilliseconds(2 * MIN_MS);
		});
		Clock().AdvanceMilliseconds(MIN_MS);
	}
	CheckEq("callback that advances the clock: its time is not lost", 3, Clock().GetSnapshot().total_minutes);
	CheckEq("... and the events of that time were sent", 3, calls);

	/* Unsubscribe by destroying the object. */
	Fresh();
	int hits = 0;
	const size_t before = Clock().GetSubscriberCount(GameTimeEvent::Minute); // a, b and c are still alive
	{
		GameTimeSubscription s(GameTimeEvent::Minute, [&](const GameTimeSnapshot &) { hits++; });
		Clock().AdvanceMilliseconds(MIN_MS);
	}
	Clock().AdvanceMilliseconds(MIN_MS);
	CheckEq("no call after the subscription is destroyed", 1, hits);
	CheckEq("subscriber list is back to what it was", before, Clock().GetSubscriberCount(GameTimeEvent::Minute));
}

void TestCountdownAndIndex()
{
	Fresh();
	Clock().AdvanceMilliseconds(5 * HOUR_MS + 20 * MIN_MS);
	CheckEq("candle index 1h", 5, Clock().GetCandleIndex(GameTimeframe::H1));
	CheckEq("candle index 4h", 1, Clock().GetCandleIndex(GameTimeframe::H4));
	CheckEq("candle index 12h", 0, Clock().GetCandleIndex(GameTimeframe::H12));
	CheckEq("ms to next hour", 40 * MIN_MS, Clock().GetMillisecondsUntil(GameTimeEvent::Hour));
	CheckEq("ms to next 4h (at hour 8)", 2 * HOUR_MS + 40 * MIN_MS, Clock().GetMillisecondsUntil(GameTimeEvent::Hour4));
	CheckEq("ms to next 12h (at hour 12)", 6 * HOUR_MS + 40 * MIN_MS, Clock().GetMillisecondsUntil(GameTimeEvent::Hour12));
	Fresh();
	Clock().AdvanceMilliseconds(4 * HOUR_MS);
	CheckEq("exactly on a boundary: a full period to the next", 4 * HOUR_MS, Clock().GetMillisecondsUntil(GameTimeEvent::Hour4));
}

void TestSaveState()
{
	Fresh();
	Clock().SetStartYear(1960);
	Clock().AdvanceMilliseconds(DAY_MS + 3 * HOUR_MS + 30 * MIN_MS + 123);
	const GameTimeSaveState saved = Clock().GetSaveState();

	Fresh();
	Counter c;
	Clock().LoadSaveState(saved);
	CheckEq("loaded: total ms", saved.total_ms, Clock().GetTotalMilliseconds());
	CheckEq("loaded: start year", 1960, Clock().GetCurrentGameYear());
	CheckEq("loaded: hour", 3, Clock().GetCurrentGameHour());
	CheckEq("loaded: no events for the time that has passed", 0, c.Get(GameTimeEvent::Minute) + c.Get(GameTimeEvent::Hour) + c.Get(GameTimeEvent::Day));

	/* After loading, the next boundary comes at the right moment: the clock is at 03:30:00.123, so 04:00 is 29,877 ms away. */
	Clock().AdvanceMilliseconds(29 * MIN_MS + 876);
	CheckEq("1 ms before the hour: no hour event", 0, c.Get(GameTimeEvent::Hour));
	Clock().AdvanceMilliseconds(1);
	CheckEq("on the hour: hour event", 1, c.Get(GameTimeEvent::Hour));
	CheckEq("4h event at hour 4 of day 2 (04:00)", 1, c.Get(GameTimeEvent::Hour4));

	/* A start year that is out of range is ignored. */
	GameTimeSaveState bad = saved;
	bad.start_year = 0;
	Clock().LoadSaveState(bad);
	CheckEq("start year 0 is ignored", 1960, Clock().GetSaveState().start_year);
}

/** The cross-check: the same samples in the aggregator and in the independent model sim_time::CandleSeries. */
void TestAggregatorAgainstSimTime()
{
	Fresh();
	TimeframeAggregator agg;
	std::array<sim_time::CandleSeries, 3> model{sim_time::CandleSeries(sim_time::Timeframe::H1), sim_time::CandleSeries(sim_time::Timeframe::H4), sim_time::CandleSeries(sim_time::Timeframe::H12)};

	auto sample = [&](int64_t price) {
		agg.AddSample(price);
		const int64_t sim_ms = sim_time::RealMsToSimMs(static_cast<int64_t>(Clock().GetTotalMilliseconds()));
		for (auto &m : model) m.Update(sim_ms, price);
	};

	/* A price every 7.3 game minutes for 3 game days, with a gap of 20 hours in the middle (no sample). */
	int64_t price = 1000;
	uint64_t t = 0;
	uint32_t seed = 12345;
	while (t < 3 * DAY_MS) {
		seed = seed * 1664525u + 1013904223u;
		price += static_cast<int64_t>((seed >> 16) % 41) - 20;
		sample(price);
		const uint64_t step = 7300;
		Clock().AdvanceMilliseconds(step);
		t += step;
		if (t > DAY_MS && t < DAY_MS + 8000) { Clock().AdvanceMilliseconds(20 * HOUR_MS); t += 20 * HOUR_MS; }
	}
	sample(price + 1); // The sample in the newest bucket makes the model close what the clock already closed.

	for (size_t i = 0; i < 3; i++) {
		const GameTimeframe tf = static_cast<GameTimeframe>(i);
		const auto &a = agg.Closed(tf);
		const auto &m = model[i].Closed();
		bool same = a.size() == m.size();
		for (size_t k = 0; same && k < a.size(); k++) {
			same = static_cast<int64_t>(a[k].index) == m[k].index && a[k].open == m[k].open && a[k].high == m[k].high &&
					a[k].low == m[k].low && a[k].close == m[k].close && a[k].samples == static_cast<uint32_t>(m[k].samples);
		}
		Check(std::string("aggregator = sim_time candles, timeframe ") + GameTimeframeName(tf), same && !a.empty());
	}
}

void TestAggregatorBasics()
{
	Fresh();
	TimeframeAggregator agg;
	std::vector<std::pair<GameTimeframe, TimeframeBucket>> closed;
	/* The 5m timeframe closes a candle every 5 game minutes; it has its own test (TestFiveMinuteTimeframe). */
	agg.SetCloseCallback([&](GameTimeframe tf, const TimeframeBucket &b) { if (tf != GameTimeframe::M5) closed.push_back({tf, b}); });

	Check("no running bucket before the first sample", !agg.HasRunning(GameTimeframe::H1));
	Clock().AdvanceMilliseconds(3 * HOUR_MS);
	CheckEq("no buckets close when nothing was sampled", 0, closed.size());

	/* Hour 3: 10, 14, 8, 12 -> OHLC 10/14/8/12. */
	for (int64_t p : {10, 14, 8, 12}) { agg.AddSample(p); Clock().AdvanceMilliseconds(MIN_MS); }
	Check("running bucket exists", agg.HasRunning(GameTimeframe::H1));
	const TimeframeBucket &r = agg.Running(GameTimeframe::H1);
	CheckEq("running index (hour 3)", 3, r.index);
	CheckEq("open", 10, r.open); CheckEq("high", 14, r.high); CheckEq("low", 8, r.low); CheckEq("close", 12, r.close); CheckEq("samples", 4, r.samples);
	CheckEq("nothing closed yet", 0, closed.size());

	Clock().AdvanceMilliseconds(56 * MIN_MS); // To hour 4:00 exactly.
	CheckEq("on 04:00 the 1h and the 4h bucket close, by the events, without a new sample", 2, closed.size());
	Check("first the 1h timeframe", closed.at(0).first == GameTimeframe::H1);
	CheckEq("closed index", 3, closed.at(0).second.index);
	CheckEq("closed close", 12, closed.at(0).second.close);
	Check("second close is the 4h bucket", closed.at(1).first == GameTimeframe::H4);
	CheckEq("4h bucket 0 has all 4 samples", 4, closed.at(1).second.samples);

	/* Three empty hours: flat buckets at the last close. */
	closed.clear();
	Clock().AdvanceMilliseconds(3 * HOUR_MS);
	size_t flat = 0;
	for (auto &c : closed) if (c.first == GameTimeframe::H1 && c.second.samples == 0 && c.second.open == 12 && c.second.high == 12 && c.second.low == 12 && c.second.close == 12) flat++;
	CheckEq("3 empty hours give 3 flat 1h buckets at the last close", 3, flat);
	CheckEq("1h buckets 3..6 are closed without holes", 4, agg.Closed(GameTimeframe::H1).size());
	CheckEq("... the newest is 6", 6, agg.Closed(GameTimeframe::H1).back().index);
	CheckEq("running 1h bucket is hour 7", 7, agg.Running(GameTimeframe::H1).index);
}

void TestAggregatorSaveLoad()
{
	/* Run A without a break; run B is saved and restored in the middle. They must end equal. */
	auto feed = [](TimeframeAggregator &agg, uint64_t from, uint64_t to, uint32_t &seed, int64_t &price) {
		for (uint64_t t = from; t < to; t += 9000) {
			seed = seed * 1664525u + 1013904223u;
			price += static_cast<int64_t>((seed >> 16) % 21) - 10;
			agg.AddSample(price);
			if ((seed >> 8) % 3 == 0) agg.AddVolume(1 + (seed >> 20) % 50);
			Clock().AdvanceMilliseconds(9000);
		}
	};

	Fresh();
	TimeframeAggregator a;
	{ uint32_t seed = 7; int64_t price = 500; feed(a, 0, 2 * DAY_MS, seed, price); }
	const auto end_a = a.Export();

	Fresh();
	uint32_t seed = 7;
	int64_t price = 500;
	GameTimeSaveState clock_state;
	std::vector<TimeframeRecord> records;
	{
		TimeframeAggregator first;
		feed(first, 0, DAY_MS + 5 * HOUR_MS + 1234, seed, price);
		clock_state = Clock().GetSaveState();
		records = first.Export();
	}
	/* "Load": a new aggregator and a restored clock. */
	Fresh();
	TimeframeAggregator b;
	Clock().LoadSaveState(clock_state);
	Check("import of a valid state succeeds", b.Import(records));
	Check("export after import gives the same records", b.Export().size() == records.size());
	const uint64_t from = clock_state.total_ms;
	/* Continue exactly where the first part stopped (the generator state carries over). */
	for (uint64_t t = from; t < 2 * DAY_MS + 9000; t += 9000) {
		if (Clock().GetTotalMilliseconds() >= 2 * DAY_MS) break;
		seed = seed * 1664525u + 1013904223u;
		price += static_cast<int64_t>((seed >> 16) % 21) - 10;
		b.AddSample(price);
		if ((seed >> 8) % 3 == 0) b.AddVolume(1 + (seed >> 20) % 50);
		Clock().AdvanceMilliseconds(9000);
	}
	const auto end_b = b.Export();
	bool same = end_a.size() == end_b.size();
	for (size_t i = 0; same && i < end_a.size(); i++) {
		same = end_a[i].timeframe == end_b[i].timeframe && end_a[i].running == end_b[i].running && end_a[i].bucket == end_b[i].bucket;
	}
	Check("save + load in the middle gives the same result as no break", same);

	/* Invalid data is rejected and leaves a clean state. */
	TimeframeAggregator c;
	std::vector<TimeframeRecord> bad = records;
	std::swap(bad.front(), bad.back());
	Check("records out of order are rejected", !c.Import(bad));
	Check("... and the state is empty", c.Export().empty());
	bad = records;
	bad.front().timeframe = 9;
	Check("unknown timeframe is rejected", !c.Import(bad));
	bad = records;
	for (auto &r : bad) if (r.bucket.samples > 0) { r.bucket.high = r.bucket.low - 1; break; }
	Check("high below low is rejected", !c.Import(bad));
	bad = records;
	for (auto &r : bad) if (r.running) { bad.push_back(r); break; }
	Check("two running buckets of a timeframe are rejected", !c.Import(bad));

	/* A state that is older than the clock: the next event closes the missing buckets flat. */
	Fresh();
	TimeframeAggregator d;
	d.AddSample(100);
	std::vector<TimeframeRecord> old = d.Export();
	Fresh();
	TimeframeAggregator e;
	Clock().LoadSaveState({5 * HOUR_MS, 1950});
	Check("old state imports", e.Import(old));
	e.AddSample(120);
	CheckEq("1h: buckets 0..4 are closed flat, then the sample goes in bucket 5", 5, e.Closed(GameTimeframe::H1).size());
	CheckEq("1h running index", 5, e.Running(GameTimeframe::H1).index);
	CheckEq("1h bucket 1 is flat at the old close", 100, e.Closed(GameTimeframe::H1)[1].close);
	CheckEq("4h: bucket 0 closed", 1, e.Closed(GameTimeframe::H4).size());

	/* The cap on the number of closed buckets. */
	Fresh();
	TimeframeAggregator f(10);
	f.AddSample(1);
	Clock().AdvanceMilliseconds(50 * HOUR_MS);
	CheckEq("only 10 closed 1h buckets are kept", 10, f.Closed(GameTimeframe::H1).size());
	CheckEq("... the newest ones", 49, f.Closed(GameTimeframe::H1).back().index);
}

void TestAggregatorVolumeAndArray()
{
	Fresh();
	/* An array of aggregators (one per company in the game) must be default constructible. */
	std::array<TimeframeAggregator, 3> many;
	many[0].AddVolume(5); // No sample yet: ignored.
	Check("volume before the first sample is ignored", !many[0].HasRunning(GameTimeframe::H1));

	many[1].AddSample(100);
	many[1].AddVolume(10);
	many[1].AddVolume(-4); // Ignored.
	many[1].AddVolume(0);  // Ignored.
	Clock().AdvanceMilliseconds(30 * MIN_MS);
	many[1].AddVolume(7);
	CheckEq("volume of the running bucket adds up", 17, many[1].Running(GameTimeframe::H1).volume);
	CheckEq("... in all timeframes", 17, many[1].Running(GameTimeframe::H12).volume);

	Clock().AdvanceMilliseconds(30 * MIN_MS); // 01:00: the 1h bucket closes.
	many[1].AddVolume(3);
	CheckEq("closed bucket keeps its volume", 17, many[1].Closed(GameTimeframe::H1).back().volume);
	CheckEq("the next bucket starts at 0 and counts on", 3, many[1].Running(GameTimeframe::H1).volume);
	CheckEq("the 4h bucket still collects", 20, many[1].Running(GameTimeframe::H4).volume);

	/* Volume goes through Export / Import; a negative volume is rejected. */
	auto rec = many[1].Export();
	TimeframeAggregator copy;
	Check("import with volume", copy.Import(rec));
	CheckEq("volume survives export and import", 20, copy.Running(GameTimeframe::H4).volume);
	for (auto &r : rec) { r.bucket.volume = -1; break; }
	Check("negative volume is rejected", !copy.Import(rec));
}

} // namespace

/** The day, week, month and year timeframes: one event per period, aligned to game day 0, and candles of the right length. */
void TestLongTimeframes()
{
	Fresh();
	Counter c;
	GameTimeSubscription week{GameTimeEvent::Week, [&c](const GameTimeSnapshot &) { c.Hit(GameTimeEvent::Week); }};
	GameTimeSubscription month{GameTimeEvent::Month, [&c](const GameTimeSnapshot &) { c.Hit(GameTimeEvent::Month); }};
	GameTimeSubscription year{GameTimeEvent::Year, [&c](const GameTimeSnapshot &) { c.Hit(GameTimeEvent::Year); }};
	Clock().AdvanceMilliseconds(DAY_MS * 365 * 2);
	CheckEq("week events in 2 years", 730 / 7, c.Get(GameTimeEvent::Week));
	CheckEq("month events in 2 years", 730 / 30, c.Get(GameTimeEvent::Month));
	CheckEq("year events in 2 years", 2, c.Get(GameTimeEvent::Year));
	CheckEq("hours of the day timeframe", 24, GameTimeframeHours(GameTimeframe::D1));
	CheckEq("hours of the week timeframe", 24 * 7, GameTimeframeHours(GameTimeframe::W1));
	CheckEq("hours of the month timeframe", 24 * 30, GameTimeframeHours(GameTimeframe::MN));
	CheckEq("hours of the year timeframe", 24 * 365, GameTimeframeHours(GameTimeframe::Y1));

	Fresh();
	TimeframeAggregator agg;
	agg.AddSample(100);
	Clock().AdvanceMilliseconds(DAY_MS * 3);
	agg.AddSample(120);
	Clock().AdvanceMilliseconds(DAY_MS * 5); // Day 8: the first week (days 0..6) is closed.
	agg.AddSample(90);
	Check("a closed week candle exists", agg.Closed(GameTimeframe::W1).size() == 1);
	Check("the week candle has the highest and the lowest price of its days", agg.Closed(GameTimeframe::W1).size() == 1 &&
			agg.Closed(GameTimeframe::W1)[0].high == 120 && agg.Closed(GameTimeframe::W1)[0].low == 100);
	CheckEq("closed day candles after 8 days", 8, static_cast<int>(agg.Closed(GameTimeframe::D1).size()));
	Check("no month candle yet", agg.Closed(GameTimeframe::MN).empty() && agg.HasRunning(GameTimeframe::MN));
}

/** The original calendar follows the clock: a new calendar day starts exactly at 00:00, and the fraction of the day follows HH:MM. */
void TestCalendarSync()
{
	Fresh();
	CheckEq("no calendar day at the start", 0, Clock().TryConsumeCalendarDay());
	CheckEq("day fraction at 00:00", 0, Clock().GetDayFraction(74));

	Clock().AdvanceMilliseconds(DAY_MS - 1);
	CheckEq("no new calendar day at 23:59:59.999", 0, Clock().TryConsumeCalendarDay());
	CheckEq("day fraction just before midnight", 73, Clock().GetDayFraction(74));
	Clock().AdvanceMilliseconds(1);
	CheckEq("new calendar day at 00:00", 1, Clock().TryConsumeCalendarDay());
	CheckEq("only one new day per midnight", 0, Clock().TryConsumeCalendarDay());
	CheckEq("day fraction after midnight", 0, Clock().GetDayFraction(74));

	Clock().AdvanceMilliseconds(DAY_MS / 2); // 12:00
	CheckEq("day fraction at 12:00", 37, Clock().GetDayFraction(74));
	CheckEq("no calendar day at noon", 0, Clock().TryConsumeCalendarDay());

	/* Fast-forward over 3 midnights: one day per call, none lost. */
	Clock().AdvanceMilliseconds(DAY_MS * 3);
	int days = 0;
	while (Clock().TryConsumeCalendarDay()) days++;
	CheckEq("3 midnights give 3 calendar days", 3, days);
	CheckEq("consumed days equal the day of the clock", Clock().GetCurrentDayIndex(), Clock().GetCalendarDaysConsumed());

	/* Loading a clock never makes the calendar jump. */
	GameTimeSaveState st;
	st.total_ms = DAY_MS * 400 + HOUR_MS * 5;
	Clock().LoadSaveState(st);
	CheckEq("no calendar day after loading", 0, Clock().TryConsumeCalendarDay());
	Clock().Restore(DAY_MS * 7);
	CheckEq("no calendar day after Restore", 0, Clock().TryConsumeCalendarDay());
	Clock().Reset();
	CheckEq("no calendar day after Reset", 0, Clock().TryConsumeCalendarDay());
	CheckEq("zero ticks per day is safe", 0, Clock().GetDayFraction(0));

	/* One calendar day is exactly 24 real minutes of ticks: the 74 steps of the day are all visited, in order. */
	Fresh();
	uint32_t last = 0;
	bool in_order = true;
	std::vector<bool> seen(74, false);
	for (uint64_t ms = 0; ms < DAY_MS; ms += 27) { // one tick of OpenTTD 15.3 is 27 ms
		Clock().AdvanceMilliseconds(ms == 0 ? 0 : 27);
		const uint32_t f = Clock().GetDayFraction(74);
		if (f < last) in_order = false;
		last = f;
		seen[f] = true;
	}
	Check("day fraction never goes back inside a day", in_order);
	Check("all 74 steps of the day are visited", std::all_of(seen.begin(), seen.end(), [](bool b) { return b; }));
}

/** The 5 minute timeframe: an event every 5 game minutes (5 real seconds), candles numbered from minute 0, and the old timeframes keep their numbers. */
void TestFiveMinuteTimeframe()
{
	Fresh();
	CheckEq("5m is 5 game minutes", 5, GameTimeframeMinutes(GameTimeframe::M5));
	CheckEq("5m has no whole hours", 0, GameTimeframeHours(GameTimeframe::M5));
	CheckEq("1h is 60 game minutes", 60, GameTimeframeMinutes(GameTimeframe::H1));
	CheckEq("12h is 720 game minutes", 720, GameTimeframeMinutes(GameTimeframe::H12));
	CheckEq("the numbers of the old timeframes did not move (savegames)", 0, static_cast<int>(GameTimeframe::H1));
	CheckEq("... 4h", 1, static_cast<int>(GameTimeframe::H4));
	CheckEq("... 12h", 2, static_cast<int>(GameTimeframe::H12));
	CheckEq("... year", 6, static_cast<int>(GameTimeframe::Y1));
	CheckEq("5m is the last one", 7, static_cast<int>(GameTimeframe::M5));
	Check("5m listens to its own event", GameTimeframeEvent(GameTimeframe::M5) == GameTimeEvent::Minute5);

	/* Events: 12 per game hour, and at a full hour the 5m event comes right after the minute event and before the hour event. */
	Counter c;
	GameTimeSubscription five{GameTimeEvent::Minute5, [&c](const GameTimeSnapshot &) { c.Hit(GameTimeEvent::Minute5); }};
	Clock().AdvanceMilliseconds(HOUR_MS * 2);
	CheckEq("12 five-minute events per game hour", 24, c.Get(GameTimeEvent::Minute5));
	CheckEq("... 120 minute events", 120, c.Get(GameTimeEvent::Minute));
	CheckEq("... 2 hour events", 2, c.Get(GameTimeEvent::Hour));
	size_t h = 0;
	for (size_t i = 0; i + 1 < c.order.size(); i++) {
		if (c.order[i] == GameTimeEvent::Hour) { h = i; break; }
	}
	Check("order at a full hour: minute, 5 minutes, hour", h >= 2 && c.order[h - 1] == GameTimeEvent::Minute5 && c.order[h - 2] == GameTimeEvent::Minute);
	CheckEq("period of the 5m event", 5000, GameTimeManager::EventPeriodMs(GameTimeEvent::Minute5));

	/* Countdown and candle index. */
	Fresh();
	Clock().AdvanceMilliseconds(3 * MIN_MS + 250);
	CheckEq("5m candle index at 00:03", 0, Clock().GetCandleIndex(GameTimeframe::M5));
	CheckEq("countdown to the next 5m candle", 5 * MIN_MS - (3 * MIN_MS + 250), Clock().GetMillisecondsUntil(GameTimeEvent::Minute5));
	Clock().AdvanceMilliseconds(2 * MIN_MS);
	CheckEq("5m candle index at 00:05", 1, Clock().GetCandleIndex(GameTimeframe::M5));
	CheckEq("1h candle index is not changed by the 5m timeframe", 0, Clock().GetCandleIndex(GameTimeframe::H1));

	/* The aggregator: candles close at 00:05, 00:10, ... and the 1h candle is still the same as before. */
	Fresh();
	TimeframeAggregator agg;
	std::vector<std::pair<GameTimeframe, TimeframeBucket>> closed;
	agg.SetCloseCallback([&](GameTimeframe tf, const TimeframeBucket &b) { closed.push_back({tf, b}); });
	agg.AddSample(100);
	Clock().AdvanceMilliseconds(2 * MIN_MS);
	agg.AddSample(130);
	Clock().AdvanceMilliseconds(2 * MIN_MS);
	agg.AddSample(90);
	CheckEq("nothing closed at 00:04 (5m)", 0, agg.Closed(GameTimeframe::M5).size());
	Clock().AdvanceMilliseconds(MIN_MS); // 00:05
	CheckEq("the first 5m candle closed at 00:05", 1, agg.Closed(GameTimeframe::M5).size());
	const TimeframeBucket &b0 = agg.Closed(GameTimeframe::M5)[0];
	Check("5m candle has open 100, high 130, low 90, close 90", b0.open == 100 && b0.high == 130 && b0.low == 90 && b0.close == 90 && b0.index == 0);
	CheckEq("the running 5m candle is number 1", 1, agg.Running(GameTimeframe::M5).index);
	CheckEq("the 1h candle is still running", 0, agg.Closed(GameTimeframe::H1).size());
	Clock().AdvanceMilliseconds(HOUR_MS); // 01:05: 12 more 5m candles (the first hour is complete: 12 candles) and one 1h candle
	CheckEq("12 + 1 5m candles are closed 1 hour after the first", 13, agg.Closed(GameTimeframe::M5).size());
	CheckEq("one 1h candle closed", 1, agg.Closed(GameTimeframe::H1).size());
	Check("candles without a sample are flat at the previous close", agg.Closed(GameTimeframe::M5).back().close == 90 && agg.Closed(GameTimeframe::M5).back().samples == 0);

	/* Savegame: the 5m candles are saved with their own number, and an old save without them still loads. */
	const std::vector<TimeframeRecord> rec = agg.Export();
	bool has5 = false;
	for (const TimeframeRecord &r : rec) has5 = has5 || r.timeframe == static_cast<uint8_t>(GameTimeframe::M5);
	Check("the 5m candles are exported", has5);
	TimeframeAggregator copy;
	Check("import with 5m candles", copy.Import(rec));
	CheckEq("5m candles survive export and import", agg.Closed(GameTimeframe::M5).size(), copy.Closed(GameTimeframe::M5).size());
	std::vector<TimeframeRecord> old_save;
	for (const TimeframeRecord &r : rec) if (r.timeframe != static_cast<uint8_t>(GameTimeframe::M5)) old_save.push_back(r);
	TimeframeAggregator legacy;
	Check("a savegame from before 5m loads", legacy.Import(old_save));
	Check("... and has no 5m candles", legacy.Closed(GameTimeframe::M5).empty() && !legacy.HasRunning(GameTimeframe::M5));
	Check("... but keeps its 1h candles", legacy.Closed(GameTimeframe::H1).size() == agg.Closed(GameTimeframe::H1).size());

	/* The 5m timeframe keeps more closed candles than the others. */
	Fresh();
	TimeframeAggregator small(10);
	small.AddSample(5);
	Clock().AdvanceMilliseconds(HOUR_MS * 20);
	small.AddSample(6);
	CheckEq("10 closed 1h candles are kept", 10, small.Closed(GameTimeframe::H1).size());
	CheckEq("30 closed 5m candles are kept", 30, small.Closed(GameTimeframe::M5).size());
	CheckEq("... the newest ones", 20 * 12 - 1, small.Closed(GameTimeframe::M5).back().index);
}

int main()
{
	TestScale();
	TestEventCounts();
	TestBoundariesAndOrder();
	TestPriorityAndReentrancy();
	TestCountdownAndIndex();
	TestSaveState();
	TestAggregatorBasics();
	TestAggregatorVolumeAndArray();
	TestAggregatorAgainstSimTime();
	TestAggregatorSaveLoad();
	TestLongTimeframes();
	TestFiveMinuteTimeframe();
	TestCalendarSync();

	std::cout << _passed << " checks passed, " << _failed << " failed\n";
	return _failed == 0 ? 0 : 1;
}

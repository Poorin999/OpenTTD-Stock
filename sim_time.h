/*
 * Simulation time for the trading terminal: 1 simulation (system) day = 24 real minutes,
 * so 1 simulation hour = 1 real minute and the time runs 60 times faster than real time.
 *
 * - Converter between real time and simulation time (integer maths only, exact, the same on every client).
 * - Timeframes 1h / 4h / 12h with their candle-closing conditions.
 * - OHLC candle builder per timeframe.
 *
 * The header has no dependency on OpenTTD, so it can be compiled and tested on its own, see sim_time_test.cpp.
 */

#ifndef STOCK_SIM_TIME_H
#define STOCK_SIM_TIME_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <vector>

namespace sim_time {

/* ------------------------------------------------------------------------------------------------------------
 * Constants. Every duration is in milliseconds, in int64_t. Times are never negative.
 * ------------------------------------------------------------------------------------------------------------ */

inline constexpr int64_t MS_PER_SECOND = 1000;
inline constexpr int64_t SECONDS_PER_MINUTE = 60;
inline constexpr int64_t MINUTES_PER_HOUR = 60;
inline constexpr int64_t HOURS_PER_DAY = 24;

/** Length of the units of the simulation clock, in simulation milliseconds. */
inline constexpr int64_t SIM_MS_PER_MINUTE = SECONDS_PER_MINUTE * MS_PER_SECOND; ///< 60,000
inline constexpr int64_t SIM_MS_PER_HOUR = MINUTES_PER_HOUR * SIM_MS_PER_MINUTE; ///< 3,600,000
inline constexpr int64_t SIM_MS_PER_DAY = HOURS_PER_DAY * SIM_MS_PER_HOUR;       ///< 86,400,000

/** The rule of the simulation: one simulation day lasts this many real minutes. */
inline constexpr int64_t REAL_MINUTES_PER_SIM_DAY = 24;
inline constexpr int64_t REAL_MS_PER_MINUTE = SECONDS_PER_MINUTE * MS_PER_SECOND;                 ///< 60,000
inline constexpr int64_t REAL_MS_PER_SIM_DAY = REAL_MINUTES_PER_SIM_DAY * REAL_MS_PER_MINUTE;     ///< 1,440,000

static_assert(SIM_MS_PER_DAY % REAL_MS_PER_SIM_DAY == 0, "the time scale must be a whole number, otherwise conversions are not exact");

/** Simulation time runs this many times faster than real time (60: one real second is one simulation minute). */
inline constexpr int64_t TIME_SCALE = SIM_MS_PER_DAY / REAL_MS_PER_SIM_DAY;

static_assert(TIME_SCALE == 60, "1 day = 24 real minutes means that the simulation runs 60 times faster");
static_assert(SIM_MS_PER_HOUR == REAL_MS_PER_MINUTE * TIME_SCALE, "1 simulation hour must be 1 real minute");

/** Length of one game tick of OpenTTD in real milliseconds (the game counts ticks, not wall-clock time). */
inline constexpr int64_t REAL_MS_PER_TICK = 27;

/* ------------------------------------------------------------------------------------------------------------
 * 1. Converter between real time and simulation time.
 * ------------------------------------------------------------------------------------------------------------ */

/** Real milliseconds to simulation milliseconds. Exact. */
constexpr int64_t RealMsToSimMs(int64_t real_ms) { return real_ms * TIME_SCALE; }

/** Simulation milliseconds to real milliseconds, rounded down (a simulation millisecond is 1/60 of a real one). */
constexpr int64_t SimMsToRealMs(int64_t sim_ms) { return sim_ms / TIME_SCALE; }

/** Simulation milliseconds to real milliseconds, rounded up: the first real millisecond at which the simulation time has been reached. */
constexpr int64_t SimMsToRealMsCeil(int64_t sim_ms) { return (sim_ms + TIME_SCALE - 1) / TIME_SCALE; }

/** Whole real minutes to simulation milliseconds. */
constexpr int64_t RealMinutesToSimMs(int64_t real_minutes) { return RealMsToSimMs(real_minutes * REAL_MS_PER_MINUTE); }

/** Whole simulation hours to real milliseconds. */
constexpr int64_t SimHoursToRealMs(int64_t sim_hours) { return SimMsToRealMs(sim_hours * SIM_MS_PER_HOUR); }

/** Game ticks to real milliseconds, and the other way round (rounded down). */
constexpr int64_t TicksToRealMs(int64_t ticks) { return ticks * REAL_MS_PER_TICK; }
constexpr int64_t RealMsToTicks(int64_t real_ms) { return real_ms / REAL_MS_PER_TICK; }

/** Game ticks to simulation milliseconds: one tick is 27 * 60 = 1620 simulation milliseconds. */
constexpr int64_t TicksToSimMs(int64_t ticks) { return RealMsToSimMs(TicksToRealMs(ticks)); }

/** A simulation time split in days, hours, minutes, seconds and milliseconds. */
struct SimDateTime {
	int64_t day = 0;      ///< Number of complete simulation days since the start.
	int hour = 0;         ///< 0..23
	int minute = 0;       ///< 0..59
	int second = 0;       ///< 0..59
	int millisecond = 0;  ///< 0..999
};

/** Split simulation milliseconds (not negative) in days, hours, minutes, seconds and milliseconds. */
constexpr SimDateTime SplitSimMs(int64_t sim_ms)
{
	SimDateTime t;
	t.day = sim_ms / SIM_MS_PER_DAY;
	int64_t rest = sim_ms % SIM_MS_PER_DAY;
	t.hour = static_cast<int>(rest / SIM_MS_PER_HOUR);
	rest %= SIM_MS_PER_HOUR;
	t.minute = static_cast<int>(rest / SIM_MS_PER_MINUTE);
	rest %= SIM_MS_PER_MINUTE;
	t.second = static_cast<int>(rest / MS_PER_SECOND);
	t.millisecond = static_cast<int>(rest % MS_PER_SECOND);
	return t;
}

/** Print as "day 2 04:00:00". */
inline std::ostream &operator<<(std::ostream &os, const SimDateTime &t)
{
	const char old_fill = os.fill('0');
	os << "day " << t.day << ' ' << std::setw(2) << t.hour << ':' << std::setw(2) << t.minute << ':' << std::setw(2) << t.second;
	os.fill(old_fill);
	return os;
}

/**
 * The simulation clock. The single source of truth is the elapsed real time in milliseconds; the simulation time
 * is derived from it, so the two can never drift apart. Feed it with game ticks (the same on all clients of a
 * network game, stops when the game is paused) or with wall-clock milliseconds.
 */
class SimClock {
public:
	void Reset() { this->real_ms = 0; }

	/** Let real time pass. Negative values are ignored. */
	void AdvanceRealMs(int64_t ms) { if (ms > 0) this->real_ms += ms; }

	/** Let game ticks pass; every tick is REAL_MS_PER_TICK real milliseconds. */
	void AdvanceTicks(int64_t ticks) { this->AdvanceRealMs(TicksToRealMs(ticks)); }

	int64_t RealMs() const { return this->real_ms; }
	int64_t SimMs() const { return RealMsToSimMs(this->real_ms); }
	SimDateTime Now() const { return SplitSimMs(this->SimMs()); }

private:
	int64_t real_ms = 0; ///< Elapsed real time.
};

/* ------------------------------------------------------------------------------------------------------------
 * 2. Timeframes and the conditions under which a candle closes.
 * ------------------------------------------------------------------------------------------------------------ */

/** The timeframes of the chart. Candles of a timeframe always start at multiples of its length since simulation midnight. */
enum class Timeframe : uint8_t {
	H1,   ///< 1 simulation hour  = 1 real minute
	H4,   ///< 4 simulation hours = 4 real minutes
	H12,  ///< 12 simulation hours = 12 real minutes
	End,  ///< End marker.
};

inline constexpr size_t TIMEFRAME_COUNT = static_cast<size_t>(Timeframe::End);

/** Fixed properties of a timeframe. */
struct TimeframeInfo {
	const char *name;   ///< Short name for display.
	int64_t sim_hours;  ///< Length in simulation hours. The length in real minutes is the same number.
};

inline constexpr std::array<TimeframeInfo, TIMEFRAME_COUNT> TIMEFRAMES{{
	{"1h", 1},
	{"4h", 4},
	{"12h", 12},
}};

constexpr const TimeframeInfo &GetTimeframeInfo(Timeframe tf) { return TIMEFRAMES[static_cast<size_t>(tf)]; }

/** Length of a candle in simulation milliseconds. */
constexpr int64_t TimeframeSimMs(Timeframe tf) { return GetTimeframeInfo(tf).sim_hours * SIM_MS_PER_HOUR; }

/** Length of a candle in real milliseconds: 1h = 60,000, 4h = 240,000, 12h = 720,000. */
constexpr int64_t TimeframeRealMs(Timeframe tf) { return SimMsToRealMs(TimeframeSimMs(tf)); }

/** Number of candles of a timeframe in one simulation day (24, 6 and 2). */
constexpr int64_t CandlesPerSimDay(Timeframe tf) { return SIM_MS_PER_DAY / TimeframeSimMs(tf); }

static_assert(SIM_MS_PER_DAY % TimeframeSimMs(Timeframe::H1) == 0, "candles must fit a day exactly");
static_assert(SIM_MS_PER_DAY % TimeframeSimMs(Timeframe::H4) == 0, "candles must fit a day exactly");
static_assert(SIM_MS_PER_DAY % TimeframeSimMs(Timeframe::H12) == 0, "candles must fit a day exactly");
static_assert(TimeframeRealMs(Timeframe::H1) == 1 * REAL_MS_PER_MINUTE, "1h candle = 1 real minute");
static_assert(TimeframeRealMs(Timeframe::H4) == 4 * REAL_MS_PER_MINUTE, "4h candle = 4 real minutes");
static_assert(TimeframeRealMs(Timeframe::H12) == 12 * REAL_MS_PER_MINUTE, "12h candle = 12 real minutes");

/** Number of the candle that contains a simulation time. Candle 0 starts at simulation time 0. */
constexpr int64_t CandleIndex(int64_t sim_ms, Timeframe tf) { return sim_ms / TimeframeSimMs(tf); }

/** Simulation time at which a candle opens, and the (exclusive) time at which it closes. */
constexpr int64_t CandleOpenSimMs(int64_t index, Timeframe tf) { return index * TimeframeSimMs(tf); }
constexpr int64_t CandleCloseSimMs(int64_t index, Timeframe tf) { return (index + 1) * TimeframeSimMs(tf); }

/**
 * The closing condition. A candle closes when the clock reaches the start of the next one, that is when the
 * simulation time passes a multiple of the length of the timeframe:
 *   1h  closes when the minute turns 00 (every real minute),
 *   4h  closes at 04:00, 08:00, 12:00, 16:00, 20:00, 00:00 (every 4 real minutes),
 *   12h closes at 12:00 and 00:00 (every 12 real minutes).
 * The test uses the candle number, not "time modulo length == 0", so it still fires when a tick jumps over the
 * boundary (a tick is longer than a simulation millisecond) and when more time passes at once (game speed, pause
 * and resume, loading a game). In that case more than one candle can close at once.
 * @param prev_sim_ms Simulation time at the previous check.
 * @param now_sim_ms Simulation time now.
 * @param tf The timeframe.
 * @return Number of candles of this timeframe that have been completed since the previous check.
 */
constexpr int64_t CandlesClosed(int64_t prev_sim_ms, int64_t now_sim_ms, Timeframe tf)
{
	return now_sim_ms > prev_sim_ms ? CandleIndex(now_sim_ms, tf) - CandleIndex(prev_sim_ms, tf) : 0;
}

/** Whether at least one candle of the timeframe has been completed since the previous check. */
constexpr bool IsCandleClosing(int64_t prev_sim_ms, int64_t now_sim_ms, Timeframe tf)
{
	return CandlesClosed(prev_sim_ms, now_sim_ms, tf) > 0;
}

/** Bit of a timeframe in the mask returned by TimeframesClosing(). */
constexpr uint8_t TimeframeBit(Timeframe tf) { return static_cast<uint8_t>(1U << static_cast<unsigned>(tf)); }

/** All timeframes that close between two checks, as a bit mask: one call per update is enough. */
constexpr uint8_t TimeframesClosing(int64_t prev_sim_ms, int64_t now_sim_ms)
{
	uint8_t mask = 0;
	for (size_t i = 0; i < TIMEFRAME_COUNT; i++) {
		if (IsCandleClosing(prev_sim_ms, now_sim_ms, static_cast<Timeframe>(i))) mask |= TimeframeBit(static_cast<Timeframe>(i));
	}
	return mask;
}

/* ------------------------------------------------------------------------------------------------------------
 * Data aggregation: price samples to OHLC candles.
 * ------------------------------------------------------------------------------------------------------------ */

/** One candle. */
struct Candle {
	int64_t index = 0;        ///< Candle number, see CandleIndex().
	int64_t open_sim_ms = 0;  ///< Simulation time at which it opened.
	int64_t close_sim_ms = 0; ///< Simulation time at which it closes (exclusive).
	int64_t open = 0;         ///< First price in the candle.
	int64_t high = 0;         ///< Highest price.
	int64_t low = 0;          ///< Lowest price.
	int64_t close = 0;        ///< Last price.
	int64_t samples = 0;      ///< Number of price samples. 0 for a candle during which no price arrived (the clock jumped).
};

/**
 * The candles of one timeframe, built from price samples. The running candle collects the samples that belong
 * to it; when a sample arrives in a later candle, the running candle is closed. Candles that were skipped by a
 * jump of the clock are closed too, flat at the last closing price, so the series has no holes.
 */
class CandleSeries {
public:
	/**
	 * @param tf The timeframe of the candles.
	 * @param max_closed Number of closed candles that are kept; the oldest are dropped.
	 */
	explicit CandleSeries(Timeframe tf, size_t max_closed = 10000) : tf(tf), max_closed(max_closed) {}

	Timeframe GetTimeframe() const { return this->tf; }
	const std::vector<Candle> &Closed() const { return this->closed; }
	bool HasRunning() const { return this->has_running; }
	const Candle &Running() const { return this->running; }

	/**
	 * Add a price sample.
	 * @param sim_ms Simulation time of the sample. Samples that are older than the running candle are ignored.
	 * @param price The price.
	 * @return Number of candles that were closed by this sample.
	 */
	size_t Update(int64_t sim_ms, int64_t price)
	{
		const int64_t index = CandleIndex(sim_ms, this->tf);
		if (!this->has_running) {
			this->Start(index, price);
			this->AddSample(price);
			return 0;
		}
		if (index < this->running.index) return 0;

		size_t count = 0;
		while (this->running.index < index) {
			const int64_t last_close = this->running.close;
			this->Push(this->running);
			this->Start(this->running.index + 1, last_close); // Flat until a sample arrives.
			count++;
		}
		this->AddSample(price);
		return count;
	}

private:
	/** Start an empty candle; its prices are all the given price until the first sample replaces them. */
	void Start(int64_t index, int64_t price)
	{
		this->running = Candle{};
		this->running.index = index;
		this->running.open_sim_ms = CandleOpenSimMs(index, this->tf);
		this->running.close_sim_ms = CandleCloseSimMs(index, this->tf);
		this->running.open = this->running.high = this->running.low = this->running.close = price;
		this->has_running = true;
	}

	void AddSample(int64_t price)
	{
		Candle &c = this->running;
		if (c.samples == 0) {
			c.open = c.high = c.low = c.close = price;
		} else {
			c.high = std::max(c.high, price);
			c.low = std::min(c.low, price);
			c.close = price;
		}
		c.samples++;
	}

	void Push(const Candle &candle)
	{
		this->closed.push_back(candle);
		if (this->closed.size() > this->max_closed) this->closed.erase(this->closed.begin());
	}

	Timeframe tf;
	size_t max_closed;
	bool has_running = false;
	Candle running{};
	std::vector<Candle> closed{};
};

/** The candle series of all timeframes (1h, 4h, 12h), fed with the same price samples. */
class MultiTimeframeCandles {
public:
	MultiTimeframeCandles() : series{{CandleSeries(Timeframe::H1), CandleSeries(Timeframe::H4), CandleSeries(Timeframe::H12)}} {}

	/** Add a price sample to all timeframes. @return The number of candles that closed, per timeframe. */
	std::array<size_t, TIMEFRAME_COUNT> Update(int64_t sim_ms, int64_t price)
	{
		std::array<size_t, TIMEFRAME_COUNT> closed{};
		for (size_t i = 0; i < TIMEFRAME_COUNT; i++) closed[i] = this->series[i].Update(sim_ms, price);
		return closed;
	}

	const CandleSeries &Get(Timeframe tf) const { return this->series[static_cast<size_t>(tf)]; }

private:
	std::array<CandleSeries, TIMEFRAME_COUNT> series;
};

} // namespace sim_time

#endif /* STOCK_SIM_TIME_H */

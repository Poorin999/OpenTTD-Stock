/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file game_time_manager.h The clock of the mod: game minutes, hours, days and years that are mapped to real time.
 *
 * Scale (at normal game speed):
 *   1 game minute = 1 real second       (1,000 ms)
 *   1 game hour   = 1 real minute       (60 game minutes)
 *   1 game day    = 24 real minutes     (24 game hours = 1,440 game minutes)
 *   1 game year   = 365 game days       (no leap years)
 *
 * The clock is driven by the game ticks, not by the wall clock: every tick adds MILLISECONDS_PER_TICK milliseconds
 * (the constant of OpenTTD). That is the only way for all clients of a network game to get exactly the same time. As a
 * result the clock stops when the game is paused and runs faster when the game is fast-forwarded, just like the rest
 * of the game.
 *
 * Timeframes: next to the minute, hour and day events there are the events of the 5m, 4h, 12h, week, month and year timeframes.
 * They are aligned to game midnight: 5m fires at 00:00, 00:05, 00:10, ... (every 5 real seconds), 4h fires at 00:00, 04:00, 08:00, ... and 12h fires at 00:00 and 12:00, that is every 4 and
 * every 12 real minutes. A week is 7 game days, a month 30 game days and a year 365 game days; they all start at game day 0. When one clock step crosses several boundaries (fast-forward, a big catch-up), every
 * boundary is still dispatched, one by one, in time order. At one boundary the order is
 * Minute, Minute5, Hour, Hour4, Hour12, Day, Week, Month, Year.
 *
 * This file only needs the standard library. The connection to the game ticks and to the savegame is in
 * game_time_driver.cpp, so the clock can also be tested outside of the game.
 *
 * How a system subscribes (e.g. interest of the central bank once per game day):
 * @code
 *   static GameTimeSubscription _interest(GameTimeEvent::Day, [](const GameTimeSnapshot &now) {
 *       _central_bank.UpdateDaily();
 *   });
 * @endcode
 * The subscription ends when the object is destroyed. Callbacks run in the order of their priority (lower first), then in
 * the order in which they were subscribed. A callback may subscribe or unsubscribe.
 */

#ifndef GAME_TIME_MANAGER_H
#define GAME_TIME_MANAGER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

/**
 * Temporary switches. They decouple parts of the old game from the new clock while the clock is being built.
 * Change the value here (and build again) to turn the part back on.
 */
/** True: the candlestick chart of the trading terminal (the MT5 part: candles, SMA, volume, RSI) is not drawn. The data is still recorded. False now: the chart shows the 1h, 4h and 12h candles of game time. */
static constexpr bool GAME_TIME_DECOUPLE_CHART = false;
/**
 * True: the date of the original game (TimerGameCalendar) is driven by this clock. The calendar moves to the next day exactly when the clock
 * passes 23:59 -> 00:00, so one calendar day is one game day (24 real minutes) and the HH:MM of the status bar and the date always belong
 * together. Needs the change of timer_game_calendar.cpp that apply_overlay.py makes. It works in calendar and in wallclock timekeeping units. The
 * economy keeps running in its own 74-tick days, so incomes, costs and the economy timers of OpenTTD are not changed.
 */
static constexpr bool GAME_TIME_SYNC_CALENDAR = true;
/**
 * True: the quarter of the company statistics (the figures behind the earnings, the P/E and the dividends of the trading terminal, and the graphs
 * and ratings of the original game) closes by this clock, every 90 game days (36 real hours), and not every 3 economy months. Needs the change
 * of economy.cpp that apply_overlay.py makes. The monthly parts (bankruptcy check, infrastructure costs, interest) stay in economy time.
 */
static constexpr bool GAME_TIME_SYNC_QUARTERS = true;
/** The cash that a new company starts with: there is no original loan. It is the company's own money: the company starts without any loan. In the base currency (pounds). */
static constexpr int64_t GAME_TIME_STARTING_CAPITAL = 500'000;

/** The kinds of events that a system can subscribe to. */
enum class GameTimeEvent : uint8_t {
	Minute, ///< Once per game minute (every real second).
	Minute5, ///< Once per 5 game minutes (every 5 real seconds): end of a 5m timeframe.
	Hour,   ///< Once per game hour (every real minute). This is also the end of a 1h timeframe.
	Hour4,  ///< Once per 4 game hours (every 4 real minutes): end of a 4h timeframe.
	Hour12, ///< Once per 12 game hours (every 12 real minutes): end of a 12h timeframe.
	Day,    ///< Once per game day (every 24 real minutes). This is also the end of a day timeframe.
	Week,   ///< Once per 7 game days (every 168 real minutes): end of a week timeframe.
	Month,  ///< Once per 30 game days (every 720 real minutes): end of a month timeframe.
	Year,   ///< Once per 365 game days (every 8,760 real minutes): end of a year timeframe.
	End,    ///< End marker.
};

/** The timeframes of the chart and of the data aggregation. Each one has its own event. */
enum class GameTimeframe : uint8_t {
	/* The numbers are saved in savegames (see SRK_TIMEFRAME), so a new timeframe is always added at the END of the list, before Count. The chart
	 * shows the timeframes in its own order (5m first), see the buttons of stock_gui.cpp. */
	H1,    ///< 1 game hour  = 1 real minute.
	H4,    ///< 4 game hours = 4 real minutes.
	H12,   ///< 12 game hours = 12 real minutes.
	D1,    ///< 1 game day = 24 game hours = 24 real minutes.
	W1,    ///< 1 game week = 7 game days = 168 real minutes.
	MN,    ///< 1 game month = 30 game days = 720 real minutes. The game year has no calendar months; a month is always 30 days.
	Y1,    ///< 1 game year = 365 game days = 8,760 real minutes.
	M5,    ///< 5 game minutes = 5 real seconds. Added after the others, so that the numbers in old savegames keep their meaning.
	Count, ///< Number of timeframes.
};

inline constexpr size_t GAME_TIMEFRAME_COUNT = static_cast<size_t>(GameTimeframe::Count);

/** Length of a timeframe in game minutes. */
constexpr int GameTimeframeMinutes(GameTimeframe tf)
{
	switch (tf) {
		case GameTimeframe::M5:  return 5;
		case GameTimeframe::H1:  return 60;
		case GameTimeframe::H4:  return 60 * 4;
		case GameTimeframe::H12: return 60 * 12;
		case GameTimeframe::D1:  return 60 * 24;
		case GameTimeframe::W1:  return 60 * 24 * 7;
		case GameTimeframe::MN:  return 60 * 24 * 30;
		case GameTimeframe::Y1:  return 60 * 24 * 365;
		default:                 return 0;
	}
}

/** Length of a timeframe in whole game hours. 0 for a timeframe that is shorter than an hour (5m): use GameTimeframeMinutes() for those. */
constexpr int GameTimeframeHours(GameTimeframe tf)
{
	switch (tf) {
		case GameTimeframe::H1:  return 1;
		case GameTimeframe::H4:  return 4;
		case GameTimeframe::H12: return 12;
		case GameTimeframe::D1:  return 24;
		case GameTimeframe::W1:  return 24 * 7;
		case GameTimeframe::MN:  return 24 * 30;
		case GameTimeframe::Y1:  return 24 * 365;
		default:                 return 0;
	}
}

/** The event that is sent when a candle of the timeframe is complete. */
constexpr GameTimeEvent GameTimeframeEvent(GameTimeframe tf)
{
	switch (tf) {
		case GameTimeframe::M5:  return GameTimeEvent::Minute5;
		case GameTimeframe::H4:  return GameTimeEvent::Hour4;
		case GameTimeframe::H12: return GameTimeEvent::Hour12;
		case GameTimeframe::D1:  return GameTimeEvent::Day;
		case GameTimeframe::W1:  return GameTimeEvent::Week;
		case GameTimeframe::MN:  return GameTimeEvent::Month;
		case GameTimeframe::Y1:  return GameTimeEvent::Year;
		default:                 return GameTimeEvent::Hour;
	}
}

/** Short name for display. */
constexpr const char *GameTimeframeName(GameTimeframe tf)
{
	switch (tf) {
		case GameTimeframe::M5:  return "5m";
		case GameTimeframe::H4:  return "4h";
		case GameTimeframe::H12: return "12h";
		case GameTimeframe::D1:  return "1d";
		case GameTimeframe::W1:  return "1w";
		case GameTimeframe::MN:  return "1mo";
		case GameTimeframe::Y1:  return "1y";
		default:                 return "1h";
	}
}

/** The time at one moment. */
struct GameTimeSnapshot {
	uint64_t total_minutes = 0; ///< Game minutes since the start of the game.
	uint64_t total_hours = 0;   ///< Complete game hours since the start of the game; total_hours / 4 is the number of the 4h candle.
	/* The number of the 5m candle is total_minutes / 5. */
	int minute = 0;             ///< Minute of the hour, 0..59.
	int hour = 0;               ///< Hour of the day, 0..23.
	int day = 1;                ///< Day of the year, 1..365.
	int year = 0;               ///< Game year.
};

/**
 * Everything of the clock that has to go in a savegame. The clock is a single number: all other values (minute, hour,
 * day, the candle number of every timeframe) are calculated from it, so they cannot get out of step after loading.
 */
struct GameTimeSaveState {
	uint64_t total_ms = 0;  ///< Milliseconds since the start of the game.
	int start_year = 1950;  ///< Game year at game minute 0.
};

/** The clock. There is one, see GameTimeManager::Get(). All functions can be called from any thread. */
class GameTimeManager {
public:
	static constexpr uint64_t MS_PER_MINUTE = 1000;       ///< Game minute in milliseconds.
	static constexpr int MINUTES_PER_HOUR = 60;           ///< Game minutes in a game hour.
	static constexpr int HOURS_PER_DAY = 24;              ///< Game hours in a game day.
	static constexpr int DAYS_PER_YEAR = 365;             ///< Game days in a game year.
	static constexpr int DAYS_PER_WEEK = 7;               ///< Game days in a game week.
	static constexpr int DAYS_PER_QUARTER = 90;           ///< Game days in a game quarter (3 game months); the company statistics close every quarter.
	static constexpr int DAYS_PER_MONTH = 30;             ///< Game days in a game month (the game year has no calendar months).
	static constexpr int MINUTES_PER_DAY = MINUTES_PER_HOUR * HOURS_PER_DAY; ///< Game minutes in a game day.
	static constexpr uint64_t MS_PER_DAY = MS_PER_MINUTE * MINUTES_PER_DAY;  ///< Game day in milliseconds.
	static constexpr int DEFAULT_START_YEAR = 1950;       ///< Game year at game minute 0.
	static constexpr int MIN_START_YEAR = 1;              ///< Lowest start year that is accepted when loading.
	static constexpr int MAX_START_YEAR = 9999;           ///< Highest start year that is accepted when loading.

	/** Length of the period of an event in milliseconds; 0 for End. */
	static constexpr uint64_t EventPeriodMs(GameTimeEvent event)
	{
		switch (event) {
			case GameTimeEvent::Minute: return MS_PER_MINUTE;
			case GameTimeEvent::Minute5: return MS_PER_MINUTE * 5;
			case GameTimeEvent::Hour:   return MS_PER_MINUTE * MINUTES_PER_HOUR;
			case GameTimeEvent::Hour4:  return MS_PER_MINUTE * MINUTES_PER_HOUR * 4;
			case GameTimeEvent::Hour12: return MS_PER_MINUTE * MINUTES_PER_HOUR * 12;
			case GameTimeEvent::Day:    return MS_PER_MINUTE * MINUTES_PER_DAY;
			case GameTimeEvent::Week:   return MS_PER_MINUTE * MINUTES_PER_DAY * DAYS_PER_WEEK;
			case GameTimeEvent::Month:  return MS_PER_MINUTE * MINUTES_PER_DAY * DAYS_PER_MONTH;
			case GameTimeEvent::Year:   return MS_PER_MINUTE * MINUTES_PER_DAY * DAYS_PER_YEAR;
			default:                    return 0;
		}
	}

	using SubscriptionId = uint64_t;                      ///< Number that identifies a subscription; 0 is never used.
	using Callback = std::function<void(const GameTimeSnapshot &)>; ///< Function that is called at an event.

	static GameTimeManager &Get();

	/* Time. */
	int GetCurrentGameMinute() const;
	int GetCurrentGameHour() const;
	int GetCurrentGameDay() const;
	int GetCurrentGameYear() const;
	uint64_t GetTotalGameMinutes() const;
	uint64_t GetTotalMilliseconds() const;
	GameTimeSnapshot GetSnapshot() const;

	/** Number of the candle of a timeframe that the clock is in now (candle 0 starts at game minute 0). */
	uint64_t GetCandleIndex(GameTimeframe tf) const;
	/** Real milliseconds until the next event of this kind, e.g. for the countdown of a candle. Never 0: at a boundary it is a full period. */
	uint64_t GetMillisecondsUntil(GameTimeEvent event) const;

	/* Moving the clock. */
	void AdvanceMilliseconds(uint64_t delta_ms);
	void Reset();
	void Restore(uint64_t total_ms);
	void SetStartYear(int year);

	/* Savegames. LoadSaveState() sends no events, and rejects a start year that is out of range by keeping the current one. */
	GameTimeSaveState GetSaveState() const;
	void LoadSaveState(const GameTimeSaveState &state);

	/* Events. */
	SubscriptionId Subscribe(GameTimeEvent event, Callback callback, int priority = 0);
	void Unsubscribe(SubscriptionId id);
	size_t GetSubscriberCount(GameTimeEvent event) const;

	/** The dispatchers. They call the subscribers of the event for the current time; AdvanceMilliseconds calls them at every boundary. */
	void OnMinuteUpdate();
	void OnMinute5Update();
	void OnHourlyUpdate();
	void OnHour4Update();
	void OnHour12Update();
	void OnDailyUpdate();
	void OnWeeklyUpdate();
	void OnMonthlyUpdate();
	void OnYearlyUpdate();

	/* The original calendar. TimerGameCalendar asks the clock when to start a new day and how far the day has come. */
	/** Game day number (since game minute 0) that the calendar has started. A day of the clock that is not consumed yet is a day that the calendar still has to start. */
	uint64_t GetCalendarDaysConsumed() const;
	/** Game day number that the clock is in now. */
	uint64_t GetCurrentDayIndex() const;
	/** Start one more calendar day if the clock has passed a midnight that the calendar has not seen. @return True if a day was consumed. */
	bool TryConsumeCalendarDay();
	/** How far the clock is in the current day, in steps of 1 / ticks_per_day of a day: 0 .. ticks_per_day - 1 (the value for TimerGameCalendar::date_fract). */
	uint32_t GetDayFraction(uint32_t ticks_per_day) const;

	/* The temporary switches, see above. */
	static constexpr bool AreQuartersSynced() { return GAME_TIME_SYNC_QUARTERS; }
	static constexpr bool IsCalendarSynced() { return GAME_TIME_SYNC_CALENDAR; }
	static constexpr bool IsChartDecoupled() { return GAME_TIME_DECOUPLE_CHART; }
	static constexpr int64_t GetStartingCapital() { return GAME_TIME_STARTING_CAPITAL; }

private:
	GameTimeManager() = default;
	GameTimeManager(const GameTimeManager &) = delete;
	GameTimeManager &operator=(const GameTimeManager &) = delete;

	/** One subscription. */
	struct Subscriber {
		SubscriptionId id;
		GameTimeEvent event;
		int priority;
		Callback callback;
	};

	void Dispatch(GameTimeEvent event);
	static GameTimeSnapshot MakeSnapshot(uint64_t total_ms, int start_year);

	std::atomic<uint64_t> total_ms_{0};      ///< Milliseconds since the start of the game.
	std::atomic<int> start_year_{DEFAULT_START_YEAR}; ///< Game year at game minute 0.
	std::atomic<uint64_t> calendar_days_{0}; ///< Number of game days that the original calendar has started; follows #total_ms_ at every jump (load, reset).

	mutable std::mutex subscribers_mutex_;   ///< Protects #subscribers_ and #next_id_.
	std::vector<Subscriber> subscribers_;    ///< All subscriptions.
	SubscriptionId next_id_ = 1;             ///< Number of the next subscription.

	std::recursive_mutex advance_mutex_;     ///< One caller moves the clock at a time; recursive, because a callback may call AdvanceMilliseconds.
	bool in_advance_ = false;                ///< True while the clock is being moved (protected by #advance_mutex_).
	uint64_t queued_ms_ = 0;                 ///< Time that a callback asked for while the clock was being moved (protected by #advance_mutex_).
};

/**
 * A subscription that ends when the object is destroyed. Keep it as a static variable or a member of the system that
 * listens, so the function is never called after the system is gone.
 */
class GameTimeSubscription {
public:
	GameTimeSubscription(GameTimeEvent event, GameTimeManager::Callback callback, int priority = 0) :
		id_(GameTimeManager::Get().Subscribe(event, std::move(callback), priority)) {}
	~GameTimeSubscription() { GameTimeManager::Get().Unsubscribe(this->id_); }
	GameTimeSubscription(const GameTimeSubscription &) = delete;
	GameTimeSubscription &operator=(const GameTimeSubscription &) = delete;

private:
	GameTimeManager::SubscriptionId id_;
};

#endif /* GAME_TIME_MANAGER_H */

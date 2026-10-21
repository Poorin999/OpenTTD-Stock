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
 * The clock is driven by the game ticks, not by the wall clock: every tick adds MILLISECONDS_PER_TICK milliseconds.
 * That is the only way for all clients of a network game to get exactly the same time. As a result the clock stops when
 * the game is paused and runs faster when the game is fast-forwarded, just like the rest of the game.
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
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

/**
 * Temporary switches. They decouple parts of the old game from the new clock while the clock is being built.
 * Change the value here (and build again) to turn the part back on.
 */
/** True: the candlestick chart of the trading terminal (the MT5 part: candles, SMA, volume, RSI) is not drawn. The data is still recorded. */
static constexpr bool GAME_TIME_DECOUPLE_CHART = true;
/** True: the original monthly interest on the OpenTTD loan of a company is not charged. Needs the change of economy.cpp that apply_overlay.py makes. */
static constexpr bool GAME_TIME_BYPASS_LEGACY_LOANS = true;

/** The kinds of events that a system can subscribe to. */
enum class GameTimeEvent : uint8_t {
	Minute, ///< Once per game minute (every real second).
	Hour,   ///< Once per game hour (every real minute).
	Day,    ///< Once per game day (every 24 real minutes).
	End,    ///< End marker.
};

/** The time at one moment. */
struct GameTimeSnapshot {
	uint64_t total_minutes = 0; ///< Game minutes since the start of the game.
	int minute = 0;             ///< Minute of the hour, 0..59.
	int hour = 0;               ///< Hour of the day, 0..23.
	int day = 1;                ///< Day of the year, 1..365.
	int year = 0;               ///< Game year.
};

/** The clock. There is one, see GameTimeManager::Get(). All functions can be called from any thread. */
class GameTimeManager {
public:
	static constexpr uint64_t MS_PER_MINUTE = 1000;       ///< Game minute in milliseconds.
	static constexpr int MINUTES_PER_HOUR = 60;           ///< Game minutes in a game hour.
	static constexpr int HOURS_PER_DAY = 24;              ///< Game hours in a game day.
	static constexpr int DAYS_PER_YEAR = 365;             ///< Game days in a game year.
	static constexpr int MINUTES_PER_DAY = MINUTES_PER_HOUR * HOURS_PER_DAY; ///< Game minutes in a game day.
	static constexpr int DEFAULT_START_YEAR = 1950;       ///< Game year at game minute 0.

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

	/* Moving the clock. */
	void AdvanceMilliseconds(uint64_t delta_ms);
	void Reset();
	void Restore(uint64_t total_ms);
	void SetStartYear(int year);

	/* Events. */
	SubscriptionId Subscribe(GameTimeEvent event, Callback callback, int priority = 0);
	void Unsubscribe(SubscriptionId id);
	size_t GetSubscriberCount(GameTimeEvent event) const;

	/** The dispatchers. They call the subscribers of the event for the current time; AdvanceMilliseconds calls them at every boundary. */
	void OnMinuteUpdate();
	void OnHourlyUpdate();
	void OnDailyUpdate();

	/* The temporary switches, see above. */
	static constexpr bool IsChartDecoupled() { return GAME_TIME_DECOUPLE_CHART; }
	static constexpr bool IsLegacyLoanBypassed() { return GAME_TIME_BYPASS_LEGACY_LOANS; }

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

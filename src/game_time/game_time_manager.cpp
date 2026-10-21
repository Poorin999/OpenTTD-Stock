/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file game_time_manager.cpp Implementation of the clock of the mod. */

#include "../stdafx.h"
#include "game_time_manager.h"

#include <algorithm>

#include "../safeguards.h"

/** The one clock. */
GameTimeManager &GameTimeManager::Get()
{
	static GameTimeManager instance;
	return instance;
}

/**
 * Calculate the time from the milliseconds. Integer arithmetic only, so every machine gets the same result.
 * @param total_ms Milliseconds since the start of the game.
 * @param start_year Game year at the start.
 * @return The time.
 */
GameTimeSnapshot GameTimeManager::MakeSnapshot(uint64_t total_ms, int start_year)
{
	GameTimeSnapshot s;
	s.total_minutes = total_ms / MS_PER_MINUTE;
	s.minute = static_cast<int>(s.total_minutes % MINUTES_PER_HOUR);
	s.hour = static_cast<int>((s.total_minutes / MINUTES_PER_HOUR) % HOURS_PER_DAY);
	const uint64_t total_days = s.total_minutes / MINUTES_PER_DAY;
	s.day = static_cast<int>(total_days % DAYS_PER_YEAR) + 1;
	s.year = start_year + static_cast<int>(total_days / DAYS_PER_YEAR);
	return s;
}

GameTimeSnapshot GameTimeManager::GetSnapshot() const
{
	return MakeSnapshot(this->total_ms_.load(), this->start_year_.load());
}

/** Minute of the hour, 0..59. */
int GameTimeManager::GetCurrentGameMinute() const { return this->GetSnapshot().minute; }

/** Hour of the day, 0..23. */
int GameTimeManager::GetCurrentGameHour() const { return this->GetSnapshot().hour; }

/** Day of the year, 1..365. */
int GameTimeManager::GetCurrentGameDay() const { return this->GetSnapshot().day; }

/** The game year. */
int GameTimeManager::GetCurrentGameYear() const { return this->GetSnapshot().year; }

/** Game minutes since the start of the game. */
uint64_t GameTimeManager::GetTotalGameMinutes() const { return this->total_ms_.load() / MS_PER_MINUTE; }

/** Milliseconds since the start of the game, including the part of the running minute. */
uint64_t GameTimeManager::GetTotalMilliseconds() const { return this->total_ms_.load(); }

/** Set the game year at game minute 0. */
void GameTimeManager::SetStartYear(int year)
{
	this->start_year_.store(year);
}

/** Go back to game minute 0, e.g. for a new game. The subscriptions stay. */
void GameTimeManager::Reset()
{
	std::lock_guard<std::recursive_mutex> lock(this->advance_mutex_);
	this->total_ms_.store(0);
	this->queued_ms_ = 0;
}

/**
 * Set the clock to a saved time. No events are sent for the time that has passed.
 * @param total_ms Milliseconds since the start of the game.
 */
void GameTimeManager::Restore(uint64_t total_ms)
{
	std::lock_guard<std::recursive_mutex> lock(this->advance_mutex_);
	this->total_ms_.store(total_ms);
	this->queued_ms_ = 0;
}

/**
 * Add a subscription.
 * @param event What to listen to.
 * @param callback Function to call; it gets the time of the event.
 * @param priority Callbacks with a lower number are called first.
 * @return Number to give to Unsubscribe(); 0 when the subscription is not valid.
 */
GameTimeManager::SubscriptionId GameTimeManager::Subscribe(GameTimeEvent event, Callback callback, int priority)
{
	if (event >= GameTimeEvent::End || !callback) return 0;
	std::lock_guard<std::mutex> lock(this->subscribers_mutex_);
	const SubscriptionId id = this->next_id_++;
	this->subscribers_.push_back({id, event, priority, std::move(callback)});
	return id;
}

/** Remove a subscription. Does nothing for a number that is not in use. */
void GameTimeManager::Unsubscribe(SubscriptionId id)
{
	if (id == 0) return;
	std::lock_guard<std::mutex> lock(this->subscribers_mutex_);
	this->subscribers_.erase(std::remove_if(this->subscribers_.begin(), this->subscribers_.end(),
			[id](const Subscriber &s) { return s.id == id; }), this->subscribers_.end());
}

/** Number of subscriptions to an event. */
size_t GameTimeManager::GetSubscriberCount(GameTimeEvent event) const
{
	std::lock_guard<std::mutex> lock(this->subscribers_mutex_);
	return static_cast<size_t>(std::count_if(this->subscribers_.begin(), this->subscribers_.end(),
			[event](const Subscriber &s) { return s.event == event; }));
}

/**
 * Call the subscribers of an event. The list is copied first and called without the lock, so a callback can subscribe
 * or unsubscribe, and a slow callback does not block other threads.
 */
void GameTimeManager::Dispatch(GameTimeEvent event)
{
	std::vector<Subscriber> todo;
	{
		std::lock_guard<std::mutex> lock(this->subscribers_mutex_);
		for (const Subscriber &s : this->subscribers_) {
			if (s.event == event) todo.push_back(s);
		}
	}
	std::stable_sort(todo.begin(), todo.end(), [](const Subscriber &a, const Subscriber &b) { return a.priority < b.priority; });

	const GameTimeSnapshot now = this->GetSnapshot();
	for (const Subscriber &s : todo) s.callback(now);
}

/** Dispatcher: a game minute has passed (every real second). */
void GameTimeManager::OnMinuteUpdate() { this->Dispatch(GameTimeEvent::Minute); }

/** Dispatcher: a game hour has passed (every real minute). */
void GameTimeManager::OnHourlyUpdate() { this->Dispatch(GameTimeEvent::Hour); }

/** Dispatcher: a game day has passed (every 24 real minutes). */
void GameTimeManager::OnDailyUpdate() { this->Dispatch(GameTimeEvent::Day); }

/**
 * Move the clock forward. Every minute boundary that is crossed sends its events, in the order minute, hour, day,
 * also when the time that is added is many minutes (fast-forward), so no event is lost. When a callback calls this
 * function, the time is added after the running events have been sent.
 * @param delta_ms Milliseconds to add.
 */
void GameTimeManager::AdvanceMilliseconds(uint64_t delta_ms)
{
	std::lock_guard<std::recursive_mutex> lock(this->advance_mutex_);
	if (this->in_advance_) {
		this->queued_ms_ += delta_ms;
		return;
	}

	/** Makes sure #in_advance_ is cleared, also when a callback throws. */
	struct Guard {
		GameTimeManager &m;
		explicit Guard(GameTimeManager &manager) : m(manager) { m.in_advance_ = true; }
		~Guard() { m.in_advance_ = false; }
	} guard(*this);

	uint64_t remaining = delta_ms;
	while (remaining > 0) {
		const uint64_t now = this->total_ms_.load();
		const uint64_t to_boundary = MS_PER_MINUTE - (now % MS_PER_MINUTE);
		const uint64_t step = std::min(remaining, to_boundary);
		this->total_ms_.store(now + step);
		remaining -= step;

		if ((now + step) % MS_PER_MINUTE == 0) {
			const uint64_t minutes = (now + step) / MS_PER_MINUTE;
			this->OnMinuteUpdate();
			if (minutes % MINUTES_PER_HOUR == 0) this->OnHourlyUpdate();
			if (minutes % MINUTES_PER_DAY == 0) this->OnDailyUpdate();
		}

		if (remaining == 0 && this->queued_ms_ > 0) {
			remaining = this->queued_ms_;
			this->queued_ms_ = 0;
		}
	}
}

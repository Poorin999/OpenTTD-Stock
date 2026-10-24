/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file game_time_aggregator.h Collects samples (e.g. prices) into buckets of the 5m, 1h, 4h, 12h, day, week, month and year timeframes.
 *
 * The aggregator listens to the Minute5, Hour, Hour4, Hour12, Day, Week, Month and Year events of the GameTimeManager, so a bucket is closed exactly at
 * the boundary of its timeframe, also when no sample arrived in it. Only the standard library and the manager are
 * needed. It does not know about savegames: Export() and Import() convert the state to a flat list of records that
 * any chunk handler can write.
 *
 * Buckets of a timeframe are numbered from game minute 0 (bucket 0 starts at 00:00 of the first day), so the number of
 * the 4h bucket at game hour H is H / 4 and the number of the 5m bucket at game minute M is M / 5. That makes the numbers the same on every client and after every load.
 *
 * Saving: Export() gives a flat list of records and Import() takes it back; see SRK_TIMEFRAME in stock_sl.cpp for a
 * mapping to savegame rows. Collect all rows of one aggregator in a std::vector<TimeframeRecord> and call Import()
 * once after the last row (it checks the whole list); call Reset() where the game state is reset. The clock itself is
 * saved with GameTimeManager::GetSaveState() / LoadSaveState(), see SRK_TIME.
 *
 * Example:
 * @code
 *   static TimeframeAggregator _prices;
 *   _prices.SetCloseCallback([](GameTimeframe tf, const TimeframeBucket &b) { UpdateChart(tf, b); });
 *   // anywhere in the game, at any time:
 *   _prices.AddSample(current_price);
 * @endcode
 */

#ifndef GAME_TIME_AGGREGATOR_H
#define GAME_TIME_AGGREGATOR_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "game_time_manager.h"

/** The samples of one bucket of a timeframe. */
struct TimeframeBucket {
	uint64_t index = 0;   ///< Number of the bucket, see the file comment.
	int64_t open = 0;     ///< First sample.
	int64_t high = 0;     ///< Highest sample.
	int64_t low = 0;      ///< Lowest sample.
	int64_t close = 0;    ///< Last sample.
	uint32_t samples = 0; ///< Number of samples. 0 for a bucket in which nothing arrived: its prices are flat at the previous close.
	int64_t volume = 0;   ///< Sum of the volumes that were added with AddVolume() while the bucket was running.

	bool operator==(const TimeframeBucket &o) const
	{
		return index == o.index && open == o.open && high == o.high && low == o.low && close == o.close && samples == o.samples && volume == o.volume;
	}
};

/** One bucket in the flat form that is used for saving. */
struct TimeframeRecord {
	uint8_t timeframe = 0;  ///< A GameTimeframe.
	bool running = false;   ///< True for the bucket that was still open; false for a closed bucket.
	TimeframeBucket bucket; ///< The data.
};

/** The aggregator. One instance per kind of data. Not thread-safe by itself: use it from the game thread, like the rest of the game. */
class TimeframeAggregator {
public:
	/** Called for every bucket that is closed, including empty (flat) ones. It runs inside the clock event. */
	using CloseCallback = std::function<void(GameTimeframe, const TimeframeBucket &)>;

	/** Number of closed buckets that are kept per timeframe by default. */
	static constexpr size_t DEFAULT_MAX_CLOSED = 500;
	/** The 5m timeframe closes a bucket every 5 real seconds, so it keeps this many times more buckets (the same stretch of real time would be too short otherwise). */
	static constexpr size_t SHORT_FRAME_FACTOR = 3;

	/** Not explicit, so that an array of aggregators can be default constructed. */
	TimeframeAggregator() : TimeframeAggregator(DEFAULT_MAX_CLOSED) {}

	/** @param max_closed Number of closed buckets that are kept per timeframe; the oldest are dropped. */
	explicit TimeframeAggregator(size_t max_closed) : max_closed_(max_closed == 0 ? 1 : max_closed)
	{
		for (size_t i = 0; i < GAME_TIMEFRAME_COUNT; i++) {
			const GameTimeframe tf = static_cast<GameTimeframe>(i);
			this->subscriptions_[i] = std::make_unique<GameTimeSubscription>(GameTimeframeEvent(tf),
					[this, tf](const GameTimeSnapshot &now) { this->OnBoundary(tf, now.total_minutes); }, TimeframeAggregator::PRIORITY);
		}
	}

	TimeframeAggregator(const TimeframeAggregator &) = delete;
	TimeframeAggregator &operator=(const TimeframeAggregator &) = delete;

	/** The aggregator runs before the default priority (0), so that other systems that listen to the same event see closed buckets. */
	static constexpr int PRIORITY = -100;

	void SetCloseCallback(CloseCallback callback) { this->on_close_ = std::move(callback); }

	/** Forget all samples, e.g. for a new game. The subscriptions stay. */
	void Reset()
	{
		for (Series &s : this->series_) s = Series{};
	}

	/**
	 * Add a sample at the current game time to all timeframes.
	 * @param value The value, e.g. a price.
	 */
	void AddSample(int64_t value)
	{
		const uint64_t minutes = GameTimeManager::Get().GetSnapshot().total_minutes;
		for (size_t i = 0; i < GAME_TIMEFRAME_COUNT; i++) {
			const GameTimeframe tf = static_cast<GameTimeframe>(i);
			Series &s = this->series_[i];
			const uint64_t index = minutes / static_cast<uint64_t>(GameTimeframeMinutes(tf));
			if (!s.has_running) {
				s.running = TimeframeBucket{};
				s.running.index = index;
				s.has_running = true;
			} else {
				this->CloseUntil(tf, index); // Normally nothing to do: the boundary event did it already.
			}
			TimeframeBucket &b = s.running;
			if (b.samples == 0) {
				b.open = b.high = b.low = b.close = value;
			} else {
				if (value > b.high) b.high = value;
				if (value < b.low) b.low = value;
				b.close = value;
			}
			b.samples++;
		}
	}

	/**
	 * Add traded volume to the running buckets of all timeframes, e.g. the shares of an executed order. A timeframe that
	 * has no running bucket yet (no sample so far) ignores it.
	 */
	void AddVolume(int64_t volume)
	{
		if (volume <= 0) return;
		const uint64_t minutes = GameTimeManager::Get().GetSnapshot().total_minutes;
		for (size_t i = 0; i < GAME_TIMEFRAME_COUNT; i++) {
			const GameTimeframe tf = static_cast<GameTimeframe>(i);
			if (!this->series_[i].has_running) continue;
			this->CloseUntil(tf, minutes / static_cast<uint64_t>(GameTimeframeMinutes(tf)));
			this->series_[i].running.volume += volume;
		}
	}

	const std::deque<TimeframeBucket> &Closed(GameTimeframe tf) const { return this->series_[static_cast<size_t>(tf)].closed; }
	bool HasRunning(GameTimeframe tf) const { return this->series_[static_cast<size_t>(tf)].has_running; }
	const TimeframeBucket &Running(GameTimeframe tf) const { return this->series_[static_cast<size_t>(tf)].running; }

	/** Flat copy of the state: for each timeframe the closed buckets from old to new, then the running bucket. */
	std::vector<TimeframeRecord> Export() const
	{
		std::vector<TimeframeRecord> out;
		for (size_t i = 0; i < GAME_TIMEFRAME_COUNT; i++) {
			const Series &s = this->series_[i];
			for (const TimeframeBucket &b : s.closed) out.push_back({static_cast<uint8_t>(i), false, b});
			if (s.has_running) out.push_back({static_cast<uint8_t>(i), true, s.running});
		}
		return out;
	}

	/**
	 * Replace the state by saved records. Nothing is sent to the close callback. The data of a savegame is not trusted:
	 * when a record is not valid (unknown timeframe, numbers that do not increase, a high below a low, a second running
	 * bucket) the state is emptied and false is returned, so that the game goes on with a clean state.
	 * A state that is older than the clock is fine; the next sample or event closes the missing buckets flat.
	 * @return True when all records were valid.
	 */
	bool Import(const std::vector<TimeframeRecord> &records)
	{
		std::array<Series, GAME_TIMEFRAME_COUNT> fresh;
		for (const TimeframeRecord &r : records) {
			if (r.timeframe >= GAME_TIMEFRAME_COUNT || !TimeframeAggregator::IsValid(r.bucket)) return this->Fail();
			Series &s = fresh[r.timeframe];
			if (s.has_running) return this->Fail(); // Nothing may follow the running bucket.
			const bool has_previous = !s.closed.empty();
			if (has_previous && r.bucket.index <= s.closed.back().index) return this->Fail();
			if (r.running) {
				s.running = r.bucket;
				s.has_running = true;
			} else {
				s.closed.push_back(r.bucket);
				if (s.closed.size() > this->MaxClosed(static_cast<GameTimeframe>(r.timeframe))) s.closed.pop_front();
			}
		}
		for (size_t i = 0; i < GAME_TIMEFRAME_COUNT; i++) {
			if (fresh[i].has_running && !fresh[i].closed.empty() && fresh[i].running.index <= fresh[i].closed.back().index) return this->Fail();
		}
		this->series_ = std::move(fresh);
		return true;
	}

private:
	/** The data of one timeframe. */
	struct Series {
		std::deque<TimeframeBucket> closed{};
		TimeframeBucket running{};
		bool has_running = false;
	};

	static bool IsValid(const TimeframeBucket &b)
	{
		if (b.low > b.high || b.volume < 0) return false;
		if (b.samples == 0) return b.open == b.high && b.high == b.low && b.low == b.close; // Flat.
		return b.open >= b.low && b.open <= b.high && b.close >= b.low && b.close <= b.high;
	}

	/** Number of closed buckets that are kept for a timeframe. */
	size_t MaxClosed(GameTimeframe tf) const
	{
		return tf == GameTimeframe::M5 ? this->max_closed_ * SHORT_FRAME_FACTOR : this->max_closed_;
	}

	bool Fail()
	{
		this->Reset();
		return false;
	}

	/** The clock reached the start of a bucket: close everything before it. */
	void OnBoundary(GameTimeframe tf, uint64_t total_minutes)
	{
		Series &s = this->series_[static_cast<size_t>(tf)];
		if (!s.has_running) return; // No sample yet: nothing to close.
		this->CloseUntil(tf, total_minutes / static_cast<uint64_t>(GameTimeframeMinutes(tf)));
	}

	/** Close the running bucket, and the empty ones after it, until the running one has the number #index. */
	void CloseUntil(GameTimeframe tf, uint64_t index)
	{
		Series &s = this->series_[static_cast<size_t>(tf)];
		while (s.has_running && s.running.index < index) {
			const TimeframeBucket done = s.running;
			s.closed.push_back(done);
			if (s.closed.size() > this->MaxClosed(tf)) s.closed.pop_front();

			TimeframeBucket next;
			next.index = done.index + 1;
			next.open = next.high = next.low = next.close = done.close; // Flat until a sample arrives.
			s.running = next;

			if (this->on_close_) this->on_close_(tf, done);
		}
	}

	std::array<Series, GAME_TIMEFRAME_COUNT> series_{};
	std::array<std::unique_ptr<GameTimeSubscription>, GAME_TIMEFRAME_COUNT> subscriptions_{};
	CloseCallback on_close_{};
	size_t max_closed_;
};

#endif /* GAME_TIME_AGGREGATOR_H */

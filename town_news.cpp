/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file town_news.cpp Real town news and weather, once per game day. See town_news.h and market_events.h. */

#include "stdafx.h"
#include "town_news.h"
#include "market_events.h"
#include "town_treasury.h"
#include "central_bank.h"
#include "openttd.h"
#include "settings_type.h"
#include "town.h"
#include "game_time/game_time_manager.h"

#include <algorithm>
#include <vector>

#include "safeguards.h"

std::map<uint32_t, TownNewsSnap> _town_news_snaps;

void TownNewsReset()
{
	_town_news_snaps.clear();
}

namespace {

static constexpr size_t TOWN_NEWS_MAX_MILESTONES_PER_DAY = 3; ///< Marks are rare; more than this on one day wait for the next one.

/** Take the damage of a storm or a meteorite out of the treasury of the town, as far as it holds. */
static void ChargeTreasury(uint32_t town, int64_t damage)
{
	const auto it = _town_treasury.accounts.find(town);
	if (it == _town_treasury.accounts.end() || damage <= 0) return;
	it->second.treasury -= std::min<int64_t>(std::max<int64_t>(it->second.treasury, 0), damage);
}

/** Make the weather event of a town: the real damage, the treasury, the story. */
static MarketEvent MakeWeather(MarketEventKind kind, int severity, uint32_t town, int64_t population, int64_t drought_days, int64_t minute)
{
	MarketEvent e = _market_events.NewEvent(kind, minute, severity, town);
	e.town = static_cast<uint16_t>(town);
	if (kind == MarketEventKind::WeatherDrought || kind == MarketEventKind::WeatherDroughtEnd) {
		e.fact_ppm = drought_days;
	} else {
		e.fact_ppm = MarketWeatherDamage(kind, severity, population, GetPriceIndexPpm());
		ChargeTreasury(town, e.fact_ppm);
	}
	return e;
}

/** One game day: real town news, the end of droughts, new weather. */
static void TownNewsDaily(const GameTimeSnapshot &now)
{
	if (_game_mode != GM_NORMAL) return;
	MarketEvents &ev = _market_events;
	const int64_t minute = static_cast<int64_t>(now.total_minutes);
	const int64_t day = minute / MARKET_MINUTES_PER_DAY;

	/* Snapshots of towns that are gone. */
	for (auto it = _town_news_snaps.begin(); it != _town_news_snaps.end();) {
		if (!Town::IsValidID(TownID{static_cast<uint16_t>(it->first)})) it = _town_news_snaps.erase(it); else ++it;
	}

	/* ---- True town news ---- */
	bool have_best = false;
	int best_sev = 0;
	MarketEventKind best_kind = MarketEventKind::TownGrew;
	uint32_t best_town = 0;
	int64_t best_fact = 0;
	size_t milestones = 0;
	for (const Town *t : Town::Iterate()) {
		const uint32_t idx = t->index.base();
		const int64_t pop = t->cache.population;
		auto it = _town_news_snaps.find(idx);
		if (it == _town_news_snaps.end()) { // First day we see this town: start the window, no news from nothing.
			_town_news_snaps[idx] = TownNewsSnap{pop, day, TownLevelOf(pop)};
			continue;
		}
		TownNewsSnap &sn = it->second;
		TownFacts f;
		f.town = static_cast<uint16_t>(idx);
		f.pop_now = pop;
		f.pop_prev = sn.pop_prev;
		f.level_prev = sn.level_prev;
		f.days = day - sn.day_prev;

		int64_t fact = 0; int sev = 1;
		if (TownEventFact(MarketEventKind::TownMilestone, f, fact, sev)) {
			if (milestones < TOWN_NEWS_MAX_MILESTONES_PER_DAY) {
				milestones++;
				MarketEvent e = ev.NewEvent(MarketEventKind::TownMilestone, minute, sev, idx);
				e.town = static_cast<uint16_t>(idx);
				e.fact_ppm = fact;
				PublishMarketEvent(e);
				sn.level_prev = TownLevelNext(sn.level_prev, pop);
			} // else the mark waits for tomorrow: level_prev stays.
		} else {
			sn.level_prev = TownLevelNext(sn.level_prev, pop);
		}

		if (f.days >= MARKET_TOWN_WINDOW_DAYS) {
			for (MarketEventKind kind : { MarketEventKind::TownGrew, MarketEventKind::TownShrank }) {
				if (!TownEventFact(kind, f, fact, sev)) continue;
				if (ev.RecentlyReportedTown(kind, idx, minute, MARKET_TOWN_REPEAT_DAYS)) continue;
				if (!have_best || sev > best_sev) { have_best = true; best_sev = sev; best_kind = kind; best_town = idx; best_fact = fact; }
			}
			sn.pop_prev = pop;
			sn.day_prev = day;
		}
	}
	if (have_best) { // At most one growth / shrink story per game day: the biggest.
		MarketEvent e = ev.NewEvent(best_kind, minute, best_sev, best_town);
		e.town = static_cast<uint16_t>(best_town);
		e.fact_ppm = best_fact;
		PublishMarketEvent(e);
	}

	/* ---- Weather ---- */
	/* The end of a drought is news. */
	for (const Town *t : Town::Iterate()) {
		int64_t days = 0;
		if (!ev.DroughtEndDue(t->index.base(), minute, days)) continue;
		PublishMarketEvent(MakeWeather(MarketEventKind::WeatherDroughtEnd, 1, t->index.base(), t->cache.population, days, minute));
	}

	/* New weather: the dice are a hash of the game seed, the day and the town. At most one new weather event per game day; when several towns are hit
	 * the same day the day number picks one, so no town is always first. A town that has weather on it is not hit again until it is over. */
	const uint64_t seed = static_cast<uint64_t>(_settings_game.game_creation.generation_seed);
	struct Hit { uint32_t town; int64_t population; MarketEventKind kind; int severity; int64_t drought_days; };
	std::vector<Hit> hits;
	for (const Town *t : Town::Iterate()) {
		if (t->cache.population < TOWN_MIN_POPULATION) continue;
		MarketEventKind kind; int severity; int64_t drought_days;
		if (!MarketWeatherRoll(seed, day, t->index.base(), kind, severity, drought_days)) continue;
		if (ev.WeatherActive(t->index.base(), minute)) continue;
		hits.push_back(Hit{t->index.base(), t->cache.population, kind, severity, drought_days});
	}
	if (!hits.empty()) {
		const Hit &h = hits[static_cast<size_t>(day) % hits.size()];
		PublishMarketEvent(MakeWeather(h.kind, h.severity, h.town, h.population, h.drought_days, minute));
	}
}

} // namespace

bool TownNewsForce(MarketEventKind kind, MarketEvent &out)
{
	const int64_t minute = static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes());
	for (const Town *t : Town::Iterate()) {
		if (t->cache.population < TOWN_MIN_POPULATION) continue;
		const uint32_t idx = t->index.base();
		MarketEvent e;
		if (MarketKindIsWeather(kind)) {
			e = MakeWeather(kind, 2, idx, t->cache.population, 15, minute);
		} else {
			e = _market_events.NewEvent(kind, minute, 2, idx);
			e.town = static_cast<uint16_t>(idx);
			e.fact_ppm = kind == MarketEventKind::TownGrew ? 200'000 : (kind == MarketEventKind::TownShrank ? -100'000 : TownLevelPopulation(std::min(TownLevelOf(t->cache.population) + 1, MARKET_TOWN_LEVEL_COUNT)));
		}
		PublishMarketEvent(e);
		out = e;
		return true;
	}
	return false;
}

/** Real town news and weather once per game day (after the town treasuries, which have priority 20). */
static const GameTimeSubscription _town_news_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &now) { TownNewsDaily(now); }, 30);

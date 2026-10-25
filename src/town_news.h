/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file town_news.h Real news about the towns (a town grew, shrank, passed a population mark) and the weather that really hits a town (storm, meteorite,
 * drought). The rules and numbers are in market_events.h (TownEventFact, MarketWeatherOf, MarketWeatherRoll); this file is the game layer (town_news.cpp).
 *
 * Weather really changes the town: the damage of a storm or a meteorite is taken from the town treasury (as far as it holds), the income of the citizens falls
 * while the weather lasts (town_treasury.cpp reads MarketEvents::TownIncomeDropPpm), and the town does not grow (RetailMarketManager.cpp reads
 * MarketEvents::TownGrowthHalted). Which weather is on is read from the saved news log, so nothing else is needed to know it after a load.
 */

#ifndef TOWN_NEWS_H
#define TOWN_NEWS_H

#include <cstdint>
#include <map>

#include "market_events.h"

/** What the daily scan remembers about one town. Saved (SRK_TOWN_NEWS), so a client that joins later makes the same news as the others. */
struct TownNewsSnap {
	int64_t pop_prev = 0;    ///< Population at the start of the window.
	int64_t day_prev = 0;    ///< Game day of that snapshot.
	int level_prev = 0;      ///< Number of population marks that were reached (see TownLevelOf).
};

/** One snapshot per town, key = index of the town. An ordered map, so every client walks the towns in the same order. */
extern std::map<uint32_t, TownNewsSnap> _town_news_snaps;

/** Forget the snapshots (new game, load). */
void TownNewsReset();

/** Test tool (console command stock_event, kinds TownGrew .. WeatherDroughtEnd): make that kind of news about the first town that has a treasury, with the real effects. @return false when there is no such town. */
bool TownNewsForce(MarketEventKind kind, MarketEvent &out);

#endif /* TOWN_NEWS_H */

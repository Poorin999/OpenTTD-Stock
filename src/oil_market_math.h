/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file oil_market_math.h The pure rules of the oil market (XTI): stock, scarcity, what wells may add, the pull of the world, the fuel index and the real news
 * from the stock. Standard library only, all integer, so they can be tested outside the game (oil_market_test.cpp) and give the same numbers on every client.
 * valuables.{h,cpp} and market_events.cpp use them.
 */

#ifndef OIL_MARKET_MATH_H
#define OIL_MARKET_MATH_H

#include <algorithm>
#include <cstdint>

namespace oilmath {

static constexpr int64_t PPM = 1'000'000;

static constexpr int64_t STOCK_START = 20'000'000;            ///< Barrels that the market has at the start.
static constexpr int64_t STOCK_CAP = STOCK_START * 3 / 2;     ///< Deliveries never fill the stock above this (1.5 x the start stock).
static constexpr int64_t BARRELS_PER_UNIT = 400;              ///< One delivered unit of oil adds at most this many barrels.
static constexpr int64_t WELL_DAILY_MAX = 40'000;             ///< The market takes at most this many barrels from wells per game day (0.2% of the start stock).
static constexpr int64_t REBALANCE_PERMILLE = 8;              ///< Every game day the world moves the stock this many thousandths of the way back to the start stock.
static constexpr int64_t SCARCITY_FLOOR_DIV = 4;              ///< The scarcity factor stops growing at 1/4 of the start stock (price x4).
static constexpr int64_t FUEL_PASS_PERCENT = 50;              ///< Vehicles feel this share of the change of the oil price.
static constexpr int64_t PRICE_INDEX_MIN_PPM = PPM / 2;       ///< The oil price index is limited to 0.5 ...
static constexpr int64_t PRICE_INDEX_MAX_PPM = 3 * PPM;       ///< ... 3.0.

/* Real news from the stock (permille of the start stock). */
static constexpr int64_t GLUT_PERMILLE_1 = 1'250;             ///< Stock at or above this: news "wells found / glut", severity 1.
static constexpr int64_t GLUT_PERMILLE_2 = 1'380;             ///< Severity 2.
static constexpr int64_t GLUT_PERMILLE_3 = 1'460;             ///< Severity 3.
static constexpr int64_t SHORT_PERMILLE_1 = 700;              ///< Stock at or below this: news "supply is short", severity 1.
static constexpr int64_t SHORT_PERMILLE_2 = 550;              ///< Severity 2.
static constexpr int64_t SHORT_PERMILLE_3 = 400;              ///< Severity 3.
static constexpr int64_t NEWS_GAP_DAYS = 20;                  ///< The same oil news does not come again within this many game days.

/** The price factor of the oil stock: start stock / stock now, x0.5 .. x4 (half the stock = twice the price). */
inline int64_t ScarcityPpm(int64_t stock)
{
	const int64_t floor_stock = STOCK_START / SCARCITY_FLOOR_DIV;
	return std::clamp<int64_t>(PPM * STOCK_START / std::max(stock, floor_stock), PPM / 2, SCARCITY_FLOOR_DIV * PPM);
}

/** What wells may add, in ppm: all of it up to the start stock, falling in a straight line to 0 at the cap (a full market does not want more). */
inline int64_t YieldPpm(int64_t stock)
{
	if (stock <= STOCK_START) return PPM;
	if (stock >= STOCK_CAP) return 0;
	return PPM * (STOCK_CAP - stock) / (STOCK_CAP - STOCK_START);
}

/** Barrels that a delivery of `count` units adds: limited by the yield, the room below the cap and what the wells may still add today. Never negative. */
inline int64_t DeliveredBarrels(int64_t count, int64_t stock, int64_t allowance)
{
	if (count <= 0) return 0;
	const int64_t room = STOCK_CAP - stock;
	if (room <= 0) return 0; // A stock above the cap is not cut, it gets nothing more.
	const int64_t barrels = count * BARRELS_PER_UNIT * YieldPpm(stock) / PPM;
	return std::max<int64_t>(0, std::min({barrels, room, allowance}));
}

/** One step of the world (steps_per_day steps make a game day): the stock moves a little towards the start stock; never a step of 0 while there is a gap. */
inline int64_t RebalanceStep(int64_t stock, int64_t steps_per_day)
{
	const int64_t gap = STOCK_START - stock;
	int64_t step = gap * REBALANCE_PERMILLE / (1000 * std::max<int64_t>(1, steps_per_day));
	if (step == 0 && gap != 0) step = gap > 0 ? 1 : -1;
	return step;
}

/** The oil price now against the oil price at the start, in ppm, limited. base and anchor are in the same unit. */
inline int64_t PriceIndexPpm(int64_t base, int64_t anchor)
{
	if (base <= 0 || anchor <= 0) return PPM;
	return std::clamp<int64_t>(base * PPM / anchor, PRICE_INDEX_MIN_PPM, PRICE_INDEX_MAX_PPM);
}

/** The fuel index (1,000,000 = plain price): the index of the news changed by FUEL_PASS_PERCENT of the change of the oil price. */
inline int64_t FuelIndexPpm(int64_t news_index_ppm, int64_t oil_index_ppm)
{
	const int64_t oil = PPM + (oil_index_ppm - PPM) * FUEL_PASS_PERCENT / 100;
	return std::clamp<int64_t>(news_index_ppm, 0, 10 * PPM) * oil / PPM;
}

/** The stock against the start stock in permille. */
inline int64_t StockPermille(int64_t stock)
{
	return stock * 1000 / STOCK_START;
}

/**
 * Whether the real stock is a reason for oil news.
 * @param stock_permille The stock against the start stock, in permille.
 * @param glut Set to true for too much oil (wells found, the price falls), false for too little (the supply is short, the price rises).
 * @param severity Set to 1 .. 3 by how far the stock is from the start stock.
 * @return True when there is a story.
 */
inline bool NewsFromStock(int64_t stock_permille, bool &glut, int &severity)
{
	if (stock_permille >= GLUT_PERMILLE_1) {
		glut = true;
		severity = stock_permille >= GLUT_PERMILLE_3 ? 3 : (stock_permille >= GLUT_PERMILLE_2 ? 2 : 1);
		return true;
	}
	if (stock_permille <= SHORT_PERMILLE_1) {
		glut = false;
		severity = stock_permille <= SHORT_PERMILLE_3 ? 3 : (stock_permille <= SHORT_PERMILLE_2 ? 2 : 1);
		return true;
	}
	return false;
}

} // namespace oilmath

#endif /* OIL_MARKET_MATH_H */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file town_treasury.h The citizens of a town and its treasury.
 *
 *  - Citizens:  every game day they earn money (per head, follows the price index of the game). Part of it is the town income tax and goes
 *               to the treasury, the rest is their savings.
 *  - Gold:      the citizens buy gold when the price is below its 30 day average and sell it when the price is above it (so they calm the
 *               market a little). They pay the same ask / bid price and commission as a company. The profit of a sale pays the town capital
 *               gains tax into the treasury. Their gold is not part of the gold reserve of the companies (it does not change the backing).
 *  - Treasury:  when it holds enough money, the town spends it on development: "fund new buildings" for 3 months (the same effect as the
 *               town action of the game).
 * All maths is integer and runs from the clock of the mod, so it is the same on every client.
 * Saved as SRK_TOWN (saveload/stock_sl.cpp), reset in StockMarket::Reset.
 */

#ifndef TOWN_TREASURY_H
#define TOWN_TREASURY_H

#include "stdafx.h"
#include <cstdint>
#include <map>

/* Rates. Change here and build again. Money amounts are in the base currency at price index 1.0. */
static constexpr int64_t TOWN_MIN_POPULATION = 200;          ///< Towns below this have no treasury and no gold trade.
static constexpr int64_t TOWN_INCOME_CENTS_PER_CAPITA_DAY = 50; ///< Income of one citizen per game day, in 1/100 of a money unit.
static constexpr int64_t TOWN_INCOME_TAX_PERCENT = 25;       ///< Town income tax: share of the income that goes to the treasury.
static constexpr int64_t TOWN_GAINS_TAX_PERCENT = 10;        ///< Town tax on the profit of a gold sale of the citizens.
static constexpr size_t  TOWN_GOLD_REF_DAYS = 30;            ///< The price is compared with the average of this many days.
static constexpr size_t  TOWN_GOLD_MIN_HISTORY_DAYS = 10;    ///< No trade before the chart has this many days.
static constexpr int64_t TOWN_GOLD_BUY_BELOW_PPM = 20'000;   ///< Buy when the price is this far (2%) below the average.
static constexpr int64_t TOWN_GOLD_SELL_ABOVE_PPM = 30'000;  ///< Sell when the price is this far (3%) above the average.
static constexpr int64_t TOWN_GOLD_BUY_SHARE_PERCENT = 50;   ///< A purchase uses at most this share of the savings.
static constexpr int64_t TOWN_GOLD_SELL_SHARE_PERCENT = 25;  ///< A sale gives at most this share of the gold (at least 1 ounce).
static constexpr int64_t TOWN_GOLD_DAILY_STOCK_DIV = 10'000; ///< One town trades at most 1/10000 of the market stock per day (at least 1 ounce).
static constexpr int64_t TOWN_DEVELOP_BASE_COST = 4'000;     ///< Cost of the development in the treasury, at price index 1.0 ...
static constexpr int64_t TOWN_DEVELOP_COST_PER_CAPITA = 1;   ///< ... plus this much per citizen.
static constexpr int64_t TOWN_DEVELOP_NEWS_MIN_POPULATION = 500; ///< Only towns of this size make a news item when they fund buildings.
static constexpr int64_t TOWN_REPORT_DAYS = 30;              ///< A news report of all towns every this many game days.

/** Money and gold of the citizens of one town, and its treasury. Money units unless the name says otherwise. */
struct TownAccount {
	int64_t gold_oz = 0;     ///< Ounces held by the citizens.
	int64_t gold_cost = 0;   ///< What that gold cost (without commission), the base of the profit. 0 = nothing held.
	int64_t savings = 0;     ///< Cash of the citizens.
	int64_t treasury = 0;    ///< Cash of the town.
	int64_t spent = 0;       ///< Everything the town has spent on development so far.
	int64_t taxed = 0;       ///< Everything the town has collected so far (income tax and gains tax).
};

/** All town accounts. Key = index of the town. An ordered map, so every client walks the towns in the same order. */
struct TownTreasuryManager {
	std::map<uint32_t, TownAccount> accounts;

	void Reset() { this->accounts.clear(); }
	void UpdateDaily();
	int64_t GetTotalTreasury() const;   ///< Money in all treasuries (what could be lent out later).
	int64_t GetTotalCitizenGold() const; ///< Ounces held by all citizens.
};

extern TownTreasuryManager _town_treasury;

#endif /* TOWN_TREASURY_H */

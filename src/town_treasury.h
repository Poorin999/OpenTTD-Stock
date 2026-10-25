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
 *  - Gold:      the citizens are four groups with four habits (citizen_gold.h): hoarders (keep buying, stock up when scared), speculators (follow
 *               the trend, take profit / stop loss, often lose), overtraders (trade all the time, pay the spread again and again, mostly lose) and
 *               balanced savers (keep a target share of their wealth in gold). Nobody is sure to win. Every group has its own cash, gold and results.
 *               They trade every game hour at the ask / bid price with a commission; the profit of a sale pays the town gains tax into the treasury.
 *               Their orders move the market stock and (a little) the price pressure. Their gold is not part of the gold reserve of the companies.
 *  - Treasury:  when it holds enough money, the town spends it on development: "fund new buildings" for 3 months (the same effect as the
 *               town action of the game).
 * All maths is integer and runs from the clock of the mod, so it is the same on every client.
 * Saved as SRK_TOWN (saveload/stock_sl.cpp), reset in StockMarket::Reset.
 */

#ifndef TOWN_TREASURY_H
#define TOWN_TREASURY_H

#include "stdafx.h"
#include "citizen_gold.h"
#include "citizen_terminal.h"
#include <array>
#include <cstdint>
#include <map>
#include <vector>

/* Rates. Change here and build again. Money amounts are in the base currency at price index 1.0. */
static constexpr int64_t TOWN_MIN_POPULATION = 200;          ///< Towns below this have no treasury and no gold trade.
static constexpr int64_t TOWN_INCOME_CENTS_PER_CAPITA_DAY = 50; ///< Income of one citizen per game day, in 1/100 of a money unit.
static constexpr int64_t TOWN_INCOME_TAX_PERCENT = 25;       ///< Town income tax: share of the income that goes to the treasury.
static constexpr int64_t TOWN_GAINS_TAX_PERCENT = 10;        ///< Town tax on the profit of a gold sale of the citizens.
static constexpr size_t  TOWN_GOLD_MIN_HISTORY_DAYS = 10;    ///< The 30 day average is only used when the chart has this many days.
static constexpr int64_t TOWN_CITIZEN_DEPTH_OUNCES = 500'000; ///< Market depth against the orders of the citizens: 25 times deeper than for a company (DEPTH_OUNCES 20,000), so many small orders do not push the price by themselves. Net buying of this many ounces = +100% pressure.
static constexpr int64_t TOWN_STOCK_FLOOR_PERCENT = 50;     ///< The citizens never take the market stock below this share of the start stock.
static constexpr int64_t TOWN_DEVELOP_BASE_COST = 4'000;     ///< Cost of the development in the treasury, at price index 1.0 ...
static constexpr int64_t TOWN_DEVELOP_COST_PER_CAPITA = 1;   ///< ... plus this much per citizen.
static constexpr int64_t TOWN_DEVELOP_NEWS_MIN_POPULATION = 500; ///< Only towns of this size make a news item when they fund buildings.
static constexpr int64_t TOWN_REPORT_DAYS = 30;              ///< A news report of all towns every this many game days.

/** Money and gold of the citizens of one town (four groups), and its treasury. Money units unless the name says otherwise. */
struct TownAccount {
	bool started = false;    ///< The groups have been given their start wealth.
	std::array<citizengold::Book, citizengold::STYLE_COUNT> books{}; ///< Cash, gold and results of each group (citizengold::Style order).
	int64_t treasury = 0;    ///< Cash of the town.
	int64_t spent = 0;       ///< Everything the town has spent on development so far.
	int64_t taxed = 0;       ///< Everything the town has collected so far (income tax and gains tax). Not saved.
	std::vector<citizengold::TermPos> positions{}; ///< Open Trading Terminal positions of the speculators and the overtraders (citizen_terminal.h). Saved (SRK_TOWN_POS).

	int64_t GoldCz() const { int64_t t = 0; for (const auto &b : this->books) t += b.gold_cz; return t; }
};

/** All town accounts. Key = index of the town. An ordered map, so every client walks the towns in the same order. */
struct TownTreasuryManager {
	std::map<uint32_t, TownAccount> accounts;
	int64_t stock_carry_cz = 0;      ///< Rest (hundredths of an ounce) of the market stock that the citizens took or gave back, less than one ounce. Not saved.
	int64_t pressure_carry_cz = 0;   ///< Rest of the price pressure of the citizens' orders. Not saved.

	void Reset() { this->accounts.clear(); this->stock_carry_cz = 0; this->pressure_carry_cz = 0; }
	void UpdateDaily();
	int64_t GetTotalTreasury() const;   ///< Money in all treasuries (what could be lent out later).
	int64_t GetTotalCitizenGold() const; ///< Ounces held by all citizens.
	citizengold::Book GetStyleTotals(citizengold::Style style) const; ///< The books of one group over all towns, added up (for reports).
};

extern TownTreasuryManager _town_treasury;

#endif /* TOWN_TREASURY_H */

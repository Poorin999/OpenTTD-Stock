/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file valuables.h The valuables of the trading terminal: gold, silver, platinum and diamonds, traded like shares (candles, SL/TP, leverage, wallets). */

#ifndef VALUABLES_H
#define VALUABLES_H

#include "company_type.h"
#include <array>
#include <cstddef>
#include <cstdint>

/** The valuables that can be traded in the terminal, in the order of the Market Watch. */
enum class ValuableId : uint8_t {
	Gold,      ///< XAU: follows the price of the gold market (1 unit = 1 ounce).
	Silver,    ///< XAG: follows gold with its own cycle and drift (1 unit = 10 ounces).
	Platinum,  ///< XPT: follows gold with its own cycle and drift (1 unit = 1 ounce).
	Diamond,   ///< XDI: only loosely linked to gold, the most volatile (1 unit = 1 carat).
	End,       ///< End marker.
};

/** Number of valuables. */
static constexpr size_t VALUABLE_COUNT = static_cast<size_t>(ValuableId::End);

/** The symbol number of the first valuable. The symbols 0 .. MAX_COMPANIES - 1 are the companies, the valuables follow them. */
static constexpr size_t STOCK_FIRST_VALUABLE = MAX_COMPANIES;

/** Number of symbols of the terminal: the companies, then the valuables. Arrays of the market have this size. */
static constexpr size_t STOCK_SYMBOLS = MAX_COMPANIES + VALUABLE_COUNT;

/** Whether a symbol number is a valuable (the number is not checked against anything else). */
inline bool IsValuableSymbol(size_t symbol)
{
	return symbol >= STOCK_FIRST_VALUABLE && symbol < STOCK_SYMBOLS;
}

static constexpr int64_t DRIFT_STEPS_PER_DAY = 24; ///< The drift steps once per game hour.
static constexpr int64_t DRIFT_STEP_DIVISOR = 5;   ///< One hourly step is 1/5 of the daily step (5 is about the square root of 24: the same spread over a day).
static constexpr int64_t DRIFT_PULL_DAYS = 60;     ///< The drift is pulled back to 0 by 1/60 per game day.

/** The fixed properties of a valuable. */
struct ValuableInfo {
	const char *code;       ///< Short code of the symbol, e.g. XAU.
	const char *name;       ///< Name.
	const char *unit;       ///< What one volume unit is (1/100 LOT), for the texts.
	int64_t anchor;         ///< Price of one unit at the start of the game, in money units, when gold costs its start price.
	int64_t link_ppm;       ///< How much of the change of the gold price this valuable follows (1,000,000 = all of it).
	int64_t leverage;       ///< Leverage: the margin of a position is its value divided by this.
	int64_t spread_ppm;     ///< Spread (ask - bid) in parts per million of the price; never less than 1 money unit.
	int64_t depth;          ///< Market depth: an order moves the price this many times less than the same order moves a share.
	int64_t cycle_ppm;      ///< Height of the slow cycle, in ppm of the price (+- this).
	int64_t cycle_days;     ///< Length of the slow cycle in game days.
	int64_t drift_max_ppm;  ///< The slow random drift stays inside +- this.
	int64_t drift_step_ppm; ///< Size of the drift step per game DAY; it is taken in DRIFT_STEPS_PER_DAY small steps (see StepSlow).
	int64_t noise_ppm;      ///< Largest fresh noise per economy day.
};

/**
 * The price model of the valuables. All the maths is integer and the random numbers come from a generator of its own that is
 * saved with the game, so every client of a network game gets exactly the same prices.
 * price = anchor x (1 + link x change of gold) x (1 + cycle) x (1 + drift) x (1 + noise)
 * The terminal turns this price into the price that the market glides to (see StockMarket::UpdateDaily).
 * Saved in the 'STCK' chunk (SRK_VALUABLES).
 */
struct ValuablesManager {
	std::array<int64_t, VALUABLE_COUNT> drift_ppm{};  ///< Slow random drift, in ppm; mean reverting.
	std::array<int64_t, VALUABLE_COUNT> noise_ppm{};  ///< Fast noise, in ppm; renewed every economy day.
	uint64_t rng_state = 0;                           ///< State of the random number generator; 0 = not seeded yet.

	/* The diamond market has a stock of its own (carats), separate from the gold stock. Mines add to it (DiamondsFromDelivery), the world
	 * cuts and sells diamonds out of it every hour; the less is left, the dearer the diamonds (GetBaseFixed). */
	static constexpr int64_t DIAMOND_STOCK_START = 10'000'000;                 ///< Carats that the market has at the start.
	static constexpr int64_t DIAMOND_STOCK_CAP = DIAMOND_STOCK_START * 3 / 2;  ///< Deliveries never fill the stock above this (1.5 x the start stock).
	static constexpr int64_t DIAMOND_CARATS_PER_UNIT = 500;                    ///< One delivered unit of cargo adds at most this many carats.
	static constexpr int64_t DIAMOND_MINE_DAILY_MAX = 20'000;                  ///< The market takes at most this many carats from mines per game day (0.2% of the start stock; a bucket that fills every hour).
	static constexpr int64_t DIAMOND_DEMAND_DAILY = 15'000;                    ///< The world takes this many carats a game day (0.15% of the start stock): cutting, jewellery.
	static constexpr int64_t DIAMOND_SCARCITY_FLOOR_DIV = 4;                   ///< The scarcity factor stops growing at 1/4 of the start stock (price x4).
	int64_t diamond_stock_carats = DIAMOND_STOCK_START; ///< Carats that the market still has. Saved (v4 of the SRK_VALUABLES row of the diamond, +1).
	int64_t diamond_mine_allowance = DIAMOND_MINE_DAILY_MAX; ///< Carats that mines may still add (a bucket, +1/24 of the daily maximum every hour). Not saved.
	int64_t DiamondScarcityPpm() const;
	static int64_t DiamondYieldPpm(int64_t stock_carats);

	void Reset();
	void StepFast();
	void StepSlow();
	int64_t GetBaseFixed(size_t index) const;
	uint64_t Next();
	int64_t RandomSigned(int64_t max);

	static const ValuableInfo &Info(size_t index);
};

extern ValuablesManager _valuables;

/** Diamonds that are delivered to a station add to the diamond stock (hook: GoldFromDelivery in gold_reserve.cpp). */
void DiamondsFromDelivery(uint count);

/** The symbol number of the diamonds (XDI) in the terminal. */
static constexpr size_t STOCK_DIAMOND_SYMBOL = STOCK_FIRST_VALUABLE + static_cast<size_t>(ValuableId::Diamond);

/** A trade in the terminal moves the real diamond stock: a buy (a long opens, a short closes) takes carats out of it, a sell (a short opens, a long closes) gives them back. */
void DiamondTerminalTrade(bool buy, uint32_t carats);

/** The leverage of a symbol as the divisor of the margin (10 for the shares of a company). */
int64_t GetStockMarginDivisor(size_t symbol);

#endif /* VALUABLES_H */

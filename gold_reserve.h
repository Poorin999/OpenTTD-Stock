/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gold_reserve.h The gold reserve: an organic gold price (it follows the economy of the game) and the gold that the companies hold. */

#ifndef GOLD_RESERVE_H
#define GOLD_RESERVE_H

#include "command_type.h"
#include "company_type.h"
#include "economy_type.h"
#include "cargo_type.h"
#include <array>
#include <cstdint>
#include <vector>

/** Credit standing of the money system, from the backing of the cash by gold. */
enum class CreditTier : uint8_t {
	C,   ///< Less than 6% backing.
	B,   ///< 6% and more.
	A,   ///< 12% and more.
	AA,  ///< 20% and more.
	AAA, ///< 30% and more.
};

/**
 * The gold market. All the maths is integer, so every client of a multiplayer game gets exactly the same price.
 * The price follows (see UpdateDaily): inflation, the cash in circulation (loans make it bigger), the gold that the companies buy
 * into the reserve (demand), and the safe haven effect when the company values of the map fall.
 * Saved in the 'STCK' chunk together with the wallets.
 */
/** The parts of the price that the economy of the game asks for (all factors in ppm, 1.0 = 1,000,000); see GoldReserveManager::GetFactors. */
struct GoldFactors {
	int64_t anchor_cents = 0;    ///< Base price x inflation: the line the price is kept near.
	int64_t inflation_ppm = 0;   ///< Inflation of the game.
	int64_t cash_ppm = 0;        ///< Cash in circulation against what inflation alone explains (half of the difference counts).
	int64_t demand_ppm = 0;      ///< Buying and selling of companies, citizens and news.
	int64_t scarcity_ppm = 0;    ///< Stock of the market.
	int64_t haven_ppm = 0;       ///< Safe haven: company values of the map fell.
	int64_t cycle_ppm = 0;       ///< Slow bull / bear waves (gold_market_math.h); a pure function of the game seed and the game day.
	int64_t noise_ppm = 0;       ///< Chance move of today (not part of the target: it is added to the day-end price, see GlideCents). Shown by the console only.
	int64_t raw_cents = 0;       ///< anchor x cash x demand x scarcity x haven, before the limit.
	int64_t target_cents = 0;    ///< The target after the soft and the hard limit around the anchor.
};

struct GoldReserveManager {
	static constexpr int64_t BASE_PRICE_CENTS = 2000 * 100; ///< Price of one ounce at the start (no inflation, baseline cash supply, no demand), in 1/100 of a money unit.
	static constexpr int64_t OUNCES_PER_TON = 32151;        ///< Troy ounces in one metric ton (rounded).
	static constexpr int64_t PPM = 1000000;                 ///< 1.0 in the factors of the price.
	static constexpr int64_t DEPTH_OUNCES = 20000;          ///< Market depth: buying this many ounces moves the price by +100%.
	static constexpr int64_t PRESSURE_MAX_PPM = 300000;     ///< The demand effect never moves the price more than 30%.
	static constexpr int64_t MARKET_STOCK_START = 1000 * OUNCES_PER_TON; ///< Ounces that the market has for sale at the start: 1000 metric tons (32,151,000 oz). Buying takes from it, selling gives back.
	static constexpr int64_t SPREAD_MIN_PPM = 2000;         ///< Smallest total spread: 0.2% of the base price (calm market, low inflation).
	static constexpr int64_t SPREAD_MAX_PPM = 5000;         ///< Largest total spread: 0.5% (heavy buying or selling, high inflation).
	static constexpr int64_t COMMISSION_PPM = 1000;         ///< Commission: 0.1% of the value of the order.
	static constexpr int64_t COMMISSION_MIN = 100;          ///< Smallest commission of one order, in money units (applies to small orders).
	static constexpr int64_t OUNCES_PER_DELIVERED_UNIT = 800;  ///< Gold or valuables delivered to a station: one unit of cargo (one bag) adds at most this many ounces to the market stock (was 5 with a start stock of 200,000 oz; scaled 160x with the 1000 ton start stock). The real yield falls as the stock fills, see MineYieldPpm.
	static constexpr int64_t MARKET_STOCK_CAP = MARKET_STOCK_START * 3 / 2; ///< Deliveries never fill the market stock above this (1.5 x the start stock, was 2 x).
	static constexpr int64_t MINE_DAILY_INTAKE_MAX_OZ = 64300; ///< The market takes at most this many ounces from mines per game day (0.2% of the start stock); more is not added.
	static constexpr int64_t SCARCITY_FLOOR_DIV = 4;        ///< The scarcity factor stops growing when the stock is 1/4 of the start stock (price x4).
	static constexpr int64_t TARGET_SOFT_DEV_PPM = 250'000; ///< The target may be 25% away from anchor (base price x inflation) without any limit; further away it moves only a third as fast.
	static constexpr int64_t TARGET_MAX_DEV_PPM = 500'000;  ///< The target is never more than 50% above or below the anchor (the scarcity, cash and demand together can not run away).
	static constexpr int64_t GLIDE_PERCENT_PER_DAY = 5;     ///< The price moves this share of the way to its target in one game day (it glides there minute by minute, not in one jump at midnight).
	static constexpr int64_t GLIDE_EVERY_MINUTES = 2;       ///< The live price is updated every this many game minutes (same pace as the Terminal).
	static constexpr size_t HISTORY_MAX_DAYS = 730;         ///< Number of daily prices that are kept for the gold chart (the oldest are dropped).


	int64_t price_cents = 0;      ///< Live base price of one ounce (the middle price) in 1/100 of a money unit. 0 = not started yet. Glides during the day (UpdateGlide); at midnight it is the last point of history_cents.
	int64_t spread_ppm = SPREAD_MIN_PPM; ///< Total spread (ask - bid) in ppm of the base price. Set once per game day (UpdateDaily), so it never jumps inside a day. Not saved: the minimum until the next day starts.
	int64_t baseline_supply = 0;  ///< Cash in circulation when the market started; the supply factor is measured against it.
	int64_t bank_reserve_oz = 0;  ///< The ounces that the central bank holds from the start.
	int64_t market_stock_oz = 0;  ///< Ounces that the market still has for sale. Less stock = a dearer price (scarcity).
	int64_t mine_intake_today_oz = 0; ///< Ounces that mines added today (limit MINE_DAILY_INTAKE_MAX_OZ). Cleared by UpdateDaily. Not saved: a load gives a new allowance for the rest of the day.
	int64_t pressure_ppm = 0;     ///< Demand effect of the buying and selling of the companies; fades by 3% a day.
	std::array<int64_t, MAX_COMPANIES> holdings_oz{}; ///< Ounces of every company.
	std::vector<int64_t> history_cents; ///< The price at the end of every game day (1/100 money units), oldest first, at most HISTORY_MAX_DAYS. Saved (SRK_GOLD_HISTORY). Drawn by the gold chart.
	std::array<int64_t, MAX_COMPANIES> holdings_cost{}; ///< What the gold of every company cost (money units, without commission): the base of the realised profit. 0 = unknown (old savegame).

	void Reset();
	void UpdateDaily(int64_t finished_day = -1);
	void UpdateGlide(uint64_t total_minutes);
	int64_t ComputeTargetCents() const;
	GoldFactors GetFactors() const;
	int64_t GetDayStartCents() const;
	static int64_t GlideCents(int64_t day_start, int64_t target, int64_t minute_of_day, int64_t noise_ppm = 0);
	static int64_t NoisePpmForDay(int64_t day);
	static int64_t CyclePpmForDay(int64_t day);

	/**
	 * How much of the normal yield a mine delivery adds, in ppm. Full while the stock is at or below the start stock; it falls in a
	 * straight line to 0 at MARKET_STOCK_CAP: a market that is full does not want more gold, so the mines slow down by themselves.
	 */
	static constexpr int64_t MineYieldPpm(int64_t stock_oz)
	{
		if (stock_oz <= MARKET_STOCK_START) return PPM;
		if (stock_oz >= MARKET_STOCK_CAP) return 0;
		return PPM * (MARKET_STOCK_CAP - stock_oz) / (MARKET_STOCK_CAP - MARKET_STOCK_START);
	}
	void PushHistory();

	int64_t GetPriceCents() const { return this->price_cents; }
	int64_t GetHoldings(CompanyID company) const;
	int64_t GetTotalReserveOunces() const;
	int64_t GetReserveValue() const;
	int64_t GetCirculatingCash() const;
	int64_t GetBackingBp() const;
	CreditTier GetCreditTier() const;
	int64_t ComputeSpreadPpm() const;
	int64_t GetSpreadPpm() const { return this->spread_ppm; }
	int64_t GetAskCents() const;
	int64_t GetBidCents() const;
	int64_t GetBuyCost(int64_t ounces) const;
	static int64_t GetCommission(int64_t value);
	int64_t GetBuyTotal(int64_t ounces) const;
	int64_t GetMaxBuyOunces(int64_t cash) const;
	int64_t GetSellProceeds(int64_t ounces) const;
	int64_t GetSellNet(int64_t ounces) const;
	bool EnsureStarted();
};

extern GoldReserveManager _gold_reserve;

const char *GetCreditTierName(CreditTier tier);

/** Gold (and valuables) delivered to a station feeds the stock of the gold market. Called from CargoPayment::PayFinalDelivery (economy.cpp). */
void GoldFromDelivery(CargoType cargo, uint count);

/** The total price of buying this many ounces now: ask price x amount + commission (what leaves the cash). */
int64_t CalculateBuyCost(uint32_t amount);
/** What selling this many ounces now puts into the cash: bid price x amount - commission. */
int64_t CalculateSellReturn(uint32_t amount);
/**
 * One gold trade of a company, paid from its unborrowed cash. The commission and the spread go into the budget of the central bank.
 * @param company The company.
 * @param is_buy True: buy for the vault; false: sell from the vault.
 * @param amount Ounces.
 * @param execute False: only check, change nothing.
 * @return Zero cost, or the error.
 */
CommandCost ExecuteGoldTrade(CompanyID company, bool is_buy, uint32_t amount, bool execute = true);

CommandCost CmdGoldBuy(DoCommandFlags flags, uint32_t ounces);
CommandCost CmdGoldSell(DoCommandFlags flags, uint32_t ounces);

DEF_CMD_TRAIT(CMD_GOLD_BUY,  CmdGoldBuy,  {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_GOLD_SELL, CmdGoldSell, {}, CommandType::MoneyManagement)

#endif /* GOLD_RESERVE_H */

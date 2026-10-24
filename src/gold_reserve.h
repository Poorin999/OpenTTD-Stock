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
struct GoldReserveManager {
	static constexpr int64_t BASE_PRICE_CENTS = 2000 * 100; ///< Price of one ounce at the start (no inflation, baseline cash supply, no demand), in 1/100 of a money unit.
	static constexpr int64_t OUNCES_PER_TON = 32151;        ///< Troy ounces in one metric ton (rounded).
	static constexpr int64_t PPM = 1000000;                 ///< 1.0 in the factors of the price.
	static constexpr int64_t DEPTH_OUNCES = 20000;          ///< Market depth: buying this many ounces moves the price by +100%.
	static constexpr int64_t PRESSURE_MAX_PPM = 300000;     ///< The demand effect never moves the price more than 30%.
	static constexpr int64_t MARKET_STOCK_START = 200000;   ///< Ounces that the market has for sale at the start. Buying takes from it, selling gives back.
	static constexpr int64_t SPREAD_MIN_PPM = 2000;         ///< Smallest total spread: 0.2% of the base price (calm market, low inflation).
	static constexpr int64_t SPREAD_MAX_PPM = 5000;         ///< Largest total spread: 0.5% (heavy buying or selling, high inflation).
	static constexpr int64_t COMMISSION_PPM = 1000;         ///< Commission: 0.1% of the value of the order.
	static constexpr int64_t COMMISSION_MIN = 100;          ///< Smallest commission of one order, in money units (applies to small orders).
	static constexpr int64_t OUNCES_PER_DELIVERED_UNIT = 10; ///< Gold or valuables delivered to a station: one unit of cargo (one bag) adds this many ounces to the market stock.
	static constexpr int64_t MARKET_STOCK_CAP = 2 * MARKET_STOCK_START; ///< Deliveries never fill the market stock above this.
	static constexpr int64_t SCARCITY_FLOOR_DIV = 4;        ///< The scarcity factor stops growing when the stock is 1/4 of the start stock (price x4).
	static constexpr size_t HISTORY_MAX_DAYS = 730;         ///< Number of daily prices that are kept for the gold chart (the oldest are dropped).


	int64_t price_cents = 0;      ///< Base market price of one ounce (the middle price) in 1/100 of a money unit. 0 = not started yet.
	int64_t spread_ppm = SPREAD_MIN_PPM; ///< Total spread (ask - bid) in ppm of the base price. Set once per game day (UpdateDaily), so it never jumps inside a day. Not saved: the minimum until the next day starts.
	int64_t baseline_supply = 0;  ///< Cash in circulation when the market started; the supply factor is measured against it.
	int64_t bank_reserve_oz = 0;  ///< The ounces that the central bank holds from the start.
	int64_t market_stock_oz = 0;  ///< Ounces that the market still has for sale. Less stock = a dearer price (scarcity).
	int64_t pressure_ppm = 0;     ///< Demand effect of the buying and selling of the companies; fades by 3% a day.
	std::array<int64_t, MAX_COMPANIES> holdings_oz{}; ///< Ounces of every company.
	std::vector<int64_t> history_cents; ///< The price at the end of every game day (1/100 money units), oldest first, at most HISTORY_MAX_DAYS. Saved (SRK_GOLD_HISTORY). Drawn by the gold chart.
	std::array<int64_t, MAX_COMPANIES> holdings_cost{}; ///< What the gold of every company cost (money units, without commission): the base of the realised profit. 0 = unknown (old savegame).

	void Reset();
	void UpdateDaily();
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

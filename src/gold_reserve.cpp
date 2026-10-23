/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gold_reserve.cpp Implementation of the gold reserve and its price. */

#include "stdafx.h"
#include "gold_reserve.h"
#include "central_bank.h"
#include "game_time/game_time_manager.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "economy_func.h"
#include "window_func.h"
#include "timer/timer.h"

#include "table/strings.h"

#include "safeguards.h"

GoldReserveManager _gold_reserve;

/** Forget everything: a new game, or a savegame that is loaded. */
void GoldReserveManager::Reset()
{
	this->price_cents = 0;
	this->baseline_supply = 0;
	this->bank_reserve_oz = 0;
	this->market_stock_oz = 0;
	this->pressure_ppm = 0;
	this->holdings_oz.fill(0);
}

const char *GetCreditTierName(CreditTier tier)
{
	switch (tier) {
		case CreditTier::AAA: return "AAA";
		case CreditTier::AA:  return "AA";
		case CreditTier::A:   return "A";
		case CreditTier::B:   return "B";
		default:              return "C";
	}
}

/** The cash in circulation: the money of all the companies (loans put money into the companies, so they make this bigger). */
int64_t GoldReserveManager::GetCirculatingCash() const
{
	int64_t sum = 0;
	for (const Company *c : Company::Iterate()) sum += std::max<int64_t>(0, static_cast<int64_t>(c->money));
	return sum;
}

/** Start the market with the first update: the baseline cash and the reserve of the central bank (8% of the cash in gold). */
bool GoldReserveManager::EnsureStarted()
{
	if (this->price_cents > 0) return true;
	const int64_t supply = this->GetCirculatingCash();
	if (supply <= 0) return false;
	this->price_cents = BASE_PRICE_CENTS;
	this->market_stock_oz = MARKET_STOCK_START;
	this->baseline_supply = supply;
	this->bank_reserve_oz = std::max<int64_t>(1, supply * 8 / 100 / (BASE_PRICE_CENTS / 100));
	return true;
}

int64_t GoldReserveManager::GetHoldings(CompanyID company) const
{
	return company.base() < MAX_COMPANIES ? this->holdings_oz[company.base()] : 0;
}

/** All the gold of the reserve: what the central bank holds and what the companies bought. */
int64_t GoldReserveManager::GetTotalReserveOunces() const
{
	int64_t sum = this->bank_reserve_oz;
	for (int64_t h : this->holdings_oz) sum += h;
	return sum;
}

int64_t GoldReserveManager::GetReserveValue() const
{
	return this->GetTotalReserveOunces() * this->price_cents / 100;
}

/** Backing ratio in basis points: value of the gold reserve / cash in circulation (10000 = 100%). */
int64_t GoldReserveManager::GetBackingBp() const
{
	const int64_t cash = this->GetCirculatingCash();
	if (cash <= 0) return 0;
	return std::min<int64_t>(1000000, this->GetReserveValue() * 10000 / cash);
}

CreditTier GoldReserveManager::GetCreditTier() const
{
	const int64_t bp = this->GetBackingBp();
	if (bp >= 3000) return CreditTier::AAA;
	if (bp >= 2000) return CreditTier::AA;
	if (bp >= 1200) return CreditTier::A;
	if (bp >= 600) return CreditTier::B;
	return CreditTier::C;
}

/** Money that buying this many ounces costs now (rounded up). */
int64_t GoldReserveManager::GetBuyCost(int64_t ounces) const
{
	return (ounces * this->price_cents + 99) / 100;
}

/** Money that selling this many ounces brings now (rounded down). */
int64_t GoldReserveManager::GetSellProceeds(int64_t ounces) const
{
	return ounces * this->price_cents / 100;
}

/** One day of the gold market: the price moves 5% of the way to the target that the economy of the game gives. */
void GoldReserveManager::UpdateDaily()
{
	/* The gold of a company that does not exist any more is gone. */
	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		if (!Company::IsValidID(CompanyID{i})) this->holdings_oz[i] = 0;
	}
	if (!this->EnsureStarted()) return;

	/* 1. Inflation of the game (1.0 = 1 << 16). */
	const int64_t inflation = std::clamp<int64_t>((static_cast<int64_t>(_economy.inflation_prices) * PPM) >> 16, PPM / 2, 20 * PPM);

	/* 2. Cash in circulation against the cash at the start: half of the change goes into the price. */
	const int64_t supply = this->GetCirculatingCash();
	const int64_t supply_factor = std::clamp<int64_t>(PPM + (supply - this->baseline_supply) * PPM / this->baseline_supply / 2, PPM / 2, 3 * PPM);

	/* 3. Demand of the reserve: what the companies bought (and sold) moves the price, and the effect fades. */
	this->pressure_ppm = std::clamp<int64_t>(this->pressure_ppm - this->pressure_ppm * 3 / 100, -PRESSURE_MAX_PPM, PRESSURE_MAX_PPM);
	const int64_t demand_factor = PPM + this->pressure_ppm;

	/* 3b. Scarcity: the market has a limited stock. Half of the stock left = twice the price, down to a quarter (x4). Stock above the start makes it cheaper. */
	const int64_t stock_floor = MARKET_STOCK_START / SCARCITY_FLOOR_DIV;
	const int64_t scarcity_factor = std::clamp<int64_t>(PPM * MARKET_STOCK_START / std::max(this->market_stock_oz, stock_floor), PPM / 2, SCARCITY_FLOOR_DIV * PPM);

	/* 4. Safe haven: when the company values of the map fall from one quarter to the next, gold gets up to 20% dearer. */
	int64_t now_value = 0, before_value = 0;
	for (const Company *c : Company::Iterate()) {
		now_value += static_cast<int64_t>(c->old_economy[0].company_value);
		before_value += static_cast<int64_t>(c->old_economy[1].company_value);
	}
	int64_t haven_factor = PPM;
	if (before_value > 0 && now_value < before_value) haven_factor += std::min<int64_t>(PPM / 5, (before_value - now_value) * PPM / before_value / 2);

	int64_t target = BASE_PRICE_CENTS;
	target = target * inflation / PPM;
	target = target * supply_factor / PPM;
	target = target * demand_factor / PPM;
	target = target * scarcity_factor / PPM;
	target = target * haven_factor / PPM;
	target = std::max<int64_t>(target, BASE_PRICE_CENTS / 10);

	int64_t step = (target - this->price_cents) * 5 / 100;
	if (step == 0 && target != this->price_cents) step = target > this->price_cents ? 1 : -1;
	this->price_cents = std::max<int64_t>(1, this->price_cents + step);

	InvalidateWindowData(WC_STOCK_MARKET, 0);
}

/* The market starts in the first game minute (new game or a savegame from before the gold), not on the first new day: until then the price is 0 and nothing can be traded. */
static const GameTimeSubscription _gold_start(GameTimeEvent::Minute, [](const GameTimeSnapshot &)
{
	if (_gold_reserve.GetPriceCents() <= 0) _gold_reserve.EnsureStarted();
});

static const GameTimeSubscription _gold_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &)
{
	_gold_reserve.UpdateDaily();
});

/** The gold windows have to redraw. */
static void InvalidateGoldWindows()
{
	InvalidateWindowData(WC_STOCK_MARKET, 0);
	InvalidateWindowData(WC_CENTRAL_BANK, 0);
}

/**
 * Buy gold for the reserve of the company. Strict fund isolation: the gold is paid from the unborrowed liquid cash only, so money of
 * the central bank can never be turned into gold. The money is an exchange for an asset, not an expense of the company.
 * @param flags Type of operation.
 * @param ounces Number of ounces, more than zero.
 * @return Zero cost, or an error.
 */
CommandCost CmdGoldBuy(DoCommandFlags flags, uint32_t ounces)
{
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;
	if (ounces == 0 || ounces > 1000000) return CommandCost(STR_GOLD_ERROR_BAD_QUANTITY);
	if (!_gold_reserve.EnsureStarted()) return CommandCost(STR_GOLD_ERROR_NO_MARKET);

	if (_gold_reserve.market_stock_oz < static_cast<int64_t>(ounces)) return CommandCost(STR_GOLD_ERROR_NO_STOCK);
	const int64_t cost = _gold_reserve.GetBuyCost(ounces);
	Company *c = Company::Get(_current_company);
	if (static_cast<int64_t>(c->money) >= cost && cost > GetUnborrowedLiquidCash(_current_company)) return CommandCost(STR_GOLD_ERROR_BORROWED);
	if (cost > GetUnborrowedLiquidCash(_current_company)) return CommandCost(STR_GOLD_ERROR_NO_CASH);

	if (flags.Test(DoCommandFlag::Execute)) {
		c->money -= Money(cost);
		_gold_reserve.holdings_oz[_current_company.base()] += ounces;
		_gold_reserve.market_stock_oz -= ounces;
		_gold_reserve.pressure_ppm = std::min<int64_t>(GoldReserveManager::PRESSURE_MAX_PPM,
				_gold_reserve.pressure_ppm + static_cast<int64_t>(ounces) * GoldReserveManager::PPM / GoldReserveManager::DEPTH_OUNCES);
		InvalidateGoldWindows();
	}
	return CommandCost();
}

/**
 * Sell gold of the reserve of the company. The money goes into the company cash.
 * @param flags Type of operation.
 * @param ounces Number of ounces, more than zero and at most what the company holds.
 * @return Zero cost, or an error.
 */
CommandCost CmdGoldSell(DoCommandFlags flags, uint32_t ounces)
{
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;
	if (ounces == 0 || ounces > 1000000) return CommandCost(STR_GOLD_ERROR_BAD_QUANTITY);
	if (!_gold_reserve.EnsureStarted()) return CommandCost(STR_GOLD_ERROR_NO_MARKET);
	if (_gold_reserve.GetHoldings(_current_company) < static_cast<int64_t>(ounces)) return CommandCost(STR_GOLD_ERROR_NO_GOLD);

	if (flags.Test(DoCommandFlag::Execute)) {
		Company::Get(_current_company)->money += Money(_gold_reserve.GetSellProceeds(ounces));
		_gold_reserve.holdings_oz[_current_company.base()] -= ounces;
		_gold_reserve.market_stock_oz += ounces;
		_gold_reserve.pressure_ppm = std::max<int64_t>(-GoldReserveManager::PRESSURE_MAX_PPM,
				_gold_reserve.pressure_ppm - static_cast<int64_t>(ounces) * GoldReserveManager::PPM / GoldReserveManager::DEPTH_OUNCES);
		InvalidateGoldWindows();
	}
	return CommandCost();
}

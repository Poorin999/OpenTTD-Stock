/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file gold_reserve.cpp Implementation of the gold reserve and its price. */

#include "stdafx.h"
#include "gold_reserve.h"
#include "valuables.h"
#include "gold_market_math.h"
#include "settings_type.h"
#include "tax.h"
#include "central_bank.h"
#include "zone_market.h"
#include "game_time/game_time_manager.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "economy_func.h"
#include "cargotype.h"
#include "window_func.h"
#include "timer/timer.h"

#include "table/strings.h"

#include "safeguards.h"

GoldReserveManager _gold_reserve;

/** The seed of the game for the chance parts of the gold price: the same on every client, different in every game. */
static uint64_t GoldSeed()
{
	return static_cast<uint64_t>(_settings_game.game_creation.generation_seed) ^ 0x601DB00CULL;
}

/** The chance move of a game day, in ppm (see gold_market_math.h). */
int64_t GoldReserveManager::NoisePpmForDay(int64_t day) { return goldmath::DailyNoisePpm(GoldSeed(), day); }

/** The slow bull / bear waves on a game day, in ppm. */
int64_t GoldReserveManager::CyclePpmForDay(int64_t day) { return goldmath::CyclePpm(GoldSeed(), day); }

/** Forget everything: a new game, or a savegame that is loaded. */
void GoldReserveManager::Reset()
{
	this->price_cents = 0;
	this->baseline_supply = 0;
	for (CentralBank &b : _banks) b.reserve_oz = 0; // The gold of the banks is part of the reserve.
	this->market_stock_oz = 0;
	this->pressure_ppm = 0;
	this->impact_ppm = 0;
	this->holdings_oz.fill(0);
	this->holdings_cost.fill(0);
	this->history_cents.clear();
	this->mine_news_oz = 0;
	this->day_shift_cents = 0;
	this->last_level_cents = 0;
	this->cash_avg = 0;
	this->spread_ppm = SPREAD_MIN_PPM;
}

/** Add the price of now to the history of the chart (one entry per game day). */
void GoldReserveManager::PushHistory()
{
	if (this->price_cents <= 0) return;
	this->history_cents.push_back(this->price_cents);
	if (this->history_cents.size() > HISTORY_MAX_DAYS) {
		this->history_cents.erase(this->history_cents.begin(), this->history_cents.begin() + (this->history_cents.size() - HISTORY_MAX_DAYS));
	}
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

/** Start the market with the first update: the baseline cash and the reserve of the central bank (START_BACKING_PERCENT of the notes in gold). */
bool GoldReserveManager::EnsureStarted()
{
	if (this->price_cents > 0) return true;
	if (_zone_market.count <= 0) return false; // The banks are the banks of the zones: wait until there are zones.
	const int64_t supply = this->GetNotesInCirculation();
	if (supply <= 0) return false;
	this->price_cents = BASE_PRICE_CENTS;
	this->market_stock_oz = MARKET_STOCK_START;
	this->baseline_supply = supply;
	/* Every bank starts with gold worth START_BACKING_PERCENT of the notes of its own zone, but at least of the average zone: a bank with (almost) no gold can not buy any (its daily limit is a part of what it holds), so a zone without companies at the start would stay without backing for ever. */
	for (int z = 0; z < BankCount(); z++) {
		const int64_t notes = std::max(BankNotes(_banks[z]), supply / BankCount());
		_banks[z].reserve_oz = std::max<int64_t>(1, notes * START_BACKING_PERCENT / 100 / (BASE_PRICE_CENTS / 100));
	}
	this->PushHistory(); // The first point of the gold chart: the price at the start.
	return true;
}

int64_t GoldReserveManager::GetHoldings(CompanyID company) const
{
	return company.base() < MAX_COMPANIES ? this->holdings_oz[company.base()] : 0;
}

/** All the gold of the reserve: what the central bank holds and what the companies bought. */
int64_t GoldReserveManager::GetTotalReserveOunces() const
{
	int64_t sum = BankReserveTotalOz();
	for (int64_t h : this->holdings_oz) sum += h;
	return sum;
}

int64_t GoldReserveManager::GetReserveValue() const
{
	return this->GetTotalReserveOunces() * this->price_cents / 100;
}

/** The ounces that back the notes: the gold of the central bank (and the gold of the companies only with BACKING_COUNTS_COMPANY_GOLD). */
int64_t GoldReserveManager::GetBackingOunces() const
{
	return BACKING_COUNTS_COMPANY_GOLD ? this->GetTotalReserveOunces() : BankReserveTotalOz();
}

int64_t GoldReserveManager::GetBackingValue() const
{
	return this->GetBackingOunces() * this->price_cents / 100;
}

/** Backing ratio in basis points: value of the gold reserve / cash in circulation (10000 = 100%). */
int64_t GoldReserveManager::GetBackingBp() const
{
	const int64_t cash = this->GetNotesInCirculation();
	if (cash <= 0) return 0;
	return std::min<int64_t>(1000000, this->GetBackingValue() * 10000 / cash);
}

CreditTier GoldReserveManager::GetCreditTier() const
{
	const int64_t bp = this->GetBackingBp();
	if (bp >= TIER_AAA_BP) return CreditTier::AAA;
	if (bp >= TIER_AA_BP) return CreditTier::AA;
	if (bp >= TIER_A_BP) return CreditTier::A;
	if (bp >= TIER_B_BP) return CreditTier::B;
	return CreditTier::C;
}

/**
 * The total spread now, in ppm of the base price: SPREAD_MIN_PPM (0.2%) in a calm market up to SPREAD_MAX_PPM (0.5%).
 * Stress = 60% the demand pressure of the companies (heavy buying or selling) + 40% the inflation of the game. Integer maths only.
 */
int64_t GoldReserveManager::ComputeSpreadPpm() const
{
	const int64_t pressure = std::min<int64_t>(PPM, std::abs(this->GetDemandPressurePpm()) * PPM / PRESSURE_MAX_PPM);
	const int64_t inflation = std::clamp<int64_t>((static_cast<int64_t>(_economy.inflation_prices) * PPM) >> 16, PPM / 2, 20 * PPM);
	const int64_t inflation_stress = std::min<int64_t>(PPM, std::max<int64_t>(0, inflation - PPM) * 2);
	const int64_t stress = (pressure * 6 + inflation_stress * 4) / 10;
	return SPREAD_MIN_PPM + (SPREAD_MAX_PPM - SPREAD_MIN_PPM) * stress / PPM;
}

/** Ask: the price at which the company buys = base price x (1 + spread / 2), in 1/100 of a money unit (rounded up). */
int64_t GoldReserveManager::GetAskCents() const
{
	return (this->price_cents * (PPM + this->spread_ppm / 2) + PPM - 1) / PPM;
}

/** Bid: the price at which the company sells = base price x (1 - spread / 2), in 1/100 of a money unit (rounded down). */
int64_t GoldReserveManager::GetBidCents() const
{
	return this->price_cents * (PPM - this->spread_ppm / 2) / PPM;
}

/** Money that buying this many ounces costs now, without the commission (rounded up). */
int64_t GoldReserveManager::GetBuyCost(int64_t ounces) const
{
	return (ounces * this->GetAskCents() + 99) / 100;
}

/** Commission of an order of this value: 0.1% (rounded up), but at least COMMISSION_MIN. Zero for an empty order. */
int64_t GoldReserveManager::GetCommission(int64_t value)
{
	if (value <= 0) return 0;
	return std::max<int64_t>(COMMISSION_MIN, (value * COMMISSION_PPM + PPM - 1) / PPM);
}

/** Everything that a purchase takes from the cash: the gold and the commission. */
int64_t GoldReserveManager::GetBuyTotal(int64_t ounces) const
{
	const int64_t cost = this->GetBuyCost(ounces);
	return cost + GetCommission(cost);
}

/** The most ounces that this cash pays, with the commission (not looking at the stock of the market). */
int64_t GoldReserveManager::GetMaxBuyOunces(int64_t cash) const
{
	const int64_t ask = this->GetAskCents();
	if (cash <= 0 || ask <= 0) return 0;
	int64_t n = cash * 100 / ask;
	while (n > 0 && this->GetBuyTotal(n) > cash) n--;
	while (this->GetBuyTotal(n + 1) <= cash) n++;
	return n;
}

/** Money that selling this many ounces brings now, before the commission (at the bid price, rounded down). */
int64_t GoldReserveManager::GetSellProceeds(int64_t ounces) const
{
	return ounces * this->GetBidCents() / 100;
}

/** What a sale puts into the cash: the proceeds minus the commission. */
int64_t GoldReserveManager::GetSellNet(int64_t ounces) const
{
	const int64_t gross = this->GetSellProceeds(ounces);
	return std::max<int64_t>(0, gross - GetCommission(gross));
}

void GoldFromDelivery(CargoType cargo, uint count)
{
	if (count == 0 || !IsValidCargoType(cargo)) return;
	const CargoLabel label = CargoSpec::Get(cargo)->label;
	if (label == CargoLabel{'DIAM'}) { DiamondsFromDelivery(count); return; } // Diamonds have a stock of their own (valuables.cpp).
	if (label == CargoLabel{'OIL_'}) { OilFromDelivery(count); return; } // So has oil (valuables.cpp); it never counts as gold.
	if (label != CT_GOLD && label != CargoLabel{'VALU'}) return;
	if (_gold_reserve.price_cents <= 0) return; // The market has not started.

	/* Mines react to the market: no room, no gold. A stock above the cap (older savegame) is left as it is, never cut. */
	const int64_t room = GoldReserveManager::MARKET_STOCK_CAP - _gold_reserve.market_stock_oz;
	if (room <= 0) return;
	int64_t ounces = static_cast<int64_t>(count) * GoldReserveManager::OUNCES_PER_DELIVERED_UNIT
			* GoldReserveManager::MineYieldPpm(_gold_reserve.market_stock_oz) / GoldReserveManager::PPM;
	ounces = std::min({ounces, room, GoldReserveManager::MINE_DAILY_INTAKE_MAX_OZ - _gold_reserve.mine_intake_today_oz});
	if (ounces <= 0) return;
	_gold_reserve.market_stock_oz += ounces;
	_gold_reserve.mine_intake_today_oz += ounces;
	_gold_reserve.mine_news_oz = std::min<int64_t>(1'000'000'000, _gold_reserve.mine_news_oz + ounces); // For the news about new mines (market_events.cpp).
}

/** The previous close: the last point of the chart (the price at midnight); the live price when the chart is empty (old savegame). Needs no saved value. */
int64_t GoldReserveManager::GetDayStartCents() const
{
	return this->history_cents.empty() ? this->price_cents : this->history_cents.back();
}

/**
 * A shock on the live price that does not come from the level (not used by the game now: MARKET_GOLD_REAL_SHOCK is false, because every news and every trade
 * already moves the level through pressure_ppm and the price follows the level at once, see UpdateGlide). A shock on top of that would count the same news twice.
 * @param ppm The change, 1,000,000 = 100%; negative = the price falls.
 */
void GoldReserveManager::ApplyShock(int64_t ppm)
{
	if (this->price_cents <= 0 || ppm == 0) return;
	const int64_t step = this->price_cents / PPM * ppm + this->price_cents % PPM * ppm / PPM;
	this->price_cents = std::max<int64_t>(1, this->price_cents + step);
	InvalidateWindowData(WC_STOCK_MARKET, 0);
}

/**
 * The parts of the price that the economy of the game asks for now. Pure function of the state, the same on every client.
 * The cash in circulation is compared with what inflation alone explains (baseline x inflation), not with the baseline: the pay of OpenTTD
 * grows with inflation, so the cash grows with it, and counting both would count inflation twice. The result may not run away from
 * anchor = base price x inflation: beyond 25% it moves a third as fast, beyond 50% it stops.
 */
GoldFactors GoldReserveManager::GetFactors() const
{
	GoldFactors f;

	/* 1. Inflation of the game (1.0 = 1 << 16). */
	f.inflation_ppm = std::clamp<int64_t>((static_cast<int64_t>(_economy.inflation_prices) * PPM) >> 16, PPM / 2, 20 * PPM);
	f.anchor_cents = BASE_PRICE_CENTS * f.inflation_ppm / PPM;

	/* 2. Cash in circulation against the cash that inflation explains: half of the difference goes into the price. */
	const int64_t supply = this->cash_avg > 0 ? this->cash_avg : this->GetNotesInCirculation(); // The average of about 7 days (UpdateGlide), not the till of this minute.
	const int64_t expected = this->baseline_supply * f.inflation_ppm / PPM;
	f.cash_ppm = PRICE_USES_CASH && expected > 0 ? std::clamp<int64_t>(PPM + (supply - expected) * PPM / expected / 2, PPM / 2, 3 * PPM) : PPM;

	/* 3. Demand of the reserve: what the companies, the citizens and the news bought (and sold). */
	f.demand_ppm = PPM + this->GetDemandPressurePpm();

	/* 3b. Scarcity: the market has a limited stock. Half of the stock left = twice the price, down to a quarter (x4). Stock above the start makes it cheaper. */
	const int64_t stock_floor = MARKET_STOCK_START / SCARCITY_FLOOR_DIV;
	f.scarcity_ppm = std::clamp<int64_t>(PPM * MARKET_STOCK_START / std::max(this->market_stock_oz, stock_floor), PPM / 2, SCARCITY_FLOOR_DIV * PPM);

	/* 4. Safe haven: when the company values of the map fall from one quarter to the next, gold gets up to 20% dearer. */
	int64_t now_value = 0, before_value = 0;
	for (const Company *c : Company::Iterate()) {
		now_value += static_cast<int64_t>(c->old_economy[0].company_value);
		before_value += static_cast<int64_t>(c->old_economy[1].company_value);
	}
	f.haven_ppm = PPM;
	if (before_value > 0 && now_value < before_value) f.haven_ppm += std::min<int64_t>(PPM / 5, (before_value - now_value) * PPM / before_value / 2);

	/* 5. Slow waves (bull and bear phases) of the game seed: they move the price up and down by turns (mean zero). */
	const int64_t today = static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes() / GameTimeManager::MINUTES_PER_DAY);
	f.cycle_ppm = PPM + (PRICE_USES_CYCLE ? CyclePpmForDay(today) : 0);
	f.noise_ppm = 0; // The chance part is a random walk of its own (UpdateGlide), not a factor of the level.

	int64_t raw = f.anchor_cents;
	raw = raw * f.cycle_ppm / PPM;
	raw = raw * f.cash_ppm / PPM;
	raw = raw * f.demand_ppm / PPM;
	raw = raw * f.haven_ppm / PPM;
	const int64_t raw_without_scarcity = std::max<int64_t>(raw, 1);
	f.raw_cents = std::max<int64_t>(raw_without_scarcity * f.scarcity_ppm / PPM, 1); // Everything, before the limit.

	/* The limit around the anchor holds for cash, demand and safe haven. Scarcity comes AFTER it: the less gold the market has, the dearer it gets
	 * (x0.5 .. x4 by the stock), also beyond +-50% of the anchor. */
	int64_t dev = f.anchor_cents > 0 ? (raw_without_scarcity - f.anchor_cents) * PPM / f.anchor_cents : 0;
	const int64_t sign = dev < 0 ? -1 : 1;
	int64_t mag = dev < 0 ? -dev : dev;
	if (mag > TARGET_SOFT_DEV_PPM) mag = TARGET_SOFT_DEV_PPM + (mag - TARGET_SOFT_DEV_PPM) / 3;
	mag = std::min(mag, TARGET_MAX_DEV_PPM);
	const int64_t limited_cents = f.anchor_cents * (PPM + sign * mag) / PPM;
	f.target_cents = std::max<int64_t>(limited_cents * f.scarcity_ppm / PPM, BASE_PRICE_CENTS / 10);
	return f;
}

/** The price that the economy of the game asks for now (see GetFactors). */
int64_t GoldReserveManager::ComputeTargetCents() const
{
	return this->GetFactors().target_cents;
}

static_assert(goldmath::STEPS_PER_DAY * GoldReserveManager::GLIDE_EVERY_MINUTES == 1440, "goldmath::STEPS_PER_DAY must match the 2 minute step of the price");

/**
 * Every 2 game minutes: ONE step of the price, the way a real market moves it (owner 2026-10-28: "like the real market, no balance").
 *  1. A chance move (goldmath::StepNoisePpm): a random walk with calm and stormy spells. Nobody can read the end of the day from the morning.
 *  2. The CHANGE of the level that the economy explains (ComputeTargetCents: inflation, cash, scarcity, safe haven, and the demand pressure of the trades and the news).
 *     The price follows a change of the level at once, by the same ratio, so a news or a big trade moves the price in the same minute. It is the change that counts,
 *     not the distance: the price is not led to a target on a fixed path, so tomorrow can not be read from the gap (a real price has no goal it glides to).
 * The old rule (5% of the way to the target per game day, a smooth line through the day) is gone. The state of the price is the price itself (price_cents, saved) and
 * the last level (last_level_cents, saved), so a client that joins later gets exactly the same price. Integer only.
 */
void GoldReserveManager::UpdateGlide(uint64_t total_minutes)
{
	if (this->price_cents <= 0) return;
	if (total_minutes % GLIDE_EVERY_MINUTES != 0) return;
	if (total_minutes % GameTimeManager::MINUTES_PER_DAY == 0) return; // Midnight belongs to UpdateDaily.

	int64_t p = this->price_cents;
	const int64_t noise = goldmath::StepNoisePpm(GoldSeed(), static_cast<int64_t>(total_minutes));
	p = p / PPM * (PPM + noise) + p % PPM * (PPM + noise) / PPM;

	if (PRICE_USES_CASH) this->cash_avg = goldmath::SmoothedCash(this->cash_avg, this->GetNotesInCirculation());
	this->impact_ppm = goldmath::FadedImpact(this->impact_ppm); // The push of the trades fades fast.
	this->pressure_ppm = goldmath::FadedPressureStep(this->pressure_ppm); // The news pressure fades 3% a day, in small steps (no step at midnight).
	this->market_stock_oz = goldmath::RebalancedStockStep(this->market_stock_oz, MARKET_STOCK_START); // The world refills / empties the stock in small steps.
	const int64_t level = this->ComputeTargetCents();
	if (this->last_level_cents > 0 && level > 0 && level != this->last_level_cents) p = p * level / this->last_level_cents;
	if (level > 0) this->last_level_cents = level;
	if (total_minutes % 60 == 0) p = goldmath::RevertedPriceHour(p, level); // The weak pull to the level, a 24th of the daily pull every game hour.

	this->price_cents = std::max<int64_t>(1, p);
}

/** One game day of the gold market: the price is already the end of the day (it is a random walk), so it goes into the chart; the weak pull, the stock and the demand effect fade. */
void GoldReserveManager::UpdateDaily([[maybe_unused]] int64_t finished_day)
{
	/* The gold of a company that does not exist any more is gone. */
	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		if (!Company::IsValidID(CompanyID{i})) { this->holdings_oz[i] = 0; this->holdings_cost[i] = 0; }
	}
	if (!this->EnsureStarted()) return;
	this->spread_ppm = this->ComputeSpreadPpm(); // Once per game day: the spread is stable inside a day.
	this->mine_intake_today_oz = 0;               // A new day: the mines may add gold again.

	/* The only pull of the price: 0.5% of the gap to the level of the economy per day. It keeps the price from running away for good (a real price is tied to money, inflation and mining
	 * by years, not by days) and takes no part in the daily path. last_level_cents is not touched: the next step still sees every change of the level. */
	/* (The pull, the stock rebalance and the fade of the news pressure used to be one step here at midnight: the price jumped every midnight. They run in the 2-minute steps now, see UpdateGlide.) */

	/* The world takes gold out of the stock and puts new gold in (scrap, jewellery, new mines): the stock moves back to its start level, so mines can not sink the price for good. */
	this->PushHistory();
	this->day_shift_cents = 0;

	/* The demand effect of the buying and selling fades by 3% a day. */
	InvalidateWindowData(WC_STOCK_MARKET, 0);
}

/* The market starts in the first game minute (new game or a savegame from before the gold), not on the first new day: until then the price is 0 and nothing can be traded. */
static const GameTimeSubscription _gold_start(GameTimeEvent::Minute, [](const GameTimeSnapshot &)
{
	if (_gold_reserve.GetPriceCents() <= 0) _gold_reserve.EnsureStarted();
});

static const GameTimeSubscription _gold_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &now)
{
	const int64_t day = static_cast<int64_t>(now.total_minutes / GameTimeManager::MINUTES_PER_DAY);
	_gold_reserve.UpdateDaily(std::max<int64_t>(0, day - 1)); // The day that has just ended.
});

/** The gold windows have to redraw. */
static void InvalidateGoldWindows()
{
	InvalidateWindowData(WC_STOCK_MARKET, 0);
	InvalidateWindowData(WC_CENTRAL_BANK, 0);
}

int64_t CalculateBuyCost(uint32_t amount)
{
	return _gold_reserve.GetBuyTotal(amount);
}

int64_t CalculateSellReturn(uint32_t amount)
{
	return _gold_reserve.GetSellNet(amount);
}

/** Why a gold trade cannot be done now, as an error; success when it can. */
static CommandCost CheckGoldTrade(CompanyID company, bool is_buy, uint32_t amount)
{
	if (!Company::IsValidID(company)) return CMD_ERROR;
	if (amount == 0 || amount > 1000000) return CommandCost(STR_GOLD_ERROR_BAD_QUANTITY);
	if (!_gold_reserve.EnsureStarted()) return CommandCost(STR_GOLD_ERROR_NO_MARKET);
	if (!is_buy) {
		if (_gold_reserve.GetHoldings(company) < static_cast<int64_t>(amount)) return CommandCost(STR_GOLD_ERROR_NO_GOLD);
		return CommandCost();
	}
	if (_gold_reserve.market_stock_oz < static_cast<int64_t>(amount)) return CommandCost(STR_GOLD_ERROR_NO_STOCK);
	const int64_t total = _gold_reserve.GetBuyTotal(amount);
	/* Fund isolation: only unborrowed cash buys gold. Money of the central bank can never be turned into gold. */
	if (static_cast<int64_t>(Company::Get(company)->money) >= total && total > GetUnborrowedLiquidCash(company)) return CommandCost(STR_GOLD_ERROR_BORROWED);
	if (total > GetUnborrowedLiquidCash(company)) return CommandCost(STR_GOLD_ERROR_NO_CASH);
	return CommandCost();
}

CommandCost ExecuteGoldTrade(CompanyID company, bool is_buy, uint32_t amount, bool execute)
{
	CommandCost check = CheckGoldTrade(company, is_buy, amount);
	if (check.Failed() || !execute) return check;

	const int64_t ounces = amount;
	const int i = company.base();
	Company *c = Company::Get(company);
	if (is_buy) {
		const int64_t cost = _gold_reserve.GetBuyCost(ounces);
		const int64_t commission = GoldReserveManager::GetCommission(cost);
		c->money -= Money(cost + commission);
		/* The spread (ask minus the base price) and the commission are the income of the central bank. */
		BankOfCompany(company).trading_revenue_cents += (_gold_reserve.GetAskCents() - _gold_reserve.price_cents) * ounces + commission * 100;
		RecordLedger(company, LedgerItem::GoldBuy, -cost);
		RecordLedger(company, LedgerItem::TradeCommission, -commission);
		_gold_reserve.holdings_cost[i] += cost;
		_gold_reserve.holdings_oz[i] += ounces;
		_gold_reserve.market_stock_oz -= ounces;
		_gold_reserve.AddTradeImpact(ounces * GoldReserveManager::PPM / GoldReserveManager::DEPTH_OUNCES);
	} else {
		const int64_t gross = _gold_reserve.GetSellProceeds(ounces);
		const int64_t commission = std::min(gross, GoldReserveManager::GetCommission(gross)); // Never more than the proceeds.
		const int64_t held = _gold_reserve.GetHoldings(company);
		int64_t &cost_base = _gold_reserve.holdings_cost[i];
		/* The part of the cost that belongs to the ounces sold; an unknown cost (old savegame) gives no profit and no loss. */
		const int64_t basis = cost_base > 0 ? (held == ounces ? cost_base : cost_base * ounces / held) : gross;
		cost_base = held == ounces ? 0 : std::max<int64_t>(0, cost_base - basis);
		c->money += Money(gross - commission);
		BankOfCompany(company).trading_revenue_cents += (_gold_reserve.price_cents - _gold_reserve.GetBidCents()) * ounces + commission * 100;
		RecordLedger(company, LedgerItem::GoldSell, gross);
		RecordLedger(company, LedgerItem::TradeCommission, -commission);
		RecordLedger(company, LedgerItem::RealizedProfit, gross - basis);
		c->money -= Money(TakeInvestorTax(company, LedgerItem::RealizedProfit, gross - basis)); // Capital gains tax on the profit of the sale.
		_gold_reserve.holdings_oz[i] -= ounces;
		_gold_reserve.market_stock_oz += ounces;
		_gold_reserve.AddTradeImpact(-(ounces * GoldReserveManager::PPM / GoldReserveManager::DEPTH_OUNCES));
	}
	InvalidateGoldWindows();
	return CommandCost();
}

/**
 * Buy gold for the reserve of the company (the command). Everything is in #ExecuteGoldTrade.
 * @param flags Type of operation.
 * @param ounces Number of ounces, more than zero.
 * @return Zero cost, or an error.
 */
CommandCost CmdGoldBuy(DoCommandFlags flags, uint32_t ounces)
{
	return ExecuteGoldTrade(_current_company, true, ounces, flags.Test(DoCommandFlag::Execute));
}

/**
 * Sell gold of the reserve of the company (the command). The money goes into the company cash.
 * @param flags Type of operation.
 * @param ounces Number of ounces, more than zero and at most what the company holds.
 * @return Zero cost, or an error.
 */
CommandCost CmdGoldSell(DoCommandFlags flags, uint32_t ounces)
{
	return ExecuteGoldTrade(_current_company, false, ounces, flags.Test(DoCommandFlag::Execute));
}

/* Every second game minute (the pace of the Terminal): the live price glides. */
static const GameTimeSubscription _gold_glide(GameTimeEvent::Minute, [](const GameTimeSnapshot &now)
{
	_gold_reserve.UpdateGlide(now.total_minutes);
});

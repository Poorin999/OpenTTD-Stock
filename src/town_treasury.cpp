/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file town_treasury.cpp The citizens of a town and its treasury, see town_treasury.h. */

#include "stdafx.h"
#include "town_treasury.h"
#include "3rdparty/fmt/format.h"
#include "openttd.h"
#include "central_bank.h"
#include "gold_reserve.h"
#include "game_time/game_time_manager.h"
#include "news_func.h"
#include "strings_func.h"
#include "town.h"
#include "table/strings.h"

#include <algorithm>

#include "safeguards.h"

TownTreasuryManager _town_treasury;

int64_t TownTreasuryManager::GetTotalTreasury() const
{
	int64_t total = 0;
	for (const auto &[key, a] : this->accounts) total += a.treasury;
	return total;
}

int64_t TownTreasuryManager::GetTotalCitizenGold() const
{
	int64_t total = 0;
	for (const auto &[key, a] : this->accounts) total += a.gold_oz;
	return total;
}

namespace {

/** Average gold price (1/100 money units) of the last #TOWN_GOLD_REF_DAYS days; 0 when the chart is too short. */
static int64_t ReferencePriceCents()
{
	const std::vector<int64_t> &h = _gold_reserve.history_cents;
	if (h.size() < TOWN_GOLD_MIN_HISTORY_DAYS) return 0;
	const size_t n = std::min(h.size(), TOWN_GOLD_REF_DAYS);
	int64_t sum = 0;
	for (size_t i = h.size() - n; i < h.size(); i++) sum += h[i];
	return sum / static_cast<int64_t>(n);
}

/** The citizens buy gold with part of their savings. */
static void CitizensBuy(TownAccount &a, int64_t cap_oz, int64_t scale_ppm)
{
	int64_t n = _gold_reserve.GetMaxBuyOunces(a.savings * TOWN_GOLD_BUY_SHARE_PERCENT / (100 * TOWN_GOLD_TRADES_PER_DAY) * scale_ppm / GoldReserveManager::PPM);
	cap_oz = std::max<int64_t>(1, cap_oz * scale_ppm / GoldReserveManager::PPM);
	/* Never take the market below half of its start stock: the citizens must not make gold scarce on their own. */
	n = std::min({n, cap_oz, _gold_reserve.market_stock_oz - GoldReserveManager::MARKET_STOCK_START / 2});
	if (n <= 0) return;

	const int64_t cost = _gold_reserve.GetBuyCost(n);
	const int64_t commission = GoldReserveManager::GetCommission(cost);
	if (cost + commission > a.savings) return;

	a.savings -= cost + commission;
	a.gold_oz += n;
	a.gold_cost += cost;
	/* Spread and commission are income of the central bank, like for a company. */
	_central_bank.trading_revenue_cents += (_gold_reserve.GetAskCents() - _gold_reserve.price_cents) * n + commission * 100;
	_gold_reserve.market_stock_oz -= n;
	_gold_reserve.pressure_ppm = std::min<int64_t>(GoldReserveManager::PRESSURE_MAX_PPM,
			_gold_reserve.pressure_ppm + n * GoldReserveManager::PPM / GoldReserveManager::DEPTH_OUNCES);
}

/** The citizens sell part of their gold; the tax on the profit goes to the treasury. */
static void CitizensSell(TownAccount &a, int64_t cap_oz, int64_t scale_ppm)
{
	if (a.gold_oz <= 0) return;
	const int64_t held = a.gold_oz;
	const int64_t n = std::min(held, std::max<int64_t>(1, std::min(cap_oz, std::max<int64_t>(1, held * TOWN_GOLD_SELL_SHARE_PERCENT / (100 * TOWN_GOLD_TRADES_PER_DAY) * scale_ppm / GoldReserveManager::PPM))));

	const int64_t gross = _gold_reserve.GetSellProceeds(n);
	const int64_t commission = std::min(gross, GoldReserveManager::GetCommission(gross));
	/* The part of the cost that belongs to the ounces sold; an unknown cost gives no profit and no tax. */
	const int64_t basis = a.gold_cost > 0 ? (held == n ? a.gold_cost : a.gold_cost * n / held) : gross;
	a.gold_cost = held == n ? 0 : std::max<int64_t>(0, a.gold_cost - basis);
	const int64_t profit = gross - basis;
	const int64_t tax = profit > 0 ? std::min(gross - commission, profit * TOWN_GAINS_TAX_PERCENT / 100) : 0;

	a.savings += gross - commission - tax;
	a.treasury += tax;
	a.taxed += tax;
	a.gold_oz -= n;
	_central_bank.trading_revenue_cents += (_gold_reserve.price_cents - _gold_reserve.GetBidCents()) * n + commission * 100;
	_gold_reserve.market_stock_oz += n;
	_gold_reserve.pressure_ppm = std::max<int64_t>(-GoldReserveManager::PRESSURE_MAX_PPM,
			_gold_reserve.pressure_ppm - n * GoldReserveManager::PPM / GoldReserveManager::DEPTH_OUNCES);
}

/**
 * One game hour: the citizens compare the price with the average of the last 30 days. Further than 0.2% below it they buy, further than 0.2% above
 * it they sell, so they trade in most hours. The size grows with the distance: full size (1/24 of the daily share) at 2% below (buy) or 3% above (sell),
 * smaller closer to the average. Only towns that already have an account (made by TownsDaily) and 10 days of price history are needed.
 */
static void TownsHourly()
{
	if (_game_mode != GM_NORMAL) return;
	if (!_gold_reserve.EnsureStarted()) return;

	const int64_t ref = ReferencePriceCents();
	if (ref <= 0) return;
	const int64_t PPM = GoldReserveManager::PPM;
	const int64_t dev_ppm = (_gold_reserve.price_cents - ref) * PPM / ref; // + dear, - cheap
	if (dev_ppm > -TOWN_GOLD_TRADE_BAND_PPM && dev_ppm < TOWN_GOLD_TRADE_BAND_PPM) return;

	const bool buy = dev_ppm < 0;
	const int64_t dist = buy ? -dev_ppm : dev_ppm;
	const int64_t full = buy ? TOWN_GOLD_BUY_BELOW_PPM : TOWN_GOLD_SELL_ABOVE_PPM;
	const int64_t scale_ppm = std::clamp<int64_t>(dist * PPM / full, PPM / 10, PPM);

	const int64_t cap_oz = std::max<int64_t>(1, _gold_reserve.market_stock_oz / (TOWN_GOLD_DAILY_STOCK_DIV * TOWN_GOLD_TRADES_PER_DAY));
	for (Town *t : Town::Iterate()) {
		if (t->cache.population < TOWN_MIN_POPULATION) continue;
		auto it = _town_treasury.accounts.find(t->index.base());
		if (it == _town_treasury.accounts.end()) continue;
		if (buy) CitizensBuy(it->second, cap_oz, scale_ppm); else CitizensSell(it->second, cap_oz, scale_ppm);
	}
}

/** One game day for all towns. */
static void TownsDaily(const GameTimeSnapshot &now)
{
	if (_game_mode != GM_NORMAL) return;

	const int64_t index_ppm = GetPriceIndexPpm();

	/* Accounts of towns that no longer exist are dropped (their gold and money are simply gone, like the gold of a removed company). */
	for (auto it = _town_treasury.accounts.begin(); it != _town_treasury.accounts.end();) {
		if (!Town::IsValidID(TownID{static_cast<uint16_t>(it->first)})) it = _town_treasury.accounts.erase(it); else ++it;
	}

	for (Town *t : Town::Iterate()) {
		const int64_t pop = t->cache.population;
		if (pop < TOWN_MIN_POPULATION) continue;
		TownAccount &a = _town_treasury.accounts[t->index.base()];

		/* Income of the citizens and the town income tax. */
		const int64_t income = pop * TOWN_INCOME_CENTS_PER_CAPITA_DAY * index_ppm / 100 / 1'000'000;
		const int64_t tax = income * TOWN_INCOME_TAX_PERCENT / 100;
		a.savings += income - tax;
		a.treasury += tax;
		a.taxed += tax;

		/* The gold trade of the citizens is not here any more: it runs every game hour (TownsHourly), so it does not hit the price at midnight. */

		/* Development: fund new buildings for 3 months, once the treasury can pay for it. */
		const int64_t cost = (TOWN_DEVELOP_BASE_COST + pop * TOWN_DEVELOP_COST_PER_CAPITA) * index_ppm / 1'000'000;
		if (t->fund_buildings_months == 0 && a.treasury >= cost) {
			a.treasury -= cost;
			a.spent += cost;
			t->fund_buildings_months = 3;
			t->grow_counter = 1;
			if (pop >= TOWN_DEVELOP_NEWS_MIN_POPULATION) {
				AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, fmt::format("{} spent {} of its treasury on new buildings.",
						GetString(STR_TOWN_NAME, t->index), GetString(STR_JUST_CURRENCY_LONG, cost))), NewsType::General, NewsStyle::Small, {});
			}
		}
	}

	/* A report of all towns now and then. */
	const uint64_t day = now.total_minutes / GameTimeManager::MINUTES_PER_DAY;
	if (day != 0 && day % TOWN_REPORT_DAYS == 0 && !_town_treasury.accounts.empty()) {
		AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, fmt::format("Towns: citizens hold {} oz of gold, the treasuries hold {}.",
				_town_treasury.GetTotalCitizenGold(), GetString(STR_JUST_CURRENCY_LONG, _town_treasury.GetTotalTreasury()))),
				NewsType::General, NewsStyle::Small, {});
	}
}

} // namespace

static const GameTimeSubscription _town_treasury_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &now) { TownsDaily(now); }, 20);

static const GameTimeSubscription _town_treasury_hourly(GameTimeEvent::Hour, [](const GameTimeSnapshot &) { TownsHourly(); }, 20);

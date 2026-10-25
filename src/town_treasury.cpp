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
#include "settings_type.h"
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
	for (const auto &[key, a] : this->accounts) total += a.GoldCz();
	return total / citizengold::CZ_PER_OZ;
}

citizengold::Book TownTreasuryManager::GetStyleTotals(citizengold::Style style) const
{
	citizengold::Book t;
	for (const auto &[key, a] : this->accounts) {
		const citizengold::Book &b = a.books[static_cast<size_t>(style)];
		t.gold_cz += b.gold_cz; t.cost += b.cost; t.cash += b.cash; t.profit += b.profit; t.wins += b.wins; t.losses += b.losses; t.turnover += b.turnover;
	}
	return t;
}

namespace {

static uint64_t CitizenSeed()
{
	return static_cast<uint64_t>(_settings_game.game_creation.generation_seed) ^ 0xC171BE0ULL;
}

/** Average of the last n daily closes (1/100 money units); 0 when there are fewer than min_days of them. */
static int64_t HistoryAverage(size_t n, size_t min_days)
{
	const std::vector<int64_t> &h = _gold_reserve.history_cents;
	if (h.size() < min_days || h.empty()) return 0;
	n = std::min(n, h.size());
	int64_t sum = 0;
	for (size_t i = h.size() - n; i < h.size(); i++) sum += h[i];
	return sum / static_cast<int64_t>(n);
}

/** Give the four groups of a town their start wealth: a part is cash, a part is already gold (bought from the market stock at the middle price). */
static void EndowTown(TownAccount &a, uint32_t town, int64_t population, int64_t index_ppm)
{
	using namespace citizengold;
	const auto share = GroupSharePpm(CitizenSeed(), town);
	const int64_t price = _gold_reserve.price_cents;
	for (size_t i = 0; i < STYLE_COUNT; i++) {
		const int64_t wealth = population * START_WEALTH_PER_CAPITA * share[i] / PPM * index_ppm / PPM;
		const int64_t gold_money = wealth * START_GOLD_SHARE_PPM / PPM;
		Book &b = a.books[i];
		b.cash += wealth - gold_money;
		if (price > 0 && gold_money > 0) {
			const int64_t cz = gold_money * 10'000 / price;
			b.gold_cz += cz;
			b.cost += gold_money;
			_town_treasury.stock_carry_cz -= cz;
		}
	}
	a.started = true;
}

/** Move whole ounces of the carries into the market stock and the demand pressure. */
static void SettleCarries(int64_t net_bought_cz)
{
	/* Stock: a buy takes gold out of the market, a sale gives it back. */
	int64_t &sc = _town_treasury.stock_carry_cz;
	sc -= net_bought_cz;
	const int64_t oz = sc / citizengold::CZ_PER_OZ;
	sc -= oz * citizengold::CZ_PER_OZ;
	_gold_reserve.market_stock_oz = std::max<int64_t>(0, _gold_reserve.market_stock_oz + oz);

	/* Pressure: net buying pushes the price up, net selling down, but 25 times less per ounce than a company order. */
	int64_t &pc = _town_treasury.pressure_carry_cz;
	pc += net_bought_cz;
	const int64_t per_ppm = TOWN_CITIZEN_DEPTH_OUNCES * citizengold::CZ_PER_OZ / GoldReserveManager::PPM; // cz for 1 ppm
	const int64_t ppm = pc / std::max<int64_t>(1, per_ppm);
	pc -= ppm * std::max<int64_t>(1, per_ppm);
	_gold_reserve.pressure_ppm = std::clamp<int64_t>(_gold_reserve.pressure_ppm + ppm, -GoldReserveManager::PRESSURE_MAX_PPM, GoldReserveManager::PRESSURE_MAX_PPM);
}

/**
 * One game hour: every group of every town looks at the market and decides (citizen_gold.h). Orders are filled at the ask / bid price with a
 * commission; the spread and the commission are income of the central bank, the profit of a sale pays the town gains tax. The dice are a hash of the
 * game seed, the town, the group and the game hour, so nothing is saved and every client decides the same.
 */
static void TownsHourly(const GameTimeSnapshot &now)
{
	using namespace citizengold;
	if (_game_mode != GM_NORMAL) return;
	if (!_gold_reserve.EnsureStarted()) return;

	const uint64_t hour = now.total_minutes / 60;
	MarketView m;
	m.price_cents = _gold_reserve.price_cents;
	m.ask_cents = _gold_reserve.GetAskCents();
	m.bid_cents = _gold_reserve.GetBidCents();
	m.avg7_cents = HistoryAverage(7, 3);
	m.avg30_cents = HistoryAverage(30, TOWN_GOLD_MIN_HISTORY_DAYS);
	m.prev_close_cents = _gold_reserve.GetDayStartCents();
	m.pressure_ppm = _gold_reserve.pressure_ppm;
	const int64_t floor_oz = GoldReserveManager::MARKET_STOCK_START * TOWN_STOCK_FLOOR_PERCENT / 100;
	m.stock_cz = std::max<int64_t>(0, _gold_reserve.market_stock_oz - floor_oz) * CZ_PER_OZ;

	int64_t net_bought_cz = 0;
	for (Town *t : Town::Iterate()) {
		if (t->cache.population < TOWN_MIN_POPULATION) continue;
		auto it = _town_treasury.accounts.find(t->index.base());
		if (it == _town_treasury.accounts.end() || !it->second.started) continue;
		TownAccount &a = it->second;
		for (size_t i = 0; i < STYLE_COUNT; i++) {
			Book &b = a.books[i];
			if (b.cooldown > 0) b.cooldown--;
			const Order o = Decide(static_cast<Style>(i), b, m, Mix(CitizenSeed(), t->index.base(), i, hour));
			if (o.cz <= 0) continue;
			if (o.buy) {
				const Fill f = ApplyBuy(b, o.cz, m.ask_cents, m.stock_cz);
				if (f.cz <= 0) continue;
				m.stock_cz -= f.cz;
				net_bought_cz += f.cz;
				_central_bank.trading_revenue_cents += (m.ask_cents - m.price_cents) * f.cz / CZ_PER_OZ + f.commission * 100;
			} else {
				const Fill f = ApplySell(b, o.cz, m.bid_cents);
				if (f.cz <= 0) continue;
				m.stock_cz += f.cz;
				net_bought_cz -= f.cz;
				_central_bank.trading_revenue_cents += (m.price_cents - m.bid_cents) * f.cz / CZ_PER_OZ + f.commission * 100;
				if (static_cast<Style>(i) == Style::Speculator && b.gold_cz == 0) b.cooldown = static_cast<int32_t>(SPEC_COOLDOWN_HOURS);
				/* The town gains tax on the profit of the sale. */
				if (f.gain > 0) {
					const int64_t tax = std::min(b.cash, f.gain * TOWN_GAINS_TAX_PERCENT / 100);
					b.cash -= tax;
					a.treasury += tax;
					a.taxed += tax;
				}
			}
		}
	}
	SettleCarries(net_bought_cz);
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

		/* A new account: the groups get their start wealth (needs the gold market for the price of their first gold). */
		if (!a.started) {
			if (!_gold_reserve.EnsureStarted()) continue;
			EndowTown(a, t->index.base(), pop, index_ppm);
		}

		/* Income of the citizens and the town income tax. The income goes to the four groups by their share of the town. */
		const int64_t income = pop * TOWN_INCOME_CENTS_PER_CAPITA_DAY * index_ppm / 100 / 1'000'000;
		const int64_t tax = income * TOWN_INCOME_TAX_PERCENT / 100;
		const auto share = citizengold::GroupSharePpm(CitizenSeed(), t->index.base());
		int64_t given = 0;
		for (size_t i = 0; i < citizengold::STYLE_COUNT; i++) {
			const int64_t part = i + 1 == citizengold::STYLE_COUNT ? (income - tax) - given : (income - tax) * share[i] / citizengold::PPM;
			a.books[i].cash += part;
			given += part;
		}
		a.treasury += tax;
		a.taxed += tax;

		/* The gold trade of the citizens runs every game hour (TownsHourly). */

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

	/* A report of all towns now and then: gold held and how the four groups did. */
	const uint64_t day = now.total_minutes / GameTimeManager::MINUTES_PER_DAY;
	if (day != 0 && day % TOWN_REPORT_DAYS == 0 && !_town_treasury.accounts.empty()) {
		using citizengold::Style;
		auto result = [](Style st) { return GetString(STR_JUST_CURRENCY_LONG, _town_treasury.GetStyleTotals(st).profit); };
		AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, fmt::format(
				"Towns: citizens hold {} oz of gold, the treasuries hold {}. Gold trading result so far: hoarders {}, speculators {}, overtraders {}, balanced savers {}.",
				_town_treasury.GetTotalCitizenGold(), GetString(STR_JUST_CURRENCY_LONG, _town_treasury.GetTotalTreasury()),
				result(Style::Hoarder), result(Style::Speculator), result(Style::Overtrader), result(Style::Balanced))),
				NewsType::General, NewsStyle::Small, {});
	}
}

} // namespace

static const GameTimeSubscription _town_treasury_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &now) { TownsDaily(now); }, 20);

static const GameTimeSubscription _town_treasury_hourly(GameTimeEvent::Hour, [](const GameTimeSnapshot &now) { TownsHourly(now); }, 20);

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
#include "stock_base.h"
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
	const auto share = citizengold::GroupSharePpm(CitizenSeed(), town);
	const int64_t price = _gold_reserve.price_cents;
	for (size_t i = 0; i < citizengold::STYLE_COUNT; i++) {
		const int64_t wealth = population * citizengold::START_WEALTH_PER_CAPITA * share[i] / citizengold::PPM * index_ppm / citizengold::PPM;
		const int64_t gold_money = wealth * citizengold::START_GOLD_SHARE_PPM / citizengold::PPM;
		citizengold::Book &b = a.books[i];
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

/* The citizens use the rules of the terminal (citizen_terminal.h repeats them so that it can be tested alone): they must stay equal. */
static_assert(citizengold::TERM_COMMISSION_PPM == STOCK_COMMISSION_PPM, "citizen_terminal.h: commission differs from stock_base.h");
static_assert(citizengold::TERM_SWAP_PPM_PER_DAY == STOCK_SWAP_PPM_PER_DAY, "citizen_terminal.h: swap differs from stock_base.h");
static_assert(citizengold::TERM_STOP_OUT_PERCENT == STOCK_STOP_OUT_LEVEL, "citizen_terminal.h: stop out level differs from stock_base.h");
static_assert(STOCK_SYMBOLS <= citizengold::TERM_MAX_SYMBOLS, "citizen_terminal.h: too many symbols");

using TermQuotes = std::array<citizengold::TermQuote, STOCK_SYMBOLS>;

/** The terminal as the citizens see it this hour: prices, the 3 and 10 day averages of the daily closes, the leverage. */
static void BuildTermQuotes(TermQuotes &qs)
{
	for (size_t s = 0; s < STOCK_SYMBOLS; s++) {
		citizengold::TermQuote &q = qs[s];
		q = citizengold::TermQuote{};
		if (!IsStockSymbolValid(s)) continue;
		const CompanyID c{static_cast<uint8_t>(s)};
		q.mid = GetStockPrice(c);
		q.bid = GetStockBid(c);
		q.ask = GetStockAsk(c);
		if (q.mid <= 0 || q.bid <= 0 || q.ask <= 0) continue;
		q.valid = true;
		q.divisor = GetStockMarginDivisor(s);
		const auto &closed = _stock_market.frames[s].Closed(GameTimeframe::D1);
		if (closed.size() >= 10) {
			int64_t sum3 = 0, sum10 = 0;
			for (size_t i = 0; i < 10; i++) {
				const int64_t v = closed[closed.size() - 1 - i].close;
				sum10 += v;
				if (i < 3) sum3 += v;
			}
			q.fast = sum3 / 3 / STOCK_PRICE_SCALE;
			q.slow = sum10 / 10 / STOCK_PRICE_SCALE;
		}
	}
}

/** Close a position of a group at the price of the terminal (with the market impact of a market order): the result goes into the cash and the book, a gain pays the town gains tax. */
static void CloseTermPosition(TownAccount &a, size_t index, const TermQuotes &qs)
{
	const citizengold::TermPos p = a.positions[index];
	a.positions.erase(a.positions.begin() + static_cast<ptrdiff_t>(index));
	citizengold::Book &b = a.books[static_cast<size_t>(p.style)];

	int64_t price = 0;
	if (p.symbol < STOCK_SYMBOLS && qs[p.symbol].valid) {
		price = GetStockExecutionPrice(CompanyID{p.symbol}, !p.is_long, static_cast<uint32_t>(p.volume));
		if (price > 0) _stock_market.ApplyImpact(CompanyID{p.symbol}, !p.is_long, static_cast<uint32_t>(p.volume));
	}
	if (price <= 0) price = p.entry; // the symbol is gone (a company that disappeared): closed flat
	const int64_t profit = p.is_long ? (price - p.entry) * p.volume : (p.entry - price) * p.volume;
	b.cash = std::max<int64_t>(0, b.cash + profit); // a loss is never more than the cash of the group
	b.profit += profit;
	b.turnover += price * p.volume;
	if (profit > 0) b.wins++; else if (profit < 0) b.losses++;
	if (profit > 0) {
		const int64_t tax = std::min(b.cash, profit * TOWN_GAINS_TAX_PERCENT / 100);
		b.cash -= tax;
		a.treasury += tax;
		a.taxed += tax;
	}
	bool any = false;
	for (const citizengold::TermPos &o : a.positions) any = any || o.style == p.style;
	if (!any && p.style == citizengold::Style::Speculator) b.cooldown = static_cast<int32_t>(citizengold::SPEC_COOLDOWN_HOURS);
}

/** Margin used and floating profit of the open positions of one group. */
static void TermTotals(const TownAccount &a, citizengold::Style style, const TermQuotes &qs, int64_t &used, int64_t &floating)
{
	used = floating = 0;
	for (const citizengold::TermPos &p : a.positions) {
		if (p.style != style || p.symbol >= STOCK_SYMBOLS) continue;
		const citizengold::TermQuote &q = qs[p.symbol];
		used += citizengold::TermMargin(p, q.divisor);
		if (q.valid) floating += citizengold::TermProfit(p, q);
	}
}

/** One game hour of the terminal trade of one group (speculators and overtraders): stop loss, take profit, stop out, then a new decision. */
static void TermStep(TownAccount &a, citizengold::Style style, uint64_t dice, const TermQuotes &qs)
{
	citizengold::Book &b = a.books[static_cast<size_t>(style)];

	/* Levels of the positions; a position of a symbol that is gone is closed. */
	for (size_t i = a.positions.size(); i-- > 0;) {
		const citizengold::TermPos &p = a.positions[i];
		if (p.style != style) continue;
		if (p.symbol >= STOCK_SYMBOLS || !qs[p.symbol].valid || citizengold::TermLevelHit(p, qs[p.symbol])) CloseTermPosition(a, i, qs);
	}

	/* Stop out: when the equity is under half of the used margin, the worst position is closed by force. */
	int64_t used = 0, floating = 0;
	for (;;) {
		TermTotals(a, style, qs, used, floating);
		if (used <= 0 || (b.cash + floating) * 100 >= used * citizengold::TERM_STOP_OUT_PERCENT) break;
		size_t worst = a.positions.size();
		int64_t worst_profit = INT64_MAX;
		for (size_t i = 0; i < a.positions.size(); i++) {
			const citizengold::TermPos &p = a.positions[i];
			if (p.style != style || p.symbol >= STOCK_SYMBOLS) continue;
			const int64_t pr = citizengold::TermProfit(p, qs[p.symbol]);
			if (pr < worst_profit) { worst_profit = pr; worst = i; }
		}
		if (worst == a.positions.size()) break;
		CloseTermPosition(a, worst, qs);
	}

	/* The decision. */
	std::vector<citizengold::TermPos> mine;
	std::vector<size_t> where;
	for (size_t i = 0; i < a.positions.size(); i++) {
		if (a.positions[i].style == style) { mine.push_back(a.positions[i]); where.push_back(i); }
	}
	TermTotals(a, style, qs, used, floating);
	const int64_t free_margin = b.cash + floating - used;
	const citizengold::TermAction act = citizengold::DecideTerminal(style, mine, free_margin, b.cooldown, qs.data(), qs.size(), dice);
	if (act.kind == citizengold::TermAction::Kind::Close) {
		if (act.close_index < where.size()) CloseTermPosition(a, where[act.close_index], qs);
		return;
	}
	if (act.kind != citizengold::TermAction::Kind::Open || act.symbol >= STOCK_SYMBOLS || !qs[act.symbol].valid) return;

	const CompanyID target{act.symbol};
	const int64_t price = GetStockExecutionPrice(target, act.is_long, static_cast<uint32_t>(act.units));
	if (price <= 0) return;
	const int64_t value = price * act.units;
	const int64_t commission = GetStockCommission(value);
	const int64_t margin = qs[act.symbol].divisor > 0 ? value / qs[act.symbol].divisor : 0;
	if (b.cash < commission || free_margin < margin + commission) return;
	b.cash -= commission;
	b.turnover += value;
	_central_bank.trading_revenue_cents += commission * 100;
	_stock_market.ApplyImpact(target, act.is_long, static_cast<uint32_t>(act.units));

	citizengold::TermPos p;
	p.style = style;
	p.symbol = act.symbol;
	p.is_long = act.is_long;
	p.volume = act.units;
	p.entry = price;
	const int64_t sl_d = std::max<int64_t>(1, price * act.sl_ppm / citizengold::PPM);
	const int64_t tp_d = std::max<int64_t>(1, price * act.tp_ppm / citizengold::PPM);
	if (act.sl_ppm > 0) p.sl = act.is_long ? std::max<int64_t>(1, price - sl_d) : price + sl_d;
	if (act.tp_ppm > 0) p.tp = act.is_long ? price + tp_d : std::max<int64_t>(1, price - tp_d);
	a.positions.push_back(p);
}

/** Swap: every game day a position costs a small part of its value. */
static void TermSwap(TownAccount &a)
{
	for (const citizengold::TermPos &p : a.positions) {
		int64_t price = p.entry;
		if (p.symbol < STOCK_SYMBOLS && IsStockSymbolValid(p.symbol)) {
			const int64_t mid = GetStockPrice(CompanyID{p.symbol});
			if (mid > 0) price = mid;
		}
		const int64_t swap = price * p.volume * citizengold::TERM_SWAP_PPM_PER_DAY / citizengold::PPM;
		citizengold::Book &b = a.books[static_cast<size_t>(p.style)];
		const int64_t paid = std::min(b.cash, swap);
		b.cash -= paid;
		_central_bank.trading_revenue_cents += paid * 100;
	}
}

/**
 * One game hour: every group of every town looks at the market and decides (citizen_gold.h). Orders are filled at the ask / bid price with a
 * commission; the spread and the commission are income of the central bank, the profit of a sale pays the town gains tax. The dice are a hash of the
 * game seed, the town, the group and the game hour, so nothing is saved and every client decides the same.
 */
static void TownsHourly(const GameTimeSnapshot &now)
{
	if (_game_mode != GM_NORMAL) return;
	if (!_gold_reserve.EnsureStarted()) return;

	const uint64_t hour = now.total_minutes / 60;
	citizengold::MarketView m;
	m.price_cents = _gold_reserve.price_cents;
	m.ask_cents = _gold_reserve.GetAskCents();
	m.bid_cents = _gold_reserve.GetBidCents();
	m.avg7_cents = HistoryAverage(7, 3);
	m.avg30_cents = HistoryAverage(30, TOWN_GOLD_MIN_HISTORY_DAYS);
	m.prev_close_cents = _gold_reserve.GetDayStartCents();
	m.pressure_ppm = _gold_reserve.pressure_ppm;
	const int64_t floor_oz = GoldReserveManager::MARKET_STOCK_START * TOWN_STOCK_FLOOR_PERCENT / 100;
	m.stock_cz = std::max<int64_t>(0, _gold_reserve.market_stock_oz - floor_oz) * citizengold::CZ_PER_OZ;

	TermQuotes term_quotes;
	BuildTermQuotes(term_quotes);

	int64_t net_bought_cz = 0;
	for (Town *t : Town::Iterate()) {
		if (t->cache.population < TOWN_MIN_POPULATION) continue;
		auto it = _town_treasury.accounts.find(t->index.base());
		if (it == _town_treasury.accounts.end() || !it->second.started) continue;
		TownAccount &a = it->second;
		for (size_t i = 0; i < citizengold::STYLE_COUNT; i++) {
			citizengold::Book &b = a.books[i];
			if (b.cooldown > 0) b.cooldown--;
			/* The speculators and the overtraders trade in the Trading Terminal (positions), the others buy physical gold. */
			if (static_cast<citizengold::Style>(i) == citizengold::Style::Speculator || static_cast<citizengold::Style>(i) == citizengold::Style::Overtrader) {
				TermStep(a, static_cast<citizengold::Style>(i), citizengold::Mix(CitizenSeed() ^ 0x7E2D, t->index.base(), i, hour), term_quotes);
				continue;
			}
			const citizengold::GoldOrder o = citizengold::Decide(static_cast<citizengold::Style>(i), b, m, citizengold::Mix(CitizenSeed(), t->index.base(), i, hour));
			if (o.cz <= 0) continue;
			if (o.buy) {
				const citizengold::Fill f = citizengold::ApplyBuy(b, o.cz, m.ask_cents, m.stock_cz);
				if (f.cz <= 0) continue;
				m.stock_cz -= f.cz;
				net_bought_cz += f.cz;
				_central_bank.trading_revenue_cents += (m.ask_cents - m.price_cents) * f.cz / citizengold::CZ_PER_OZ + f.commission * 100;
			} else {
				const citizengold::Fill f = citizengold::ApplySell(b, o.cz, m.bid_cents);
				if (f.cz <= 0) continue;
				m.stock_cz += f.cz;
				net_bought_cz -= f.cz;
				_central_bank.trading_revenue_cents += (m.price_cents - m.bid_cents) * f.cz / citizengold::CZ_PER_OZ + f.commission * 100;
				if (static_cast<citizengold::Style>(i) == citizengold::Style::Speculator && b.gold_cz == 0) b.cooldown = static_cast<int32_t>(citizengold::SPEC_COOLDOWN_HOURS);
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

		/* The gold trade and the terminal trade of the citizens run every game hour (TownsHourly); the swap of their positions is paid here. */
		TermSwap(a);

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
				"Towns: citizens hold {} oz of gold, the treasuries hold {}. Trading result so far (gold and terminal): hoarders {}, speculators {}, overtraders {}, balanced savers {}.",
				_town_treasury.GetTotalCitizenGold(), GetString(STR_JUST_CURRENCY_LONG, _town_treasury.GetTotalTreasury()),
				result(Style::Hoarder), result(Style::Speculator), result(Style::Overtrader), result(Style::Balanced))),
				NewsType::General, NewsStyle::Small, {});
	}
}

} // namespace

static const GameTimeSubscription _town_treasury_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &now) { TownsDaily(now); }, 20);

static const GameTimeSubscription _town_treasury_hourly(GameTimeEvent::Hour, [](const GameTimeSnapshot &now) { TownsHourly(now); }, 20);

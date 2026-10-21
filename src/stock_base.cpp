/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file stock_base.cpp Implementation of the in-game trading terminal. */

#include "stdafx.h"
#include "3rdparty/fmt/format.h"
#include "stock_base.h"
#include "stock_gui.h"
#include "central_bank.h"
#include "command_type.h"
#include "company_base.h"
#include "company_func.h"
#include "economy_func.h"
#include "news_func.h"
#include "strings_func.h"
#include "window_func.h"
#include "core/backup_type.hpp"
#include "timer/timer.h"
#include "timer/timer_game_economy.h"
#include "timer/timer_game_tick.h"

#include "table/strings.h"

#include "safeguards.h"

StockMarket _stock_market;

/** Whether the margin call warning has already been shown to a company; it is shown again after the level has recovered. Not game state. */
static std::array<std::array<bool, 256>, MAX_COMPANIES> _stock_margin_call_shown{}; ///< Per company and wallet: whether the margin call warning is showing.

/** Forget everything, e.g. when a new game is started. */
void StockMarket::Reset()
{
	for (auto &list : this->candles) list.clear();
	this->current.fill({});
	this->has_current.fill(false);
	this->price_fp.fill(0);
	this->base_fp.fill(0);
	this->impact.fill(0);
	this->valuation.fill({});
	_wallets.Reset();
	_central_bank.Reset();
	for (auto &list : this->daily) list.clear();
	this->daily_current.fill({});
	this->has_daily_current.fill(false);
	this->record_daily = false;
	this->history.clear();
	this->stats.fill({});
	this->positions.clear();
	this->pending.clear();
	this->next_id = 1;
	this->dividend_quarter_id = 0;
	this->dividend_delay = 0;
	this->dividend_quarter = 0;
	for (auto &row : _stock_margin_call_shown) row.fill(false);
}

/** The current economy month as year * 12 + month (month 0 = January). */
int32_t GetStockCurrentMonth()
{
	return TimerGameEconomy::year.base() * 12 + TimerGameEconomy::month;
}

/** Convert a price with #STOCK_PRICE_SCALE steps to whole money units, rounded, at least 1. */
static int64_t FixedToPrice(int64_t fixed)
{
	return std::max<int64_t>(1, (fixed + STOCK_PRICE_SCALE / 2) / STOCK_PRICE_SCALE);
}

/** The price a company is heading to: the valuation price, moved by the accumulated market impact. 0 when there is no valuation yet. */
static int64_t GetGoalFixed(size_t company)
{
	const int64_t base = _stock_market.base_fp[company];
	if (base <= 0) return 0;
	return std::max<int64_t>(STOCK_MIN_PRICE * STOCK_PRICE_SCALE, base * (STOCK_PPM + _stock_market.impact[company]) / STOCK_PPM);
}

/** How far an order of this volume moves the price, in parts per million. */
static int64_t GetImpactPpm(uint32_t volume)
{
	return static_cast<int64_t>(volume) * STOCK_IMPACT_FACTOR * STOCK_PPM / STOCK_SHARES_PER_COMPANY;
}

/**
 * Show a news item about a trading event. Only the company that the event is about is told, and a news item
 * does not change the game state, so this is safe in a network game.
 * @param owner Company that the event is about.
 * @param text The message.
 */
static void NotifyStockEvent(CompanyID owner, std::string text)
{
	if (owner != _local_company) return;
	AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, std::move(text)), NewsType::General, NewsStyle::Small, {});
}

/**
 * Valuation model, step 1: collect the figures of a company.
 * The book value is the company value; the earnings are the net result (income plus expenses, which are negative)
 * of the last four completed quarters. With fewer quarters on record the result is annualised.
 * Only game state is used, so every client of a network game gets the same figures.
 * @param c The company.
 * @return The valuation.
 */
static StockValuation ComputeStockValuation(const Company *c)
{
	StockValuation v;
	/* Money in the wallets still belongs to the company, so a deposit must not lower the share price. */
	/* A loan at the central bank raises the cash, but it has to be paid back, so it is taken off again. */
	v.book = static_cast<int64_t>(CalculateCompanyValue(c)) + GetWalletsTotal(c->index) - GetCentralBankDebt(c->index);

	int64_t sum = 0;
	for (size_t q = 0; q < STOCK_EARNINGS_QUARTERS; q++) {
		const CompanyEconomyEntry &e = c->old_economy[q];
		if (e.income == 0 && e.expenses == 0) continue; // This quarter has not happened yet.
		sum += static_cast<int64_t>(e.income) + static_cast<int64_t>(e.expenses);
		v.quarters++;
	}
	if (v.quarters > 0) v.earnings = sum * static_cast<int64_t>(STOCK_EARNINGS_QUARTERS) / v.quarters;
	return v;
}

/**
 * Valuation model, step 2: the mid price of one share.
 * The company is worth its book value plus STOCK_EARNINGS_MULTIPLE years of earnings. The earnings premium
 * is limited to +400% / -50% of the book value, so that one extreme year can not make the price jump
 * wildly or turn negative. Without earnings the price is simply the book value per share.
 * @param v The valuation.
 * @return The price times #STOCK_PRICE_SCALE, at least #STOCK_MIN_PRICE.
 */
static int64_t StockPriceFromValuation(const StockValuation &v)
{
	const int64_t book = std::max<int64_t>(v.book, 0);
	const int64_t premium = std::clamp<int64_t>(v.earnings * STOCK_EARNINGS_MULTIPLE, -book / 2, book * 4);
	return std::max<int64_t>(STOCK_MIN_PRICE * STOCK_PRICE_SCALE, (book + premium) * STOCK_PRICE_SCALE / STOCK_SHARES_PER_COMPANY);
}

/**
 * Get the mid price of one share of a company.
 * Only the stored prices are used, so the result is the same on all clients of a network game.
 * @param company The company.
 * @return The price, or 0 when the company does not exist.
 */
int64_t GetStockPrice(CompanyID company)
{
	if (!Company::IsValidID(company)) return 0;
	const int64_t stored = _stock_market.price_fp[company.base()];
	if (stored > 0) return FixedToPrice(stored);
	/* Before the first update there is no stored price yet. */
	return FixedToPrice(StockPriceFromValuation(ComputeStockValuation(Company::Get(company))));
}

/**
 * Market capitalisation of a company: the price of one share times the number of shares.
 * @param company The company.
 * @return The market cap, or 0 when the company does not exist.
 */
int64_t GetStockMarketCap(CompanyID company)
{
	return GetStockPrice(company) * STOCK_SHARES_PER_COMPANY;
}

/**
 * The difference between Ask and Bid for a given mid price.
 * Normally this is 0.5% of the price, at least 1. When the market cap is lower than #STOCK_UNDERVALUED_PERCENT of the
 * book value the company looks cheap, so the spread tightens (the lower the market cap, the tighter, but not below
 * #STOCK_UNDERVALUED_MIN_SPREAD_PERCENT of the normal spread) to attract buyers. Because the market cap follows
 * the price, selling pressure that pushes the price down also makes the spread tighter: a feedback loop.
 * Whole money units are used, so a normal spread of 1 can only tighten to 0.
 * @param company The company.
 * @param mid The mid price of one share.
 * @return The spread, 0 or more.
 */
static int64_t GetSpread(CompanyID company, int64_t mid)
{
	const int64_t normal = std::max<int64_t>(1, mid / 200);

	const int64_t book = _stock_market.valuation[company.base()].book;
	if (book <= 0) return normal; // No valuation yet.

	const int64_t cap_percent = GetStockMarketCap(company) * 100 / book;
	if (cap_percent >= STOCK_UNDERVALUED_PERCENT) return normal;

	/* 100% at the threshold, getting smaller together with the market cap. */
	const int64_t factor = std::clamp<int64_t>(cap_percent * 100 / STOCK_UNDERVALUED_PERCENT, STOCK_UNDERVALUED_MIN_SPREAD_PERCENT, 100);
	return (normal * factor + 50) / 100;
}

/** Price at which you can sell (and at which long positions are closed). */
int64_t GetStockBid(CompanyID company)
{
	const int64_t mid = GetStockPrice(company);
	if (mid == 0) return 0;
	return std::max<int64_t>(1, mid - GetSpread(company, mid) / 2);
}

/** Price at which you can buy (and at which short positions are closed). */
int64_t GetStockAsk(CompanyID company)
{
	const int64_t mid = GetStockPrice(company);
	if (mid == 0) return 0;
	return std::max<int64_t>(1, mid - GetSpread(company, mid) / 2) + GetSpread(company, mid);
}

/**
 * The price that a market order of a given volume is executed at: the Ask (buy) or Bid (sell) price,
 * moved against the trader by half of the market impact of the order itself. The other half is the shift
 * of the market price after the trade. Because of this, opening and closing a trade right away never makes a profit.
 * @param company The company whose shares are traded.
 * @param buy True for a buy (the price goes up), false for a sell (the price goes down).
 * @param volume Volume of the order.
 * @return The price, or 0 when the company does not exist.
 */
int64_t GetStockExecutionPrice(CompanyID company, bool buy, uint32_t volume)
{
	const int64_t quote = buy ? GetStockAsk(company) : GetStockBid(company);
	if (quote == 0) return 0;
	const int64_t half = GetImpactPpm(volume) / 2;
	return std::max<int64_t>(1, quote * (STOCK_PPM + (buy ? half : -half)) / STOCK_PPM);
}

/**
 * Commission for a new trade.
 * @param value Value of the position: price times volume.
 * @return The commission, rounded up.
 */
int64_t GetStockCommission(int64_t value)
{
	if (value <= 0) return 0;
	return (value * STOCK_COMMISSION_PPM + STOCK_PPM - 1) / STOCK_PPM;
}

/**
 * Floating profit or loss of a position, if it would be closed now.
 * @param position The position.
 * @return Profit (negative for a loss) in money units.
 */
int64_t GetStockProfit(const StockOrder &position)
{
	const CompanyID target{position.target};
	const int64_t units = position.volume; // hundredths of a LOT, which is one share per unit.
	if (IsStockBuy(position.type)) return (GetStockBid(target) - position.price) * units;
	return (position.price - GetStockAsk(target)) * units;
}

/**
 * Figures for the summary row of a symbol: EPS, P/E and dividend yield, based on the daily valuation.
 * The yield is the dividend (STOCK_PAYOUT_PERCENT of the earnings) that is paid out per year, divided by the price.
 * @param company The company whose shares are shown.
 * @return The figures; see #StockSummary for what is missing when there are no (positive) earnings.
 */
StockSummary GetStockSummary(CompanyID company)
{
	StockSummary s;
	if (!Company::IsValidID(company)) return s;

	const StockValuation &v = _stock_market.valuation[company.base()];
	if (v.quarters == 0) return s;

	s.has_earnings = true;
	s.eps = v.earnings / STOCK_SHARES_PER_COMPANY;

	const int64_t price = GetStockPrice(company);
	if (v.earnings > 0 && v.book > 0 && price > 0) {
		s.has_pe = true;
		s.pe_tenths = price * STOCK_SHARES_PER_COMPANY * 10 / v.earnings;
		s.yield_tenths = v.earnings * STOCK_PAYOUT_PERCENT * 10 / (price * STOCK_SHARES_PER_COMPANY);
	}
	return s;
}

/** Margin that is held for a position. */
int64_t GetStockMargin(const StockOrder &position)
{
	return position.price * static_cast<int64_t>(position.volume) / STOCK_MARGIN_DIVISOR;
}

/** Sum of the margins of the open positions in one wallet. */
int64_t GetStockUsedMargin(CompanyID owner, uint8_t wallet)
{
	int64_t sum = 0;
	for (const StockOrder &p : _stock_market.positions) {
		if (p.owner == owner.base() && p.wallet == wallet) sum += GetStockMargin(p);
	}
	return sum;
}

/** Sum of the floating profit of the open positions in one wallet. */
int64_t GetStockFloatingProfit(CompanyID owner, uint8_t wallet)
{
	int64_t sum = 0;
	for (const StockOrder &p : _stock_market.positions) {
		if (p.owner == owner.base() && p.wallet == wallet && Company::IsValidID(CompanyID{p.target})) sum += GetStockProfit(p);
	}
	return sum;
}

/** Equity of a wallet: its money plus the floating profit of its open positions. Every wallet is judged on its own. */
int64_t GetStockEquity(CompanyID owner, uint8_t wallet)
{
	if (!Company::IsValidID(owner)) return 0;
	return GetWalletBalance(owner, wallet) + GetStockFloatingProfit(owner, wallet);
}

/** Equity of a wallet minus the margin that its open positions hold. */
int64_t GetStockFreeMargin(CompanyID owner, uint8_t wallet)
{
	return GetStockEquity(owner, wallet) - GetStockUsedMargin(owner, wallet);
}

/**
 * Margin level of a wallet: equity divided by the used margin, in percent.
 * @param owner The company.
 * @param wallet The wallet.
 * @return The margin level, or -1 when the wallet has no open positions.
 */
int64_t GetStockMarginLevel(CompanyID owner, uint8_t wallet)
{
	const int64_t used = GetStockUsedMargin(owner, wallet);
	if (used <= 0) return -1;
	const int64_t equity = GetStockEquity(owner, wallet);
	if (equity <= 0) return 0;
	/* Split the division so that a huge equity can not overflow. */
	return std::min<int64_t>(equity / used, 99999) * 100 + (equity % used) * 100 / used;
}

/**
 * Add a realised profit or loss to a wallet. Also used for dividends (profit) and fees (loss).
 * A trading wallet never goes below zero; see AddToWallet().
 * @param owner Company that gets (or pays) the money.
 * @param wallet Wallet of the company; WALLET_CASH for the company cash.
 * @param profit Profit; negative for a loss.
 */
void PayStockProfit(CompanyID owner, uint8_t wallet, int64_t profit)
{
	if (!Company::IsValidID(owner)) return;
	AddToWallet(owner, wallet, profit);
}

StockOrder *StockMarket::FindPosition(uint32_t id, CompanyID owner)
{
	for (StockOrder &p : this->positions) {
		if (p.id == id && p.owner == owner.base()) return &p;
	}
	return nullptr;
}

StockOrder *StockMarket::FindPending(uint32_t id, CompanyID owner)
{
	for (StockOrder &p : this->pending) {
		if (p.id == id && p.owner == owner.base()) return &p;
	}
	return nullptr;
}

/** Number of positions and pending orders of a company. */
size_t StockMarket::CountOrders(CompanyID owner) const
{
	size_t n = 0;
	for (const StockOrder &p : this->positions) n += (p.owner == owner.base());
	for (const StockOrder &p : this->pending) n += (p.owner == owner.base());
	return n;
}

/** Number of positions and pending orders that use a wallet. */
size_t StockMarket::CountWalletOrders(CompanyID owner, uint8_t wallet) const
{
	size_t n = 0;
	for (const StockOrder &p : this->positions) n += (p.owner == owner.base() && p.wallet == wallet);
	for (const StockOrder &p : this->pending) n += (p.owner == owner.base() && p.wallet == wallet);
	return n;
}

/**
 * Market impact: buying moves the mid price up, selling moves it down, in proportion to the volume
 * of the order relative to the number of shares of the company. The shift is applied to the price at once.
 * It is also remembered in #impact, so the valuation price that the market glides to includes it; the
 * remembered part fades away over the following days.
 * @param target Company whose shares are traded.
 * @param buy True when the order bought shares, false when it sold shares.
 * @param volume Volume of the order.
 */
void StockMarket::ApplyImpact(CompanyID target, bool buy, uint32_t volume)
{
	if (!Company::IsValidID(target)) return;
	const size_t i = target.base();
	if (this->price_fp[i] <= 0) return;

	/* Every executed order is traded volume: it goes into the running candles, where the chart shows it as bars. */
	if (this->has_current[i]) this->current[i].volume += volume;
	if (this->record_daily && this->has_daily_current[i]) this->daily_current[i].volume += volume;

	const int64_t shift = buy ? GetImpactPpm(volume) : -GetImpactPpm(volume);
	this->impact[i] = std::clamp<int64_t>(this->impact[i] + shift, -STOCK_IMPACT_MAX_PPM, STOCK_IMPACT_MAX_PPM);
	this->price_fp[i] = std::max<int64_t>(STOCK_MIN_PRICE * STOCK_PRICE_SCALE, this->price_fp[i] * (STOCK_PPM + shift) / STOCK_PPM);
}

/**
 * Clean up after companies that have disappeared (bankrupt or bought): orders of such a company vanish,
 * positions in its shares are closed at the last known price.
 */
void StockMarket::HandleRemovedCompanies()
{
	for (size_t i = 0; i < this->positions.size();) {
		const StockOrder p = this->positions[i];
		const bool owner_ok = Company::IsValidID(CompanyID{p.owner});
		const bool target_ok = Company::IsValidID(CompanyID{p.target});
		if (owner_ok && target_ok) {
			i++;
			continue;
		}
		if (owner_ok) {
			const int64_t last = this->price_fp[p.target] > 0 ? FixedToPrice(this->price_fp[p.target]) : p.price;
			const int64_t units = p.volume;
			const int64_t profit = IsStockBuy(p.type) ? (last - p.price) * units : (p.price - last) * units;
			PayStockProfit(CompanyID{p.owner}, p.wallet, profit);
			this->AddHistory(p, last, profit, StockCloseReason::Delisted);
		}
		this->positions.erase(this->positions.begin() + i);
	}

	for (size_t i = 0; i < this->pending.size();) {
		const StockOrder &p = this->pending[i];
		if (Company::IsValidID(CompanyID{p.owner}) && Company::IsValidID(CompanyID{p.target})) {
			i++;
		} else {
			this->pending.erase(this->pending.begin() + i);
		}
	}

	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		if (Company::IsValidID(CompanyID{i})) continue;
		const bool stale = !this->candles[i].empty() || this->has_current[i] || this->price_fp[i] != 0 ||
				this->base_fp[i] != 0 || this->impact[i] != 0 || this->valuation[i].quarters != 0 || _wallets.HasOwner(CompanyID{i}) || !this->daily[i].empty() || this->has_daily_current[i] ||
				this->stats[i].closed != 0 || !this->stats[i].months.empty() || _central_bank.accounts[i].debt != 0;
		if (!stale) continue;
		this->candles[i].clear();
		this->current[i] = {};
		this->has_current[i] = false;
		this->price_fp[i] = 0;
		this->base_fp[i] = 0;
		this->impact[i] = 0;
		this->valuation[i] = {};
		_wallets.RemoveOwner(CompanyID{i});
		this->daily[i].clear();
		this->daily_current[i] = {};
		this->has_daily_current[i] = false;
		this->stats[i] = {};
		_central_bank.accounts[i] = {};
		std::erase_if(this->history, [i](const StockClosedTrade &t) { return t.owner == i; });
	}
}

/**
 * Remember a closed position in the history. Only the newest STOCK_MAX_HISTORY_PER_COMPANY trades per company are kept.
 * @param position The position that has been closed (a copy, not a reference into #positions). Its volume is the closed volume.
 * @param close_price Price at which it was closed.
 * @param profit Realised profit; negative for a loss.
 * @param reason Why it was closed.
 */
void StockMarket::AddHistory(const StockOrder &position, int64_t close_price, int64_t profit, StockCloseReason reason)
{
	StockClosedTrade t;
	t.id = position.id;
	t.owner = position.owner;
	t.target = position.target;
	t.type = IsStockBuy(position.type) ? StockOrderType::Buy : StockOrderType::Sell;
	t.reason = reason;
	t.wallet = position.wallet;
	t.volume = position.volume;
	t.open_price = position.price;
	t.close_price = close_price;
	t.profit = profit;
	t.closed_ym = GetStockCurrentMonth();
	t.open_ym = position.open_ym;
	this->history.push_back(t);
	this->RecordStats(t);

	size_t count = 0;
	for (const StockClosedTrade &h : this->history) count += (h.owner == t.owner);
	if (count > STOCK_MAX_HISTORY_PER_COMPANY) {
		const auto oldest = std::find_if(this->history.begin(), this->history.end(), [&t](const StockClosedTrade &h) { return h.owner == t.owner; });
		this->history.erase(oldest);
	}
}

/**
 * Close a position, or a part of it, at a given price. The profit or loss of the closed part is paid to the owner and
 * written to the history. The closed volume is subtracted from the position; when nothing is left, the position is removed.
 * Closing a long position sells shares, closing a short position buys shares, which moves the market price.
 * @param index Index of the position in #positions.
 * @param volume Volume to close; limited to the volume of the position.
 * @param close_price Price at which the position is closed.
 * @param reason Why it is closed.
 * @return The realised profit; negative for a loss.
 */
int64_t StockMarket::ClosePosition(size_t index, uint32_t volume, int64_t close_price, StockCloseReason reason)
{
	StockOrder closed = this->positions[index];
	volume = std::clamp<uint32_t>(volume, 1, std::max<uint32_t>(1, closed.volume));
	const bool full = volume >= closed.volume;
	closed.volume = volume;

	/* The profit is linear in the volume, so a partial close gets its proportional share. */
	const bool was_long = IsStockBuy(closed.type);
	const int64_t per_unit = was_long ? close_price - closed.price : closed.price - close_price;
	const int64_t profit = per_unit * static_cast<int64_t>(volume);

	if (full) {
		this->positions.erase(this->positions.begin() + index);
	} else {
		this->positions[index].volume -= volume;
	}

	PayStockProfit(CompanyID{closed.owner}, closed.wallet, profit);
	this->AddHistory(closed, close_price, profit, reason);
	this->ApplyImpact(CompanyID{closed.target}, !was_long, volume);
	return profit;
}

/**
 * Count a closed trade in the results of its company: the total, the win rate and the profit of the month.
 * @param trade The trade that has been closed.
 */
void StockMarket::RecordStats(const StockClosedTrade &trade)
{
	if (trade.owner >= MAX_COMPANIES) return;
	StockStats &s = this->stats[trade.owner];
	const bool win = trade.profit > 0;
	s.total_profit += trade.profit;
	s.closed++;
	if (win) s.wins++;

	if (s.months.empty() || s.months.back().ym != trade.closed_ym) {
		/* Trades are normally closed in the order of the months; a trade of an older month (rebuilding from the history) goes into its own entry. */
		auto it = std::find_if(s.months.begin(), s.months.end(), [&](const StockMonthProfit &m) { return m.ym == trade.closed_ym; });
		if (it == s.months.end()) {
			StockMonthProfit m;
			m.ym = trade.closed_ym;
			s.months.push_back(m);
			std::sort(s.months.begin(), s.months.end(), [](const StockMonthProfit &a, const StockMonthProfit &b) { return a.ym < b.ym; });
			while (s.months.size() > STOCK_MAX_STATS_MONTHS) s.months.erase(s.months.begin());
		}
	}
	auto it = std::find_if(s.months.begin(), s.months.end(), [&](const StockMonthProfit &m) { return m.ym == trade.closed_ym; });
	if (it == s.months.end()) return; // The month is older than the months that are remembered.
	it->profit += trade.profit;
	it->trades++;
	if (win) it->wins++;
}

/** Count all trades of the history again. Used for savegames from before the results were saved. */
void StockMarket::RebuildStats()
{
	this->stats.fill({});
	for (const StockClosedTrade &t : this->history) this->RecordStats(t);
}

/**
 * Trailing stop: the Stop Loss of a position with a trailing distance follows the price, at that distance.
 * A long position (which is closed at the Bid price) moves its Stop Loss up to Bid - distance, a short position
 * (closed at the Ask price) moves it down to Ask + distance. The Stop Loss never moves the other way, so the
 * profit that has been made is locked in when the price turns. A position that has no Stop Loss yet gets one.
 * This runs in the game loop with integers only, so every client calculates exactly the same levels.
 */
void StockMarket::ApplyTrailingStops()
{
	for (StockOrder &p : this->positions) {
		if (p.trail <= 0) continue;
		const CompanyID target{p.target};
		if (!Company::IsValidID(CompanyID{p.owner}) || !Company::IsValidID(target) || this->price_fp[p.target] <= 0) continue;

		if (IsStockBuy(p.type)) {
			const int64_t level = GetStockBid(target) - p.trail;
			if (level > 0 && (p.sl == 0 || level > p.sl)) p.sl = level;
		} else {
			const int64_t level = GetStockAsk(target) + p.trail;
			if (p.sl == 0 || level < p.sl) p.sl = level;
		}
	}
}

/**
 * The price that the change of a symbol is measured against: the close of the last completed month, or the opening
 * price of the running month when there is no completed month yet.
 * @param company The company.
 * @return The reference price, or the current price when nothing is known.
 */
int64_t GetStockReferencePrice(CompanyID company)
{
	if (!Company::IsValidID(company)) return 0;
	const size_t index = company.base();
	if (!_stock_market.candles[index].empty()) return _stock_market.candles[index].back().close;
	if (_stock_market.has_current[index]) return _stock_market.current[index].open;
	return GetStockPrice(company);
}

/**
 * The companies in the order of the ranking. Ties are broken by the company number, so the order is stable.
 * @param sort What the ranking is sorted by.
 * @return One entry per company, the best first.
 */
std::vector<StockRankEntry> GetStockRanking(StockRankSort sort)
{
	std::vector<StockRankEntry> list;
	for (const Company *c : Company::Iterate()) {
		StockRankEntry e;
		e.company = c->index;
		e.price = GetStockPrice(c->index);
		e.market_cap = GetStockMarketCap(c->index);
		const int64_t reference = GetStockReferencePrice(c->index);
		e.change_tenths = reference > 0 ? (e.price - reference) * 1000 / reference : 0;
		list.push_back(e);
	}

	std::sort(list.begin(), list.end(), [sort](const StockRankEntry &a, const StockRankEntry &b) {
		int64_t ka = 0, kb = 0;
		switch (sort) {
			case StockRankSort::MarketCap: ka = a.market_cap; kb = b.market_cap; break;
			case StockRankSort::Price:     ka = a.price; kb = b.price; break;
			case StockRankSort::Change:    ka = a.change_tenths; kb = b.change_tenths; break;
		}
		if (ka != kb) return ka > kb;
		return a.company.base() < b.company.base();
	});
	return list;
}

/**
 * Close positions that have reached their Stop Loss or Take Profit.
 * The position is closed exactly at the level that was reached, not at the price the market has moved to since.
 */
void StockMarket::CheckPositions()
{
	this->ApplyTrailingStops();

	for (size_t i = 0; i < this->positions.size();) {
		const StockOrder p = this->positions[i];
		const CompanyID owner{p.owner};
		const CompanyID target{p.target};
		/* Positions of vanished companies are cleaned up by HandleRemovedCompanies. */
		if (!Company::IsValidID(owner) || !Company::IsValidID(target) || this->price_fp[p.target] <= 0) {
			i++;
			continue;
		}

		const int64_t bid = GetStockBid(target);
		const int64_t ask = GetStockAsk(target);

		bool hit_sl;
		bool hit_tp;
		if (IsStockBuy(p.type)) {
			hit_sl = p.sl != 0 && bid <= p.sl;
			hit_tp = p.tp != 0 && bid >= p.tp;
		} else {
			hit_sl = p.sl != 0 && ask >= p.sl;
			hit_tp = p.tp != 0 && ask <= p.tp;
		}

		if (!hit_sl && !hit_tp) {
			i++;
			continue;
		}

		/* The position is removed, so the index stays the same for the next one. */
		this->ClosePosition(i, p.volume, hit_sl ? p.sl : p.tp, hit_sl ? StockCloseReason::StopLoss : StockCloseReason::TakeProfit);
		NotifyStockEvent(owner, fmt::format("Stock Market: Company [{}] closed Position #{} at {} price.", GetString(STR_STOCK_SYMBOL, owner), p.id, hit_sl ? "SL" : "TP"));
	}
}

/**
 * Margin call. When the equity of a company is below the stop-out level of the margin it has to hold,
 * its position with the biggest loss is closed at the current price, again and again, until the margin level
 * is restored or no position is left. The loss is paid from the money of the company.
 */
void StockMarket::CheckMarginCall()
{
	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		const CompanyID owner{i};
		if (!Company::IsValidID(owner)) continue;

		/* Every trading wallet is judged on its own: one wallet running out of margin does not close the positions of another. */
		for (const uint8_t wallet : _wallets.GetIds(owner)) {
			if (!IsTradingWallet(owner, wallet)) continue;

			bool stopped_out = false;
			for (;;) {
				const int64_t used = GetStockUsedMargin(owner, wallet);
				if (used <= 0) break;
				if (GetStockEquity(owner, wallet) >= used * STOCK_STOP_OUT_LEVEL / 100) break;

				size_t worst = this->positions.size();
				int64_t worst_profit = 0;
				for (size_t k = 0; k < this->positions.size(); k++) {
					const StockOrder &p = this->positions[k];
					if (p.owner != owner.base() || p.wallet != wallet || !Company::IsValidID(CompanyID{p.target})) continue;
					const int64_t profit = GetStockProfit(p);
					if (worst == this->positions.size() || profit < worst_profit) {
						worst = k;
						worst_profit = profit;
					}
				}
				if (worst == this->positions.size()) break;

				const StockOrder &w = this->positions[worst];
				const CompanyID target{w.target};
				const int64_t price = IsStockBuy(w.type) ? GetStockBid(target) : GetStockAsk(target);
				this->ClosePosition(worst, w.volume, price, StockCloseReason::StopOut);
				stopped_out = true;
			}

			/* One message per check, however many positions were closed. */
			if (stopped_out) {
				NotifyStockEvent(owner, fmt::format("CRITICAL: Broker enforced Stop-Out on Company [{}], {}, due to low Margin Level!", GetString(STR_STOCK_SYMBOL, owner), GetWalletName(owner, wallet)));
			}

			/* The margin call warning: shown once when the margin level falls below the margin call level, and again after it has recovered. */
			const int64_t level = GetStockMarginLevel(owner, wallet);
			const bool warning = level >= 0 && level < STOCK_MARGIN_CALL_LEVEL;
			if (warning && !_stock_margin_call_shown[i][wallet]) {
				NotifyStockEvent(owner, fmt::format("WARNING: Margin Call warning active for Company [{}], {}!", GetString(STR_STOCK_SYMBOL, owner), GetWalletName(owner, wallet)));
			}
			_stock_margin_call_shown[i][wallet] = warning;
		}
	}
}

/**
 * Turn pending orders into positions when their price has been reached.
 * The position is opened exactly at the price of the order. The commission is charged now.
 */
void StockMarket::CheckPending()
{
	for (size_t i = 0; i < this->pending.size();) {
		StockOrder o = this->pending[i];
		const CompanyID owner{o.owner};
		const CompanyID target{o.target};
		if (!Company::IsValidID(owner) || !Company::IsValidID(target) || this->price_fp[o.target] <= 0) {
			i++;
			continue;
		}

		const int64_t bid = GetStockBid(target);
		const int64_t ask = GetStockAsk(target);

		bool hit = false;
		switch (o.type) {
			case StockOrderType::BuyLimit:  hit = ask <= o.price; break;
			case StockOrderType::BuyStop:   hit = ask >= o.price; break;
			case StockOrderType::SellLimit: hit = bid >= o.price; break;
			case StockOrderType::SellStop:  hit = bid <= o.price; break;
			default: break;
		}

		if (!hit) {
			i++;
			continue;
		}

		this->pending.erase(this->pending.begin() + i);

		const bool buy = IsStockBuy(o.type);
		o.type = buy ? StockOrderType::Buy : StockOrderType::Sell;
		o.expiry = 0;
		o.open_ym = GetStockCurrentMonth();

		/* Not enough free margin: the order is dropped. */
		if (!IsTradingWallet(owner, o.wallet) || GetStockUsedMargin(owner, o.wallet) + GetStockMargin(o) > GetStockEquity(owner, o.wallet)) {
			NotifyStockEvent(owner, "Stock Market: Pending order expired or cancelled due to insufficient margin.");
			continue;
		}

		PayStockProfit(owner, o.wallet, -GetStockCommission(o.price * static_cast<int64_t>(o.volume)));
		this->positions.push_back(o);
		this->ApplyImpact(target, buy, o.volume);
	}
}

/** Check stop levels, pending orders and margin calls against the current prices. */
void StockMarket::CheckExecutions()
{
	this->CheckPositions();
	this->CheckPending();
	this->CheckMarginCall();
}

/**
 * Swap (overnight financing): every open position pays a small part of its value every day.
 * The fee of a company is collected and paid at once.
 */
void StockMarket::ChargeSwap()
{
	for (const StockOrder &p : this->positions) {
		if (!Company::IsValidID(CompanyID{p.owner}) || !Company::IsValidID(CompanyID{p.target})) continue;
		const int64_t value = GetStockPrice(CompanyID{p.target}) * static_cast<int64_t>(p.volume);
		PayStockProfit(CompanyID{p.owner}, p.wallet, -(value * STOCK_SWAP_PPM_PER_DAY / STOCK_PPM));
	}
}

/** Tell the player that a pending order has expired. Only the owner is told; this does not change the game state. */
static void NotifyOrderExpired(const StockOrder &order)
{
	if (CompanyID{order.owner} != _local_company) return;
	AddNewsItem(GetEncodedString(STR_STOCK_NEWS_ORDER_EXPIRED, order.id, GetStockOrderTypeString(order.type), CompanyID{order.target}),
			NewsType::General, NewsStyle::Small, {});
}

/** Count down the lifetime of the pending orders; an order whose time is up is removed and its owner is notified. */
void StockMarket::ExpireOrders()
{
	for (size_t i = 0; i < this->pending.size();) {
		StockOrder &o = this->pending[i];
		if (o.expiry == 0 || --o.expiry > 0) {
			i++;
			continue;
		}

		const StockOrder expired = o;
		this->pending.erase(this->pending.begin() + i);
		NotifyOrderExpired(expired);
	}
}

/**
 * Pay the dividend of the companies to the holders of their shares.
 * A company distributes STOCK_PAYOUT_PERCENT of its net result of the given number of completed quarters,
 * divided by the number of shares; the owners of long positions get that amount for every share they hold.
 * The money comes from the company that pays the dividend. A company can only pay what it has in cash:
 * when its cash is less than what the holders are owed, every holder gets the same fraction of their share.
 * Only integer arithmetic on game state is used, so every client of a network game gets the same figures.
 * @param quarters Number of completed quarters to take the net result of, the most recent first.
 */
void StockMarket::PayDividends(size_t quarters)
{
	/** A dividend that a holder gets, paid into the wallet of the position. */
	struct Credit {
		uint8_t owner;
		uint8_t wallet;
		int64_t amount;
	};
	std::vector<Credit> credits;
	std::array<int64_t, MAX_COMPANIES> paid{};     ///< Dividend that each company pays out as the issuer.

	for (const Company *c : Company::Iterate()) {
		int64_t net = 0;
		for (size_t q = 0; q < quarters; q++) {
			const CompanyEconomyEntry &e = c->old_economy[q];
			if (e.income == 0 && e.expenses == 0) continue; // This quarter has not happened yet.
			net += static_cast<int64_t>(e.income) + static_cast<int64_t>(e.expenses);
		}
		if (net <= 0) continue; // Nothing is paid out for a loss.

		const int64_t payout = net * STOCK_PAYOUT_PERCENT / 100;

		/* What the holders are owed in total. */
		int64_t owed = 0;
		for (const StockOrder &p : this->positions) {
			if (p.target != c->index.base() || !IsStockBuy(p.type) || !Company::IsValidID(CompanyID{p.owner})) continue;
			owed += payout * static_cast<int64_t>(p.volume) / STOCK_SHARES_PER_COMPANY;
		}
		if (owed <= 0) continue;

		/* The company can only pay what it has. A company without cash pays nothing this time. */
		const int64_t cash = std::max<int64_t>(0, static_cast<int64_t>(c->money));
		const int64_t total = std::min(owed, cash);
		if (total <= 0) continue;

		/* The fraction that is paid, in parts per million; exactly 1 when the company can pay everything. */
		const int64_t fraction = total >= owed ? STOCK_PPM : std::min<int64_t>(total, INT64_MAX / STOCK_PPM) * STOCK_PPM / owed;

		int64_t given = 0;
		for (const StockOrder &p : this->positions) {
			if (p.target != c->index.base() || !IsStockBuy(p.type) || !Company::IsValidID(CompanyID{p.owner})) continue;
			const int64_t share = payout * static_cast<int64_t>(p.volume) / STOCK_SHARES_PER_COMPANY;
			const int64_t amount = fraction >= STOCK_PPM ? share : share * fraction / STOCK_PPM;
			credits.push_back({p.owner, p.wallet, amount});
			given += amount;
		}
		paid[c->index.base()] += given;
	}

	/* Pay out only after all amounts are known, so that the cash of a company does not depend on the order of the companies. */
	for (const Credit &credit : credits) PayStockProfit(CompanyID{credit.owner}, credit.wallet, credit.amount);
	for (uint8_t i = 0; i < MAX_COMPANIES; i++) PayStockProfit(CompanyID{i}, WALLET_CASH, -paid[i]);
}

/**
 * Connect the dividend to the quarterly economy cycle. When a new quarter has started, the dividend of the quarter
 * that has just ended falls due after a short delay (see #STOCK_DIVIDEND_DELAY_DAYS). It is paid when the third and
 * when the fourth quarter of the year have been completed: the first time for the first three quarters together, the second time
 * for the fourth quarter alone, so every quarter of earnings is counted exactly once per year.
 */
void StockMarket::HandleDividends()
{
	const int32_t quarter_id = TimerGameEconomy::year.base() * 4 + TimerGameEconomy::month / 3;
	if (this->dividend_quarter_id == 0) {
		this->dividend_quarter_id = quarter_id; // First update of this game, or of a savegame without this information.
	} else if (quarter_id != this->dividend_quarter_id) {
		this->dividend_quarter_id = quarter_id;
		this->dividend_quarter = static_cast<uint8_t>((quarter_id + 3) % 4 + 1);
		this->dividend_delay = STOCK_DIVIDEND_DELAY_DAYS;
	}

	if (this->dividend_delay > 0 && --this->dividend_delay == 0) {
		if (this->dividend_quarter == STOCK_DIVIDEND_QUARTER_FIRST) {
			this->PayDividends(STOCK_DIVIDEND_QUARTER_FIRST);
		} else if (this->dividend_quarter == 4) {
			this->PayDividends(1);
		}
	}
}

/**
 * Once per day: refresh the valuation, let the market impact fade, start a new candle at the start of a month,
 * charge the swap, expire pending orders and pay dividends.
 * Stop levels and pending orders are not checked here, but in #UpdateTick.
 */
void StockMarket::UpdateDaily()
{
	const int32_t ym = TimerGameEconomy::year.base() * 12 + TimerGameEconomy::month;

	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		const CompanyID cid{i};
		if (!Company::IsValidID(cid)) continue;

		const StockValuation valuation = ComputeStockValuation(Company::Get(cid));
		this->valuation[i] = valuation;
		this->base_fp[i] = StockPriceFromValuation(valuation);

		/* The impact of the orders fades away. */
		this->impact[i] -= this->impact[i] / STOCK_IMPACT_DECAY_DIV;
		if (std::abs(this->impact[i]) < STOCK_IMPACT_DECAY_DIV) this->impact[i] = 0;

		if (this->price_fp[i] <= 0) this->price_fp[i] = GetGoalFixed(i);

		const int64_t price = GetStockPrice(cid);
		StockCandle &cur = this->current[i];

		if (!this->has_current[i]) {
			cur = {ym, price, price, price, price};
			this->has_current[i] = true;
		} else if (cur.ym != ym) {
			auto &list = this->candles[i];
			list.push_back(cur);
			if (list.size() > STOCK_MAX_CANDLES) list.erase(list.begin());
			const int64_t prev_close = cur.close;
			cur = {ym, prev_close, std::max(prev_close, price), std::min(prev_close, price), price};
		} else {
			cur.high = std::max(cur.high, price);
			cur.low = std::min(cur.low, price);
			cur.close = price;
		}

		/* Daily candles: only recorded once the Daily view has been turned on. */
		if (this->record_daily) {
			const int32_t day = TimerGameEconomy::date.base();
			StockCandle &d = this->daily_current[i];
			if (!this->has_daily_current[i]) {
				d = {day, price, price, price, price};
				this->has_daily_current[i] = true;
			} else if (d.ym != day) {
				auto &days = this->daily[i];
				days.push_back(d);
				if (days.size() > STOCK_MAX_DAILY_CANDLES) days.erase(days.begin());
				const int64_t prev_close = d.close;
				d = {day, prev_close, std::max(prev_close, price), std::min(prev_close, price), price};
			} else {
				d.high = std::max(d.high, price);
				d.low = std::min(d.low, price);
				d.close = price;
			}
		}
	}

	this->HandleRemovedCompanies();
	this->ChargeSwap();
	this->ExpireOrders();
	this->HandleDividends();

	InvalidateWindowData(WC_STOCK_MARKET, 0);
	InvalidateWindowData(WC_STOCK_ORDER, 0);
}

/**
 * Several times per day: let the prices glide towards the valuation, and check the stop levels, pending orders and margins.
 * Because this happens in small steps, a position is closed as soon as its level is reached, and a price can
 * not jump over a level for a whole day.
 */
void StockMarket::UpdateTick()
{
	this->HandleRemovedCompanies();

	/* Every company has a trading wallet and a fuel reserve from the first moment. */
	for (const Company *c : Company::Iterate()) _wallets.EnsureDefaults(c->index);

	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		if (!Company::IsValidID(CompanyID{i}) || this->price_fp[i] <= 0) continue;

		this->price_fp[i] += (GetGoalFixed(i) - this->price_fp[i]) / STOCK_GLIDE_DIV;

		if (this->has_current[i]) {
			const int64_t price = FixedToPrice(this->price_fp[i]);
			StockCandle &cur = this->current[i];
			cur.high = std::max(cur.high, price);
			cur.low = std::min(cur.low, price);
			cur.close = price;
		}

		if (this->record_daily && this->has_daily_current[i]) {
			const int64_t price = FixedToPrice(this->price_fp[i]);
			StockCandle &d = this->daily_current[i];
			d.high = std::max(d.high, price);
			d.low = std::min(d.low, price);
			d.close = price;
		}
	}

	this->CheckExecutions();

	InvalidateWindowData(WC_STOCK_MARKET, 0);
	InvalidateWindowData(WC_STOCK_ORDER, 0);
}

/** Update the market once per economy day. */
static const IntervalTimer<TimerGameEconomy> _stock_daily({TimerGameEconomy::DAY, TimerGameEconomy::Priority::COMPANY}, [](auto)
{
	_stock_market.UpdateDaily();
});

/** Check the market every few game ticks. */
static const IntervalTimer<TimerGameTick> _stock_tick({TimerGameTick::Priority::NONE, STOCK_TICK_INTERVAL}, [](auto)
{
	_stock_market.UpdateTick();
});


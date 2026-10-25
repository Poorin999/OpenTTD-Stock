/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file RetailMarketManager.cpp The retail investors of the towns: capital pool, sentiment, order matching and the wealth effect. */

#include "stdafx.h"
#include "RetailMarketManager.h"
#include "stock_base.h"
#include "central_bank.h"
#include "wallet_base.h"
#include "company_base.h"
#include "company_func.h"
#include "town.h"
#include "market_events.h"
#include "openttd.h"
#include "game_time/game_time_manager.h"
#include "stock_cmd.h"
#include "command_func.h"
#include "core/backup_type.hpp"
#include <utility>

#include "safeguards.h"

/*
 * ---- The only place that knows how a Town looks in this version of OpenTTD -------------------------------------------------------------
 * If a new OpenTTD version renames one of these fields, only these four functions have to change.
 */
namespace {

/** Inhabitants of a town. */
uint32_t TownPopulation(const Town *t)
{
	return t->cache.population;
}

/**
 * Service coverage of a town, 0 .. PPM: how much of what the houses produced last month (passengers, mail, ...) was transported.
 * This is the "transport service" of the town: with no station there is no coverage, and the retail investors save less.
 */
int64_t TownCoveragePpm(const Town *t)
{
	uint64_t produced = 0;
	uint64_t transported = 0;
	for (const auto &stat : t->supplied) {
		produced += stat.history[LAST_MONTH].production;
		transported += stat.history[LAST_MONTH].transported;
	}
	if (produced == 0) return 0;
	return static_cast<int64_t>(std::min<uint64_t>(retail::PPM, transported * retail::PPM / produced));
}

/**
 * Let a town grow faster. OpenTTD calculates Town::growth_rate (game ticks between two growth steps) now and then; this function
 * shortens that rate by the boost and writes it back every day, and it remembers the original rate to start from again.
 * A town that does not grow at all (no service) stays as it is.
 */
void ApplyGrowthBoost(Town *t, int64_t boost_ppm, int64_t &base_rate, int64_t &applied_rate)
{
	if (t->growth_rate == TOWN_GROWTH_RATE_NONE) {
		applied_rate = 0;
		return;
	}
	if (applied_rate == 0 || static_cast<int64_t>(t->growth_rate) != applied_rate) base_rate = t->growth_rate; // OpenTTD has calculated a new rate.

	const int64_t boosted = std::min<int64_t>(TOWN_GROWTH_RATE_NONE - 1, retail::BoostedGrowthRate(base_rate, boost_ppm));
	t->growth_rate = static_cast<decltype(t->growth_rate)>(boosted);
	applied_rate = boosted;
	if (t->grow_counter > t->growth_rate) t->grow_counter = t->growth_rate;
}

} // namespace

RetailMarketManager &RetailMarketManager::Get()
{
	static RetailMarketManager instance;
	return instance;
}

/** Forget everything, e.g. when a new game is started or a savegame is loaded. */
void RetailMarketManager::Reset()
{
	this->companies.fill({});
	this->cash.fill(0);
	this->capital_wanted = 0;
	this->wealth_pool = 0;
	this->last_dividend_day = 0;
	this->towns.clear();
	this->initialized = false;
	this->day_plan.fill({});
	this->plan_active = false;
}

/** Money that the retail investors have not invested. */
int64_t RetailMarketManager::GetCash() const
{
	int64_t sum = 0;
	for (int64_t c : this->cash) sum += c;
	return sum;
}

/** What all the shares of the retail investors are worth at the prices of now. */
int64_t RetailMarketManager::GetHoldingsValue() const
{
	int64_t sum = 0;
	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		const int64_t held = this->GetHeldShares(CompanyID{i});
		if (held > 0) sum += held * GetStockPrice(CompanyID{i});
	}
	return sum;
}

/** Shares of a company that the retail investors own, both groups together. */
int64_t RetailMarketManager::GetHeldShares(CompanyID company) const
{
	if (company.base() >= MAX_COMPANIES) return 0;
	int64_t sum = 0;
	for (int64_t h : this->companies[company.base()].held) sum += h;
	return sum;
}

/** What the retail investors did with a company on the last day. */
const RetailCompanyReport &RetailMarketManager::GetReport(CompanyID company) const
{
	static const RetailCompanyReport empty{};
	return company.base() < MAX_COMPANIES ? this->companies[company.base()].report : empty;
}

/** How much faster a town grows now because of the dividends and gains of the retail investors, in ppm. */
int64_t RetailMarketManager::GetTownGrowthBoostPpm(TownID town) const
{
	const int64_t own = town.base() < this->towns.size() ? this->towns[town.base()].boost_ppm : 0;
	return std::min<int64_t>(retail::MAX_BOOST_PPM, own + _market_events.TownBoostPpm(town.base(), static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes())));
}

/**
 * How much more passengers and cargo the houses of a town produce now, in ppm (+100,000 = +10%).
 * The houses of OpenTTD do not read this by themselves: the production of a house tile (TileLoop_Town in town_cmd.cpp) has to
 * multiply what it produces by (1 + GetTownDemandBoostPpm() / 1,000,000). See the notes of the delivery.
 */
int64_t RetailMarketManager::GetTownDemandBoostPpm(TownID town) const
{
	return this->GetTownGrowthBoostPpm(town) * retail::DEMAND_PERCENT_OF_BOOST / 100;
}

/** Step 0: every day, note the price of each company; take the "IPO price" on the first day. A company that is gone takes its holdings with it. */
void RetailMarketManager::UpdateCompanyRecords()
{
	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		const CompanyID cid{i};
		CompanyRecord &rec = this->companies[i];

		if (!Company::IsValidID(cid)) {
			rec = {}; // The shares of a company that is gone are worth nothing. A new company in this place is a new IPO.
			continue;
		}
		const int64_t price_fp = _stock_market.price_fp[i];
		if (price_fp <= 0) continue; // The terminal has no price yet.

		if (!rec.seen) {
			rec.seen = true;
			/* A company that exists when the retail investors start is established. Only a company that is founded later is an IPO. */
			rec.age_days = this->initialized ? 0 : retail::IPO_WINDOW_DAYS;
			rec.first_price = GetStockPrice(cid);
		} else {
			rec.age_days++;
		}

		rec.price_fp[rec.price_head] = price_fp;
		rec.price_head = (rec.price_head + 1) % static_cast<int>(rec.price_fp.size());
		rec.price_count = std::min<int>(rec.price_count + 1, static_cast<int>(rec.price_fp.size()));
	}
	this->initialized = true;
}

/**
 * Step 1: the capital pool. The towns say how much money they want to hold in the market (population, prosperity, transport coverage);
 * what the retail investors do not own yet comes in as new savings, a little each day, and is shared between the two groups.
 */
void RetailMarketManager::RefreshCapital([[maybe_unused]] const GameTimeSnapshot &now)
{
	this->towns.resize(Town::GetPoolSize());

	int64_t wanted = 0;
	for (size_t i = 0; i < this->towns.size(); i++) {
		if (!Town::IsValidID(TownID{static_cast<uint16_t>(i)})) {
			this->towns[i] = {}; // The town is gone.
			continue;
		}
	}
	for (const Town *t : Town::Iterate()) {
		TownRecord &rec = this->towns[t->index.base()];
		rec.input.population = TownPopulation(t);
		rec.input.coverage_ppm = TownCoveragePpm(t);
		wanted += retail::TownInvestableCapital(rec.input);
	}
	this->capital_wanted = wanted;

	const int64_t inflow = retail::DailyInflow(this->capital_wanted, this->GetCash() + this->GetHoldingsValue());
	int64_t given = 0;
	for (int k = 0; k < retail::COHORT_COUNT; k++) {
		const int64_t part = (k == retail::COHORT_COUNT - 1) ? inflow - given : inflow * retail::COHORT_CAPITAL_PERCENT[k] / 100; // the last group gets the rest
		this->cash[k] += part;
		given += part;
	}
}

/** The figures that the retail investors look at for one company. */
retail::CompanySignals RetailMarketManager::GatherSignals(CompanyID company) const
{
	retail::CompanySignals s;
	const size_t i = company.base();
	if (i >= MAX_COMPANIES || !Company::IsValidID(company)) return s;

	const CompanyRecord &rec = this->companies[i];
	if (!rec.seen || _stock_market.price_fp[i] <= 0) return s;

	s.valid = true;
	s.price = GetStockPrice(company);
	s.age_days = rec.age_days;
	s.first_price = rec.first_price;

	/* Momentum: the price now against the price MOMENTUM_LOOKBACK_DAYS ago. When the ring buffer is full, the next place to write is the oldest price. */
	const int size = static_cast<int>(rec.price_fp.size());
	if (rec.price_count == size) {
		const int64_t oldest = rec.price_fp[rec.price_head];
		const int64_t newest = rec.price_fp[(rec.price_head + size - 1) % size];
		if (oldest > 0) {
			s.has_momentum = true;
			s.momentum_ppm = (newest - oldest) * retail::PPM / oldest;
		}
	}

	/* The balance sheet: the book value of the valuation is already the equity (company value plus wallets minus the central bank debt). */
	const StockValuation &v = _stock_market.valuation[i];
	s.earnings = v.earnings;
	s.equity = v.book;
	s.de_ppm = BankDebtToEquityPpm(GetCentralBankDebt(company), v.book);

	/* The penalty interest rate of the central bank. */
	const BankAccount &account = _central_bank.accounts[i];
	s.in_default = account.in_default;
	s.penalty_level = account.penalty_level;

	/* The dividend: the yield that the terminal shows, and whether the last quarters were all profitable. */
	s.yield_ppm = GetStockSummary(company).yield_tenths * 1000;
	const Company *c = Company::Get(company);
	int known = 0;
	bool all_profitable = true;
	for (size_t q = 0; q < STOCK_EARNINGS_QUARTERS; q++) {
		const CompanyEconomyEntry &e = c->old_economy[q];
		if (e.income == 0 && e.expenses == 0) continue; // This quarter has not happened yet.
		known++;
		if (static_cast<int64_t>(e.income) + static_cast<int64_t>(e.expenses) <= 0) all_profitable = false;
	}
	s.dividend_consistent = known >= retail::MIN_DIVIDEND_QUARTERS && all_profitable;
	return s;
}

/**
 * Steps 2 and 3: opinions, orders, order matching and the injection into the trading terminal.
 *
 *   Pass 1: the opinion about every company; a panic starts a cool-down in which nobody buys it.
 *   Pass 2: the orders of the two groups (buys limited by the cash and by the 40% holding limit, sells by the holdings).
 *   Pass 3: per company the buy orders and sell orders meet each other at the mid price. Only the difference is sent to the terminal:
 *           StockMarket::ApplyImpact() moves the price, adds the volume to the candles, and fades away over the next days like every other order.
 */
void RetailMarketManager::TradeDay()
{
	using namespace retail;

	struct Plan {
		bool valid = false;
		int64_t price = 0;
		std::array<int64_t, COHORT_COUNT> buy{};
		std::array<int64_t, COHORT_COUNT> sell{};
	};
	std::array<Plan, MAX_COMPANIES> plan{};

	/* Pass 1: the opinions. */
	std::array<int64_t, COHORT_COUNT> score_sum{}; // The sum of the scores that pass the buy threshold, per group: what the cash is shared by.
	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		CompanyRecord &rec = this->companies[i];
		rec.report = {};

		const CompanySignals signals = this->GatherSignals(CompanyID{i});
		if (!signals.valid) continue;

		const Sentiment sentiment = ScoreCompany(signals);
		rec.report.sentiment = sentiment;
		rec.report.held_shares = this->GetHeldShares(CompanyID{i});
		plan[i].valid = true;
		plan[i].price = signals.price;

		if (sentiment.panic_ppm > 0) {
			rec.panic_cooldown = PANIC_COOLDOWN_DAYS;
		} else if (rec.panic_cooldown > 0) {
			rec.panic_cooldown--;
		}
		if (rec.panic_cooldown > 0) continue;
		for (int k = 0; k < COHORT_COUNT; k++) {
			if (sentiment.cohort_score[k] >= BUY_THRESHOLD_PPM) score_sum[k] += sentiment.cohort_score[k];
		}
	}

	/* Pass 2: the orders. */
	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		if (!plan[i].valid) continue;
		CompanyRecord &rec = this->companies[i];
		const Sentiment &sentiment = rec.report.sentiment;

		int64_t planned_buy = 0; // Together, the two groups may not pass the holding limit either.
		for (int k = 0; k < COHORT_COUNT; k++) {
			plan[i].sell[k] = SellShares(rec.held[k], sentiment.cohort_score[k], sentiment.panic_ppm);
			if (rec.panic_cooldown > 0) continue;
			const int64_t budget = BuyBudget(this->cash[k], sentiment.cohort_score[k], score_sum[k]);
			plan[i].buy[k] = BuyShares(budget, plan[i].price, this->GetHeldShares(CompanyID{i}) + planned_buy, STOCK_SHARES_PER_COMPANY);
			planned_buy += plan[i].buy[k];
		}
	}

	/* Pass 3: keep the day's orders. They are carried out in 24 parts, one per game hour (see #ExecuteSlice), like a real market
	 * where the orders arrive during the whole day. The first part is carried out at once. */
	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		this->day_plan[i].valid = plan[i].valid;
		this->day_plan[i].buy = plan[i].buy;
		this->day_plan[i].sell = plan[i].sell;
	}
	this->plan_active = true;
	this->ExecuteSlice(0);
}

/**
 * Carry out one hour's part of the day's orders: the part of hour h is plan * (h + 1) / 24 - plan * h / 24, so the 24 parts add up to the whole order.
 * The buyers and sellers of this hour meet at the price of this hour. The matched shares are volume without effect on the price;
 * the difference goes to the terminal (StockMarket::ApplyImpact), which moves the price and adds that volume too.
 * @param hour Game hour of the day, 0 .. 23.
 */
void RetailMarketManager::ExecuteSlice(int hour)
{
	using namespace retail;
	if (!this->plan_active || hour < 0 || hour >= GameTimeManager::HOURS_PER_DAY) return;
	const int64_t H = GameTimeManager::HOURS_PER_DAY;

	auto part = [&](int64_t whole) { return whole * (hour + 1) / H - whole * hour / H; };

	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		if (!this->day_plan[i].valid || !Company::IsValidID(CompanyID{i}) || _stock_market.price_fp[i] <= 0) continue;
		CompanyRecord &rec = this->companies[i];
		const int64_t price = GetStockPrice(CompanyID{i});
		if (price <= 0) continue;

		std::array<int64_t, COHORT_COUNT> want_buy{};
		std::array<int64_t, COHORT_COUNT> want_sell{};
		RetailOrder order;
		for (int k = 0; k < COHORT_COUNT; k++) {
			want_buy[k] = part(this->day_plan[i].buy[k]);
			want_sell[k] = std::min(rec.held[k], part(this->day_plan[i].sell[k]));
			order.buy_shares += want_buy[k];
			order.sell_shares += want_sell[k];
		}
		if (order.buy_shares == 0 && order.sell_shares == 0) continue;

		const MatchResult match = MatchOrders(order, STOCK_SHARES_PER_COMPANY, static_cast<int>(H));
		const int64_t filled_buy = match.matched + match.net_buy;   // What could be done of the buy orders; the rest of the orders is dropped.
		const int64_t filled_sell = match.matched + match.net_sell;

		int64_t bought = 0;
		int64_t sold = 0;
		for (int k = 0; k < COHORT_COUNT; k++) {
			/* Sells first: the proceeds and the gain. A part of every gain is spent in the towns, the rest stays in the cash of the group. */
			const int64_t s = std::min(rec.held[k], ProRata(want_sell[k], order.sell_shares, filled_sell));
			if (s > 0) {
				const int64_t proceeds = s * price;
				const int64_t cost = CostOfShares(rec.basis[k], rec.held[k], s);
				const int64_t consumed = ConsumedPart(proceeds - cost);
				rec.held[k] -= s;
				rec.basis[k] -= cost;
				this->cash[k] += proceeds - consumed;
				this->wealth_pool += consumed;
				sold += s;
			}

			/* Then the buys, paid from the cash of the group. */
			int64_t b = ProRata(want_buy[k], order.buy_shares, filled_buy);
			b = std::min(b, this->cash[k] / price);
			if (b > 0) {
				rec.held[k] += b;
				rec.basis[k] += b * price;
				this->cash[k] -= b * price;
				bought += b;
			}
		}

		if (hour == 0) rec.report.bought = rec.report.sold = rec.report.matched = rec.report.net_pressure = 0;
		rec.report.bought += bought;
		rec.report.sold += sold;
		rec.report.matched += match.matched;
		rec.report.net_pressure += match.net_buy - match.net_sell;
		rec.report.held_shares = this->GetHeldShares(CompanyID{i});

		/* Volume without effect on the price: the shares that the two groups traded between themselves. */
		_stock_market.AddTradedVolume(CompanyID{i}, static_cast<uint32_t>(std::min<int64_t>(match.matched, UINT32_MAX)));

		/* The injection: the terminal moves the price by the imbalance (and adds that volume). */
		if (match.net_buy > 0) _stock_market.ApplyImpact(CompanyID{i}, true, static_cast<uint32_t>(match.net_buy));
		if (match.net_sell > 0) _stock_market.ApplyImpact(CompanyID{i}, false, static_cast<uint32_t>(match.net_sell));
	}
}

/**
 * Step 4: once per game month the companies pay the dividend of the shares that the retail investors own, the same way as they pay the
 * players: from their cash, as a part (STOCK_PAYOUT_PERCENT) of the earnings, and never more than the cash they have.
 * A part of it is spent in the towns, the rest is reinvested by the group that owns the shares.
 */
void RetailMarketManager::PayDividends(int64_t day)
{
	using namespace retail;
	if (day - this->last_dividend_day < DIVIDEND_PERIOD_DAYS) return;
	this->last_dividend_day = day;

	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		const CompanyID cid{i};
		if (!Company::IsValidID(cid)) continue;
		CompanyRecord &rec = this->companies[i];

		const int64_t held = this->GetHeldShares(cid);
		const int64_t wanted = RetailDividend(_stock_market.valuation[i].earnings, STOCK_PAYOUT_PERCENT, held, STOCK_SHARES_PER_COMPANY, DIVIDEND_PERIOD_DAYS);
		const int64_t paid = std::min(wanted, std::max<int64_t>(0, static_cast<int64_t>(Company::Get(cid)->money)));
		if (paid <= 0) continue;

		PayStockProfit(cid, WALLET_CASH, -paid, LedgerItem::Dividends); // The company pays; it is on its account statement.

		for (int k = 0; k < COHORT_COUNT; k++) {
			const int64_t share = paid * rec.held[k] / held; // paid is at most the cash of one company, so this does not overflow in practice.
			const int64_t consumed = ConsumedPart(share);
			this->cash[k] += share - consumed;
			this->wealth_pool += consumed;
		}
	}
}

/**
 * Step 5: the feedback to the economy. The money that the retail investors spend out of dividends and gains is shared between the towns
 * (more to the bigger towns and the towns with good transport). It raises the boost of a town, which makes the town grow faster; the boost
 * fades by itself. The demand for passengers and cargo follows the boost, see GetTownDemandBoostPpm().
 */
void RetailMarketManager::ApplyMacroFeedback()
{
	using namespace retail;

	int64_t weight_sum = 0;
	for (const Town *t : Town::Iterate()) weight_sum += TownWeight(this->towns[t->index.base()].input);

	const int64_t pool = (weight_sum > 0) ? this->wealth_pool : 0; // Without towns the money waits.
	for (Town *t : Town::Iterate()) {
		TownRecord &rec = this->towns[t->index.base()];
		const int64_t fraction = weight_sum > 0 ? TownWeight(rec.input) * PPM / weight_sum : 0;
		const int64_t share = pool / PPM * fraction + (pool % PPM) * fraction / PPM;
		rec.boost_ppm = NextBoost(rec.boost_ppm, BoostIncrease(share, rec.input));
		/* A town boom in the news (market_events.h) adds to the boost for a while. It is read from the news log, so it is not part of rec.boost_ppm and fades by itself. */
		const int64_t news_boost = _market_events.TownBoostPpm(t->index.base(), static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes()));
		ApplyGrowthBoost(t, std::min<int64_t>(MAX_BOOST_PPM, rec.boost_ppm + news_boost), rec.base_growth_rate, rec.applied_growth_rate);
	}
	if (weight_sum > 0) this->wealth_pool = 0;
}

/** The whole day, at 00:00 of the game clock. */
void RetailMarketManager::OnNewGameDay(const GameTimeSnapshot &now)
{
	if (_game_mode != GM_NORMAL) return;

	const int64_t day = static_cast<int64_t>(now.total_minutes / GameTimeManager::MINUTES_PER_DAY);

	this->UpdateCompanyRecords();
	this->RefreshCapital(now);
	this->TradeDay();
	this->PayDividends(day);
	this->ApplyMacroFeedback();
}

/** Hours 1 .. 23: the investors carry out the next part of the day's orders. (Hour 0 is done by OnNewGameDay, together with the new plan.) */
void RetailMarketManager::OnNewGameHour(const GameTimeSnapshot &now)
{
	if (_game_mode != GM_NORMAL) return;
	const int hour = static_cast<int>((now.total_minutes / GameTimeManager::MINUTES_PER_HOUR) % GameTimeManager::HOURS_PER_DAY);
	if (hour == 0) return;
	this->ExecuteSlice(hour);
}

/** The retail investors decide at the start of every game day (00:00), together with the daily swap and the interest of the central bank. */
static const GameTimeSubscription _retail_game_day(GameTimeEvent::Day, [](const GameTimeSnapshot &now)
{
	RetailMarketManager::Get().OnNewGameDay(now);
});

/** ... and trade a part of it every game hour. */
static const GameTimeSubscription _retail_game_hour(GameTimeEvent::Hour, [](const GameTimeSnapshot &now)
{
	RetailMarketManager::Get().OnNewGameHour(now);
});

/*
 * ---- The computer players (AI companies) as traders --------------------------------------------------------------------------------------
 *
 * Every AI company looks at the market once per game day, in its own hour (the companies are spread over the day, like real traders who do not all
 * act at the same moment). It uses the same commands as a human player (CMD_STOCK_OPEN, CMD_STOCK_CLOSE, CMD_WALLET_TRANSFER), so every rule of the
 * terminal (margin, commission, order limit, strict trading capital) applies to it too. Only integer arithmetic and no random numbers: every client
 * calculates the same.
 *
 * Strategy ("value with risk control"):
 *   - Score of a company = how far the price is below the valuation (the book + earnings value), plus a little of the trend of the last month.
 *   - Buy the best undervalued company, sell short the most overvalued one; never a company with heavy debt or in default on the long side.
 *   - Never more than a small part of the free margin in one position, never more than BOT_MAX_POSITIONS positions, only one new position per day.
 *   - Every position has a Stop Loss and a Take Profit; it is also closed when the reason to hold it is gone or after BOT_MAX_HOLD_MONTHS months.
 *   - The bot only feeds its trading wallet with a small part of its cash, and only when the company has a lot of money: it must keep building transport.
 */
namespace {

constexpr int BOT_MAX_POSITIONS = 4;             ///< Open positions per AI company.
constexpr int BOT_MAX_HOLD_MONTHS = 3;           ///< A position is closed after this many game months.
constexpr int64_t BOT_MIN_CASH = 300'000;        ///< The company must have more cash than this before it puts money in its trading wallet.
constexpr int64_t BOT_WALLET_PERCENT = 10;       ///< Share of the company cash that is put in the trading wallet at a time.
constexpr int64_t BOT_WALLET_TARGET_PERCENT = 6; ///< The wallet is filled up when it holds less than this share of the company cash.
constexpr int64_t BOT_RISK_PERCENT = 25;         ///< Part of the free margin that one position may use.
constexpr uint32_t BOT_MAX_VOLUME = 2000;        ///< Largest position in shares (2% of a company): the bot does not push the price around.
constexpr int64_t BOT_BUY_SCORE_PPM = 60'000;    ///< A company is bought when it is at least 6% undervalued (with the trend).
constexpr int64_t BOT_SHORT_SCORE_PPM = 100'000; ///< A company is sold short when it is at least 10% overvalued.
constexpr int64_t BOT_STOP_LOSS_PPM = 50'000;    ///< Stop Loss: 5% from the entry price.
constexpr int64_t BOT_TAKE_PROFIT_PPM = 90'000;  ///< Take Profit: 9% from the entry price.
constexpr int64_t BOT_MAX_DE_PPM = 2 * retail::RATIO_ONE; ///< No long position in a company with more debt than twice its equity.

/** The opinion of a bot about a company: positive = cheap, negative = expensive; PPM units.. */
struct BotView {
	bool valid = false;
	bool can_long = true;
	int64_t score = 0;
};

BotView BotOpinion(CompanyID self, CompanyID target)
{
	BotView v;
	if (self == target || !Company::IsValidID(target)) return v;
	const size_t t = target.base();
	const int64_t price_fp = _stock_market.price_fp[t];
	const int64_t base_fp = _stock_market.base_fp[t];
	if (price_fp <= 0 || base_fp <= 0) return v;

	/* Value: the valuation against the price. */
	int64_t score = std::clamp<int64_t>((base_fp - price_fp) * retail::PPM / price_fp, -retail::PPM / 2, retail::PPM / 2);

	/* Trend: a third of the change since the last monthly close; the market keeps going the same way for a while. */
	const auto &candles = _stock_market.candles[t];
	if (!candles.empty() && candles.back().close > 0) {
		const int64_t last_close = candles.back().close;
		const int64_t now = StockFixedToPrice(price_fp);
		score += std::clamp<int64_t>((now - last_close) * retail::PPM / last_close, -retail::PPM / 2, retail::PPM / 2) / 3;
	}

	/* Quality: no long position in a company that may fail. */
	const StockValuation &val = _stock_market.valuation[t];
	const int64_t de = BankDebtToEquityPpm(GetCentralBankDebt(target), val.book);
	const BankAccount &account = _central_bank.accounts[t];
	if (account.in_default || de > BOT_MAX_DE_PPM || (val.quarters > 0 && val.earnings < 0)) v.can_long = false;
	if (account.in_default) score -= retail::PPM / 10; // a company in default is more likely to fall further

	v.valid = true;
	v.score = score;
	return v;
}

/** One game day of one AI company: look after the positions, then maybe open a new one. */
void BotTrade(CompanyID id)
{
	if (!Company::IsValidID(id)) return;
	Company *c = Company::Get(id);
	if (!c->is_ai) return;

	_wallets.EnsureDefaults(id);
	const Wallet *w = _wallets.FindFirst(id, WalletKind::Trading);
	if (w == nullptr) return;
	const uint8_t wallet = w->id;

	Backup<CompanyID> cur_company(_current_company);
	_current_company = id;

	/* 1. Feed the wallet, a little at a time, only from a rich company. */
	const int64_t cash = static_cast<int64_t>(c->money);
	if (cash > BOT_MIN_CASH && GetWalletBalance(id, wallet) < cash * BOT_WALLET_TARGET_PERCENT / 100) {
		const int64_t amount = cash * BOT_WALLET_PERCENT / 100;
		Command<CMD_WALLET_TRANSFER>::Do(DoCommandFlag::Execute, WALLET_CASH, wallet, Money(amount));
	}

	/* 2. The open positions. */
	std::vector<std::pair<uint32_t, uint32_t>> to_close;
	int open_count = 0;
	std::array<bool, MAX_COMPANIES> held{};
	const int32_t now_ym = GetStockCurrentMonth();
	for (const StockOrder &p : _stock_market.positions) {
		if (p.owner != id.base() || p.wallet != wallet) continue;
		open_count++;
		held[p.target] = true;

		const int64_t value = std::max<int64_t>(1, p.price * static_cast<int64_t>(p.volume));
		const int64_t pnl_ppm = GetStockProfit(p) * retail::PPM / value;
		const BotView view = BotOpinion(id, CompanyID{p.target});
		const bool is_long = IsStockBuy(p.type);
		bool close = false;
		if (!view.valid) close = true;                                                  // no opinion any more
		else if (pnl_ppm >= BOT_TAKE_PROFIT_PPM || pnl_ppm <= -BOT_STOP_LOSS_PPM) close = true; // the levels (a safety net: the market has them too)
		else if (is_long && (view.score < -BOT_BUY_SCORE_PPM / 2 || !view.can_long)) close = true; // cheap no more, or no longer safe
		else if (!is_long && view.score > BOT_BUY_SCORE_PPM / 2) close = true;          // expensive no more
		else if (p.open_ym != 0 && now_ym - p.open_ym >= BOT_MAX_HOLD_MONTHS) close = true;
		if (close) to_close.emplace_back(p.id, p.volume);
	}
	for (const auto &[pid, vol] : to_close) {
		Command<CMD_STOCK_CLOSE>::Do(DoCommandFlag::Execute, pid, vol);
		open_count--;
	}

	/* 3. One new position at most. */
	if (open_count < BOT_MAX_POSITIONS && _stock_market.CountOrders(id) < STOCK_MAX_ORDERS_PER_COMPANY) {
		CompanyID best{};
		int64_t best_score = 0;
		bool best_long = true;
		for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
			const CompanyID t{i};
			if (held[i]) continue;
			const BotView v = BotOpinion(id, t);
			if (!v.valid) continue;
			if (v.can_long && v.score >= BOT_BUY_SCORE_PPM && v.score > best_score) { best = t; best_score = v.score; best_long = true; }
			else if (v.score <= -BOT_SHORT_SCORE_PPM && -v.score > best_score) { best = t; best_score = -v.score; best_long = false; }
		}

		if (best_score > 0) {
			const int64_t free_margin = GetStockFreeMargin(id, wallet);
			StockOrder probe;
			probe.price = best_long ? GetStockAsk(best) : GetStockBid(best);
			probe.owner = id.base();
			probe.wallet = wallet;
			probe.volume = 1000;
			const int64_t margin_per_1000 = GetStockMargin(probe);
			if (probe.price > 0 && margin_per_1000 > 0 && free_margin > 0) {
				const int64_t budget = free_margin * BOT_RISK_PERCENT / 100 * 9 / 10; // 10% stays for the commission
				const uint32_t volume = static_cast<uint32_t>(std::min<int64_t>(BOT_MAX_VOLUME, budget * 1000 / margin_per_1000));
				if (volume >= 1) {
					const int64_t entry = GetStockExecutionPrice(best, best_long, volume);
					const int64_t sl = best_long ? entry * (retail::PPM - BOT_STOP_LOSS_PPM) / retail::PPM : entry * (retail::PPM + BOT_STOP_LOSS_PPM) / retail::PPM;
					const int64_t tp = best_long ? entry * (retail::PPM + BOT_TAKE_PROFIT_PPM) / retail::PPM : entry * (retail::PPM - BOT_TAKE_PROFIT_PPM) / retail::PPM;
					Command<CMD_STOCK_OPEN>::Do(DoCommandFlag::Execute, best, best_long ? StockOrderType::Buy : StockOrderType::Sell, volume, Money(0), Money(sl), Money(tp), 0u, wallet);
				}
			}
		}
	}

	cur_company.Restore();
}

/** Every game hour: the AI companies whose hour it is. Company number n acts at hour (n * 5 + 3) % 24, so they are spread over the day. */
const GameTimeSubscription _bot_game_hour(GameTimeEvent::Hour, [](const GameTimeSnapshot &now)
{
	if (_game_mode != GM_NORMAL) return;
	const int hour = static_cast<int>((now.total_minutes / GameTimeManager::MINUTES_PER_HOUR) % GameTimeManager::HOURS_PER_DAY);
	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		if ((i * 5 + 3) % GameTimeManager::HOURS_PER_DAY != hour) continue;
		BotTrade(CompanyID{i});
	}
});

} // namespace

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file central_bank.cpp Implementation of the central bank loans. */

#include "stdafx.h"
#include "game_time/game_time_manager.h"
#include "central_bank.h"
#include "gold_reserve.h"
#include "economy_func.h"
#include "economy_news.h"
#include "market_events.h"
#include "wallet_base.h"
#include "stock_base.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "core/backup_type.hpp"
#include "window_func.h"
#include "timer/timer.h"
#include "timer/timer_game_economy.h"
#include "timer/timer_game_tick.h"
#include "news_func.h"
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "train.h"
#include "station_base.h"
#include "rail.h"
#include <vector>
#include "strings_func.h"
#include "core/format.hpp"

#include "table/strings.h"

#include "safeguards.h"

CentralBank _central_bank;
Ledger _ledger;
std::array<std::deque<BankLogEntry>, MAX_COMPANIES> _bank_log;

/** Forget all loans, e.g. when a new game is started or a savegame is loaded. */
void CentralBank::Reset()
{
	_ledger.Reset(); // The ledger is saved and loaded together with the loans.
	for (auto &log : _bank_log) log.clear(); // The account log is only for the display.
	this->accounts.fill({});
	this->policy.Reset();
	this->trading_revenue_cents = BANK_START_CAPITAL * 100;
	this->written_off = 0;
	this->uncovered_loss = 0;
	this->auction_infra.fill(0);
	this->gold_log.clear();
	this->iorb_created = 0;
}

/**
 * The part of an amount that belongs to a weight: amount * weight / total, without overflow (the weights are kept below 1e9 by the callers).
 * @param amount Money units, 0 or more.
 * @param weight 0 .. total.
 * @param total More than 0.
 */
static int64_t BankShareOf(int64_t amount, int64_t weight, int64_t total)
{
	return amount / total * weight + (amount % total) * weight / total;
}

/**
 * Put money into the cash of the companies (the bank has bought gold). Every company gets a part in proportion to the company value of the last
 * finished quarter (equally when no value is known); the rest of the division goes to the first company, so exactly #amount is paid out.
 * The money is not income of the company (it is not in the statistics of the game); it is booked as LedgerItem::BankGoldOp.
 * @param amount Money units, 0 or more.
 * @return The money units that were paid out.
 */
static int64_t BankInjectCash(int64_t amount)
{
	if (amount <= 0) return 0;
	std::array<int64_t, MAX_COMPANIES> weight{};
	int64_t total = 0;
	int shift = 0;
	for (const Company *c : Company::Iterate()) total += std::max<int64_t>(0, static_cast<int64_t>(c->old_economy[0].company_value));
	while ((total >> shift) > 1'000'000'000) shift++;
	total = 0;
	for (const Company *c : Company::Iterate()) {
		weight[c->index.base()] = std::max<int64_t>(0, static_cast<int64_t>(c->old_economy[0].company_value)) >> shift;
		total += weight[c->index.base()];
	}
	if (total <= 0) { // No value is known yet: all companies get the same.
		for (const Company *c : Company::Iterate()) { weight[c->index.base()] = 1; total++; }
	}
	if (total <= 0) return 0; // No company at all.

	int64_t paid = 0;
	Company *first = nullptr;
	for (Company *c : Company::Iterate()) {
		if (first == nullptr) first = c;
		const int64_t share = BankShareOf(amount, weight[c->index.base()], total);
		if (share <= 0) continue;
		c->money += Money(share);
		RecordLedger(c->index, LedgerItem::BankGoldOp, share);
		paid += share;
	}
	if (first != nullptr && paid < amount) {
		first->money += Money(amount - paid);
		RecordLedger(first->index, LedgerItem::BankGoldOp, amount - paid);
		paid = amount;
	}
	return paid;
}

/** The money that the bank may take out of the companies today: BANK_GOLD_CASH_MAX_PERCENT of the unborrowed liquid cash of all companies. */
static int64_t BankWithdrawCapacity()
{
	int64_t total = 0;
	for (const Company *c : Company::Iterate()) total += GetUnborrowedLiquidCash(c->index);
	return total / 100 * BANK_GOLD_CASH_MAX_PERCENT + total % 100 * BANK_GOLD_CASH_MAX_PERCENT / 100;
}

/**
 * Take money out of the UNBORROWED liquid cash of the companies (the bank has sold gold), in proportion to that cash. Never more than a company has
 * unborrowed, so borrowed money is not touched. The money is booked as LedgerItem::BankGoldOp.
 * @param amount Money units, 0 or more, at most BankWithdrawCapacity().
 * @return The money units that were taken (the shares are rounded down, so it can be a few units less than #amount).
 */
static int64_t BankWithdrawCash(int64_t amount)
{
	if (amount <= 0) return 0;
	std::array<int64_t, MAX_COMPANIES> weight{};
	int64_t total = 0;
	for (const Company *c : Company::Iterate()) total += GetUnborrowedLiquidCash(c->index);
	if (total <= 0) return 0;
	int shift = 0;
	while ((total >> shift) > 1'000'000'000) shift++;
	total = 0;
	for (const Company *c : Company::Iterate()) {
		weight[c->index.base()] = GetUnborrowedLiquidCash(c->index) >> shift;
		total += weight[c->index.base()];
	}
	if (total <= 0) return 0;
	amount = std::min(amount, total << shift); // Never more than the companies have.
	const int64_t scaled = amount >> shift;
	int64_t taken = 0;
	for (Company *c : Company::Iterate()) {
		const int64_t share = std::min(BankShareOf(scaled, weight[c->index.base()], total) << shift, GetUnborrowedLiquidCash(c->index));
		if (share <= 0) continue;
		c->money -= Money(share);
		RecordLedger(c->index, LedgerItem::BankGoldOp, -share);
		taken += share;
	}
	return taken;
}

/**
 * The gold trade of the bank, once per game day (see the constants BANK_GOLD_* in central_bank.h): buys below the target backing
 * with the budget, sells its own gold above the upper limit. The budget is in 1/100 of a money unit, like the prices.
 * With BANK_GOLD_MOVES_CASH the trade also moves the cash of the companies (see the constants in central_bank.h): a purchase puts the
 * money that the bank pays into circulation, a sale takes it out of the unborrowed liquid cash of the companies.
 */
void CentralBank::TradeGold(int64_t day)
{
	if (_gold_reserve.GetPriceCents() <= 0) return; // The gold market has not started.
	const int64_t cash = _gold_reserve.GetCirculatingCash();
	if (cash <= 0) return;
	const int64_t backing = _gold_reserve.GetBackingBp();
	const int64_t value = _gold_reserve.GetReserveValue();

	int64_t ounces = 0;       // Positive: buy. Negative: sell.
	int64_t price_cents = 0;
	int64_t cash_moved = 0;   // Money units into (positive) or out of (negative) the cash of the companies.
	if (backing < BANK_GOLD_BUY_BELOW_BP) {
		price_cents = _gold_reserve.GetAskCents();
		if (price_cents <= 0) return;
		int64_t missing = std::max<int64_t>(0, cash / 10'000 * BANK_GOLD_BUY_BELOW_BP + cash % 10'000 * BANK_GOLD_BUY_BELOW_BP / 10'000 - value); // Money units to reach the target if the cash stayed the same.
		if (BANK_GOLD_MOVES_CASH) missing = missing * 10'000 / (10'000 - BANK_GOLD_BUY_BELOW_BP); // The money that the bank creates is cash in circulation too.
		ounces = (missing * 100 + price_cents - 1) / price_cents;
		ounces = std::min({ounces, BANK_GOLD_MAX_DAILY_OZ, _gold_reserve.market_stock_oz});
		if (BANK_GOLD_MOVES_CASH) {
			ounces = std::min(ounces, (cash / 100 * BANK_GOLD_CREATE_MAX_PERCENT) * 100 / price_cents); // Never more money created than the daily limit.
		} else {
			ounces = std::min(ounces, this->trading_revenue_cents / price_cents); // Only what its own money pays.
		}
		if (ounces <= 0) return;
		if (BANK_GOLD_MOVES_CASH) {
			/* The bank creates the money that it pays: it comes into circulation, the companies get it. Its own money is not used. */
			cash_moved = BankInjectCash(ounces * price_cents / 100);
			if (cash_moved <= 0) return; // No company to pay to.
		} else {
			this->trading_revenue_cents -= ounces * price_cents;
		}
		_gold_reserve.bank_reserve_oz += ounces;
		_gold_reserve.market_stock_oz -= ounces;
		_gold_reserve.pressure_ppm = std::min<int64_t>(GoldReserveManager::PRESSURE_MAX_PPM,
				_gold_reserve.pressure_ppm + ounces * GoldReserveManager::PPM / GoldReserveManager::DEPTH_OUNCES);
	} else if (backing > BANK_GOLD_SELL_ABOVE_BP) {
		price_cents = _gold_reserve.GetBidCents();
		if (price_cents <= 0) return;
		int64_t excess = std::max<int64_t>(0, value - (cash / 10'000 * BANK_GOLD_SELL_ABOVE_BP + cash % 10'000 * BANK_GOLD_SELL_ABOVE_BP / 10'000));
		if (BANK_GOLD_MOVES_CASH) excess = excess * 10'000 / (10'000 - BANK_GOLD_SELL_ABOVE_BP); // The money that leaves circulation makes the cash smaller.
		ounces = excess * 100 / price_cents;
		ounces = std::min({ounces, BANK_GOLD_MAX_DAILY_OZ, _gold_reserve.bank_reserve_oz});
		if (BANK_GOLD_MOVES_CASH) ounces = std::min(ounces, BankWithdrawCapacity() * 100 / price_cents); // The buyers can only pay with their unborrowed cash.
		if (ounces <= 0) return;
		if (BANK_GOLD_MOVES_CASH) {
			/* The money that the bank receives leaves circulation: it is taken from the companies and destroyed. Its own money is not used. */
			const int64_t taken = BankWithdrawCash(ounces * price_cents / 100);
			if (taken <= 0) return;
			cash_moved = -taken;
		} else {
			this->trading_revenue_cents += ounces * price_cents;
		}
		_gold_reserve.bank_reserve_oz -= ounces;
		_gold_reserve.market_stock_oz += ounces;
		_gold_reserve.pressure_ppm = std::max<int64_t>(-GoldReserveManager::PRESSURE_MAX_PPM,
				_gold_reserve.pressure_ppm - ounces * GoldReserveManager::PPM / GoldReserveManager::DEPTH_OUNCES);
		ounces = -ounces;
	} else {
		return;
	}

	this->gold_log.push_front(BankGoldTrade{day, ounces, price_cents, cash_moved});
	if (this->gold_log.size() > BANK_GOLD_LOG_MAX) this->gold_log.pop_back();
}

/**
 * The price index of the central bank, in ppm (1,000,000 = 1.0). It is the inflation of the game (_economy.inflation_prices,
 * 1.0 = 1 << 16), moved by POLICY_MONEY_WEIGHT_PERCENT of the change of the cash in circulation against the start of the
 * gold market. Loans make the cash bigger, so borrowing a lot pushes the index up and the bank answers with a higher rate.
 * Integer only. Until the gold market has started (no baseline of the cash) only the inflation of the game counts.
 */
int64_t GetPriceIndexPpm()
{
	const int64_t game = std::clamp<int64_t>((static_cast<int64_t>(_economy.inflation_prices) * POLICY_PPM) >> 16, POLICY_PPM / 2, 20 * POLICY_PPM);
	int64_t money_factor = POLICY_PPM;
	const int64_t baseline = _gold_reserve.baseline_supply;
	if (baseline > 0) {
		const int64_t supply = _gold_reserve.GetCirculatingCash();
		money_factor = std::clamp<int64_t>(POLICY_PPM + (supply - baseline) * POLICY_PPM / baseline * POLICY_MONEY_WEIGHT_PERCENT / 100, POLICY_PPM / 2, 3 * POLICY_PPM);
	}
	return game * money_factor / POLICY_PPM;
}

/**
 * One day of the monetary policy, Fed style. The bank meets POLICY_MEETINGS_PER_YEAR (8) times per game year (the first day of each meeting
 * period, see PolicyMeetingIndex). At a meeting it measures the yearly change of the price index (inflation) and the growth of the real
 * economy, asks the rule (Taylor rule, PolicyDesiredShiftBp) where the policy rate should be, and then does what a central bank does:
 * it moves in steps of 25 bp (50 bp when the rule is far away, 75 bp at most in emergency mode, see POLICY_EMERGENCY_*) or it holds.
 * The second mandate is employment: the growth term is the company value growth blended with the yearly growth of the jobs
 * (Transport Payrolls) by POLICY_JOBS_WEIGHT_PERCENT.
 * @param day Number of the game day.
 * @param index_ppm The price index now (GetPriceIndexPpm).
 * @param value_now Sum of the company values of the last finished quarter.
 * @param value_before Sum of the company values of the quarter before it.
 * @param jobs_growth_bp Yearly growth of the employment in bp (only used when has_jobs).
 * @param has_jobs True when at least two payroll reports exist.
 * @return True when a meeting was held today.
 */
bool MonetaryPolicy::Update(int64_t day, int64_t index_ppm, int64_t value_now, int64_t value_before, int64_t jobs_growth_bp, bool has_jobs)
{
	if (index_ppm <= 0) return false;
	const int64_t elapsed = day - this->last_decision_day;
	if (this->last_decision_day < 0 || this->prev_index_ppm <= 0 || elapsed < 0) {
		/* First day (new game, or a savegame from before the policy): only note the starting point. */
		this->prev_index_ppm = index_ppm;
		this->last_decision_day = day;
		return false;
	}
	if (PolicyMeetingIndex(day) <= PolicyMeetingIndex(this->last_decision_day)) return false; // Not a meeting day.

	/* A meeting every 45 days sees a short, noisy stretch of the index: the Fed looks at the 12 months, so the reading is averaged with the one before it. */
	const int64_t reading_bp = std::clamp<int64_t>((index_ppm - this->prev_index_ppm) * 10'000 * BANK_DAYS_PER_YEAR / (this->prev_index_ppm * std::max<int64_t>(1, elapsed)), -2000, 5000);
	this->inflation_bp = this->measured ? (reading_bp + this->inflation_bp) / 2 : reading_bp;

	/* Growth of the company values (a quarter against the quarter before it), yearly. */
	int64_t growth_period_bp = 0;
	while (value_before > 10'000'000'000'000 ) { value_before /= 1000; value_now /= 1000; } // Only for absurd values: keeps the multiplication below small.
	if (value_before > 0) growth_period_bp = std::clamp<int64_t>((value_now - value_before) * 10'000 / value_before, -10'000, 100'000);
	const int64_t value_growth_bp = growth_period_bp * BANK_DAYS_PER_YEAR / POLICY_VALUE_PERIOD_DAYS;

	/* Second mandate: employment. With a jobs figure the growth term is a blend; without it only the company values count. */
	this->has_jobs = has_jobs;
	this->jobs_bp = has_jobs ? std::clamp<int64_t>(jobs_growth_bp, -10'000, 100'000) : 0;
	this->growth_bp = has_jobs ? (value_growth_bp * (100 - POLICY_JOBS_WEIGHT_PERCENT) + this->jobs_bp * POLICY_JOBS_WEIGHT_PERCENT) / 100 : value_growth_bp;

	/* Emergency mode with a hysteresis: on above 10% yearly inflation, off below 7%. */
	if (!this->emergency && this->inflation_bp > POLICY_EMERGENCY_ON_BP) {
		this->emergency = true;
	} else if (this->emergency && this->inflation_bp < POLICY_EMERGENCY_OFF_BP) {
		this->emergency = false;
	}

	/* The target range always sits on a multiple of 25 bp (an old savegame may not): snap to the nearest one before the decision. */
	this->shift_bp = (this->shift_bp >= 0 ? this->shift_bp + POLICY_MAX_STEP_BP / 2 : this->shift_bp - POLICY_MAX_STEP_BP / 2) / POLICY_MAX_STEP_BP * POLICY_MAX_STEP_BP;
	const int64_t wanted = PolicyDesiredShiftBp(this->inflation_bp, this->growth_bp, this->emergency);
	/* What the market expected: the part of the gap that the last meeting left open (its bias), rounded like a move. Nothing is expected at the first meeting. */
	const int64_t expected_gap = this->measured ? this->wanted_gap_bp - this->last_step_bp : 0;
	this->wanted_gap_bp = wanted - this->shift_bp;
	/* The wide step also applies while the shift is still above the normal ceiling after the emergency, so it comes back down fast. */
	const int64_t max_step = this->Extended() ? POLICY_EMERGENCY_STEP_BP : POLICY_BIG_STEP_BP;
	int64_t step = PolicyRoundMove(this->wanted_gap_bp, max_step);
	const int64_t shift_before = this->shift_bp;
	this->shift_bp = std::clamp<int64_t>(this->shift_bp + step, POLICY_SHIFT_MIN_BP, this->Extended() || this->emergency ? POLICY_EMERGENCY_SHIFT_MAX_BP : POLICY_SHIFT_MAX_BP);
	step = this->shift_bp - shift_before; // A move that hits the limit is smaller.
	this->last_step_bp = step;
	this->last_surprise_bp = step - PolicyRoundMove(expected_gap, max_step);
	this->last_unscheduled = false;
	this->last_reason = PolicyUnscheduledReason::None;
	PolicyDecision d{day, shift_before, this->shift_bp, this->inflation_bp, this->growth_bp, this->emergency};
	d.jobs_bp = this->jobs_bp;
	d.has_jobs = has_jobs;
	this->log.push_front(d);
	if (this->log.size() > POLICY_LOG_MAX) this->log.pop_back();
	this->prev_index_ppm = index_ppm;
	this->last_decision_day = day;
	this->measured = true;
	return true;
}

/**
 * An unscheduled meeting between the scheduled ones, like the emergency cut of the Fed in March 2020. Checked every day; it needs at least
 * POLICY_UNSCHEDULED_MIN_DAYS days since the last meeting (so it can not repeat every day) and one of these events (the first that applies):
 *   - Spike: the price index rose at POLICY_SPIKE_INFLATION_BP a year or more since the last meeting, and inflation was already above
 *     POLICY_EMERGENCY_OFF_BP at that meeting: hike by POLICY_UNSCHEDULED_HIKE_BP.
 *   - Crash: a payroll report that came after the last meeting shows jobs falling by POLICY_CRASH_JOBS_BP a year or worse: cut by POLICY_UNSCHEDULED_CUT_BP.
 *   - Crisis news (bank scare, recession; set by market_events.cpp): cut by POLICY_NEWS_CUT_BP.
 *   - Inflation shock news (a big inflation scare), when the measured inflation is above POLICY_NEWS_MIN_INFLATION_BP: hike by POLICY_NEWS_HIKE_BP.
 * A piece of news that was not acted on within POLICY_NEWS_EXPIRE_DAYS days is forgotten. The whole move is a surprise for the markets.
 * The next scheduled meeting is not moved: it is held on the first day of its period as always.
 * @return True when an unscheduled meeting was held today.
 */
bool MonetaryPolicy::CheckUnscheduled(int64_t day, int64_t index_ppm, int64_t jobs_growth_bp, bool has_jobs, int64_t jobs_report_day)
{
	if (this->news_trigger != PolicyNewsTrigger::None && (day < this->news_trigger_day || day - this->news_trigger_day > POLICY_NEWS_EXPIRE_DAYS)) {
		this->news_trigger = PolicyNewsTrigger::None; // Too old (or from a different game): forget it.
	}
	if (!this->measured || this->last_decision_day < 0 || this->prev_index_ppm <= 0 || index_ppm <= 0) return false;
	const int64_t elapsed = day - this->last_decision_day;
	if (elapsed < POLICY_UNSCHEDULED_MIN_DAYS) return false;

	const int64_t reading_bp = std::clamp<int64_t>((index_ppm - this->prev_index_ppm) * 10'000 * BANK_DAYS_PER_YEAR / (this->prev_index_ppm * elapsed), -2000, 5000);
	const int64_t upper = this->emergency || this->Extended() ? POLICY_EMERGENCY_SHIFT_MAX_BP : POLICY_SHIFT_MAX_BP;

	int64_t move = 0;
	PolicyUnscheduledReason reason = PolicyUnscheduledReason::None;
	if (reading_bp >= POLICY_SPIKE_INFLATION_BP && this->inflation_bp > POLICY_EMERGENCY_OFF_BP && this->shift_bp < upper) {
		move = POLICY_UNSCHEDULED_HIKE_BP;
		reason = PolicyUnscheduledReason::InflationSpike;
	} else if (has_jobs && jobs_report_day > this->last_decision_day && jobs_growth_bp <= POLICY_CRASH_JOBS_BP && this->shift_bp > POLICY_SHIFT_MIN_BP) {
		move = -POLICY_UNSCHEDULED_CUT_BP;
		reason = PolicyUnscheduledReason::JobsCrash;
	} else if (this->news_trigger == PolicyNewsTrigger::Crisis && this->shift_bp > POLICY_SHIFT_MIN_BP) {
		move = -POLICY_NEWS_CUT_BP;
		reason = PolicyUnscheduledReason::CrisisNews;
	} else if (this->news_trigger == PolicyNewsTrigger::InflationShock && this->inflation_bp > POLICY_NEWS_MIN_INFLATION_BP && this->shift_bp < upper) {
		move = POLICY_NEWS_HIKE_BP;
		reason = PolicyUnscheduledReason::InflationNews;
	}
	if (move == 0) {
		/* The news can not be acted on (the rate is at its limit, or the inflation is not high): it is done with. */
		this->news_trigger = PolicyNewsTrigger::None;
		return false;
	}

	const int64_t shift_before = this->shift_bp;
	this->shift_bp = std::clamp<int64_t>(this->shift_bp + move, POLICY_SHIFT_MIN_BP, upper);
	const int64_t step = this->shift_bp - shift_before;
	if (step == 0) return false;
	this->news_trigger = PolicyNewsTrigger::None;
	this->last_step_bp = step;
	this->last_surprise_bp = step; // Nobody expected it.
	this->last_unscheduled = true;
	this->last_reason = reason;
	this->wanted_gap_bp = 0;
	this->has_jobs = has_jobs;
	this->jobs_bp = has_jobs ? std::clamp<int64_t>(jobs_growth_bp, -10'000, 100'000) : 0;
	PolicyDecision d{day, shift_before, this->shift_bp, reading_bp, this->growth_bp, this->emergency};
	d.jobs_bp = this->jobs_bp;
	d.has_jobs = has_jobs;
	d.unscheduled = true;
	this->log.push_front(d);
	if (this->log.size() > POLICY_LOG_MAX) this->log.pop_back();
	this->prev_index_ppm = index_ppm;
	this->last_decision_day = day;
	return true;
}

/** Money that a company owes to the central bank; 0 for a company that does not exist. */
int64_t GetCentralBankDebt(CompanyID company)
{
	return company.base() < MAX_COMPANIES ? _central_bank.accounts[company.base()].debt : 0;
}

/**
 * Net equity of a company: the company value (which counts the cash), plus the money in the wallets, minus the debt at the
 * central bank. A loan raises the cash and the debt by the same amount, so borrowing does not make the equity bigger.
 * @param company The company; 0 for a company that does not exist.
 */
static int64_t GetCentralBankEquity(CompanyID company)
{
	const Company *c = Company::GetIfValid(company);
	if (c == nullptr) return 0;
	return static_cast<int64_t>(CalculateCompanyValue(c)) + GetWalletsTotal(company) - GetCentralBankDebt(company);
}

/**
 * The most that a company may owe: #CENTRAL_BANK_CREDIT_PERCENT of its net equity, lowered by the penalty level of the
 * account (100% at level 0, 10% less for each level of default, never below 20%). That is how a default lowers the
 * credit limit for new loans.
 * @param company The company.
 * @return The credit limit, 0 or more.
 */
int64_t GetCentralBankCreditLimit(CompanyID company)
{
	if (!Company::IsValidID(company)) return 0;
	const int64_t equity = GetCentralBankEquity(company);
	const int64_t percent = BankCreditPercentForPenalty(_central_bank.accounts[company.base()].penalty_level);
	/* The share of the equity follows the policy shift, the credit standing of the gold reserve scales it, the penalty level lowers it. */
	return std::max<int64_t>(0, equity) * BankPolicyCreditPercent(_central_bank.policy.shift_bp) / 100 * GetCentralBankTierCreditPercent() / 100 * percent / 100;
}

/** Percent of the credit limit that the credit standing allows now. The standing is not known before the gold market has started: then 100%. */
int64_t GetCentralBankTierCreditPercent()
{
	if (_gold_reserve.GetPriceCents() <= 0) return 100;
	return BankTierCreditPercent(static_cast<int>(_gold_reserve.GetCreditTier()));
}

/**
 * The annual rate (in bp, 100 = 1%) that the company would be charged at the next midnight: the penalty rate while the
 * account is in default, otherwise the risk rate of its debt-to-equity ratio. For the future UI; 0 without debt.
 */
int64_t GetCentralBankAnnualRateBp(CompanyID company)
{
	if (!Company::IsValidID(company)) return 0;
	const BankAccount &a = _central_bank.accounts[company.base()];
	if (a.debt <= 0) return 0;
	if (a.in_default) return BankPenaltyRateBp(std::max<int>(1, a.penalty_level), _central_bank.policy.shift_bp);
	const int64_t news_bp = _market_events.CreditSpreadBp(company.base(), static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes()));
	return BankShiftedRateBp(BankAnnualRateBp(BankDebtToEquityPpm(a.debt, GetCentralBankEquity(company))) + news_bp, _central_bank.policy.shift_bp);
}

/** How much more a company may borrow now. */
int64_t GetCentralBankAvailable(CompanyID company)
{
	return std::max<int64_t>(0, GetCentralBankCreditLimit(company) - GetCentralBankDebt(company));
}

/**
 * The money of a company that may go to the trading wallets: its cash minus everything that is borrowed. That is the cash
 * minus the debt at the central bank, because a central bank loan is paid into the cash. As long as the cash is not more
 * than the borrowed money, none of it is the company's own money.
 * @param company The company.
 * @return The unborrowed liquid cash, 0 or more.
 */
int64_t GetUnborrowedLiquidCash(CompanyID company)
{
	const Company *c = Company::GetIfValid(company);
	if (c == nullptr) return 0;
	const int64_t liquid = static_cast<int64_t>(c->money) - GetCentralBankDebt(company);
	return std::max<int64_t>(0, liquid);
}

/**
 * Guard for every command that puts company money into a trading wallet: the amount must come out of the unborrowed
 * liquid cash, so money of the central bank can never be traded.
 * @param company The company that pays.
 * @param amount The money that is to enter a trading wallet.
 * @return An error when the amount is more than #GetUnborrowedLiquidCash, otherwise a zero cost.
 */
CommandCost CheckTradingCapital(CompanyID company, int64_t amount)
{
	if (amount > GetUnborrowedLiquidCash(company)) return CommandCost(STR_BANK_ERROR_BORROWED_FUNDS);
	return CommandCost();
}

/** The game year (365 game days of the HH:MM clock) in which new ledger lines are booked. */
int32_t GetLedgerCurrentYear()
{
	return GameTimeManager::Get().GetCurrentGameYear();
}

/**
 * Book a line of the account statement of a company in the current economy year.
 * @param company The company.
 * @param item The line of the statement.
 * @param amount Money that came in (positive) or went out (negative). Zero is not booked.
 */
void RecordLedger(CompanyID company, LedgerItem item, int64_t amount)
{
	if (amount == 0 || company.base() >= MAX_COMPANIES || item >= LedgerItem::End) return;

	const int32_t year = GetLedgerCurrentYear();
	auto &years = _ledger.years[company.base()];
	LedgerEntry &entry = years[year][static_cast<size_t>(item)];
	if (amount > 0) {
		entry.income += amount;
	} else {
		entry.expense += -amount;
	}

	/* Keep this year and the years before it, #LEDGER_KEEP_YEARS in all. */
	years.erase(years.begin(), years.lower_bound(year - LEDGER_KEEP_YEARS + 1));

	/* The latest lines for the account log of the Central Bank window (newest in front). */
	auto &log = _bank_log[company.base()];
	log.push_front({GameTimeManager::Get().GetCurrentGameDay(), item, amount});
	while (log.size() > BANK_LOG_MAX) log.pop_back();
}

/**
 * What a line of the statement booked in an economy year.
 * @param company The company.
 * @param item The line of the statement.
 * @param year The economy year.
 * @return The entry; empty when nothing was booked.
 */
LedgerEntry GetLedgerEntry(CompanyID company, LedgerItem item, int32_t year)
{
	if (company.base() >= MAX_COMPANIES || item >= LedgerItem::End) return {};
	const auto &years = _ledger.years[company.base()];
	const auto it = years.find(year);
	if (it == years.end()) return {};
	return it->second[static_cast<size_t>(item)];
}

/** The net cost of some kinds of yearly expenses as a ledger entry: costs are expenses, a net refund (revenue) is income. */
static LedgerEntry YearlyAsEntry(const Company *c, size_t age, std::initializer_list<ExpensesType> types)
{
	int64_t sum = 0;
	for (ExpensesType type : types) sum += static_cast<int64_t>(c->yearly_expenses[age][type]);
	LedgerEntry e;
	if (sum >= 0) e.expense = sum; else e.income = -sum;
	return e;
}

/**
 * Copy the yearly expenses of the game (it keeps this year and two years back) into the ledger, which keeps #LEDGER_KEEP_YEARS years.
 * The copy replaces the old value of the year, so it is safe to call every day. No account log line is made.
 */
void SnapshotYearlyExpenses()
{
	const int32_t now = GetLedgerCurrentYear();
	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		const Company *c = Company::GetIfValid(CompanyID{i});
		if (c == nullptr) continue;
		auto &years = _ledger.years[i];
		for (size_t age = 0; age < 3; age++) {
			auto &row = years[now - static_cast<int32_t>(age)];
			row[static_cast<size_t>(LedgerItem::YearTrain)] = YearlyAsEntry(c, age, {EXPENSES_TRAIN_REVENUE});
			row[static_cast<size_t>(LedgerItem::YearRoadVeh)] = YearlyAsEntry(c, age, {EXPENSES_ROADVEH_REVENUE});
			row[static_cast<size_t>(LedgerItem::YearShip)] = YearlyAsEntry(c, age, {EXPENSES_SHIP_REVENUE});
			row[static_cast<size_t>(LedgerItem::YearAircraft)] = YearlyAsEntry(c, age, {EXPENSES_AIRCRAFT_REVENUE});
			row[static_cast<size_t>(LedgerItem::YearRunning)] = YearlyAsEntry(c, age, {EXPENSES_TRAIN_RUN, EXPENSES_ROADVEH_RUN, EXPENSES_AIRCRAFT_RUN, EXPENSES_SHIP_RUN, EXPENSES_PROPERTY});
			row[static_cast<size_t>(LedgerItem::YearConstruction)] = YearlyAsEntry(c, age, {EXPENSES_CONSTRUCTION});
			row[static_cast<size_t>(LedgerItem::YearNewVehicles)] = YearlyAsEntry(c, age, {EXPENSES_NEW_VEHICLES});
		}
		years.erase(years.begin(), years.lower_bound(now - LEDGER_KEEP_YEARS + 1));
	}
}

/** Tell the windows that show the loan that something changed. */
static void InvalidateBankWindows()
{
	InvalidateWindowData(WC_STOCK_MARKET, 0);
	InvalidateWindowData(WC_STOCK_ORDER, 0);
	InvalidateWindowData(WC_CENTRAL_BANK, 0);
	InvalidateWindowClassesData(WC_FINANCES);
}

/**
 * Borrow money from the central bank. The money is paid out to the company cash only: loan money may never enter a
 * trading wallet, so a trading wallet as destination is refused. The loan is not booked as income, so the cash and the
 * income statistics of the company only show what the company earned.
 * @param flags Type of operation.
 * @param wallet Wallet that gets the money: must be #WALLET_CASH.
 * @param amount Money to borrow, more than zero and at most the available credit.
 * @return Zero cost, or an error.
 */
CommandCost CmdBankBorrow(DoCommandFlags flags, uint8_t wallet, Money amount_in)
{
	const int64_t amount = amount_in;
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;
	if (amount <= 0) return CommandCost(STR_STOCK_ERROR_BAD_AMOUNT);
	const WalletKind kind = GetWalletKind(_current_company, wallet);
	if (kind == WalletKind::Trading) return CommandCost(STR_BANK_ERROR_NO_TRADING_LOAN);
	if (kind != WalletKind::Cash) return CommandCost(STR_WALLET_ERROR_BAD_WALLET);
	if (amount > GetCentralBankAvailable(_current_company)) return CommandCost(STR_BANK_ERROR_LIMIT);

	if (flags.Test(DoCommandFlag::Execute)) {
		_central_bank.accounts[_current_company.base()].debt += amount;
		Company::Get(_current_company)->money += Money(amount);
		RecordLedger(_current_company, LedgerItem::BankLoan, amount);
		InvalidateBankWindows();
	}
	return CommandCost();
}

/**
 * Repay a loan with money from the company cash or from a trading wallet. From a trading wallet only the free margin can
 * be used, so the open positions of that wallet stay covered.
 * @param flags Type of operation.
 * @param wallet Wallet that pays: #WALLET_CASH or a trading wallet.
 * @param amount Money to repay, more than zero and at most the debt.
 * @return Zero cost, or an error.
 */
CommandCost CmdBankRepay(DoCommandFlags flags, uint8_t wallet, Money amount_in)
{
	const int64_t amount = amount_in;
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;
	if (amount <= 0) return CommandCost(STR_STOCK_ERROR_BAD_AMOUNT);
	const WalletKind kind = GetWalletKind(_current_company, wallet);
	if (kind != WalletKind::Cash && kind != WalletKind::Trading) return CommandCost(STR_WALLET_ERROR_BAD_WALLET);
	if (GetCentralBankDebt(_current_company) <= 0) return CommandCost(STR_BANK_ERROR_NO_DEBT);
	if (amount > GetCentralBankDebt(_current_company)) return CommandCost(STR_STOCK_ERROR_BAD_AMOUNT);
	if (GetWalletBalance(_current_company, wallet) < amount) return CommandCost(STR_STOCK_ERROR_NO_FUNDS);
	if (kind == WalletKind::Trading && GetStockFreeMargin(_current_company, wallet) < amount) return CommandCost(STR_STOCK_ERROR_NO_MARGIN);

	if (flags.Test(DoCommandFlag::Execute)) {
		if (kind == WalletKind::Cash) {
			Company::Get(_current_company)->money -= Money(amount); // A repayment is not an expense either.
		} else {
			AddToWallet(_current_company, wallet, -amount);
		}
		_central_bank.accounts[_current_company.base()].debt -= amount;
		RecordLedger(_current_company, LedgerItem::BankLoan, -amount);
		InvalidateBankWindows();
	}
	return CommandCost();
}

/**
 * The original OpenTTD loan does not exist any more: the central bank is the only lender. A loan that a company still has (an
 * old savegame, a script, a cheat) is moved to the central bank at once: the debt grows by the loan, the loan becomes 0, and
 * the cash stays as it is (it already holds that money). From then on it is repaid with the central bank commands, and it
 * costs the interest of the central bank. Only game state is used, so every client of a network game does the same.
 */
static void MoveOriginalLoansToCentralBank()
{
	for (Company *c : Company::Iterate()) {
		if (c->current_loan <= 0) continue;
		const int64_t loan = static_cast<int64_t>(c->current_loan);
		_central_bank.accounts[c->index.base()].debt += loan;
		c->current_loan = 0;
		if (c->index == _local_company) {
			AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, fmt::format("Your original loan of {} was moved to the central bank. It is repaid there, and it costs the interest of the central bank.",
					GetString(STR_JUST_CURRENCY_LONG, loan))), NewsType::General, NewsStyle::Small, {});
		}
		InvalidateBankWindows();
	}
}

/** Check every game tick, so there is never a moment in which the original loan is in use. */
static const IntervalTimer<TimerGameTick> _bank_move_original_loans({TimerGameTick::Priority::NONE, 1}, [](auto)
{
	MoveOriginalLoansToCentralBank();
});

/**
 * Take money for the debt from the free margin of the trading wallets of a company. Only the free margin is taken (as in CmdBankRepay), so the
 * open positions stay covered.
 * @param company The company.
 * @param need Money units that are still owed, more than 0.
 * @return The money units that were taken, 0 .. need.
 */
static int64_t BankSeizeWallets(CompanyID company, int64_t need)
{
	int64_t taken = 0;
	for (uint8_t id : _wallets.GetIds(company)) {
		if (taken >= need) break;
		if (GetWalletKind(company, id) != WalletKind::Trading) continue;
		const int64_t amount = std::min({need - taken, GetWalletBalance(company, id), GetStockFreeMargin(company, id)});
		if (amount <= 0) continue;
		AddToWallet(company, id, -amount);
		taken += amount;
	}
	return taken;
}

/**
 * Sell the gold of a company at the bid price (no commission) to pay a debt. Just enough ounces are sold; the gold goes back to the market
 * stock and pushes the price down like a sale of a company does. A rest of less than one ounce of value goes into the cash of the company.
 * @param company The company.
 * @param need Money units that are still owed.
 * @return The money units that went to the debt, 0 .. need.
 */
static int64_t BankSellGoldForDebt(CompanyID company, int64_t need)
{
	if (need <= 0 || _gold_reserve.GetPriceCents() <= 0) return 0;
	const int64_t held = _gold_reserve.GetHoldings(company);
	const int64_t bid = _gold_reserve.GetBidCents();
	if (held <= 0 || bid <= 0) return 0;

	const int64_t ounces = std::min(held, (need * 100 + bid - 1) / bid);
	const int64_t proceeds = ounces * bid / 100;
	if (ounces <= 0 || proceeds <= 0) return 0;
	const int64_t applied = std::min(proceeds, need);

	const int64_t cost = _gold_reserve.holdings_cost[company.base()];
	if (cost > 0) _gold_reserve.holdings_cost[company.base()] -= std::min(cost, cost / held * ounces + (cost % held) * ounces / held);
	_gold_reserve.holdings_oz[company.base()] -= ounces;
	_gold_reserve.market_stock_oz += ounces;
	_gold_reserve.pressure_ppm = std::max<int64_t>(-GoldReserveManager::PRESSURE_MAX_PPM,
			_gold_reserve.pressure_ppm - ounces * GoldReserveManager::PPM / GoldReserveManager::DEPTH_OUNCES);
	if (proceeds > applied) {
		Company *c = Company::GetIfValid(company);
		if (c != nullptr) c->money += Money(proceeds - applied);
	}
	return applied;
}

/** The price in percent of the value that the bank pays now: its credit standing, lower at the auction. */
static int64_t BankRepoPercentNow(bool auction)
{
	const int64_t percent = _gold_reserve.GetPriceCents() <= 0 ? BANK_REPO_PERCENT_NO_MARKET : BankRepoPercent(static_cast<int>(_gold_reserve.GetCreditTier()));
	return auction ? percent * BANK_AUCTION_PERCENT_OF_REPO / 100 : percent;
}

/**
 * Repossess vehicles of a company to pay a debt. See the comment at BANK_REPO_ENABLED. The vehicle that is taken is the cheapest one that covers
 * what is still owed; when none does, the most valuable one. The vehicle chain is deleted at once (trains free their reservation first).
 * @param company The company.
 * @param need Money units that are still owed.
 * @param keep_one The company keeps the vehicle with the highest profit of last year.
 * @param auction True: auction price.
 * @return The money units that went to the debt, 0 .. need.
 */
static int64_t BankSellVehiclesForDebt(CompanyID company, int64_t need, bool keep_one, bool auction)
{
	if (!BANK_REPO_ENABLED || need <= 0) return 0;
	const int64_t percent = BankRepoPercentNow(auction);

	struct Candidate {
		VehicleID id;       ///< The front vehicle of the chain.
		int64_t proceeds;   ///< What the bank pays for the whole chain.
		int64_t profit;     ///< Profit of last year of the front vehicle.
	};
	std::vector<Candidate> list;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->owner != company || v->Previous() != nullptr || !v->IsPrimaryVehicle() || !IsCompanyBuildableVehicleType(v->type)) continue;
		int64_t value = 0;
		for (const Vehicle *u = v; u != nullptr; u = u->Next()) value += static_cast<int64_t>(u->value);
		if (value <= 0) continue;
		list.push_back({v->index, value / 100 * percent + value % 100 * percent / 100, static_cast<int64_t>(v->profit_last_year)});
	}
	if (keep_one && !list.empty()) {
		size_t best = 0;
		for (size_t i = 1; i < list.size(); i++) {
			if (list[i].profit > list[best].profit || (list[i].profit == list[best].profit && list[i].id.base() < list[best].id.base())) best = i;
		}
		list.erase(list.begin() + best);
	}
	/* A fixed order, the same on every client: by price, then by number. */
	std::sort(list.begin(), list.end(), [](const Candidate &x, const Candidate &y) {
		return x.proceeds != y.proceeds ? x.proceeds < y.proceeds : x.id.base() < y.id.base();
	});

	int64_t applied = 0;
	while (applied < need && !list.empty()) {
		const int64_t remaining = need - applied;
		size_t pick = list.size() - 1; // None covers it: the most valuable one.
		for (size_t i = 0; i < list.size(); i++) {
			if (list[i].proceeds >= remaining) { pick = i; break; }
		}
		const Candidate cand = list[pick];
		list.erase(list.begin() + pick);

		Vehicle *v = Vehicle::GetIfValid(cand.id);
		if (v == nullptr || v->owner != company) continue;
		if (v->type == VEH_TRAIN) FreeTrainTrackReservation(Train::From(v));
		delete v; // The whole chain.

		const int64_t used = std::min(cand.proceeds, remaining);
		applied += used;
		if (cand.proceeds > used) {
			Company *c = Company::GetIfValid(company);
			if (c != nullptr) c->money += Money(cand.proceeds - used); // More than the debt: the rest is the company's.
		}
	}
	return applied;
}

/**
 * What the stations and the infrastructure of a company are worth at the auction (money units): the OpenTTD formula for stations
 * (facilities x station value x 25), the build price for rails, roads, signals and canals, all at the auction price.
 */
static int64_t BankInfrastructureValue(const Company *c)
{
	int64_t facilities = 0;
	for (const Station *st : Station::Iterate()) {
		if (st->owner == c->index) facilities += static_cast<int64_t>(st->facilities.Count());
	}
	int64_t value = facilities * static_cast<int64_t>(_price[PR_STATION_VALUE]) * 25;
	for (uint i = 0; i < RAILTYPE_END; i++) value += static_cast<int64_t>(c->infrastructure.rail[i]) * static_cast<int64_t>(RailBuildCost(static_cast<RailType>(i)));
	for (uint32_t count : c->infrastructure.road) value += static_cast<int64_t>(count) * static_cast<int64_t>(_price[PR_BUILD_ROAD]);
	value += static_cast<int64_t>(c->infrastructure.signal) * static_cast<int64_t>(_price[PR_BUILD_SIGNALS]);
	value += static_cast<int64_t>(c->infrastructure.water) * static_cast<int64_t>(_price[PR_BUILD_CANAL]);
	const int64_t percent = BankRepoPercentNow(true);
	return value / 100 * percent + value % 100 * percent / 100;
}

/**
 * Liquidation of a company that is in default at the highest penalty level, or (auction) in the last months before OpenTTD removes it
 * (see BANK_LIQUIDATION_ENABLED, BANK_REPO_ENABLED). In this order: the free margin of the trading wallets, the gold, the vehicles. The money repays
 * the debt. Integer only, game state only.
 * @param company The company.
 * @param auction True when called for the auction of a company in bankruptcy: every vehicle goes (the company that OpenTTD never removes keeps one),
 *                at the auction price, and the value of the stations and infrastructure is worked out for the day the company is removed.
 */
void CentralBank::Liquidate(CompanyID company, bool auction)
{
	if (!BANK_LIQUIDATION_ENABLED) return;
	BankAccount &a = this->accounts[company.base()];
	Company *c = Company::GetIfValid(company);
	if (c == nullptr || a.debt <= 0) { this->auction_infra[company.base()] = 0; return; }

	if (auction) this->auction_infra[company.base()] = BankInfrastructureValue(c);

	int64_t repaid = BankSeizeWallets(company, a.debt);
	repaid += BankSellGoldForDebt(company, a.debt - repaid);
	const bool keep_one = !auction || (!_networking && company == _local_company); // OpenTTD never removes the company of a single player.
	if (repaid < a.debt) repaid += BankSellVehiclesForDebt(company, a.debt - repaid, keep_one, auction);
	repaid = std::min(repaid, a.debt);
	if (repaid <= 0) return;

	a.debt -= repaid;
	RecordLedger(company, LedgerItem::BankLoan, -repaid); // Shown as a repayment in the statement and in the account log.
	if (company == _local_company) {
		AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, fmt::format("{} {} was taken from your trading wallets, your gold and your vehicles, and repaid on your debt.",
				auction ? "The central bank auctioned your assets because of the bankruptcy." : "The central bank liquidated your assets because of the default.",
				GetString(STR_JUST_CURRENCY_LONG, repaid))), NewsType::General, NewsStyle::Small, {});
	}
	InvalidateBankWindows();
}

/**
 * The debt of a company that does not exist any more (bankruptcy). The budget of the bank pays for it as far as it goes; the rest is an
 * uncovered loss. The debt becomes 0, so calling this again does nothing. Called from the daily update and from the cleanup of the market.
 * @param company The company that is gone.
 */
void CentralBank::WriteOffRemoved(CompanyID company)
{
	BankAccount &a = this->accounts[company.base()];
	if (a.debt <= 0) return;
	/* The auction of the stations and the infrastructure that OpenTTD removed together with the company lowers the debt first. */
	const int64_t auction = BANK_REPO_ENABLED ? std::min(a.debt, this->auction_infra[company.base()]) : 0;
	this->auction_infra[company.base()] = 0;
	a.debt -= auction;
	const int64_t loss = a.debt;
	if (loss <= 0) {
		a.debt = 0;
		AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, fmt::format("A company went bankrupt. The central bank sold its stations and infrastructure at the auction for {} and its debt is paid.",
				GetString(STR_JUST_CURRENCY_LONG, auction))), NewsType::General, NewsStyle::Small, {});
		InvalidateBankWindows();
		return;
	}
	const int64_t covered = BANK_LIQUIDATION_ENABLED ? std::min(loss, this->trading_revenue_cents / 100) : loss;
	if (BANK_LIQUIDATION_ENABLED) this->trading_revenue_cents -= covered * 100;
	this->written_off += loss;
	this->uncovered_loss += loss - covered;
	a.debt = 0;
	AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, fmt::format("A company went bankrupt. The central bank sold its stations and infrastructure at the auction for {} and wrote off the rest of its debt, {}.",
			GetString(STR_JUST_CURRENCY_LONG, auction), GetString(STR_JUST_CURRENCY_LONG, loss))), NewsType::General, NewsStyle::Small, {});
	InvalidateBankWindows();
}

/**
 * What a decision does to the markets (only when the rate moved): a hike pushes the gold demand and the sentiment of the shares down, a cut pushes them up.
 * The size follows the move plus the surprise (see PolicyGoldPushPpm), so a move that the bias of the meeting before had announced counts half. The sentiment of the
 * shares fades by itself within a few game hours, the gold pressure by 3% a day. Integer only; run on every client from the daily clock event.
 */
void CentralBank::ApplyMarketEffect()
{
	MonetaryPolicy &p = this->policy;
	p.last_gold_push_ppm = 0;
	p.last_share_push_ppm = 0;
	if (p.last_step_bp == 0) return;
	const int64_t gold = PolicyGoldPushPpm(p.last_step_bp, p.last_surprise_bp);
	const int64_t shares = PolicySharePushPpm(p.last_step_bp, p.last_surprise_bp);
	if (_gold_reserve.GetPriceCents() > 0) {
		_gold_reserve.pressure_ppm = std::clamp<int64_t>(_gold_reserve.pressure_ppm + gold, -GoldReserveManager::PRESSURE_MAX_PPM, GoldReserveManager::PRESSURE_MAX_PPM);
		p.last_gold_push_ppm = gold;
	}
	_market_events.market_sentiment_ppm = std::clamp<int64_t>(_market_events.market_sentiment_ppm + shares, -MARKET_SENTIMENT_MAX_PPM, MARKET_SENTIMENT_MAX_PPM);
	p.last_share_push_ppm = shares;
}

/**
 * Interest on reserve balances (IORB): once per game day the bank pays interest on the UNBORROWED liquid cash of every company in good standing,
 * at PolicyIorbBp (upper bound of the range - 0.10%, so always below what a loan costs). With BANK_IORB_CREATES_MONEY the money is NEW money: it adds to the
 * cash in circulation (so it raises the price index) and costs the bank nothing. Otherwise it comes from the own money of the bank, only the part above
 * BANK_IORB_BUDGET_FLOOR. When the limit is not enough every company gets the same share of what is due. The money is not income of the company
 * for the statistics and the income tax; it is booked as income on the line BankInterest. Integer only, whole money units (the rest of a day is dropped).
 */
void CentralBank::PayIorb()
{
	if (!BANK_IORB_ENABLED) return;
	const int64_t rate_bp = PolicyIorbBp(this->policy.shift_bp);
	if (rate_bp <= 0) return;

	std::array<int64_t, MAX_COMPANIES> due{};
	int64_t total = 0;
	for (const Company *c : Company::Iterate()) {
		if (this->accounts[c->index.base()].in_default) continue;
		const int64_t cash = std::min<int64_t>(GetUnborrowedLiquidCash(c->index), 1'000'000'000'000);
		due[c->index.base()] = cash * rate_bp / BANK_INTEREST_DIVISOR;
		total += due[c->index.base()];
	}
	if (total <= 0) return;
	int64_t available;
	if (BANK_IORB_CREATES_MONEY) {
		/* New money: only a safety limit against a bug or a huge rate. */
		const int64_t circulating = _gold_reserve.GetCirculatingCash();
		available = circulating > 0 ? std::max<int64_t>(1, circulating / 100 * BANK_IORB_CREATE_MAX_PERCENT) : total;
	} else {
		available = std::max<int64_t>(0, this->trading_revenue_cents / 100 - BANK_IORB_BUDGET_FLOOR);
	}
	const int64_t pay_total = std::min(total, available);
	if (pay_total <= 0) return;

	int shift = 0;
	while ((total >> shift) > 1'000'000'000) shift++;
	int64_t paid = 0;
	for (Company *c : Company::Iterate()) {
		const int64_t d = due[c->index.base()];
		if (d <= 0) continue;
		const int64_t share = pay_total >= total ? d : (BankShareOf(pay_total >> shift, d >> shift, total >> shift) << shift);
		if (share <= 0) continue;
		c->money += Money(share);
		RecordLedger(c->index, LedgerItem::BankInterest, share);
		paid += share;
	}
	if (!BANK_IORB_CREATES_MONEY) this->trading_revenue_cents -= paid * 100; // Created money costs the bank nothing.
	this->iorb_created += paid;
}

/** The words of the news item for an unscheduled meeting. */
static const char *UnscheduledText(PolicyUnscheduledReason reason)
{
	switch (reason) {
		case PolicyUnscheduledReason::JobsCrash:      return " (UNSCHEDULED: jobs crash)";
		case PolicyUnscheduledReason::InflationSpike: return " (UNSCHEDULED: inflation spike)";
		case PolicyUnscheduledReason::CrisisNews:     return " (UNSCHEDULED: financial crisis)";
		case PolicyUnscheduledReason::InflationNews:  return " (UNSCHEDULED: inflation shock)";
		default:                                      return " (UNSCHEDULED)";
	}
}

/**
 * The statement after a meeting, as a news item: hold, raise or lower, the new target range and the two numbers behind it.
 * The text is only for the display (every client shows its own news); the policy itself was decided in MonetaryPolicy::Update().
 * @param shift_before The shift before the meeting.
 */
void CentralBank::AnnounceMeeting(int64_t shift_before)
{
	const MonetaryPolicy &p = this->policy;
	auto pct = [](int64_t bp) { return fmt::format("{}.{:02}%", bp / 100, bp % 100); };
	const int64_t moved = p.shift_bp - shift_before;
	const std::string range = fmt::format("{} - {}", pct(PolicyLowerBp(p.shift_bp)), pct(PolicyUpperBp(p.shift_bp)));
	std::string text;
	if (moved > 0) {
		text = fmt::format("FOMC{}: the central bank RAISED the target range by {} bp to {}. Inflation {}, growth {}. Loan rates follow at once.", p.last_unscheduled ? UnscheduledText(p.last_reason) : "", moved, range, pct(std::max<int64_t>(0, p.inflation_bp)), pct(std::max<int64_t>(0, p.growth_bp)));
	} else if (moved < 0) {
		text = fmt::format("FOMC{}: the central bank LOWERED the target range by {} bp to {}. Inflation {}, growth {}. Loan rates follow at once.", p.last_unscheduled ? UnscheduledText(p.last_reason) : "", -moved, range, pct(std::max<int64_t>(0, p.inflation_bp)), pct(std::max<int64_t>(0, p.growth_bp)));
	} else {
		const char *bias = p.wanted_gap_bp >= POLICY_HOLD_BAND_BP ? " Bias: tightening." : (p.wanted_gap_bp <= -POLICY_HOLD_BAND_BP ? " Bias: easing." : "");
		text = fmt::format("FOMC: the central bank KEPT the target range at {}.{}", range, bias);
	}
	if (p.emergency) text += " (Emergency mode.)";
	AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, text), NewsType::General, NewsStyle::Small, {});
}

/**
 * Once per game day, at 00:00 game time, the interest of the day is taken from the cash of every company that has a debt.
 * See BankSettleDay() for the rules: risk rate (3.5% .. 12%) from the debt-to-equity ratio; if the cash is not enough, the
 * penalty rate (15% .. 18%), the cash that exists is taken, the rest is added to the debt, and the penalty level goes up
 * (which lowers the credit limit). Interest is charged on the loan only: the interest that was paid is not added to the debt.
 * A company that is gone has no debt any more.
 * @param day Number of the game day that starts now (game minutes / 1440); a day is never charged twice.
 */
void CentralBank::UpdateDaily(int64_t day)
{
	for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
		const CompanyID company{i};
		BankAccount &a = this->accounts[i];
		Company *c = Company::GetIfValid(company);
		if (c == nullptr) {
			this->WriteOffRemoved(company);
			a = {};
			_ledger.years[i].clear();
			_bank_log[i].clear();
			continue;
		}

		/* The news about the company (and a bank scare for everyone) adds to the risk rate; read from the news log at this midnight, so it is the same on every client. */
		const int64_t news_bp = _market_events.CreditSpreadBp(company.base(), day * GameTimeManager::MINUTES_PER_DAY);
		const BankDayResult r = BankSettleDay(a, static_cast<int64_t>(c->money), GetCentralBankEquity(company), day, this->policy.shift_bp, news_bp);

		if (r.paid_from_cash > 0) {
			/* The money leaves the cash and is an expense of the company (loan interest in the finances window).
			 * Not SubtractMoneyFromCompanyFract: that one counts its cost in 1/256 of a money unit, and would take almost nothing. */
			AutoRestoreBackup cur_company(_current_company, company);
			SubtractMoneyFromCompany(CommandCost(EXPENSES_LOAN_INTEREST, Money(r.paid_from_cash)));
		}
		/* The interest that was paid in cash is income of the bank: it goes into the own money of the bank (not the part added to the debt). */
		if (r.paid_from_cash > 0) this->trading_revenue_cents += r.paid_from_cash * 100;
		/* The ledger shows all the interest of the day: the part that was paid and the part that was added to the debt. */
		RecordLedger(company, LedgerItem::BankInterest, -r.interest_due);

		/* Default at the highest penalty level: the bank takes what it can reach (trading wallets, gold). */
		if (c->months_of_bankruptcy >= BANK_AUCTION_MONTHS) this->Liquidate(company, true);
		else if (r.defaulted && a.penalty_level >= BANK_PENALTY_LEVEL_MAX) this->Liquidate(company);

	}

	/* The monetary policy: the sum of the company values of the last two finished quarters is the measure of growth (as in the gold market). */
	int64_t value_now = 0, value_before = 0;
	for (const Company *c : Company::Iterate()) {
		value_now += static_cast<int64_t>(c->old_economy[0].company_value);
		value_before += static_cast<int64_t>(c->old_economy[1].company_value);
	}
	/* Second mandate: the jobs. The two newest payroll reports (newest in front) give the yearly growth of the employment. */
	int64_t jobs_growth_bp = 0;
	bool has_jobs = false;
	if (_payroll_news.releases.size() >= 2) {
		const int64_t now_avg = _payroll_news.releases[0].avg_x100;
		const int64_t before_avg = _payroll_news.releases[1].avg_x100;
		if (before_avg > 0) {
			jobs_growth_bp = std::clamp<int64_t>((now_avg - before_avg) * 10'000 / before_avg, -10'000, 100'000) * BANK_DAYS_PER_YEAR / PAYROLL_PERIOD_DAYS;
			has_jobs = true;
		}
	}
	const int64_t shift_before_meeting = this->policy.shift_bp;
	const int64_t jobs_report_day = _payroll_news.releases.empty() ? -1 : _payroll_news.releases.front().day;
	const int64_t index_now = GetPriceIndexPpm();
	if (this->policy.Update(day, index_now, value_now, value_before, jobs_growth_bp, has_jobs) ||
			this->policy.CheckUnscheduled(day, index_now, jobs_growth_bp, has_jobs, jobs_report_day)) {
		this->ApplyMarketEffect();
		this->AnnounceMeeting(shift_before_meeting);
	}
	this->PayIorb();
	this->TradeGold(day);

	SnapshotYearlyExpenses();
	InvalidateWindowData(WC_STOCK_MARKET, 0);
	InvalidateWindowData(WC_CENTRAL_BANK, 0);
}

/** Charge the interest of the central bank once per game day of the HH:MM clock (at 00:00, when the date of the game changes). */
static const GameTimeSubscription _bank_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &now)
{
	_central_bank.UpdateDaily(static_cast<int64_t>(now.total_minutes / GameTimeManager::MINUTES_PER_DAY));
});

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
	this->trading_revenue_cents = 0;
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
 * One day of the monetary policy. Once every POLICY_DECISION_DAYS days: measure the yearly change of the price index and the
 * yearly growth of the company values, ask the rule for the shift (PolicyDesiredShiftBp) and move the shift towards it by at
 * most POLICY_MAX_STEP_BP.
 * @param day Number of the game day.
 * @param index_ppm The price index now (GetPriceIndexPpm).
 * @param value_now Sum of the company values of the last finished quarter.
 * @param value_before Sum of the company values of the quarter before it.
 * @return True when a decision was made today.
 */
bool MonetaryPolicy::Update(int64_t day, int64_t index_ppm, int64_t value_now, int64_t value_before)
{
	if (index_ppm <= 0) return false;
	const int64_t elapsed = day - this->last_decision_day;
	if (this->last_decision_day < 0 || this->prev_index_ppm <= 0 || elapsed < 0) {
		/* First day (new game, or a savegame from before the policy): only note the starting point. */
		this->prev_index_ppm = index_ppm;
		this->last_decision_day = day;
		return false;
	}
	if (elapsed < POLICY_DECISION_DAYS) return false;

	this->inflation_bp = std::clamp<int64_t>((index_ppm - this->prev_index_ppm) * 10'000 * BANK_DAYS_PER_YEAR / (this->prev_index_ppm * elapsed), -2000, 5000);

	int64_t growth_period_bp = 0;
	while (value_before > 10'000'000'000'000 ) { value_before /= 1000; value_now /= 1000; } // Only for absurd values: keeps the multiplication below small.
	if (value_before > 0) growth_period_bp = std::clamp<int64_t>((value_now - value_before) * 10'000 / value_before, -10'000, 100'000);
	this->growth_bp = growth_period_bp * BANK_DAYS_PER_YEAR / POLICY_DECISION_DAYS;

	const int64_t wanted = PolicyDesiredShiftBp(this->inflation_bp, this->growth_bp);
	const int64_t step = std::clamp<int64_t>(wanted - this->shift_bp, -POLICY_MAX_STEP_BP, POLICY_MAX_STEP_BP);
	this->shift_bp = std::clamp<int64_t>(this->shift_bp + step, POLICY_SHIFT_MIN_BP, POLICY_SHIFT_MAX_BP);
	this->last_step_bp = step;
	this->prev_index_ppm = index_ppm;
	this->last_decision_day = day;
	this->measured = true;
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
	return std::max<int64_t>(0, equity) * CENTRAL_BANK_CREDIT_PERCENT / 100 * percent / 100;
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
	if (a.in_default) return BankPenaltyRateBp(std::max<int>(1, a.penalty_level));
	return BankShiftedRateBp(BankAnnualRateBp(BankDebtToEquityPpm(a.debt, GetCentralBankEquity(company))), _central_bank.policy.shift_bp);
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
			a = {};
			_ledger.years[i].clear();
			_bank_log[i].clear();
			continue;
		}

		const BankDayResult r = BankSettleDay(a, static_cast<int64_t>(c->money), GetCentralBankEquity(company), day, this->policy.shift_bp);

		if (r.paid_from_cash > 0) {
			/* The money leaves the cash and is an expense of the company (loan interest in the finances window).
			 * Not SubtractMoneyFromCompanyFract: that one counts its cost in 1/256 of a money unit, and would take almost nothing. */
			AutoRestoreBackup cur_company(_current_company, company);
			SubtractMoneyFromCompany(CommandCost(EXPENSES_LOAN_INTEREST, Money(r.paid_from_cash)));
		}
		/* The ledger shows all the interest of the day: the part that was paid and the part that was added to the debt. */
		RecordLedger(company, LedgerItem::BankInterest, -r.interest_due);

	}

	/* The monetary policy: the sum of the company values of the last two finished quarters is the measure of growth (as in the gold market). */
	int64_t value_now = 0, value_before = 0;
	for (const Company *c : Company::Iterate()) {
		value_now += static_cast<int64_t>(c->old_economy[0].company_value);
		value_before += static_cast<int64_t>(c->old_economy[1].company_value);
	}
	this->policy.Update(day, GetPriceIndexPpm(), value_now, value_before);

	SnapshotYearlyExpenses();
	InvalidateWindowData(WC_STOCK_MARKET, 0);
	InvalidateWindowData(WC_CENTRAL_BANK, 0);
}

/** Charge the interest of the central bank once per game day of the HH:MM clock (at 00:00, when the date of the game changes). */
static const GameTimeSubscription _bank_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &now)
{
	_central_bank.UpdateDaily(static_cast<int64_t>(now.total_minutes / GameTimeManager::MINUTES_PER_DAY));
});

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file central_bank.h The central bank: loans that are paid out to, and repaid from, a wallet of a company. */

#ifndef CENTRAL_BANK_H
#define CENTRAL_BANK_H

#include "command_type.h"
#include "company_type.h"
#include "economy_type.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <map>

/** OLD flat rate (about 10% a year). No longer used by the interest; kept only because central_bank_gui.cpp still shows an estimate with it. */
static constexpr int64_t CENTRAL_BANK_RATE_PPM_PER_DAY = 274;

/** A company can borrow as long as its debt stays below this percentage of its net equity (company value plus wallets minus the debt). */
static constexpr int64_t CENTRAL_BANK_CREDIT_PERCENT = 50;

/*
 * ---- Risk-based interest, daily accrual and default penalty --------------------------------------------------------
 * All money is int64 and all rates are integers (basis points, 1 bp = 0.01% per year), so every client of a network game
 * gets exactly the same numbers. No float or double is used anywhere.
 */

/** Days in a game year, the divisor of the daily interest. Same as GameTimeManager::DAYS_PER_YEAR. */
static constexpr int64_t BANK_DAYS_PER_YEAR = 365;
/** Basis points in 100%: Daily Interest = debt * rate_bp / (10,000 * 365). */
static constexpr int64_t BANK_BP_DENOMINATOR = 10'000;
/** The divisor of the daily interest, in one number: 10,000 * 365 = 3,650,000. */
static constexpr int64_t BANK_INTEREST_DIVISOR = BANK_BP_DENOMINATOR * BANK_DAYS_PER_YEAR;

/** Debt-to-equity ratio in parts per million (1,000,000 = D/E of 1.0). */
static constexpr int64_t BANK_RATIO_ONE = 1'000'000;

/**
 * The risk curve: (D/E ratio, annual rate in bp), linear in between.
 *   D/E 0.0 ... 0.5 : 3.5% ... 5.0%   good credit (low debt, high assets)
 *   D/E 0.5 ... 1.0 : 5.0% ... 8.0%   transition, so the rate never jumps
 *   D/E 1.0 ... 3.0 : 8.0% ... 12.0%  poor credit (high debt, startup)
 * D/E above 3.0, or equity of 0 or less, is always the highest rate (12.0%).
 */
static constexpr int64_t BANK_RATE_GOOD_MIN_BP = 350;   ///< 3.5%
static constexpr int64_t BANK_RATE_GOOD_MAX_BP = 500;   ///< 5.0%
static constexpr int64_t BANK_RATE_POOR_MIN_BP = 800;   ///< 8.0%
static constexpr int64_t BANK_RATE_POOR_MAX_BP = 1200;  ///< 12.0%
static constexpr int64_t BANK_RATIO_GOOD_MAX = BANK_RATIO_ONE / 2;      ///< D/E 0.5
static constexpr int64_t BANK_RATIO_POOR_MIN = BANK_RATIO_ONE;          ///< D/E 1.0
static constexpr int64_t BANK_RATIO_POOR_MAX = 3 * BANK_RATIO_ONE;      ///< D/E 3.0

/** Penalty (default) rate: 15.0% for the first default day, +0.5% for every further level, at most 18.0%. */
static constexpr int64_t BANK_PENALTY_MIN_BP = 1500;
static constexpr int64_t BANK_PENALTY_MAX_BP = 1800;
static constexpr int64_t BANK_PENALTY_STEP_BP = 50;

/** Penalty level 0..8. Each level takes 10% off the credit limit; the limit never goes below 20% of the normal one. */
static constexpr int BANK_PENALTY_LEVEL_MAX = 8;
static constexpr int64_t BANK_PENALTY_CREDIT_STEP_PERCENT = 10;
/** Days in a row of interest paid on time that lower the penalty level by one. */
static constexpr int BANK_PENALTY_RECOVERY_DAYS = 30;

/**
 * Debt-to-equity ratio in ppm. Equity of 0 or less (or any ratio above the top of the curve) gives the top of the curve.
 * @param debt Money owed, 0 or more.
 * @param equity Company value plus wallets minus the debt.
 */
inline int64_t BankDebtToEquityPpm(int64_t debt, int64_t equity)
{
	if (debt <= 0) return 0;
	if (equity <= 0) return BANK_RATIO_POOR_MAX;
	if (debt / 3 >= equity) return BANK_RATIO_POOR_MAX; // D/E >= 3.0 (also keeps the multiplication below small).
	while (debt > INT64_MAX / BANK_RATIO_ONE) { debt /= 1000; equity = std::max<int64_t>(1, equity / 1000); } // Only for absurd amounts.
	return std::min(BANK_RATIO_POOR_MAX, debt * BANK_RATIO_ONE / equity);
}

/** Linear interpolation of the rate between two points of the curve (all integer, rounded to the nearest bp). */
inline int64_t BankLerpBp(int64_t ratio, int64_t r0, int64_t r1, int64_t bp0, int64_t bp1)
{
	return bp0 + ((bp1 - bp0) * (ratio - r0) + (r1 - r0) / 2) / (r1 - r0);
}

/** The annual interest rate in bp for a debt-to-equity ratio (ppm): 350..500 good, 500..800 transition, 800..1200 poor. */
inline int64_t BankAnnualRateBp(int64_t ratio_ppm)
{
	if (ratio_ppm <= 0) return BANK_RATE_GOOD_MIN_BP;
	if (ratio_ppm <= BANK_RATIO_GOOD_MAX) return BankLerpBp(ratio_ppm, 0, BANK_RATIO_GOOD_MAX, BANK_RATE_GOOD_MIN_BP, BANK_RATE_GOOD_MAX_BP);
	if (ratio_ppm <= BANK_RATIO_POOR_MIN) return BankLerpBp(ratio_ppm, BANK_RATIO_GOOD_MAX, BANK_RATIO_POOR_MIN, BANK_RATE_GOOD_MAX_BP, BANK_RATE_POOR_MIN_BP);
	if (ratio_ppm < BANK_RATIO_POOR_MAX) return BankLerpBp(ratio_ppm, BANK_RATIO_POOR_MIN, BANK_RATIO_POOR_MAX, BANK_RATE_POOR_MIN_BP, BANK_RATE_POOR_MAX_BP);
	return BANK_RATE_POOR_MAX_BP;
}

/** The penalty rate in bp for a penalty level (1 or more): 1500, 1550, ... capped at 1800. */
inline int64_t BankPenaltyRateBp(int level)
{
	return std::min(BANK_PENALTY_MAX_BP, BANK_PENALTY_MIN_BP + BANK_PENALTY_STEP_BP * std::max(0, level - 1));
}

/** Result of one day of interest: the whole money and the part of a money unit that is carried to the next day. */
struct BankInterestResult {
	int64_t interest = 0; ///< Whole money units of interest for this day.
	int64_t carry = 0;    ///< Rest in 1/BANK_INTEREST_DIVISOR of a money unit, 0 .. BANK_INTEREST_DIVISOR - 1.
};

/**
 * Daily Interest = debt * (rate / 100) / 365, with rate = rate_bp / 100.
 * Exact integer arithmetic: the rest that is smaller than one money unit is carried to the next day, so over a whole
 * year the interest is exactly debt * rate / 100 and nothing is lost to rounding. Overflow-safe for any int64 debt.
 * @param debt Outstanding loan amount.
 * @param rate_bp Annual rate in bp.
 * @param carry_in Rest of the days before.
 */
inline BankInterestResult BankDailyInterest(int64_t debt, int64_t rate_bp, int64_t carry_in)
{
	BankInterestResult r;
	if (debt <= 0 || rate_bp <= 0) { r.carry = std::max<int64_t>(0, carry_in); return r; }
	const int64_t whole = debt / BANK_INTEREST_DIVISOR;
	const int64_t rest = debt % BANK_INTEREST_DIVISOR;
	const int64_t rest_num = rest * rate_bp + std::max<int64_t>(0, carry_in); // < 3.65e6 * 1.8e3, no overflow.
	r.interest = whole * rate_bp + rest_num / BANK_INTEREST_DIVISOR;
	r.carry = rest_num % BANK_INTEREST_DIVISOR;
	return r;
}

/** The loan of one company at the central bank. */
struct BankAccount {
	int64_t debt = 0;            ///< Money owed to the central bank. Only the loan and unpaid (defaulted) interest; interest that is paid does not go in here.
	int64_t interest_paid = 0;   ///< Total interest that was charged, for the statistics (paid from cash, or added to the debt when the cash was not enough).
	/* Risk and default status. */
	int64_t rate_bp = 0;         ///< Annual rate that was charged at the last midnight, in bp (0 when there was no debt).
	int64_t carry = 0;           ///< Rest of the daily interest that is smaller than one money unit, see BankDailyInterest().
	int64_t last_charged_day = -1; ///< Number of the game day that was charged last: the interest is charged once per day.
	bool in_default = false;     ///< True from the day that the cash was not enough, until a day that the interest is paid in full.
	uint8_t penalty_level = 0;   ///< 0..BANK_PENALTY_LEVEL_MAX. Raises the penalty rate and lowers the credit limit. Falls by 1 after BANK_PENALTY_RECOVERY_DAYS days of paying on time.
	uint16_t clean_days = 0;     ///< Days in a row that the interest was paid on time.
};

/** What the bank does at one midnight to one account. The caller moves the money. */
struct BankDayResult {
	int64_t rate_bp = 0;         ///< Annual rate used today.
	int64_t interest_due = 0;    ///< Interest of today at that rate.
	int64_t paid_from_cash = 0;  ///< Taken from the company cash.
	int64_t added_to_debt = 0;   ///< Not paid: added to the debt (default).
	bool defaulted = false;      ///< True when the cash was not enough today.
};

/**
 * One midnight of one account: decides the rate, how much is paid from the cash and what is added to the debt, and updates
 * the account (rate, carry, default status, penalty level). The cash itself is not changed here.
 *
 *   1. No debt: nothing to pay; the default status ends, the penalty recovers.
 *   2. Rate = risk rate of the D/E ratio; or the penalty rate while the account is in default.
 *   3. Cash >= interest: all is paid from the cash. A default ends. A normal day counts for the recovery.
 *   4. Cash < interest: default. The rate becomes the penalty rate (15..18%), the interest is calculated again at that
 *      rate, the cash that exists is taken, the rest is added to the debt, and the penalty level goes up by 1.
 *
 * @param a The account (changed).
 * @param cash Company cash now; a negative cash counts as 0.
 * @param equity Company value plus wallets minus the debt.
 * @param day Number of the game day, to charge once per day.
 * @return What to book; all 0 when the day was already charged.
 */
inline BankDayResult BankSettleDay(BankAccount &a, int64_t cash, int64_t equity, int64_t day)
{
	BankDayResult out;
	if (day <= a.last_charged_day) return out; // Once per day, whatever the callers do.
	a.last_charged_day = day;

	if (a.debt <= 0) {
		a.debt = 0;
		a.rate_bp = 0;
		a.carry = 0;
		a.in_default = false;
	} else {
		const int64_t risk_bp = BankAnnualRateBp(BankDebtToEquityPpm(a.debt, equity));
		out.rate_bp = a.in_default ? BankPenaltyRateBp(std::max<int>(1, a.penalty_level)) : risk_bp;
		BankInterestResult r = BankDailyInterest(a.debt, out.rate_bp, a.carry);

		if (std::max<int64_t>(0, cash) >= r.interest) {
			/* Paid in full. */
			out.paid_from_cash = r.interest;
			if (a.in_default) { a.in_default = false; a.clean_days = 0; } else if (a.clean_days < UINT16_MAX) a.clean_days++;
		} else {
			/* Default: penalty rate, calculated again at the penalty rate. */
			a.in_default = true;
			a.clean_days = 0;
			a.penalty_level = static_cast<uint8_t>(std::min<int>(BANK_PENALTY_LEVEL_MAX, a.penalty_level + 1));
			out.defaulted = true;
			out.rate_bp = BankPenaltyRateBp(a.penalty_level);
			r = BankDailyInterest(a.debt, out.rate_bp, a.carry);
			out.paid_from_cash = std::min(std::max<int64_t>(0, cash), r.interest);
			out.added_to_debt = r.interest - out.paid_from_cash;
			a.debt += out.added_to_debt;
		}
		out.interest_due = r.interest;
		a.carry = r.carry;
		a.rate_bp = out.rate_bp;
		a.interest_paid += r.interest;
	}

	/* Recovery of the penalty level: BANK_PENALTY_RECOVERY_DAYS days in a row on time (or without debt). */
	if (!out.defaulted && !a.in_default) {
		if (a.debt <= 0 && a.clean_days < UINT16_MAX) a.clean_days++;
		if (a.penalty_level > 0 && a.clean_days >= BANK_PENALTY_RECOVERY_DAYS) { a.penalty_level--; a.clean_days = 0; }
	}
	return out;
}

/** Percentage of the normal credit limit that is left for a penalty level: 100%, 90%, ... 20%. */
inline int64_t BankCreditPercentForPenalty(int level)
{
	return 100 - BANK_PENALTY_CREDIT_STEP_PERCENT * std::min(std::max(0, level), BANK_PENALTY_LEVEL_MAX);
}

/** The central bank: one account per company. Saved in the 'STCK' chunk together with the wallets. */
struct CentralBank {
	std::array<BankAccount, MAX_COMPANIES> accounts{};
	int64_t trading_revenue_cents = 0; ///< The budget of the bank from trading: commission and spread of the gold trades, in 1/100 of a money unit. Saved (SRK_BANK_BUDGET).

	void Reset();
	void UpdateDaily(int64_t day);
};

extern CentralBank _central_bank;

/**
 * The lines of the account statement that the game books itself. The costs of vehicles, tracks and stations are read from
 * the yearly expenses of the company (see the Central Bank window), they are not in the ledger.
 */
enum class LedgerItem : uint8_t {
	BankInterest,    ///< Interest that is added to the debt at the central bank (expense).
	TradeCommission, ///< Commission of a trade, taken from the trading wallet (expense).
	MarginSwap,      ///< Daily swap of the open positions, taken from the trading wallet (expense).
	Dividends,       ///< Dividends of the shares of other companies (income) and the dividend that the company pays itself (expense).
	RealizedProfit,  ///< Profit (income) and loss (expense) of closed trades.
	WalletTransfer,  ///< Money moved between the company cash and another wallet: into the cash (income) and out of the cash (expense).
	BankLoan,        ///< Money that the central bank paid out (income) and what was repaid (expense).
	/* Copies of the yearly expenses of the game. The game keeps three years only; the copy is kept #LEDGER_KEEP_YEARS years (past statements). */
	YearTrain,       ///< Income of trains.
	YearRoadVeh,     ///< Income of road vehicles.
	YearShip,        ///< Income of ships.
	YearAircraft,    ///< Income of aircraft.
	YearRunning,     ///< Running costs of vehicles and property.
	YearConstruction,///< Construction and land purchase.
	YearNewVehicles, ///< Purchases of new vehicles.
	GoldBuy,         ///< Gold bought for the vault (expense): an exchange of cash for an asset, shown but not counted as a cost. Appended last: the index is saved.
	GoldSell,        ///< Gold sold from the vault (income): shown, not counted as earnings (the profit is on RealizedProfit).
	End,
};
static constexpr size_t LEDGER_ITEMS = static_cast<size_t>(LedgerItem::End);

/** What one line of the ledger booked in one game year. */
struct LedgerEntry {
	int64_t income = 0;  ///< Money that came in.
	int64_t expense = 0; ///< Money that went out.
};

/** How many game years the ledger keeps: this year and the nine before it. */
static constexpr int32_t LEDGER_KEEP_YEARS = 10;

/** The ledgers of the companies: per company and game year the entry of every line. Only the last #LEDGER_KEEP_YEARS years are kept. Saved in the 'STCK' chunk. */
struct Ledger {
	std::array<std::map<int32_t, std::array<LedgerEntry, LEDGER_ITEMS>>, MAX_COMPANIES> years;

	void Reset() { for (auto &y : this->years) y.clear(); }
};

extern Ledger _ledger;

/** One line of the account log that the Central Bank window shows: what was booked, on which game day, and how much. */
struct BankLogEntry {
	int32_t day = 0;                       ///< Day of the game year (1..365) when it was booked.
	LedgerItem item = LedgerItem::End;     ///< What was booked.
	int64_t amount = 0;                    ///< Money in (positive) or out (negative).
};

/** How many of the latest lines are kept per company. Only for the display: it is not saved, and it starts empty after a load. */
static constexpr size_t BANK_LOG_MAX = 16;

/** The latest lines of the account log per company, the newest in front. */
extern std::array<std::deque<BankLogEntry>, MAX_COMPANIES> _bank_log;

void RecordLedger(CompanyID company, LedgerItem item, int64_t amount);
LedgerEntry GetLedgerEntry(CompanyID company, LedgerItem item, int32_t year);
int32_t GetLedgerCurrentYear();
void SnapshotYearlyExpenses();

int64_t GetCentralBankDebt(CompanyID company);
int64_t GetCentralBankAnnualRateBp(CompanyID company);
int64_t GetCentralBankCreditLimit(CompanyID company);
int64_t GetCentralBankAvailable(CompanyID company);
int64_t GetUnborrowedLiquidCash(CompanyID company);
CommandCost CheckTradingCapital(CompanyID company, int64_t amount);

CommandCost CmdBankBorrow(DoCommandFlags flags, uint8_t wallet, Money amount);
CommandCost CmdBankRepay(DoCommandFlags flags, uint8_t wallet, Money amount);

DEF_CMD_TRAIT(CMD_BANK_BORROW, CmdBankBorrow, {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_BANK_REPAY,  CmdBankRepay,  {}, CommandType::MoneyManagement)

#endif /* CENTRAL_BANK_H */

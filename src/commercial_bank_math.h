/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/**
 * @file commercial_bank_math.h Commercial banks of the towns (integer maths only, no game headers, so that commercial_bank_test.cpp can test it alone).
 *
 * Every town has ONE commercial bank. It is a small copy of the central bank model of the game (central_bank.h):
 *   - the same RATING scale C / B / A / AA / AAA, but here it comes from the capital of the bank (equity / loans), not from gold;
 *   - the same rate curve and policy rate: a loan costs what the central bank would charge (the risk curve of the debt-to-equity ratio moved by the policy shift)
 *     plus a margin that depends on the rating of THIS bank (a strong bank is cheaper than the central bank, a weak one dearer);
 *   - the same account, default and penalty rules (BankAccount / BankSettleDay of central_bank.h are used for the loans to companies);
 *   - the same write-off: a loan of a company that is gone is lost against the capital of the bank;
 *   - the central bank is the LENDER OF LAST RESORT: it lends reserves to a commercial bank that is short (at the policy rate + REFINANCE_SPREAD_BP), pays interest
 *     on the reserves (IORB), and rescues a bank whose equity is negative (deposit insurance), as far as its own money goes.
 *
 * Balance sheet of a bank (money units):
 *   assets      = liquid (reserves at the central bank) + loans to the town treasury + loans to companies
 *   liabilities = deposits of the citizens (four groups) + refinancing from the central bank (cb_loan)
 *   equity      = assets - liabilities   (never stored: always worked out, so it can not drift)
 *
 * Money flows: the citizens put part of their cash into deposits (cash -> deposit, DEPOSIT_TARGET_PERCENT of each group) and take it out again; the deposit interest is
 * credited to the deposits; a loan moves money from the liquid assets of the bank into the cash of the borrower; the loan interest comes back into the liquid assets.
 * Nothing but the reserve interest (IORB) is made from nothing, and that is counted in Sheet::created.
 *
 * Every number is an integer: the same on every client of a network game.
 */

#ifndef COMMERCIAL_BANK_MATH_H
#define COMMERCIAL_BANK_MATH_H

#include <algorithm>
#include <array>
#include <cstdint>
#include "town_wallet_math.h"

namespace cbank {

static constexpr int64_t PPM = 1'000'000;
static constexpr int GROUPS = 4;                          ///< The four groups of citizens (citizengold::STYLE_COUNT, checked by a static_assert in commercial_bank.cpp).
static constexpr int RATINGS = 5;                         ///< 0 = C, 1 = B, 2 = A, 3 = AA, 4 = AAA (the same order as the credit standing of the central bank).

/* ---- Size and capital ---- */
static constexpr int64_t START_CAPITAL_PER_CAPITA = 40;   ///< Capital that a new bank gets per citizen (money units at price index 1.0).
static constexpr int64_t START_CAPITAL_MIN = 2'000;       ///< ... but never less than this, so a village has a bank too.
static constexpr int64_t RESERVE_PERCENT = 10;            ///< Reserve requirement: the bank keeps 10% of the deposits as liquid assets.
static constexpr int64_t MAX_LEVERAGE = 12;               ///< Loans (to the town and to companies together) are at most 12 times the equity.
static constexpr int64_t LARGE_EXPOSURE_PERCENT = 40;     ///< One borrower may owe the bank at most 40% of its equity.
static constexpr int64_t EXTRA_CREDIT_PERCENT = 25;       ///< The total credit pool of a company (central bank + commercial banks) is the central limit + 25%: local banks know their borrowers.

/* ---- Deposits ---- */
static constexpr std::array<int64_t, GROUPS> DEPOSIT_TARGET_PERCENT = {30, 5, 0, 50}; ///< Share of the money of a group (cash + deposit) that it keeps in the bank: hoarders, speculators, overtraders, balanced savers.
static constexpr int64_t DEPOSIT_BUILDING_BONUS_PERCENT = 10; ///< Where an in-game bank building stands, the groups that save keep 10 points more in the bank.
static constexpr int64_t DEPOSIT_MOVE_PERCENT = 10;       ///< Each game day a group closes 10% of the gap between its deposit and its target.
static constexpr int64_t DEPOSIT_SPREAD_BP = 150;         ///< Deposit rate = policy rate - 1.50% (never below 0).

/* ---- Rating (capital ratio = equity / loans, in bp) ---- */
static constexpr int64_t RATING_AAA_BP = 2500;            ///< 25% and more.
static constexpr int64_t RATING_AA_BP = 1700;
static constexpr int64_t RATING_A_BP = 1100;
static constexpr int64_t RATING_B_BP = 700;               ///< Below 7%, or no equity: C.

/* ---- Prices ---- */
static constexpr std::array<int64_t, RATINGS> LOAN_MARGIN_BP = {75, 25, -10, -25, -40}; ///< Added to the central bank rate of the same borrower: C +0.75%, B +0.25%, A -0.10%, AA -0.25%, AAA -0.40%.
static constexpr int64_t BUILDING_MARGIN_CUT_BP = 25;     ///< A bank with a bank building in its town (the head office of the game) is 0.25% cheaper.
static constexpr int64_t REFINANCE_SPREAD_BP = 50;        ///< The central bank lends reserves at the policy rate + 0.50%.
static constexpr int64_t TOWN_LOAN_LIMIT_PER_CAPITA = 40; ///< A town can owe its commercial bank at most 40 per citizen (like the central bank loan).
static constexpr int64_t CB_REPAY_PERCENT_PER_DAY = 20;   ///< The bank repays at most 20% of its central bank loan per game day, from liquid assets above twice the reserve.

/** Money kept by one bank. A plain struct, saved by commercial_bank.cpp (SRK_CBANK). */
struct Sheet {
	int64_t liquid = 0;                          ///< Reserves at the central bank. Not below 0.
	std::array<int64_t, GROUPS> deposits{};      ///< Deposits of the four groups of citizens.
	int64_t cb_loan = 0;                         ///< Owed to the central bank (refinancing).
	int64_t dep_carry = 0;                       ///< Rest of the deposit interest below one unit.
	int64_t res_carry = 0;                       ///< Rest of the reserve interest.
	int64_t cb_carry = 0;                        ///< Rest of the interest on the central bank loan.
	int64_t written_off = 0;                     ///< Loans lost (companies that are gone, a town that is gone), total.
	int64_t failures = 0;                        ///< Times that the central bank had to rescue the bank.
	int64_t created = 0;                         ///< Reserve interest (IORB) that was made from nothing, total.
	int64_t loan_interest = 0;                   ///< Loan interest received, total (for the statistics).
	int64_t deposit_interest = 0;                ///< Deposit interest credited, total.

	int64_t TotalDeposits() const { int64_t t = 0; for (int64_t d : this->deposits) t += d; return t; }
};

/** Equity of a bank: assets - liabilities, `loans` = everything that it has lent (town + companies). */
inline int64_t Equity(const Sheet &s, int64_t loans) { return s.liquid + loans - s.TotalDeposits() - s.cb_loan; }

/** Capital that a new bank gets: per citizen, with a minimum, both scaled by the price index. */
inline int64_t StartCapital(int64_t population, int64_t index_ppm)
{
	const int64_t base = std::max<int64_t>(START_CAPITAL_MIN, std::max<int64_t>(0, population) * START_CAPITAL_PER_CAPITA);
	return base / PPM * std::max<int64_t>(index_ppm, 0) + base % PPM * std::max<int64_t>(index_ppm, 0) / PPM;
}

/** Rating of a bank from the capital ratio equity / loans: 0 = C ... 4 = AAA. A bank with no loans is as safe as it gets, a bank with no equity is C. */
inline int RatingIndex(int64_t equity, int64_t loans)
{
	if (equity <= 0) return 0;
	if (loans <= 0) return RATINGS - 1;
	while (loans > 100'000'000'000'000LL) { loans /= 1000; equity /= 1000; if (equity <= 0) return 0; } // absurd amounts: keep the multiplication below small
	if (equity >= loans) return RATINGS - 1;                  // equity of 100% of the loans or more is far above the best band
	const int64_t ratio_bp = equity * 10'000 / loans;          // equity < loans <= 1e14, so no overflow
	if (ratio_bp >= RATING_AAA_BP) return 4;
	if (ratio_bp >= RATING_AA_BP) return 3;
	if (ratio_bp >= RATING_A_BP) return 2;
	if (ratio_bp >= RATING_B_BP) return 1;
	return 0;
}

inline const char *RatingName(int rating)
{
	static constexpr const char *N[RATINGS] = {"C", "B", "A", "AA", "AAA"};
	return N[std::clamp(rating, 0, RATINGS - 1)];
}

/** Margin of a loan of this bank over the central bank rate of the same borrower, in bp (can be negative). */
inline int64_t LoanMarginBp(int rating, bool has_building)
{
	return LOAN_MARGIN_BP[static_cast<size_t>(std::clamp(rating, 0, RATINGS - 1))] - (has_building ? BUILDING_MARGIN_CUT_BP : 0);
}

/** Rate of a loan to the town treasury (bp): the central bank rate for towns (policy rate + 1%) plus the margin of the bank. */
inline int64_t TownLoanRateBp(int64_t policy_upper_bp, int rating, bool has_building)
{
	return std::max<int64_t>(townwallet::RATE_MIN_BP, townwallet::LoanRateBp(policy_upper_bp) + LoanMarginBp(rating, has_building));
}

/** What the bank pays on deposits (bp). */
inline int64_t DepositRateBp(int64_t policy_upper_bp) { return std::max<int64_t>(0, policy_upper_bp - DEPOSIT_SPREAD_BP); }

/** What the central bank charges the bank for reserves (bp). */
inline int64_t RefinanceRateBp(int64_t policy_upper_bp) { return std::max<int64_t>(townwallet::RATE_MIN_BP, policy_upper_bp + REFINANCE_SPREAD_BP); }

/** Share (percent) of its money that a group keeps in the bank. */
inline int64_t DepositTargetPercent(int group, bool has_building)
{
	const int64_t t = DEPOSIT_TARGET_PERCENT[static_cast<size_t>(std::clamp(group, 0, GROUPS - 1))];
	return std::clamp<int64_t>(t + (has_building && t > 0 ? DEPOSIT_BUILDING_BONUS_PERCENT : 0), 0, 90);
}

/**
 * The move of one group in one game day: positive = cash goes into the deposit, negative = a withdrawal. The group closes a part of the gap between its deposit and
 * target% of (cash + deposit); a gap of 10 units or less is closed at once. Never more than the cash (deposit) or the deposit (withdrawal), cash + deposit does not change.
 */
inline int64_t DepositMove(int64_t cash, int64_t deposit, int64_t target_percent)
{
	cash = std::max<int64_t>(cash, 0);
	deposit = std::max<int64_t>(deposit, 0);
	const int64_t want = townwallet::Percent(cash + deposit, std::clamp<int64_t>(target_percent, 0, 100));
	const int64_t gap = want - deposit;
	if (gap == 0) return 0;
	const int64_t a = gap < 0 ? -gap : gap;
	const int64_t move = a <= 10 ? a : std::max<int64_t>(1, townwallet::Percent(a, DEPOSIT_MOVE_PERCENT));
	return gap < 0 ? -move : move;
}

/** Reserve that the bank must keep for its deposits. */
inline int64_t ReserveNeed(int64_t deposits) { return townwallet::Percent(std::max<int64_t>(deposits, 0), RESERVE_PERCENT); }

/**
 * How much more the bank can lend now: the liquid assets above the reserve, and never beyond MAX_LEVERAGE times the equity.
 * @param equity Equity of the bank now.
 * @param loans Everything that it has lent now.
 */
inline int64_t Capacity(const Sheet &s, int64_t equity, int64_t loans)
{
	const int64_t by_liquidity = std::max<int64_t>(0, s.liquid - ReserveNeed(s.TotalDeposits()));
	const int64_t by_capital = equity > 0 ? std::max<int64_t>(0, equity * MAX_LEVERAGE - loans) : 0;
	return std::min(by_liquidity, by_capital);
}

/** The most that ONE borrower may owe the bank. */
inline int64_t LargeExposureLimit(int64_t equity) { return townwallet::Percent(std::max<int64_t>(equity, 0), LARGE_EXPOSURE_PERCENT); }

/** The most that a town can owe its commercial bank. */
inline int64_t TownLoanLimit(int64_t scaled_population) { return std::max<int64_t>(0, scaled_population) * TOWN_LOAN_LIMIT_PER_CAPITA; }

/** What one lender may still give a company: the credit pool is the central limit plus EXTRA_CREDIT_PERCENT; the central part comes first. */
inline int64_t TotalCreditLimit(int64_t central_limit) { return std::max<int64_t>(0, central_limit) / 100 * (100 + EXTRA_CREDIT_PERCENT) + std::max<int64_t>(0, central_limit) % 100 * (100 + EXTRA_CREDIT_PERCENT) / 100; }

/** Split `amount` over the groups in proportion to their deposits; the last group with a deposit takes the rest. Returns the part of each group; sums to `amount` when there are deposits. */
inline std::array<int64_t, GROUPS> SplitByDeposits(const std::array<int64_t, GROUPS> &deposits, int64_t amount)
{
	std::array<int64_t, GROUPS> out{};
	int64_t total = 0;
	int last = -1;
	for (int i = 0; i < GROUPS; i++) if (deposits[i] > 0) { total += deposits[i]; last = i; }
	if (total <= 0 || amount <= 0) return out;
	int64_t given = 0;
	for (int i = 0; i < GROUPS; i++) {
		if (deposits[i] <= 0) continue;
		out[i] = i == last ? amount - given : amount / total * deposits[i] + amount % total * deposits[i] / total;
		given += out[i];
	}
	return out;
}

/** Input of one game day of a bank. */
struct DayIn {
	int64_t policy_upper_bp = 0;  ///< Policy rate of the central bank of the zone.
	int64_t iorb_bp = 0;          ///< Interest that the central bank pays on reserves.
	int64_t cb_free = 0;          ///< Money that the central bank can lend now (its own money above its floor).
};

/** What happened in one game day (the caller moves the money of the central bank and the citizens). */
struct DayOut {
	int64_t reserve_interest = 0;           ///< Interest on the reserves: new money from the central bank, added to liquid.
	int64_t deposit_interest = 0;           ///< Credited to the deposits of the citizens (it is not paid out).
	std::array<int64_t, GROUPS> deposit_interest_by_group{}; ///< The same by group.
	int64_t cb_interest_paid = 0;           ///< Paid to the central bank out of liquid.
	int64_t borrowed = 0;                   ///< Reserves that the central bank lent today (it takes them from its own money).
	int64_t repaid = 0;                     ///< Reserves that the bank paid back today.
};

/**
 * One game day of the balance sheet: interest on reserves, interest on deposits (credited), interest on the central bank loan, then the reserve requirement:
 * short of reserves -> borrow from the central bank (as far as it can lend); plenty -> repay it. The loans of the bank are not touched here (their interest comes
 * from the borrowers, booked by the caller).
 */
inline DayOut Settle(Sheet &s, const DayIn &in)
{
	DayOut o;
	o.reserve_interest = townwallet::DailyInterest(std::max<int64_t>(s.liquid, 0), in.iorb_bp, s.res_carry);
	s.liquid += o.reserve_interest;
	s.created += o.reserve_interest;

	const int64_t total = s.TotalDeposits();
	o.deposit_interest = townwallet::DailyInterest(total, DepositRateBp(in.policy_upper_bp), s.dep_carry);
	o.deposit_interest_by_group = SplitByDeposits(s.deposits, o.deposit_interest);
	for (int i = 0; i < GROUPS; i++) s.deposits[i] += o.deposit_interest_by_group[i];
	s.deposit_interest += o.deposit_interest;

	const int64_t due = townwallet::DailyInterest(s.cb_loan, RefinanceRateBp(in.policy_upper_bp), s.cb_carry);
	const int64_t pay = std::min(std::max<int64_t>(s.liquid, 0), due);
	s.liquid -= pay;
	s.cb_loan += due - pay; // what it can not pay is added to the loan
	o.cb_interest_paid = pay;

	const int64_t need = ReserveNeed(s.TotalDeposits());
	if (s.liquid < need) {
		const int64_t b = std::min(need - s.liquid, std::max<int64_t>(in.cb_free, 0));
		s.liquid += b;
		s.cb_loan += b;
		o.borrowed = b;
	} else if (s.cb_loan > 0) {
		const int64_t spare = std::max<int64_t>(0, s.liquid - 2 * need);
		const int64_t r = std::min({s.cb_loan, spare, std::max<int64_t>(1, townwallet::Percent(s.cb_loan, CB_REPAY_PERCENT_PER_DAY))});
		s.liquid -= r;
		s.cb_loan -= r;
		o.repaid = r;
	}
	return o;
}

/** What the rescue of a bank with negative equity did. */
struct Rescue {
	int64_t covered = 0;   ///< Paid in by the central bank (deposit insurance), added to liquid.
	int64_t haircut = 0;   ///< Deposits that were cut because the central bank could not cover everything.
};

/**
 * A bank whose equity is negative is rescued: the central bank pays in what it can (up to `cb_free`), and what is still missing is taken from the deposits in proportion.
 * Afterwards the equity is 0 (or still negative when even the deposits are not enough). Does nothing when the equity is 0 or more.
 */
inline Rescue Resolve(Sheet &s, int64_t loans, int64_t cb_free)
{
	Rescue r;
	const int64_t eq = Equity(s, loans);
	if (eq >= 0) return r;
	const int64_t deficit = -eq;
	r.covered = std::min(deficit, std::max<int64_t>(cb_free, 0));
	s.liquid += r.covered;
	const int64_t rest = std::min(deficit - r.covered, s.TotalDeposits());
	if (rest > 0) {
		const std::array<int64_t, GROUPS> cut = SplitByDeposits(s.deposits, rest);
		for (int i = 0; i < GROUPS; i++) { s.deposits[i] -= cut[i]; r.haircut += cut[i]; }
	}
	if (r.covered > 0 || r.haircut > 0) s.failures++;
	return r;
}

} // namespace cbank

#endif /* COMMERCIAL_BANK_MATH_H */

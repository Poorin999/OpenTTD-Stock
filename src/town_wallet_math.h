/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/**
 * @file town_wallet_math.h City wallet: a closed money loop for towns (integer maths only, no game headers, so that town_wallet_test.cpp can test it alone).
 *
 * Money of a town comes from four channels and goes out through two:
 *   IN   1. taxes: the income tax of the citizens (town_treasury.cpp) and a BUSINESS TAX that the town takes from the payment for industrial cargo delivered into it;
 *        2. consumer spending is an OUT: the town PAYS the company for Goods / Food / Water cargo from its treasury (the company gets no more than the town can pay);
 *        3. credit: when the treasury is too small the town sells MUNICIPAL BONDS (bought by the saving citizens, from their cash) and then borrows from the central
 *           bank of its zone (from the money of the bank); it pays interest every game day and repays when the treasury is full again;
 *        4. a yearly GRANT from the central bank of the zone, paid from the money of the bank, shared by population and need.
 * So every unit that a company earns from a town was taken from a treasury, which was filled by taxes, citizens' bonds, the bank or a grant: nothing is made from nothing
 * (the pay of cargo that is not bought by a town, e.g. coal, is still made by the base game: industries have no cash yet).
 */

#ifndef TOWN_WALLET_MATH_H
#define TOWN_WALLET_MATH_H

#include <algorithm>
#include <cstdint>

namespace townwallet {

static constexpr int64_t PPM = 1'000'000;
static constexpr int64_t BUSINESS_TAX_PERCENT = 6;        ///< The town takes 6% of the payment for industrial cargo delivered into it (paid by the company, goes to the treasury).
static constexpr int64_t LOAN_LIMIT_PER_CAPITA = 40;      ///< A town can owe the central bank at most 40 per citizen (money units at price index 1.0).
static constexpr int64_t BOND_LIMIT_PER_CAPITA = 60;      ///< ... and the citizens at most 60 per citizen in bonds.
static constexpr int64_t BOND_BUY_PERCENT_OF_CASH = 60;   ///< The saving citizens put at most 60% of their cash into the bonds of their town.
static constexpr int64_t BANK_LEND_PERCENT = 20;          ///< The bank lends one town at most 20% of its own money above the floor in one go.
static constexpr int64_t TREASURY_FLOOR_PER_CAPITA = 2;   ///< The town repays debts only from the treasury above this (per citizen).
static constexpr int64_t REPAY_PERCENT_PER_DAY = 10;      ///< ... and then pays back at most 10% of the debt in one game day.
static constexpr int64_t LOAN_SPREAD_BP = 100;            ///< A loan costs the policy rate + 1.00%.
static constexpr int64_t RATE_MIN_BP = 100;               ///< No rate below 1%.
static constexpr int64_t DAYS_PER_YEAR = 365;
static constexpr int64_t GRANT_POOL_PERCENT = 10;         ///< The yearly grant pool is 10% of the money of the bank above its floor.
static constexpr int64_t GRANT_TARGET_PER_CAPITA = 20;    ///< A treasury below this per citizen is "needy": it gets more than its population share.
static constexpr int64_t GRANT_NEED_MIN_PPM = 250'000;    ///< Weight factor of a rich town: 0.25 ...
static constexpr int64_t GRANT_NEED_MAX_PPM = 2'000'000;  ///< ... of the poorest town: 2.0.
static constexpr int64_t GRANT_MAX_PER_CAPITA = 10;       ///< No town gets more than this per citizen in one year.

/** Result of a payment for a delivery. */
struct Payment {
	int64_t paid = 0;      ///< What the company gets.
	int64_t from_town = 0; ///< Taken from the treasury (consumer cargo).
	int64_t tax = 0;       ///< Business tax that the town took (industrial cargo).
	int64_t shortfall = 0; ///< What the town could not pay (consumer cargo).
};

/** x * percent / 100 without overflow. */
inline int64_t Percent(int64_t x, int64_t percent)
{
	if (x <= 0 || percent <= 0) return 0;
	return x / 100 * percent + x % 100 * percent / 100;
}

/** A town buys consumer cargo: it pays the income from the treasury, but no more than it has. */
inline Payment PayConsumer(int64_t income, int64_t treasury)
{
	Payment p;
	if (income <= 0) return p;
	p.from_town = std::clamp<int64_t>(treasury, 0, income);
	p.paid = p.from_town;
	p.shortfall = income - p.from_town;
	return p;
}

/** Industrial cargo: the company gets the income less the business tax. */
inline Payment PayIndustrial(int64_t income)
{
	Payment p;
	if (income <= 0) return p;
	p.tax = Percent(income, BUSINESS_TAX_PERCENT);
	p.paid = income - p.tax;
	return p;
}

inline int64_t LoanLimit(int64_t population) { return std::max<int64_t>(0, population) * LOAN_LIMIT_PER_CAPITA; }
inline int64_t BondLimit(int64_t population) { return std::max<int64_t>(0, population) * BOND_LIMIT_PER_CAPITA; }
inline int64_t TreasuryFloor(int64_t population) { return std::max<int64_t>(0, population) * TREASURY_FLOOR_PER_CAPITA; }

/** Rate of a loan from the policy rate (bp). */
inline int64_t LoanRateBp(int64_t policy_upper_bp) { return std::max<int64_t>(RATE_MIN_BP, policy_upper_bp + LOAN_SPREAD_BP); }
/** Coupon of a bond: the policy rate (bp), a little less than a loan. */
inline int64_t BondRateBp(int64_t policy_upper_bp) { return std::max<int64_t>(RATE_MIN_BP, policy_upper_bp); }

/** Interest of one game day on `debt` at `rate_bp`; the rest below one unit is kept in `carry` (in 1/(10000*365) units). */
inline int64_t DailyInterest(int64_t debt, int64_t rate_bp, int64_t &carry)
{
	if (debt <= 0 || rate_bp <= 0) return 0;
	const int64_t den = 10'000 * DAYS_PER_YEAR;
	const int64_t x = debt * rate_bp + std::max<int64_t>(carry, 0); // debt stays far below 1e12, so no overflow
	carry = x % den;
	return x / den;
}

/** How a need of money that the treasury cannot pay is met. */
struct Funding {
	int64_t from_bonds = 0;
	int64_t from_loan = 0;
	int64_t unmet = 0;
};

/**
 * @param need Money that is missing (> 0).
 * @param population Citizens of the town.
 * @param bond_debt Bonds that are out now.
 * @param loan_debt Loan that is out now.
 * @param saver_cash Cash that the saving citizens could put into bonds (already limited by BOND_BUY_PERCENT_OF_CASH by the caller or here: pass their whole cash).
 * @param bank_free Money of the bank above its floor.
 * Order: bonds first (the citizens), then the central bank.
 */
inline Funding Fund(int64_t need, int64_t population, int64_t bond_debt, int64_t loan_debt, int64_t saver_cash, int64_t bank_free)
{
	Funding f;
	if (need <= 0) return f;
	const int64_t bond_room = std::max<int64_t>(0, BondLimit(population) - bond_debt);
	const int64_t buyers = Percent(std::max<int64_t>(0, saver_cash), BOND_BUY_PERCENT_OF_CASH);
	f.from_bonds = std::min({need, bond_room, buyers});
	int64_t left = need - f.from_bonds;
	const int64_t loan_room = std::max<int64_t>(0, LoanLimit(population) - loan_debt);
	const int64_t bank_can = Percent(std::max<int64_t>(0, bank_free), BANK_LEND_PERCENT);
	f.from_loan = std::min({left, loan_room, bank_can});
	left -= f.from_loan;
	f.unmet = left;
	return f;
}

/** Repayment of one game day from the treasury above the floor: the loan first (dearer), then the bonds. */
struct Repay {
	int64_t loan = 0;
	int64_t bonds = 0;
};

inline Repay RepayDay(int64_t treasury, int64_t population, int64_t loan_debt, int64_t bond_debt)
{
	Repay r;
	int64_t spare = treasury - TreasuryFloor(population);
	if (spare <= 0) return r;
	r.loan = std::min({spare, std::max<int64_t>(0, loan_debt), std::max<int64_t>(1, Percent(std::max<int64_t>(0, loan_debt), REPAY_PERCENT_PER_DAY))});
	r.loan = std::max<int64_t>(0, std::min(r.loan, loan_debt));
	spare -= r.loan;
	if (spare <= 0) return r;
	r.bonds = std::min({spare, std::max<int64_t>(0, bond_debt), std::max<int64_t>(1, Percent(std::max<int64_t>(0, bond_debt), REPAY_PERCENT_PER_DAY))});
	r.bonds = std::max<int64_t>(0, std::min(r.bonds, bond_debt));
	return r;
}

/** Weight of a town in the grant: population x need (a poor treasury per citizen weighs more, 0.25 .. 2.0). */
inline int64_t GrantWeight(int64_t population, int64_t treasury)
{
	if (population <= 0) return 0;
	const int64_t target = population * GRANT_TARGET_PER_CAPITA;
	const int64_t gap = std::clamp<int64_t>(target - std::max<int64_t>(treasury, 0), 0, target); // 0 .. target
	const int64_t need = GRANT_NEED_MIN_PPM + (GRANT_NEED_MAX_PPM - GRANT_NEED_MIN_PPM) * gap / std::max<int64_t>(target, 1);
	return population * need / PPM + 1;
}

/** The grant of one town: its share of the pool by weight, at most GRANT_MAX_PER_CAPITA per citizen. */
inline int64_t GrantOf(int64_t pool, int64_t weight, int64_t total_weight, int64_t population)
{
	if (pool <= 0 || weight <= 0 || total_weight <= 0) return 0;
	const int64_t share = pool / total_weight * weight + pool % total_weight * weight / total_weight;
	return std::min(share, std::max<int64_t>(0, population) * GRANT_MAX_PER_CAPITA);
}

/** The pool that a bank gives: 10% of its money above the floor. */
inline int64_t GrantPool(int64_t bank_money, int64_t bank_floor)
{
	return Percent(std::max<int64_t>(0, bank_money - bank_floor), GRANT_POOL_PERCENT);
}

} // namespace townwallet

#endif /* TOWN_WALLET_MATH_H */

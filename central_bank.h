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

/*
 * ---- Liquidation and write-off ---------------------------------------------------------------------------------------
 * A company that is in default at the highest penalty level (BANK_PENALTY_LEVEL_MAX) is liquidated at every midnight on which it
 * cannot pay: the bank takes the free margin of its trading wallets, then sells its gold at the bid price (no commission), and the
 * money repays the debt. Open positions stay covered (only the free margin is taken) and borrowed money is not involved. A company
 * that has gone (bankruptcy) with a debt left: the debt is written off. The budget of the bank (trading_revenue_cents) pays for it
 * as far as it goes, the rest is counted as an uncovered loss. Set BANK_LIQUIDATION_ENABLED to false for the old behaviour (debt just vanishes).
 */
static constexpr bool BANK_LIQUIDATION_ENABLED = true;

/*
 * Repossession of vehicles (third step of the liquidation) and the auction at bankruptcy.
 *   - Before bankruptcy (default at the highest penalty level): after the wallets and the gold, the bank takes VEHICLES of the company, the
 *     most valuable one that is still needed first, and pays BankRepoPercent(credit standing of the bank) of the value of the vehicle (40% .. 80%,
 *     60% before the gold market has started). The money repays the debt; a rest that is more than the debt goes into the cash of the company.
 *     The company keeps its best vehicle (highest profit last year) so that it can still earn. A vehicle is deleted where it is (as OpenTTD does it
 *     for a bankrupt company), trains first free their track reservation. Companies of the AI are treated like the others.
 *   - Auction: from BANK_AUCTION_MONTHS months of bankruptcy (OpenTTD removes the company after 10) the bank also sells every vehicle (the single-player
 *     company that OpenTTD never removes still keeps one), at BANK_AUCTION_PERCENT_OF_REPO percent of the repossession price. Stations, rails, roads,
 *     signals and canals cannot be sold without removing them, so they are valued each day (the OpenTTD formula for stations, the build price for the
 *     rest, at the auction price) and paid only when the company is really removed: that money lowers the debt that is written off.
 * Set BANK_REPO_ENABLED to false and build again to take wallets and gold only.
 */
static constexpr bool BANK_REPO_ENABLED = true;
static constexpr int BANK_AUCTION_MONTHS = 9;                ///< Months of bankruptcy (Company::months_of_bankruptcy) from which the auction runs.
static constexpr int64_t BANK_AUCTION_PERCENT_OF_REPO = 75;  ///< The auction price is this percent of the repossession price.
static constexpr int64_t BANK_REPO_PERCENT_NO_MARKET = 60;   ///< Repossession price in percent of the value before the gold market has started (credit standing A).

/** Percent of the value of a vehicle that the bank pays when it repossesses it, by its own credit standing: C 40%, B 50%, A 60%, AA 70%, AAA 80%. @param tier 0 = C ... 4 = AAA. */
inline int64_t BankRepoPercent(int tier)
{
	static constexpr int64_t PERCENT[] = {40, 50, 60, 70, 80};
	return PERCENT[std::clamp(tier, 0, 4)];
}

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

/*
 * ---- Monetary policy: price index and policy rate ------------------------------------------------------------------
 * The central bank measures a price index, compares its yearly change with a target, and moves the whole interest curve up
 * or down by a "policy shift" (in bp). Every number is an integer and the decision is made once per game day inside the
 * daily clock event, like the interest, so every client of a network game gets exactly the same shift. No AI model and no
 * random numbers: a fixed rule (a Taylor rule with a dead band and a speed limit).
 */
static constexpr int64_t POLICY_PPM = 1'000'000;            ///< 1.0 in the price index.
static constexpr int64_t POLICY_TARGET_INFLATION_BP = 250;   ///< Target: 2.5% a year.
static constexpr int64_t POLICY_DEADBAND_BP = 50;            ///< Inside target +-0.5% the bank does not react to inflation.
static constexpr int64_t POLICY_INFLATION_WEIGHT_PERCENT = 150; ///< The shift wants 1.5 bp for every bp that inflation is above the target.
static constexpr int64_t POLICY_TREND_GROWTH_BP = 600;       ///< Normal growth of the company values of the map: 6% a year.
static constexpr int64_t POLICY_GAP_DIVISOR = 8;             ///< Growth above (below) the trend moves the shift by 1/8 of the difference.
static constexpr int64_t POLICY_GAP_MAX_BP = 200;            ///< The growth term never moves the shift more than 2%.
static constexpr int64_t POLICY_SHIFT_MIN_BP = -325;         ///< Lowest shift: -3.25%, which puts the target range at 0.00% - 0.25% (the zero lower bound, as the Fed).
static constexpr int64_t POLICY_SHIFT_MAX_BP = 600;          ///< Highest shift: +6%.
static constexpr int64_t POLICY_MAX_STEP_BP = 25;            ///< The normal move of a meeting: 0.25%, as the Fed.
static constexpr int64_t POLICY_BIG_STEP_BP = 50;            ///< A bigger move (0.50%), only when the rule wants at least POLICY_BIG_STEP_MIN_BP.
static constexpr int64_t POLICY_BIG_STEP_MIN_BP = 38;        ///< The rule wants 38 bp or more away from the rate: 50 bp instead of 25.
static constexpr int64_t POLICY_MEETINGS_PER_YEAR = 8;        ///< FOMC: 8 scheduled meetings per game year (a meeting about every 45.6 game days).
static constexpr int64_t POLICY_DECISION_DAYS = BANK_DAYS_PER_YEAR / POLICY_MEETINGS_PER_YEAR; ///< Average days between meetings (45), for the display.
static constexpr int64_t POLICY_VALUE_PERIOD_DAYS = 90;      ///< The company values are compared quarter against quarter (90 game days), whatever the meeting rhythm.
static constexpr int64_t POLICY_HOLD_BAND_BP = 13;           ///< A wanted move of less than 13 bp rounds to nothing: the Fed holds (it moves in steps of 25 bp).
static constexpr int64_t POLICY_RANGE_WIDTH_BP = 25;         ///< The target range is 25 bp wide: the policy rate is its upper bound, as the federal funds target.
static constexpr int64_t POLICY_NEUTRAL_RATE_BP = 350;       ///< Policy rate (upper bound) at shift 0. Equal to BANK_RATE_GOOD_MIN_BP, so the loan rates keep their numbers.
static constexpr int64_t POLICY_PRIME_SPREAD_BP = 300;       ///< Prime rate = upper bound of the range + 3.00%.
static constexpr int64_t POLICY_IORB_SPREAD_BP = -10;        ///< Rate paid on money kept at the bank (interest on reserve balances): upper bound - 0.10%.
static constexpr int64_t POLICY_GOLD_PPM_PER_BP = 200;       ///< Effect on the gold price: ppm of demand pressure per bp of (move + surprise): a 25 bp hike that was not expected = -1.0%. Hikes push gold down, cuts up.
static constexpr int64_t POLICY_GOLD_PUSH_MAX_PPM = 50'000;  ///< One decision never moves the gold demand by more than 5%.
static constexpr int64_t POLICY_SHARE_PPM_PER_BP = 100;      ///< Effect on the shares: ppm of market sentiment per bp of (move + surprise): a 25 bp hike that was not expected = -0.5%. Hikes push the shares down, cuts up.
static constexpr int64_t POLICY_SHARE_PUSH_MAX_PPM = 30'000; ///< One decision never moves the sentiment of the shares by more than 3%.
static constexpr int64_t POLICY_UNSCHEDULED_MIN_DAYS = 14;   ///< An unscheduled meeting needs at least 14 game days since the last meeting.
static constexpr int64_t POLICY_CRASH_JOBS_BP = -5000;       ///< A new payroll report with a yearly job growth of -50% or worse is a crash: an unscheduled cut.
static constexpr int64_t POLICY_SPIKE_INFLATION_BP = 2000;   ///< Inflation since the last meeting of 20% a year or more (and above POLICY_EMERGENCY_OFF_BP at that meeting) is a spike: an unscheduled hike.
static constexpr int64_t POLICY_UNSCHEDULED_CUT_BP = 50;     ///< The unscheduled cut: 0.50% (the Fed cut 50 bp on 3 March 2020).
static constexpr int64_t POLICY_UNSCHEDULED_HIKE_BP = 75;    ///< The unscheduled hike: 0.75%.
static constexpr bool BANK_IORB_ENABLED = true;              ///< The bank pays interest on the unborrowed cash of the companies (IORB). Set to false for no interest on cash.
static constexpr bool BANK_IORB_CREATES_MONEY = true;        ///< true: IORB is NEW money (like the reserves of a central bank): the cash in circulation grows, so it feeds the price index and the inflation. false: it is paid from the own money of the bank (above BANK_IORB_BUDGET_FLOOR).
static constexpr int64_t BANK_IORB_CREATE_MAX_PERCENT = 1;   ///< The IORB that is created in one day is at most this percent of the cash in circulation (a safety limit; the normal amount is about 0.01%).
static constexpr int64_t POLICY_NEWS_EXPIRE_DAYS = 14;       ///< A crisis in the news that has not led to a meeting within 14 game days is forgotten.
static constexpr int64_t POLICY_NEWS_CUT_BP = 50;            ///< Unscheduled cut after a crisis in the news: 0.50%.
static constexpr int64_t POLICY_NEWS_HIKE_BP = 50;           ///< Unscheduled hike after an inflation shock in the news: 0.50%.
static constexpr int64_t POLICY_NEWS_MIN_INFLATION_BP = 400; ///< The bank only hikes after an inflation shock when the inflation it measured is above 4.00%.
static constexpr int64_t BANK_IORB_BUDGET_FLOOR = 12'500;    ///< The bank pays IORB only from its own money above this (money units, half of BANK_START_CAPITAL), so it can still write off bad loans.
static constexpr int64_t POLICY_JOBS_WEIGHT_PERCENT = 50;    ///< Employment (second mandate): the growth term counts the jobs growth with this share, company values with the rest.
static constexpr int64_t POLICY_MONEY_WEIGHT_PERCENT = 30;   ///< How much of the change of the cash in circulation goes into the price index.
static constexpr int64_t BANK_RATE_FLOOR_BP = 100;           ///< The rate of a loan is never below 1% ...
static constexpr int64_t BANK_RATE_CEIL_BP = 1400;           ///< ... and never above 14%, so it stays under the penalty rate (15%).

/*
 * ---- Emergency mode for high inflation -------------------------------------------------------------------------------
 * Above POLICY_EMERGENCY_ON_BP of yearly inflation the bank switches to emergency mode (it goes back below POLICY_EMERGENCY_OFF_BP,
 * so the mode does not flicker). In emergency mode the shift may go up to POLICY_EMERGENCY_SHIFT_MAX_BP and moves by
 * POLICY_EMERGENCY_STEP_BP per review. Everything else follows from the shift alone, so it is the same on every client:
 * while the shift is above the normal ceiling POLICY_SHIFT_MAX_BP the loan rate ceiling is BANK_RATE_CEIL_EMERGENCY_BP, the
 * penalty rate is BANK_PENALTY_EMERGENCY_EXTRA_BP higher (so it stays above the loan rate), and the credit share may fall to
 * POLICY_CREDIT_MIN_EMERGENCY_PERCENT.
 */
static constexpr int64_t POLICY_EMERGENCY_ON_BP = 1000;          ///< Emergency mode starts above 10% yearly inflation.
static constexpr int64_t POLICY_EMERGENCY_OFF_BP = 700;          ///< Emergency mode ends below 7% yearly inflation.
static constexpr int64_t POLICY_EMERGENCY_STEP_BP = 75;          ///< Speed limit in emergency mode: 0.75% per meeting (the Fed did 75 bp in 2022).
static constexpr int64_t POLICY_EMERGENCY_SHIFT_MAX_BP = 1500;   ///< Highest shift in emergency mode: +15%.
static constexpr int64_t BANK_RATE_CEIL_EMERGENCY_BP = 2500;     ///< Loan rate ceiling while the shift is above the normal ceiling: 25%.
static constexpr int64_t BANK_PENALTY_EMERGENCY_EXTRA_BP = BANK_RATE_CEIL_EMERGENCY_BP - BANK_RATE_CEIL_BP; ///< Added to the penalty rate: 26% .. 29%, above the loan ceiling.
static constexpr int64_t POLICY_CREDIT_MIN_EMERGENCY_PERCENT = 10; ///< The credit share of the equity may fall to 10%.

/*
 * ---- Credit limit that follows the policy and the credit standing ---------------------------------------------------
 * The limit is a share of the net equity of the company. The share is CENTRAL_BANK_CREDIT_PERCENT (50%) when the policy shift
 * is 0. A tight policy (a positive shift) lowers it by 1 percentage point for every POLICY_CREDIT_BP_PER_PERCENT bp of shift,
 * an easy policy (a negative shift) raises it. The credit standing of the gold reserve (C .. AAA) then scales the limit again.
 * A company that starts with GAME_TIME_STARTING_CAPITAL (500,000) and no debt can borrow about 250,000 at a shift of 0 and standing A.
 */
static constexpr int64_t POLICY_CREDIT_BP_PER_PERCENT = 20; ///< 20 bp of shift move the credit share by 1 percentage point.
static constexpr int64_t POLICY_CREDIT_MIN_PERCENT = 25;    ///< The share of the equity is never below 25% ...
static constexpr int64_t POLICY_CREDIT_MAX_PERCENT = 60;    ///< ... and never above 60%.

/** Share of the net equity (in percent) that a company may borrow at a policy shift: 50% at 0, 20% less at +600 bp (floor 25%), 57% at -150 bp. */
inline int64_t BankPolicyCreditPercent(int64_t shift_bp)
{
	const int64_t floor_percent = shift_bp > POLICY_SHIFT_MAX_BP ? POLICY_CREDIT_MIN_EMERGENCY_PERCENT : POLICY_CREDIT_MIN_PERCENT;
	return std::clamp<int64_t>(CENTRAL_BANK_CREDIT_PERCENT - shift_bp / POLICY_CREDIT_BP_PER_PERCENT, floor_percent, POLICY_CREDIT_MAX_PERCENT);
}

/** Percent of the credit limit that a credit standing allows: C 70%, B 85%, A 100%, AA 110%, AAA 120%. @param tier 0 = C ... 4 = AAA (the CreditTier of the gold reserve). */
inline int64_t BankTierCreditPercent(int tier)
{
	static constexpr int64_t PERCENT[] = {70, 85, 100, 110, 120};
	return PERCENT[std::clamp(tier, 0, 4)];
}

/** The rate of the risk curve moved by the policy shift, kept between the floor and the ceiling. */
inline int64_t BankShiftedRateBp(int64_t base_bp, int64_t shift_bp)
{
	const int64_t ceiling = shift_bp > POLICY_SHIFT_MAX_BP ? BANK_RATE_CEIL_EMERGENCY_BP : BANK_RATE_CEIL_BP;
	return std::clamp<int64_t>(base_bp + shift_bp, BANK_RATE_FLOOR_BP, ceiling);
}

/**
 * The shift that the rule wants (the speed limit is applied by the caller).
 * @param inflation_bp Yearly change of the price index in bp.
 * @param growth_bp Yearly growth of the company values of the map in bp.
 * @param emergency True in emergency mode: the shift may go up to POLICY_EMERGENCY_SHIFT_MAX_BP.
 */
inline int64_t PolicyDesiredShiftBp(int64_t inflation_bp, int64_t growth_bp, bool emergency = false)
{
	int64_t err = inflation_bp - POLICY_TARGET_INFLATION_BP;
	if (err > -POLICY_DEADBAND_BP && err < POLICY_DEADBAND_BP) err = 0;
	const int64_t gap = std::clamp<int64_t>((growth_bp - POLICY_TREND_GROWTH_BP) / POLICY_GAP_DIVISOR, -POLICY_GAP_MAX_BP, POLICY_GAP_MAX_BP);
	return std::clamp<int64_t>(err * POLICY_INFLATION_WEIGHT_PERCENT / 100 + gap, POLICY_SHIFT_MIN_BP, emergency ? POLICY_EMERGENCY_SHIFT_MAX_BP : POLICY_SHIFT_MAX_BP);
}

/*
 * ---- Fed style: target range, meetings, rounding to 25 bp ------------------------------------------------------------
 * The saved state is still the shift (in bp). The policy rate is the UPPER bound of the target range: neutral rate + shift. The loan
 * rates are the risk curve moved by the shift, so a loan rate is policy rate + credit spread, as bank lending follows the federal funds rate.
 */
/** Upper bound of the target range (the policy rate) in bp for a shift. */
inline int64_t PolicyUpperBp(int64_t shift_bp) { return POLICY_NEUTRAL_RATE_BP + shift_bp; }
/** Lower bound of the target range in bp. */
inline int64_t PolicyLowerBp(int64_t shift_bp) { return std::max<int64_t>(0, PolicyUpperBp(shift_bp) - POLICY_RANGE_WIDTH_BP); }
/** Prime rate: upper bound + 3.00%. */
inline int64_t PolicyPrimeBp(int64_t shift_bp) { return PolicyUpperBp(shift_bp) + POLICY_PRIME_SPREAD_BP; }
/** Discount rate (primary credit): the upper bound. */
inline int64_t PolicyDiscountBp(int64_t shift_bp) { return PolicyUpperBp(shift_bp); }
/** Interest on reserve balances: upper bound - 0.10%. */
inline int64_t PolicyIorbBp(int64_t shift_bp) { return std::max<int64_t>(0, PolicyUpperBp(shift_bp) + POLICY_IORB_SPREAD_BP); }

/** Number of the meeting period that a game day belongs to: 8 per game year, so a meeting is held on the first day of each period. */
inline int64_t PolicyMeetingIndex(int64_t day) { return day < 0 ? -1 : day * POLICY_MEETINGS_PER_YEAR / BANK_DAYS_PER_YEAR; }
/** Game days until the next meeting (1 .. 46; the meeting is held on the first day of the next period). */
inline int64_t PolicyDaysToNextMeeting(int64_t day)
{
	const int64_t next = PolicyMeetingIndex(day) + 1;
	return std::max<int64_t>(1, (next * BANK_DAYS_PER_YEAR + POLICY_MEETINGS_PER_YEAR - 1) / POLICY_MEETINGS_PER_YEAR - day);
}

/**
 * Round the move that the rule wants to what a meeting does: nothing below POLICY_HOLD_BAND_BP, 25 bp, 50 bp when the rule wants
 * POLICY_BIG_STEP_MIN_BP or more, and never more than max_step. Always a multiple of 25 bp.
 */
inline int64_t PolicyRoundMove(int64_t wanted_move_bp, int64_t max_step_bp)
{
	const int64_t a = wanted_move_bp < 0 ? -wanted_move_bp : wanted_move_bp;
	int64_t step = 0;
	if (a >= POLICY_BIG_STEP_MIN_BP) {
		step = std::max<int64_t>(POLICY_BIG_STEP_BP, (a + POLICY_MAX_STEP_BP / 2) / POLICY_MAX_STEP_BP * POLICY_MAX_STEP_BP); // Emergency: 75 and more.
	} else if (a >= POLICY_HOLD_BAND_BP) {
		step = POLICY_MAX_STEP_BP;
	}
	step = std::min(step, std::max(POLICY_MAX_STEP_BP, max_step_bp / POLICY_MAX_STEP_BP * POLICY_MAX_STEP_BP));
	return wanted_move_bp < 0 ? -step : step;
}

/** Push on the gold demand (ppm, negative = down) for a move and its surprise, both in bp. A move that was expected counts half, a surprise full. */
inline int64_t PolicyGoldPushPpm(int64_t step_bp, int64_t surprise_bp)
{
	return std::clamp<int64_t>(-(step_bp + surprise_bp) * POLICY_GOLD_PPM_PER_BP, -POLICY_GOLD_PUSH_MAX_PPM, POLICY_GOLD_PUSH_MAX_PPM);
}
/** Push on the sentiment of the shares (ppm) for a move and its surprise, both in bp. */
inline int64_t PolicySharePushPpm(int64_t step_bp, int64_t surprise_bp)
{
	return std::clamp<int64_t>(-(step_bp + surprise_bp) * POLICY_SHARE_PPM_PER_BP, -POLICY_SHARE_PUSH_MAX_PPM, POLICY_SHARE_PUSH_MAX_PPM);
}

/** What the news asked of the bank (set by market_events.cpp when a big world event happens, used by MonetaryPolicy::CheckUnscheduled). */
enum class PolicyNewsTrigger : uint8_t {
	None = 0,
	Crisis,         ///< A bank scare or a recession: the bank wants to cut.
	InflationShock, ///< A big inflation scare: the bank wants to hike.
};

/** Why the last unscheduled meeting was held. */
enum class PolicyUnscheduledReason : uint8_t { None = 0, JobsCrash, InflationSpike, CrisisNews, InflationNews };

/** One decision of the bank, for the decision log of the policy window. */
struct PolicyDecision {
	int64_t day = 0;           ///< Game day of the decision.
	int64_t shift_before = 0;  ///< Policy shift before it, in bp.
	int64_t shift_after = 0;   ///< Policy shift after it, in bp.
	int64_t inflation_bp = 0;  ///< Yearly inflation that it measured.
	int64_t growth_bp = 0;     ///< Yearly growth the rule used (company values blended with the jobs, see POLICY_JOBS_WEIGHT_PERCENT).
	bool emergency = false;    ///< True when the bank was in emergency mode at this decision.
	int64_t jobs_bp = 0;       ///< Yearly growth of the employment (Transport Payrolls) at this meeting; only meaningful when has_jobs.
	bool has_jobs = false;     ///< True when two payroll reports existed, so the jobs counted.
	bool unscheduled = false;  ///< True for a meeting between the scheduled ones (crash or inflation spike).
};

/** How many decisions the log keeps. Only for the display: it is not saved and starts empty after a load. */
static constexpr size_t POLICY_LOG_MAX = 8;

/** State of the monetary policy. Saved in the 'STCK' chunk (SRK_POLICY). */
struct MonetaryPolicy {
	int64_t shift_bp = 0;            ///< Shift of the whole interest curve now, in bp. Saved.
	int64_t prev_index_ppm = 0;      ///< The price index at the last decision (0 = not started). Saved.
	int64_t last_decision_day = -1;  ///< Game day of the last decision (-1 = not started). Saved.
	int64_t inflation_bp = 0;        ///< Yearly change of the price index that the last decision measured. Saved.
	int64_t growth_bp = 0;           ///< Yearly growth of the company values that the last decision measured. Saved.
	bool measured = false;           ///< True after the first real decision (before it there is nothing to show). Saved.
	bool emergency = false;          ///< Emergency mode for high inflation (see POLICY_EMERGENCY_*). Saved in bit 1 of the ym field of SRK_POLICY.
	int64_t last_step_bp = 0;        ///< What the last decision changed, for the display. Not saved.
	int64_t jobs_bp = 0;             ///< Yearly growth of the employment that the last meeting measured (Transport Payrolls). Not saved.
	bool has_jobs = false;           ///< True when the last meeting had a jobs figure. Not saved.
	int64_t last_surprise_bp = 0;    ///< The last move minus the move that the bias of the meeting before had led to. Not saved.
	bool last_unscheduled = false;   ///< True when the last decision was an unscheduled meeting. Not saved.
	PolicyUnscheduledReason last_reason = PolicyUnscheduledReason::None; ///< Why it was held. Not saved.
	PolicyNewsTrigger news_trigger = PolicyNewsTrigger::None; ///< A crisis from the news that waits for the bank to act on it. Not saved.
	int64_t news_trigger_day = 0;    ///< Game day of that news. Not saved.
	void RequestNewsMeeting(PolicyNewsTrigger kind, int64_t day) { this->news_trigger = kind; this->news_trigger_day = day; }
	int64_t last_gold_push_ppm = 0;  ///< What the last decision pushed the gold demand by. Not saved.
	int64_t last_share_push_ppm = 0; ///< What the last decision pushed the sentiment of the shares by. Not saved.
	int64_t wanted_gap_bp = 0;       ///< Where the rule wants the policy rate minus where it is (the bias: above 0 hawkish, below 0 dovish). Not saved.
	std::deque<PolicyDecision> log{};///< The latest decisions, the newest in front. Not saved.

	void Reset() { *this = MonetaryPolicy{}; }
	/** True while the wider limits apply: in emergency mode, and afterwards until the shift is back inside the normal range. */
	bool Extended() const { return this->emergency || this->shift_bp > POLICY_SHIFT_MAX_BP; }
	bool Update(int64_t day, int64_t index_ppm, int64_t value_now, int64_t value_before, int64_t jobs_growth_bp = 0, bool has_jobs = false);
	bool CheckUnscheduled(int64_t day, int64_t index_ppm, int64_t jobs_growth_bp, bool has_jobs, int64_t jobs_report_day);
};

/** The penalty rate in bp for a penalty level (1 or more): 1500, 1550, ... capped at 1800; 1100 more while the policy shift is above the normal ceiling (emergency). */
inline int64_t BankPenaltyRateBp(int level, int64_t policy_shift_bp = 0)
{
	const int64_t extra = policy_shift_bp > POLICY_SHIFT_MAX_BP ? BANK_PENALTY_EMERGENCY_EXTRA_BP : 0;
	return std::min(BANK_PENALTY_MAX_BP, BANK_PENALTY_MIN_BP + BANK_PENALTY_STEP_BP * std::max(0, level - 1)) + extra;
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
 * @param policy_shift_bp Shift of the risk curve by the monetary policy (0 = the plain curve).
 * @param news_spread_bp Extra rate from the news about the company (0 .. 150 bp, see MarketEvents::CreditSpreadBp); only the risk rate, not the penalty rate.
 * @return What to book; all 0 when the day was already charged.
 */
inline BankDayResult BankSettleDay(BankAccount &a, int64_t cash, int64_t equity, int64_t day, int64_t policy_shift_bp = 0, int64_t news_spread_bp = 0)
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
		const int64_t risk_bp = BankShiftedRateBp(BankAnnualRateBp(BankDebtToEquityPpm(a.debt, equity)) + std::max<int64_t>(0, news_spread_bp), policy_shift_bp);
		out.rate_bp = a.in_default ? BankPenaltyRateBp(std::max<int>(1, a.penalty_level), policy_shift_bp) : risk_bp;
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
			out.rate_bp = BankPenaltyRateBp(a.penalty_level, policy_shift_bp);
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

/*
 * ---- The bank trades gold like a central bank (open market operation) --------------------------------------------
 * Once per game day the bank looks at the backing ratio (value of all gold reserves / cash in circulation).
 *   - Below BANK_GOLD_BUY_BELOW_BP it BUYS gold at the ask price. It pays with money that it creates: the money goes into the
 *     cash of the companies (money comes into circulation), in proportion to the company value of the last finished quarter
 *     (equally when no value is known). So a purchase raises the price index and the inflation. The money that it may create
 *     is at most BANK_GOLD_CREATE_MAX_PERCENT of the cash in circulation per day.
 *   - Above BANK_GOLD_SELL_ABOVE_BP it SELLS gold of its own reserve at the bid price. The buyers pay with their UNBORROWED
 *     liquid cash, at most BANK_GOLD_CASH_MAX_PERCENT of the total of it per day; the money is destroyed (money leaves circulation).
 *   - The bank never sells the gold of the companies. At most BANK_GOLD_MAX_DAILY_OZ ounces a day. Integer only, no random numbers.
 *     The trades also move the gold price like the trades of the companies do (the market stock and the demand pressure).
 * The bank has its own money too (CentralBank::trading_revenue_cents, 1/100 of a money unit), separate from the cash of the companies:
 * the start capital BANK_START_CAPITAL, plus the commission and the spread of the gold trades of the companies, plus the loan
 * interest that the companies pay in cash. It is the capital of the bank and it pays for the loans that are lost: when a company
 * disappears (bankruptcy, bought) with a debt, the debt is written off against the own money (CentralBank::WriteOffRemoved). The own
 * money never goes below 0; what it cannot cover is simply lost. The gold trade does not use it.
 */
static constexpr int64_t BANK_GOLD_BUY_BELOW_BP = 1200;   ///< Buy while the backing is below 12% (credit standing A).
static constexpr int64_t BANK_GOLD_SELL_ABOVE_BP = 3000;  ///< Sell while the backing is above 30% (credit standing AAA).
static constexpr int64_t BANK_GOLD_MAX_DAILY_OZ = 500;    ///< At most this many ounces per game day.
static constexpr int64_t BANK_GOLD_CREATE_MAX_PERCENT = 1; ///< The bank never creates more than 1% of the cash in circulation in one day.
static constexpr int64_t BANK_GOLD_CASH_MAX_PERCENT = 5;  ///< The bank never takes more than 5% of the unborrowed liquid cash of all companies in one day.
static constexpr int64_t BANK_START_CAPITAL = 25'000;     ///< Own money of the bank at the start of a game, in money units (5% of the 500,000 that a company starts with).
static constexpr size_t BANK_GOLD_LOG_MAX = 6;            ///< How many trades the log keeps (not saved).

/*
 * Option: BANK_GOLD_MOVES_CASH false makes the bank trade only with its OWN money, without moving the cash of the companies
 * (buys only what its own money pays, the proceeds of a sale go into its own money). The default is true.
 */
static constexpr bool BANK_GOLD_MOVES_CASH = false;

/** One gold trade of the bank, for the policy window. */
struct BankGoldTrade {
	int64_t day = 0;          ///< Game day.
	int64_t ounces = 0;       ///< Positive: bought, negative: sold.
	int64_t price_cents = 0;  ///< Price per ounce of the trade, in 1/100 of a money unit.
	int64_t cash = 0;         ///< Money units that were created into (positive) or destroyed out of (negative) the cash of the companies. 0 when BANK_GOLD_MOVES_CASH is false.
};

/** The central bank: one account per company. Saved in the 'STCK' chunk together with the wallets. */
struct CentralBank {
	std::array<BankAccount, MAX_COMPANIES> accounts{};
	MonetaryPolicy policy;             ///< Price index and the shift of the interest curve (monetary policy).
	int64_t written_off = 0;           ///< Debt of companies that went bankrupt, written off by the bank, money units, total. Saved (SRK_BANK_LOSS).
	int64_t uncovered_loss = 0;        ///< The part of #written_off that the budget could not pay, money units, total. Saved (SRK_BANK_LOSS).
	int64_t trading_revenue_cents = BANK_START_CAPITAL * 100; ///< The own money of the bank, separate from the cash of the companies: start capital, commission and spread of the gold trades, loan interest paid in cash. In 1/100 of a money unit. Saved (SRK_BANK_BUDGET).

	std::deque<BankGoldTrade> gold_log{}; ///< The latest gold trades of the bank, the newest in front. Not saved: empty after a load.

	void Reset();
	void UpdateDaily(int64_t day);
	void TradeGold(int64_t day);
	void AnnounceMeeting(int64_t shift_before);
	void ApplyMarketEffect();
	void PayIorb();
	int64_t iorb_created = 0;          ///< IORB that was paid since the game was loaded (money units); only for the display. Not saved.
	/** Auction value of the stations and infrastructure of a company in bankruptcy (money units, at the auction price). Not saved. */
	std::array<int64_t, MAX_COMPANIES> auction_infra{};

	void Liquidate(CompanyID company, bool auction = false);
	void WriteOffRemoved(CompanyID company);
};

extern CentralBank _central_bank;

/** The price index in ppm (1,000,000 = 1.0 at the start): the inflation of the game, moved by the change of the cash in circulation. */
int64_t GetPriceIndexPpm();

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
	BankGoldOp,      ///< Cash that the gold trade of the central bank put into (income) or took out of (expense) the company: shown, not counted as earnings. Appended last: the index is saved.
	/* Taxes (tax.h). All expenses, paid to the central bank. Appended last: the index is saved. */
	TaxLand,         ///< Land tax: tiles that the company owns.
	TaxTransport,    ///< Transport tax: per vehicle per game day.
	TaxVat,          ///< VAT on the income of cargo delivery.
	TaxCapitalGains, ///< Tax on the profit of closed trades and gold sales.
	TaxDividend,     ///< Tax on dividends received.
	TaxIncome,       ///< Income tax on the profit of the quarter.
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
int64_t GetCentralBankTierCreditPercent();
int64_t GetCentralBankAvailable(CompanyID company);
int64_t GetUnborrowedLiquidCash(CompanyID company);
CommandCost CheckTradingCapital(CompanyID company, int64_t amount);

CommandCost CmdBankBorrow(DoCommandFlags flags, uint8_t wallet, Money amount);
CommandCost CmdBankRepay(DoCommandFlags flags, uint8_t wallet, Money amount);

DEF_CMD_TRAIT(CMD_BANK_BORROW, CmdBankBorrow, {}, CommandType::MoneyManagement)
DEF_CMD_TRAIT(CMD_BANK_REPAY,  CmdBankRepay,  {}, CommandType::MoneyManagement)

#endif /* CENTRAL_BANK_H */

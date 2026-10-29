/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file RetailMath.h The pure maths of the other players of the market: the retail investors (capital of a town, sentiment of a company, order sizes,
 * routing of the orders and the wealth effect) and the AI companies as traders (styles, private opinions).
 *
 * Since 2026-10-29 the market is not made of two groups that agree with the game model: there are FIVE groups of retail investors (trend chasers, income
 * seekers, bargain hunters, small gamblers, passive savers) and FIVE styles of AI traders (value, momentum, contrarian, noise, growth). They look at
 * different things, many of them do NOT use the game's own value as the fair price, and their orders reach the terminal one group at a time, not as one
 * net number.
 *
 * Nothing in this file knows OpenTTD. Everything is integer arithmetic (money in whole units, ratios in parts per million), because the result
 * changes the game state and every client of a network game has to get exactly the same numbers. No float or double is used anywhere.
 * All the numbers that a designer may want to tune are the constants at the top.
 */

#ifndef RETAIL_MATH_H
#define RETAIL_MATH_H

#include <algorithm>
#include <array>
#include <cstdint>

namespace retail {

static constexpr int64_t PPM = 1'000'000; ///< 100% in parts per million.

/* ---- 1. The capital pool ------------------------------------------------------------------------------------------------------------- */

static constexpr int64_t SAVINGS_PER_CAPITA = 1000;          ///< Money that one inhabitant of an average town has saved.
static constexpr int64_t PROSPERITY_REF_POPULATION = 5000;   ///< A town of this size (or more) counts as fully prosperous.
static constexpr int64_t PROSPERITY_MIN_PPM = 700'000;       ///< Savings of the smallest town, as a part of #SAVINGS_PER_CAPITA.
static constexpr int64_t BASE_PARTICIPATION_PPM = 40'000;    ///< Part of the savings that is put in the market when the town has no transport at all (4%).
static constexpr int64_t COVERAGE_PARTICIPATION_PPM = 60'000; ///< Extra part when all the passengers and mail of the town are transported (+6%).
static constexpr int64_t REPLENISH_PPM_PER_DAY = 20'000;     ///< Every day this part of the missing capital is saved again and comes to the market (2%).

/** What the retail side knows about one town. */
struct TownInput {
	uint32_t population = 0;      ///< Inhabitants.
	int64_t coverage_ppm = 0;     ///< Transported / produced passengers and mail of the last month, 0 .. PPM. The "service coverage" of the town.
};

/** How prosperous a town is, 0 .. PPM: bigger towns are richer. */
inline int64_t TownProsperityPpm(uint32_t population)
{
	return std::clamp<int64_t>(static_cast<int64_t>(population) * PPM / PROSPERITY_REF_POPULATION, 0, PPM);
}

/**
 * The money that the inhabitants of a town are willing to hold in stocks and bonds, at most.
 * Savings = population * savings per capita * prosperity. A part of that is invested: more when the transport of the town works.
 */
inline int64_t TownInvestableCapital(const TownInput &t)
{
	const int64_t coverage = std::clamp<int64_t>(t.coverage_ppm, 0, PPM);
	const int64_t per_capita = SAVINGS_PER_CAPITA * (PROSPERITY_MIN_PPM + (PPM - PROSPERITY_MIN_PPM) * TownProsperityPpm(t.population) / PPM) / PPM;
	const int64_t savings = static_cast<int64_t>(t.population) * per_capita;
	const int64_t participation = BASE_PARTICIPATION_PPM + COVERAGE_PARTICIPATION_PPM * coverage / PPM;
	return savings * participation / PPM;
}

/** New savings that reach the market in one day: a part of what is missing between the wanted capital and what the retail investors own now. */
inline int64_t DailyInflow(int64_t capital_wanted, int64_t capital_owned)
{
	return std::max<int64_t>(0, capital_wanted - capital_owned) * REPLENISH_PPM_PER_DAY / PPM;
}

/* ---- 2. Sentiment -------------------------------------------------------------------------------------------------------------------- */

static constexpr int64_t RATIO_ONE = 1'000'000;          ///< Debt / equity of 1.0 (the same unit as BANK_RATIO_ONE in central_bank.h).

static constexpr int MOMENTUM_LOOKBACK_DAYS = 30;        ///< Momentum is the price change over this many game days.
static constexpr int64_t FOMO_START_PPM = 50'000;        ///< The crowd starts to follow a rising price at +5% ...
static constexpr int64_t FOMO_FULL_PPM = 200'000;        ///< ... and follows with full force at +20%.
static constexpr int64_t TREND_SELL_FULL_PPM = 200'000;  ///< A price that fell by this much (-20%) gives the full weakness signal.

static constexpr int IPO_WINDOW_DAYS = 60;               ///< A company is "a recent IPO" for this many game days after retail first saw its price.

static constexpr int64_t YIELD_FULL_PPM = 80'000;        ///< A dividend yield of 8% is the best the investors hope for.
static constexpr int MIN_DIVIDEND_QUARTERS = 3;          ///< A dividend is "consistent" when this many completed quarters were all profitable.

static constexpr int64_t QUALITY_DE_ZERO_PPM = RATIO_ONE;        ///< D/E of 1.0: neither good nor bad.
static constexpr int64_t QUALITY_MAX_PPM = 500'000;              ///< Quality score of a company without debt (and of the worst one, negative).

/** The weights of the four reasons to buy, in parts per million; they add up to PPM. */
static constexpr int64_t W_MOMENTUM = 400'000;
static constexpr int64_t W_IPO = 150'000;
static constexpr int64_t W_YIELD = 350'000;
static constexpr int64_t W_QUALITY = 100'000;

/* Panic selling. */
static constexpr int64_t PANIC_DE_START_PPM = 2 * RATIO_ONE;     ///< Debt / equity of 2.0: the first fear.
static constexpr int64_t PANIC_DE_FULL_PPM = 3 * RATIO_ONE;      ///< Debt / equity of 3.0 (the top of the interest curve): full panic.
static constexpr int64_t PANIC_LOSS_START_PPM = 100'000;         ///< A yearly loss of 10% of the equity: the first fear.
static constexpr int64_t PANIC_LOSS_FULL_PPM = 400'000;          ///< A yearly loss of 40% of the equity: full panic.
static constexpr int64_t PANIC_PENALTY_LEVEL_PPM = 125'000;      ///< Every penalty level of the central bank is worth this much panic (8 levels = full).
static constexpr int64_t PANIC_DEFAULT_PPM = 750'000;            ///< Panic of a company that is in default (cannot pay its interest).
static constexpr int PANIC_COOLDOWN_DAYS = 20;                   ///< After a panic nobody buys the company for this many game days.

/** Everything the investors look at for one company on one day. The OpenTTD side fills this in. */
struct CompanySignals {
	bool valid = false;               ///< False for a company that does not exist or has no price yet.
	int64_t price = 0;                ///< Price of one share, money units.
	bool has_momentum = false;        ///< True when enough days of price are known.
	int64_t momentum_ppm = 0;         ///< Price change over #MOMENTUM_LOOKBACK_DAYS, in ppm (+100,000 = +10%).
	int age_days = 0;                 ///< Game days since the retail investors first saw the company.
	int64_t first_price = 0;          ///< The price on that first day ("the IPO price").
	int64_t yield_ppm = 0;            ///< Dividend yield per year, in ppm; 0 without (positive) earnings.
	bool dividend_consistent = false; ///< See #MIN_DIVIDEND_QUARTERS.
	int64_t de_ppm = 0;               ///< Debt / equity in ppm (as the central bank sees it).
	int64_t earnings = 0;             ///< Net result of the last year; negative for a loss.
	int64_t equity = 0;               ///< Book value (company value minus the debt).
	bool in_default = false;          ///< The central bank charges the penalty rate: the company could not pay its interest.
	int penalty_level = 0;            ///< Penalty level of the central bank, 0 .. 8.
	bool has_value = false;           ///< True when the model value of the company is known (only the bargain hunters and the value traders look at it).
	int64_t value_gap_ppm = 0;        ///< (model value - price) / price, limited to +-50%. Positive: the price is below the model value.
	int64_t trend_month_ppm = 0;      ///< Change of the price since the last monthly close, ppm.
	int64_t avg_dev_ppm = 0;          ///< The price against the average of the last (up to) three monthly closes, ppm. Positive: above the average.
	int64_t earnings_drift_ppm = 0;   ///< The next change of the valuation that the earnings already decide, ppm.
	int64_t mood_ppm = 0;             ///< The sentiment of the whole market (news), -15% .. +15%.
	int64_t noise_ppm = 0;            ///< A hashed number -PPM .. +PPM of this company, new every #NOISE_DAYS game days (what the small gamblers 'heard').
};

/** The groups of retail investors. They look at different things, so one sells while another buys. */
enum Cohort : int {
	COHORT_MOMENTUM = 0, ///< Trend followers: they chase a rising price and recent IPOs (FOMO).
	COHORT_INCOME = 1,   ///< Income seekers: they buy a high, consistent dividend yield from a company with a strong balance sheet.
	COHORT_VALUE = 2,    ///< Bargain hunters: they buy what is far below the value of the model and sell what is far above it (but not a falling knife).
	COHORT_NOISE = 3,    ///< Small gamblers: tips, the mood of the news and cheap 'lottery' shares; their opinion is mostly chance and it changes often.
	COHORT_PASSIVE = 4,  ///< Passive savers (pension, index): they buy every company a little every day and almost never sell, except in a panic.
	COHORT_COUNT = 5,
};

/** Part of the retail capital that each group owns, in percent (adds up to 100). */
inline constexpr int64_t COHORT_CAPITAL_PERCENT[COHORT_COUNT] = {30, 20, 15, 15, 20};

/** How much of the normal panic sale each group does, in percent: the passive savers sit it out half, the gamblers over-react. */
inline constexpr int64_t COHORT_PANIC_SELL_PERCENT[COHORT_COUNT] = {100, 80, 70, 150, 40};

/** The opinion of the retail investors about one company. */
struct Sentiment {
	int64_t cohort_score[COHORT_COUNT] = {}; ///< What each group thinks, -PPM (sell) .. +PPM (buy).
	int64_t score = 0;        ///< All the reasons together, -PPM .. +PPM. For display and tuning.
	int64_t panic_ppm = 0;    ///< 0 = calm .. PPM = full panic. Above 0, both groups sell the company.
	int64_t momentum_part = 0, ipo_part = 0, yield_part = 0, quality_part = 0; ///< The parts of the score.
	int64_t value_part = 0, noise_part = 0, passive_part = 0;
};

/** Linear ramp: 0 at or below `from`, PPM at or above `to`. */
inline int64_t Ramp(int64_t value, int64_t from, int64_t to)
{
	if (value <= from) return 0;
	if (value >= to) return PPM;
	return (value - from) * PPM / (to - from);
}

/** Trend following / FOMO: a strong rise pulls the crowd in; a clear fall makes it leave (weaker, so a dip alone is not a panic). */
inline int64_t MomentumScore(const CompanySignals &s)
{
	if (!s.has_momentum) return 0;
	if (s.momentum_ppm >= FOMO_START_PPM) return Ramp(s.momentum_ppm, FOMO_START_PPM, FOMO_FULL_PPM);
	if (s.momentum_ppm < 0) return -Ramp(-s.momentum_ppm, 0, TREND_SELL_FULL_PPM) / 2;
	return 0;
}

/** A recent IPO that still trades above its first price: the newer it is, the more the crowd wants it. */
inline int64_t IpoScore(const CompanySignals &s)
{
	if (s.age_days >= IPO_WINDOW_DAYS || s.first_price <= 0 || s.price < s.first_price) return 0;
	return PPM * (IPO_WINDOW_DAYS - s.age_days) / IPO_WINDOW_DAYS;
}

/** Balance sheet: +QUALITY_MAX without debt, 0 at D/E 1.0, -QUALITY_MAX at D/E 2.0 and more. */
inline int64_t QualityScore(const CompanySignals &s)
{
	const int64_t de = std::clamp<int64_t>(s.de_ppm, 0, 2 * RATIO_ONE);
	return (QUALITY_DE_ZERO_PPM - de) * QUALITY_MAX_PPM / QUALITY_DE_ZERO_PPM;
}

/** Dividend yield seeking: a high and consistent yield, but only from a company with a strong balance sheet (the yield counts less the higher the D/E). */
inline int64_t YieldScore(const CompanySignals &s)
{
	if (!s.dividend_consistent || s.yield_ppm <= 0) return 0;
	const int64_t strength = std::clamp<int64_t>(PPM - s.de_ppm, 0, PPM); // D/E of 1.0 or more: the yield is not trusted.
	return Ramp(s.yield_ppm, 0, YIELD_FULL_PPM) * strength / PPM;
}

/* The bargain hunters. */
static constexpr int64_t VALUE_BUY_GAP_PPM = 100'000;    ///< They start to buy when the price is 10% below the model value ...
static constexpr int64_t VALUE_FULL_GAP_PPM = 350'000;   ///< ... and buy with full force when it is 35% below.
static constexpr int64_t VALUE_SELL_GAP_PPM = 120'000;   ///< They start to sell when the price is 12% above the model value (gap -12%) ...
static constexpr int64_t VALUE_SELL_FULL_PPM = 400'000;  ///< ... and sell with full force at 40% above.
static constexpr int64_t VALUE_HOLD_PPM = 70'000;        ///< The score of a fairly priced company: above the exit level (they keep what they own), below the buy threshold (they buy nothing new).
static constexpr int64_t VALUE_KNIFE_PPM = 250'000;      ///< A price that fell by more than 25% in 30 days is a falling knife: they wait.
/* The small gamblers. */
static constexpr int NOISE_DAYS = 5;                     ///< Their 'tip' about a company changes every 5 game days.
static constexpr int64_t NOISE_RANDOM_PPM = 800'000;     ///< Weight of the tip.
static constexpr int64_t NOISE_MOOD_X = 2;               ///< The mood of the news counts double for them (a mood of +15% = +30% score): they herd.
static constexpr int64_t NOISE_LOTTERY_PRICE = 40;       ///< Shares below this price look like lottery tickets ...
static constexpr int64_t NOISE_LOTTERY_BONUS_PPM = 100'000; ///< ... and get this bonus.
/* The passive savers. */
static constexpr int64_t PASSIVE_SCORE_PPM = 400'000;    ///< They like every company the same, and enough to buy it every day.

/** Bargain hunting: far below the model value = buy, far above it = sell, in between = keep. Never a company without earnings, never a falling knife. */
inline int64_t ValueScore(const CompanySignals &s)
{
	if (!s.has_value || s.price <= 0 || s.earnings <= 0) return 0;
	if (s.has_momentum && s.momentum_ppm < -VALUE_KNIFE_PPM) return 0;
	if (s.value_gap_ppm >= VALUE_BUY_GAP_PPM) return Ramp(s.value_gap_ppm, VALUE_BUY_GAP_PPM, VALUE_FULL_GAP_PPM);
	if (s.value_gap_ppm <= -VALUE_SELL_GAP_PPM) return -Ramp(-s.value_gap_ppm, VALUE_SELL_GAP_PPM, VALUE_SELL_FULL_PPM);
	return VALUE_HOLD_PPM;
}

/** The small gamblers: a tip (chance), the mood of the news and a taste for cheap shares. */
inline int64_t NoiseScore(const CompanySignals &s)
{
	if (s.price <= 0) return 0;
	int64_t v = s.noise_ppm * NOISE_RANDOM_PPM / PPM + s.mood_ppm * NOISE_MOOD_X;
	if (s.price < NOISE_LOTTERY_PRICE) v += NOISE_LOTTERY_BONUS_PPM;
	return std::clamp<int64_t>(v, -PPM, PPM);
}

/** The passive savers: the same liking for every company. */
inline int64_t PassiveScore(const CompanySignals &s)
{
	return s.price > 0 ? PASSIVE_SCORE_PPM : 0;
}

/**
 * Panic: the worst of three fears.
 *   1. too much debt (D/E 2.0 .. 3.0),
 *   2. the central bank charges the penalty rate or has a penalty level (the company cannot pay its interest),
 *   3. a severe loss over the last year, compared with the equity.
 * @return 0 (calm) .. PPM (full panic).
 */
inline int64_t PanicLevel(const CompanySignals &s)
{
	int64_t panic = Ramp(s.de_ppm, PANIC_DE_START_PPM, PANIC_DE_FULL_PPM);
	if (s.in_default) panic = std::max(panic, PANIC_DEFAULT_PPM);
	panic = std::max(panic, std::min<int64_t>(PPM, s.penalty_level * PANIC_PENALTY_LEVEL_PPM));
	if (s.earnings < 0) {
		const int64_t loss_ratio = s.equity > 0 ? std::min<int64_t>(-s.earnings, INT64_MAX / PPM) * PPM / s.equity : PPM;
		panic = std::max(panic, Ramp(loss_ratio, PANIC_LOSS_START_PPM, PANIC_LOSS_FULL_PPM));
	}
	return panic;
}

/**
 * The score of a company: -PPM .. +PPM. The momentum group looks at the price trend and the IPO, the income group at the dividend
 * and the balance sheet, the bargain hunters at the model value, the gamblers at a tip and the mood, the passive savers at nothing.
 * A panic always wins over the buying reasons, for all groups.
 */
inline Sentiment ScoreCompany(const CompanySignals &s)
{
	Sentiment out;
	if (!s.valid) return out;

	out.momentum_part = MomentumScore(s);
	out.ipo_part = IpoScore(s);
	out.yield_part = YieldScore(s);
	out.quality_part = QualityScore(s);

	out.value_part = ValueScore(s);
	out.noise_part = NoiseScore(s);
	out.passive_part = PassiveScore(s);

	out.cohort_score[COHORT_MOMENTUM] = std::clamp<int64_t>((W_MOMENTUM * out.momentum_part + W_IPO * out.ipo_part) / (W_MOMENTUM + W_IPO), -PPM, PPM);
	out.cohort_score[COHORT_INCOME] = std::clamp<int64_t>((W_YIELD * out.yield_part + W_QUALITY * out.quality_part) / (W_YIELD + W_QUALITY), -PPM, PPM);
	out.cohort_score[COHORT_VALUE] = out.value_part;
	out.cohort_score[COHORT_NOISE] = out.noise_part;
	out.cohort_score[COHORT_PASSIVE] = out.passive_part;
	int64_t weighted = 0;
	for (int k = 0; k < COHORT_COUNT; k++) weighted += COHORT_CAPITAL_PERCENT[k] * out.cohort_score[k];
	out.score = std::clamp<int64_t>(weighted / 100, -PPM, PPM); // the opinion of the whole crowd, weighted by the money of each group

	out.panic_ppm = PanicLevel(s);
	if (out.panic_ppm > 0) {
		const int64_t panic_score = -(PPM / 2 + out.panic_ppm / 2); // -50% .. -100%
		out.score = std::min(out.score, panic_score);
		for (int64_t &c : out.cohort_score) c = std::min(c, panic_score);
	}
	return out;
}

/* ---- 3. Orders and matching ---------------------------------------------------------------------------------------------------------- */

static constexpr int64_t BUY_THRESHOLD_PPM = 150'000;        ///< Below this score nobody buys.
static constexpr int64_t SELL_EXIT_PPM = 60'000;            ///< A group that owns a company and likes it less than this (score below) starts to sell it. Lower than the buy threshold, so a holding is not bought and sold in turns.
static constexpr int64_t SELL_RAMP = 4;                      ///< The part sold grows with the dislike: a score this-many-times-the-gap below the exit level sells the largest part.
static constexpr int64_t DEPLOY_PPM_PER_DAY = 50'000;        ///< The retail cash that is spent on new buys per day (5%).
static constexpr int64_t MAX_SELL_PPM_PER_DAY = 100'000;     ///< Largest part of the holdings that a normal dislike sells in one day (10%).
static constexpr int64_t PANIC_SELL_MIN_PPM = 300'000;       ///< Part of the holdings sold in one day of weak panic (30%) ...
static constexpr int64_t PANIC_SELL_MAX_PPM = 600'000;       ///< ... and of full panic (60%).
static constexpr int64_t MAX_HOLDING_PERCENT = 40;           ///< Retail never owns more than this percentage of the shares of one company.
static constexpr int64_t MAX_DAILY_FLOW_PERCENT = 2;         ///< The net buying or selling that reaches the market is at most this percentage of the shares of a company per day.

/** The orders that the retail investors want to place in one company today, in shares. */
struct RetailOrder {
	int64_t buy_shares = 0;
	int64_t sell_shares = 0;
};

/**
 * How many shares one group sells today.
 * @param held Shares that the group owns of the company.
 * @param score What the group thinks of the company, -PPM .. +PPM.
 * @param panic_ppm The panic about the company, 0 .. PPM.
 * @param panic_sell_percent How much of the normal panic sale this group does (COHORT_PANIC_SELL_PERCENT), percent.
 */
inline int64_t SellShares(int64_t held, int64_t score, int64_t panic_ppm, int64_t panic_sell_percent = 100)
{
	if (held <= 0) return 0;
	int64_t part = 0;
	if (panic_ppm > 0) {
		part = PANIC_SELL_MIN_PPM + (PANIC_SELL_MAX_PPM - PANIC_SELL_MIN_PPM) * std::min<int64_t>(PPM, panic_ppm) / PPM; // Panic sells at once and a lot.
		part = std::min<int64_t>(PPM, part * std::max<int64_t>(0, panic_sell_percent) / 100);
	} else if (score < SELL_EXIT_PPM) {
		part = MAX_SELL_PPM_PER_DAY * std::min<int64_t>(PPM, (SELL_EXIT_PPM - score) * SELL_RAMP) / PPM;
	}
	if (part <= 0) return 0;
	return std::max<int64_t>(1, held * part / PPM); // at least one share, so the last shares can be sold too
}

/**
 * How many shares are bought today.
 * @param budget Money of the retail cash that is given to this company (see #BuyBudget).
 * @param price Price of one share.
 * @param held Shares that the retail investors own of the company.
 * @param total_shares Number of shares of the company.
 */
inline int64_t BuyShares(int64_t budget, int64_t price, int64_t held, int64_t total_shares)
{
	if (budget <= 0 || price <= 0) return 0;
	const int64_t room = std::max<int64_t>(0, total_shares * MAX_HOLDING_PERCENT / 100 - held);
	return std::min(budget / price, room);
}

/** The part of the retail cash that goes to a company: its score divided by the sum of all the scores that pass the buy threshold. */
inline int64_t BuyBudget(int64_t cash, int64_t score, int64_t score_sum)
{
	if (cash <= 0 || score < BUY_THRESHOLD_PPM || score_sum <= 0) return 0;
	return cash * DEPLOY_PPM_PER_DAY / PPM * score / score_sum;
}

/** What is left of the orders of one company after the buyers and sellers have met each other. */
struct MatchResult {
	int64_t matched = 0;   ///< Shares that were bought and sold between retail investors themselves: no effect on the price.
	int64_t net_buy = 0;   ///< Shares that the retail investors buy from the market: pushes the price up.
	int64_t net_sell = 0;  ///< Shares that the retail investors sell to the market: pushes the price down.
};

/**
 * Order matching: buy orders and sell orders of the retail investors are matched at the mid price. Only the difference
 * (the imbalance) is sent to the trading terminal, as pressure on the price, and it is limited per day.
 */
inline MatchResult MatchOrders(const RetailOrder &o, int64_t total_shares, int rounds_per_day = 1)
{
	MatchResult r;
	r.matched = std::min(o.buy_shares, o.sell_shares);
	const int64_t cap = std::max<int64_t>(1, total_shares * MAX_DAILY_FLOW_PERCENT / 100 / std::max(1, rounds_per_day));
	r.net_buy = std::min(cap, o.buy_shares - r.matched);
	r.net_sell = std::min(cap, o.sell_shares - r.matched);
	return r;
}

/** The part of an order that is filled when only `executed` of the `total` shares of all orders could be done: every group gets the same fraction. */
inline int64_t ProRata(int64_t part, int64_t total, int64_t executed)
{
	if (part <= 0 || total <= 0) return 0;
	if (executed >= total) return part;
	return part / total * executed + (part % total) * executed / total;
}

/* ---- 3b. Routing of the orders into the market ---------------------------------------------------------------------------------------- */

static constexpr int64_t CROSS_PERCENT = 25; ///< Only this part of the buy and sell orders that meet each other is crossed inside the retail side (no effect on the price). The rest of every order goes to the market as it is: real buyers and sellers never arrive at the same moment.

/** Where the orders of one hour go: crossed inside, or to the terminal group by group. */
struct Routing {
	int64_t crossed = 0;                                 ///< Shares crossed inside the retail side (volume without effect on the price).
	std::array<int64_t, COHORT_COUNT> cross_buy{};       ///< The part of the buy order of each group that was crossed.
	std::array<int64_t, COHORT_COUNT> cross_sell{};
	std::array<int64_t, COHORT_COUNT> market_buy{};      ///< The part of the buy order of each group that goes to the terminal, as its own order.
	std::array<int64_t, COHORT_COUNT> market_sell{};
};

/**
 * Routing: a part of the matching buy and sell orders is crossed (CROSS_PERCENT), everything else is sent to the terminal group by group, so the
 * terminal sees the real two-sided flow (volume, wicks of the candles) and not one net number. Every group's market order is limited per day
 * (MAX_DAILY_FLOW_PERCENT of the shares of the company).
 * @param buy Shares that each group wants to buy in this round.
 * @param sell Shares that each group wants to sell in this round.
 * @param total_shares Number of shares of the company.
 * @param rounds_per_day How many rounds make a day (the daily limit is shared between them).
 */
inline Routing RouteOrders(const std::array<int64_t, COHORT_COUNT> &buy, const std::array<int64_t, COHORT_COUNT> &sell, int64_t total_shares, int rounds_per_day = 1)
{
	Routing r;
	int64_t total_buy = 0, total_sell = 0;
	for (int k = 0; k < COHORT_COUNT; k++) { total_buy += std::max<int64_t>(0, buy[k]); total_sell += std::max<int64_t>(0, sell[k]); }
	r.crossed = std::min(total_buy, total_sell) * CROSS_PERCENT / 100;
	const int64_t cap = std::max<int64_t>(1, total_shares * MAX_DAILY_FLOW_PERCENT / 100 / std::max(1, rounds_per_day));
	for (int k = 0; k < COHORT_COUNT; k++) {
		const int64_t b = std::max<int64_t>(0, buy[k]);
		const int64_t se = std::max<int64_t>(0, sell[k]);
		r.cross_buy[k] = ProRata(b, total_buy, r.crossed);
		r.cross_sell[k] = ProRata(se, total_sell, r.crossed);
		r.market_buy[k] = std::min(cap, b - r.cross_buy[k]);
		r.market_sell[k] = std::min(cap, se - r.cross_sell[k]);
	}
	return r;
}

/* ---- 4. Dividends, gains and the wealth effect --------------------------------------------------------------------------------------- */

static constexpr int DIVIDEND_PERIOD_DAYS = 30;     ///< The retail dividend is paid once per game month.
static constexpr int64_t CONSUME_PERCENT = 50;      ///< Of every dividend and every capital gain, this percentage is spent in the town; the rest is reinvested.
static constexpr int64_t MAX_BOOST_PPM = 300'000;   ///< A town grows at most 30% faster because of the wealth effect.
static constexpr int64_t BOOST_PER_SAVINGS_PPM = 8'000'000; ///< Boost (ppm) for money that is spent worth 100% of the savings of a town; 8x: 1% spent = +8% boost.
static constexpr int64_t BOOST_DECAY_DIV = 20;      ///< Every day the boost fades by one this-many-th.
static constexpr int64_t DEMAND_PERCENT_OF_BOOST = 50; ///< The passenger and cargo demand rises by this percentage of the growth boost.

/** The dividend that the holders of `held` shares get for `days` days: the payout part of the yearly earnings, per share. Nothing for a loss. */
inline int64_t RetailDividend(int64_t earnings, int64_t payout_percent, int64_t held, int64_t total_shares, int days)
{
	if (earnings <= 0 || held <= 0 || total_shares <= 0) return 0;
	const int64_t capped = std::min<int64_t>(earnings, 1'000'000'000'000);       // keeps every product below INT64_MAX
	const int64_t per_year = capped * payout_percent / 100 * held / total_shares; // 1e12 * 100 * 1e5 = 1e19 would overflow, so divide first
	return per_year * days / 365;
}

/** What `sold` of the `held` shares cost, when all `held` shares cost `cost_basis` together. Split in two parts so the product can not overflow. */
inline int64_t CostOfShares(int64_t cost_basis, int64_t held, int64_t sold)
{
	if (held <= 0 || sold <= 0) return 0;
	if (sold >= held) return cost_basis;
	return cost_basis / held * sold + (cost_basis % held) * sold / held;
}

/** The profit of selling `sold` shares for `proceeds` when all `held` shares cost `cost_basis` together. Negative for a loss. */
inline int64_t RealizedGain(int64_t proceeds, int64_t cost_basis, int64_t held, int64_t sold)
{
	return proceeds - CostOfShares(cost_basis, held, sold);
}

/** Money that is spent in the towns out of a dividend or a gain. */
inline int64_t ConsumedPart(int64_t amount)
{
	return amount > 0 ? amount * CONSUME_PERCENT / 100 : 0;
}

/** The share of the spending that a town gets. Towns with good transport get more: the weight is population * (50% + coverage), divided by 1000 to keep the sums small. */
inline int64_t TownWeight(const TownInput &t)
{
	return static_cast<int64_t>(t.population) * (PPM / 2 + std::clamp<int64_t>(t.coverage_ppm, 0, PPM)) / 1000;
}

/** How much a town's boost rises by the money that it gets today, in ppm: relative to the savings of the town. */
inline int64_t BoostIncrease(int64_t town_share, const TownInput &t)
{
	const int64_t savings = static_cast<int64_t>(std::max<uint32_t>(1, t.population)) * SAVINGS_PER_CAPITA;
	return std::min<int64_t>(MAX_BOOST_PPM, town_share * BOOST_PER_SAVINGS_PPM / savings);
}

/** One day of the boost: it fades, and the new money is added. */
inline int64_t NextBoost(int64_t boost, int64_t increase)
{
	boost -= boost / BOOST_DECAY_DIV;
	if (boost < BOOST_DECAY_DIV) boost = 0;
	return std::clamp<int64_t>(boost + increase, 0, MAX_BOOST_PPM);
}

/** The time between two growth steps of a town, shortened by the boost. Never below 1. A rate of 0 stays 0 (not used by OpenTTD for a growing town). */
inline int64_t BoostedGrowthRate(int64_t growth_rate, int64_t boost_ppm)
{
	return std::max<int64_t>(1, growth_rate * (PPM - std::clamp<int64_t>(boost_ppm, 0, MAX_BOOST_PPM)) / PPM);
}

/* ---- 6. The AI companies as traders ------------------------------------------------------------------------------------------------- */

/** A deterministic 64 bit hash (splitmix64 style). The same numbers on every client of a network game. */
inline uint64_t Mix(uint64_t a, uint64_t b, uint64_t c = 0)
{
	uint64_t x = a * 0x9E3779B97F4A7C15ULL + 0xD1B54A32D192ED03ULL;
	for (uint64_t v : {b, c}) {
		x += v + 0x9E3779B97F4A7C15ULL;
		uint64_t z = x;
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		x = z ^ (z >> 31);
	}
	return x;
}

/** Roughly normal number with a standard deviation of PPM (the sum of three uniform numbers), limited to +-3 sigma. */
inline int64_t GaussPpm(uint64_t hash)
{
	int64_t sum = 0;
	for (int i = 0; i < 3; i++) {
		const uint64_t part = (hash >> (i * 21)) & 0x1FFFFF;
		sum += static_cast<int64_t>((part * 2'000'001) >> 21) - 1'000'000;
	}
	return std::clamp<int64_t>(sum, -3 * PPM, 3 * PPM);
}

/** What kind of trader an AI company is. Every style looks at different things, so the AI companies do NOT all buy the same cheap share. */
enum class BotStyle : int {
	Value = 0,      ///< Buys what is below ITS OWN idea of the value (the model value plus a private error), sells what is above it. Slow.
	Momentum = 1,   ///< Buys what rises and shorts what falls; does not care about the value. It makes bubbles and crashes bigger.
	Contrarian = 2, ///< Buys a share that fell far below its recent average and shorts one that rose far above it. Quick.
	Noise = 3,      ///< Trades on tips: its picks are mostly chance (and follow the mood of the news). Small positions.
	Growth = 4,     ///< Buys companies whose earnings are rising, whatever the price (long only), unless the price is absurd.
	Count = 5,
};
static constexpr int BOT_STYLE_COUNT = static_cast<int>(BotStyle::Count);
/** How many AI companies out of 100 have each style. */
inline constexpr int BOT_STYLE_PERCENT[BOT_STYLE_COUNT] = {30, 25, 15, 10, 20};

/** The numbers that make one AI company different from the next one. */
struct BotParams {
	BotStyle style = BotStyle::Value;
	int64_t buy_ppm = 80'000;        ///< The score that opens a long position.
	int64_t short_ppm = 140'000;     ///< The (negative) score that opens a short position: score <= -short_ppm. A value above PPM = never short.
	int64_t exit_ppm = 30'000;       ///< A long position is closed when the score falls below -exit_ppm, a short one when it rises above +exit_ppm.
	int64_t stop_ppm = 60'000;       ///< Stop Loss distance from the entry price.
	int64_t take_ppm = 100'000;      ///< Take Profit distance from the entry price.
	int max_hold_months = 4;         ///< A position that is open this long is closed.
	int64_t risk_percent = 25;       ///< Part of the free margin that one position may use.
	int max_positions = 4;           ///< Open positions at the same time.
	int64_t value_sigma_ppm = 0;     ///< Size of the private error about the value (one standard deviation). 0 = none.
};

/** The style of an AI company: a function of the seed of the game and of the company, so it is the same on every client and nothing has to be saved. */
inline BotStyle BotStyleOf(uint64_t seed, int company)
{
	int r = static_cast<int>(Mix(seed, static_cast<uint64_t>(company), 0xB075) % 100);
	for (int i = 0; i < BOT_STYLE_COUNT; i++) {
		if (r < BOT_STYLE_PERCENT[i]) return static_cast<BotStyle>(i);
		r -= BOT_STYLE_PERCENT[i];
	}
	return BotStyle::Value;
}

/** A number between 80% and 120% of `base`, decided by the company (so two bots of the same style are not the same). */
inline int64_t BotJitter(uint64_t seed, int company, uint64_t what, int64_t base)
{
	const int64_t pct = 80 + static_cast<int64_t>(Mix(seed, static_cast<uint64_t>(company), what) % 41);
	return base * pct / 100;
}

/** All the numbers of one AI company: its style, with a personal variation (thresholds +-20%, risk and holding time). */
inline BotParams BotParamsOf(uint64_t seed, int company)
{
	BotParams p;
	p.style = BotStyleOf(seed, company);
	switch (p.style) {
		case BotStyle::Value:      p = {BotStyle::Value,      80'000,  140'000,  30'000, 60'000, 100'000, 4, 25, 4, 100'000}; break;
		case BotStyle::Momentum:   p = {BotStyle::Momentum,   70'000,   90'000,       0, 70'000, 160'000, 2, 30, 4, 0}; break;
		case BotStyle::Contrarian: p = {BotStyle::Contrarian, 90'000,  110'000, -20'000, 60'000,  70'000, 1, 20, 3, 0}; break;
		case BotStyle::Noise:      p = {BotStyle::Noise,     150'000,  150'000, 100'000, 80'000,  80'000, 2, 10, 2, 0}; break;
		case BotStyle::Growth:     p = {BotStyle::Growth,     60'000, 2 * PPM,   50'000, 80'000, 200'000, 6, 25, 4, 60'000}; break;
		default: break;
	}
	p.buy_ppm = BotJitter(seed, company, 0xA1, p.buy_ppm);
	if (p.short_ppm <= PPM) p.short_ppm = BotJitter(seed, company, 0xA2, p.short_ppm);
	p.exit_ppm = p.exit_ppm == 0 ? 0 : BotJitter(seed, company, 0xA3, p.exit_ppm);
	p.stop_ppm = BotJitter(seed, company, 0xA4, p.stop_ppm);
	p.take_ppm = BotJitter(seed, company, 0xA5, p.take_ppm);
	p.risk_percent = std::max<int64_t>(5, BotJitter(seed, company, 0xA6, p.risk_percent));
	p.max_hold_months = std::max(1, p.max_hold_months + static_cast<int>(Mix(seed, static_cast<uint64_t>(company), 0xA7) % 3) - 1);
	p.value_sigma_ppm = p.value_sigma_ppm == 0 ? 0 : BotJitter(seed, company, 0xA8, p.value_sigma_ppm);
	return p;
}

/** The private error of one AI company about the value of one company: it changes every quarter and every bot has its own. In ppm of the value. */
inline int64_t BotValueError(uint64_t seed, int bot, int target, int64_t quarter, int64_t sigma_ppm)
{
	if (sigma_ppm <= 0) return 0;
	const int64_t g = std::clamp<int64_t>(GaussPpm(Mix(seed ^ 0x7A1E, static_cast<uint64_t>(bot) * 64 + static_cast<uint64_t>(target), static_cast<uint64_t>(quarter))), -2 * PPM, 2 * PPM);
	return g * sigma_ppm / PPM;
}

/** The 'tip' that a noise trader has about one company this week: -PPM .. +PPM, uniform. */
inline int64_t BotTip(uint64_t seed, int bot, int target, int64_t week)
{
	return static_cast<int64_t>(Mix(seed ^ 0x71B5, static_cast<uint64_t>(bot) * 64 + static_cast<uint64_t>(target), static_cast<uint64_t>(week)) % 2'000'001) - 1'000'000;
}

/** What an AI company sees of one company. The OpenTTD side fills this in. */
struct BotInputs {
	bool valid = false;
	bool can_long = true;          ///< False for a company that may fail (default, too much debt, a loss).
	bool in_default = false;
	int64_t value_gap_ppm = 0;     ///< (model value - price) / price. Positive: cheap.
	bool has_trend30 = false;
	int64_t trend30_ppm = 0;       ///< Price change over 30 game days.
	int64_t trend_month_ppm = 0;   ///< Price change since the last monthly close.
	int64_t avg_dev_ppm = 0;       ///< Price against the average of the last monthly closes.
	int64_t earnings_drift_ppm = 0;
	int64_t mood_ppm = 0;          ///< Sentiment of the whole market.
	int64_t value_error_ppm = 0;   ///< The private error of this bot about this company (BotValueError).
	int64_t tip_ppm = 0;           ///< The tip of this bot about this company (BotTip).
};

/** The opinion of one AI company about one company: score > 0 = buy, score < 0 = sell short. */
struct BotView {
	bool valid = false;
	bool can_long = true;
	bool can_short = true;
	int64_t score = 0;
};

inline BotView BotOpinion(const BotParams &p, const BotInputs &in)
{
	BotView v;
	if (!in.valid) return v;
	v.valid = true;
	v.can_long = in.can_long;
	const int64_t half = PPM / 2;
	int64_t score = 0;
	switch (p.style) {
		case BotStyle::Value: {
			/* ITS value: the model value changed by a private error (so two value traders do not agree), plus a sixth of the move of the month. */
			const int64_t own_gap = in.value_gap_ppm + in.value_error_ppm + in.value_gap_ppm / 1000 * in.value_error_ppm / 1000;
			score = std::clamp<int64_t>(own_gap, -half, half) + std::clamp<int64_t>(in.trend_month_ppm, -half, half) / 6;
			break;
		}
		case BotStyle::Momentum: {
			/* The trend of 30 days (or of the month when the history is short), with a third of the month on top. Hardly any care for the value, only a sanity limit: far above the model value the crowd gets cautious. */
			score = (in.has_trend30 ? in.trend30_ppm : in.trend_month_ppm) + in.trend_month_ppm / 3;
			if (in.value_gap_ppm < -600'000 / 2 && score > 0) score /= 2;
			score = std::clamp<int64_t>(score, -half, half);
			break;
		}
		case BotStyle::Contrarian: {
			/* The opposite of the move away from the average, and a quarter of the value gap. Never a company in default on the long side (checked by can_long). */
			score = std::clamp<int64_t>(-in.avg_dev_ppm, -half, half) + std::clamp<int64_t>(in.value_gap_ppm, -half, half) / 4;
			break;
		}
		case BotStyle::Noise: {
			/* The tip, and the mood of the news twice. It also buys what it should not (can_long is ignored). */
			score = std::clamp<int64_t>(in.tip_ppm + in.mood_ppm * 2, -PPM, PPM);
			v.can_long = true;
			break;
		}
		case BotStyle::Growth: {
			/* Rising earnings and a little value; never at an absurd price (more than ~43% above the model value). */
			score = std::clamp<int64_t>(in.earnings_drift_ppm, -half, half) + std::clamp<int64_t>(in.value_gap_ppm, -half, half) / 4;
			if (in.value_gap_ppm < -300'000) score = std::min<int64_t>(score, 0);
			break;
		}
		default: break;
	}
	if (in.in_default && p.style != BotStyle::Noise) score -= PPM / 10; // a company in default is more likely to fall further
	v.score = score;
	v.can_short = p.short_ppm <= PPM;
	return v;
}

} // namespace retail

#endif /* RETAIL_MATH_H */

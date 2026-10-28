/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/**
 * @file zone_market_math.h Money zones, step B (integer maths only, no game headers, so that zone_market_test.cpp can test it alone).
 *
 * The map is divided into up to MAX_ZONES zones. Every zone has a capital (a town) and its own currency with an exchange rate against the base unit.
 * A tile belongs to the zone of the NEAREST capital (distance on the map, no stored zone map). Internal money stays ONE unit (step B):
 * the zone only decides the exchange fee that a delivery pays when the cargo crosses a zone border. The fee depends on the rates of both sides.
 *
 * Rates: 1.0 = PPM. Since 2026-10-28 a rate is NOT a random walk around a population level any more: it follows the drivers of a real exchange rate
 * (see the section FX MODEL at the end of this file): interest differential (policy meetings, priced ahead, a surprise jumps the rate), inflation (PPP),
 * trade balance, capital flows (bigger than trade, and the trades of the players), growth, political risk, market mood (safe havens in a risk-off),
 * news and the intervention of the zone central bank (with reserves that can run out). Integer only, a pure function of (seed, day, state): the same on every client.
 */

#ifndef ZONE_MARKET_MATH_H
#define ZONE_MARKET_MATH_H

#include <algorithm>
#include <cstdint>
#include <array>
#include <cstdlib>
#include <vector>
#include "gold_market_math.h"

namespace zonemath {

static constexpr int64_t PPM = 1'000'000;
static constexpr int MAX_ZONES = 4;                     ///< At most this many zones.
static constexpr int WANT_ZONES = 4;                    ///< This many zones are made when the map has towns far enough apart.
static constexpr int64_t MIN_CAPITAL_DISTANCE = 24;     ///< Two capitals are at least this many tiles apart (distance x + y).
static constexpr int64_t RATE_MIN_PPM = 250'000;        ///< The rate stays between 0.25 and 4.0.
static constexpr int64_t RATE_MAX_PPM = 4'000'000;
static constexpr int64_t FEE_BASE_PPM = 10'000;         ///< A delivery across a border pays at least 1.0% ...
static constexpr int64_t FEE_RATE_DIV = 4;              ///< ... plus a quarter of the relative difference of the two rates ...
static constexpr int64_t FEE_MAX_PPM = 120'000;         ///< ... and never more than 12%.

/* Step C: every zone has its own central bank numbers: a yearly inflation, a yearly policy rate (what banknotes of the zone earn), and companies can hold banknotes. */
static constexpr int64_t INFL_MIN_PPM = -20'000;        ///< Yearly inflation of a zone stays between -2% ...
static constexpr int64_t INFL_MAX_PPM = 300'000;        ///< ... and +30%.
static constexpr int64_t INFL_START_PPM = 20'000;       ///< A new zone starts with 2% a year.
static constexpr int64_t INFL_SIGMA_PPM = 1'500;        ///< Chance move of the yearly inflation in one game day: 0.15% (one standard deviation).
static constexpr int64_t INFL_REVERT_PPM_PER_DAY = 10'000; ///< Every game day 1% of the gap to INFL_START_PPM is closed.
static constexpr int64_t POLICY_MIN_PPM = 0;            ///< The yearly policy rate of a zone stays between 0 ...
static constexpr int64_t POLICY_MAX_PPM = 350'000;      ///< ... and 35%.
static constexpr int64_t POLICY_START_PPM = 30'000;     ///< A new zone starts with 3% a year.
static constexpr int64_t POLICY_OVER_INFL_PPM = 10'000; ///< The bank aims for the inflation + 1.0%.
static constexpr int64_t DAYS_PER_YEAR = 365;           ///< Banknotes earn the policy rate / this every game day.
static constexpr int64_t EXCHANGE_SPREAD_PPM = 3'000;   ///< Buying or selling banknotes costs 0.3%.
static constexpr int64_t TRADE_STEP_PPM_PER_UNIT = 100; ///< A terminal trade of banknotes moves the rate by this much per unit of volume ...
static constexpr int64_t TRADE_STEP_MAX_PPM = 20'000;   ///< ... but never more than 2% in one trade.

struct Pt {
	int64_t x = 0;
	int64_t y = 0;
};

inline int64_t Distance(const Pt &a, const Pt &b)
{
	return std::abs(a.x - b.x) + std::abs(a.y - b.y);
}

/**
 * Choose the capitals: the town with the most people first; then again and again the town that is the farthest from the capitals so far
 * (score = distance x (people + 50), so a big town far away wins over a village even further away). A town closer than MIN_CAPITAL_DISTANCE to a
 * capital is never chosen. Ties go to the lower index. Returns the indices of the chosen towns (1 .. want; empty when there is no town).
 */
inline std::vector<int> PickCapitals(const std::vector<Pt> &pts, const std::vector<int64_t> &pop, int want)
{
	std::vector<int> caps;
	const int n = static_cast<int>(pts.size());
	if (n <= 0 || want <= 0) return caps;
	int first = 0;
	for (int i = 1; i < n; i++) if (pop[i] > pop[first]) first = i;
	caps.push_back(first);
	while (static_cast<int>(caps.size()) < std::min(want, MAX_ZONES)) {
		int best = -1; int64_t best_score = 0;
		for (int i = 0; i < n; i++) {
			int64_t closest = INT64_MAX;
			for (int c : caps) closest = std::min(closest, Distance(pts[i], pts[c]));
			if (closest < MIN_CAPITAL_DISTANCE) continue;
			const int64_t score = closest * (pop[i] + 50);
			if (score > best_score) { best_score = score; best = i; }
		}
		if (best < 0) break;
		caps.push_back(best);
	}
	return caps;
}

/** The zone of a point: the nearest capital (the first one on a tie). -1 when there are no capitals. */
inline int ZoneOf(const Pt &p, const std::vector<Pt> &capitals)
{
	int best = -1; int64_t best_d = INT64_MAX;
	for (int i = 0; i < static_cast<int>(capitals.size()); i++) {
		const int64_t d = Distance(p, capitals[i]);
		if (d < best_d) { best_d = d; best = i; }
	}
	return best;
}

/** The exchange fee of a delivery from a zone with rate ra into a zone with rate rb, in ppm of the income. */
inline int64_t FeePpm(int64_t ra, int64_t rb)
{
	if (ra <= 0 || rb <= 0) return 0;
	const int64_t diff = std::abs(ra - rb) * PPM / std::max(ra, rb);
	return std::min<int64_t>(FEE_MAX_PPM, FEE_BASE_PPM + diff / FEE_RATE_DIV);
}

/** income x fee (ppm) without overflow. */
inline int64_t FeeAmount(int64_t income, int64_t fee_ppm)
{
	if (income <= 0 || fee_ppm <= 0) return 0;
	return income / PPM * fee_ppm + income % PPM * fee_ppm / PPM;
}

/** One game day of the yearly inflation of a zone: a chance move, then the pull to INFL_START_PPM. */
inline int64_t NextInflation(uint64_t seed, int zone, int64_t day, int64_t infl)
{
	const int64_t g = goldmath::Gauss(goldmath::Hash(seed, static_cast<uint64_t>(day), 0x1F10 + static_cast<uint64_t>(zone)));
	int64_t v = infl + g * INFL_SIGMA_PPM / PPM;
	v += (INFL_START_PPM - v) * INFL_REVERT_PPM_PER_DAY / PPM;
	return std::clamp<int64_t>(v, INFL_MIN_PPM, INFL_MAX_PPM);
}

/** What banknotes earn in one game day: notes x policy / DAYS_PER_YEAR, without overflow. */
inline int64_t DailyInterest(int64_t notes, int64_t policy_ppm)
{
	if (notes <= 0 || policy_ppm <= 0) return 0;
	return (notes / PPM * policy_ppm + notes % PPM * policy_ppm / PPM) / DAYS_PER_YEAR;
}

/** Banknotes (units of the zone currency) that `money` buys at a rate (value of one note in money, in ppm), before the spread. */
inline int64_t NotesForMoney(int64_t money, int64_t rate_ppm)
{
	if (money <= 0 || rate_ppm <= 0) return 0;
	return money / rate_ppm * PPM + money % rate_ppm * PPM / rate_ppm;
}

/** Money that `notes` are worth at a rate, before the spread. */
inline int64_t MoneyForNotes(int64_t notes, int64_t rate_ppm)
{
	if (notes <= 0 || rate_ppm <= 0) return 0;
	return notes / PPM * rate_ppm + notes % PPM * rate_ppm / PPM;
}

/** An amount after the exchange spread of banknotes (EXCHANGE_SPREAD_PPM) is taken off. */
inline int64_t AfterSpread(int64_t x)
{
	if (x <= 0) return 0;
	return x / PPM * (PPM - EXCHANGE_SPREAD_PPM) + x % PPM * (PPM - EXCHANGE_SPREAD_PPM) / PPM;
}

/** The rate after a terminal trade of banknotes: a buy pushes it up, a sell down. */
inline int64_t RateAfterTrade(int64_t rate_ppm, bool buy, int64_t volume)
{
	const int64_t step = std::min<int64_t>(TRADE_STEP_MAX_PPM, std::max<int64_t>(volume, 0) * TRADE_STEP_PPM_PER_UNIT);
	const int64_t move = rate_ppm / PPM * step + rate_ppm % PPM * step / PPM;
	return std::clamp<int64_t>(buy ? rate_ppm + move : rate_ppm - move, RATE_MIN_PPM, RATE_MAX_PPM);
}


/*
 * ======================================================== FX MODEL (2026-10-28) ========================================================
 * What moves a real exchange rate, and where each driver is in this model (all integer, ppm = 1/1,000,000, one step per game day):
 *
 *  1. Interest differential  Policy rates move at MEETINGS (every MEETING_DAYS days, zones staggered) in 25 bp steps towards inflation + 1%.
 *                            The market knows the rule, so it prices the NEXT move ahead (ExpectedPolicy): money flows towards the zone that is
 *                            expected to pay more (driver 4). Only a SURPRISE (the real policy minus what was expected 15 days before) jumps the rate at once.
 *  2. Inflation (PPP)        Every zone keeps a price level; the fair value follows the relative price level: more inflation than the others = weaker currency.
 *  3. Trade balance          Cargo delivered across a border is an export of the source zone and an import of the destination zone (the income counts).
 *                            A surplus means that others must buy the currency of the zone to pay.
 *  4. Capital flows          A stock that grows towards (expected policy - average) x CAPITAL_GAIN: bigger than trade. The banknote trades of the players
 *                            (ZoneRateTrade) add to it too.
 *  5. Growth, political risk Growth = the income of deliveries in the zone, fast against slow average, relative to the other zones.
 *                            Political risk = rare shocks (a hash of the seed, the day and the zone) that fade; they lower the fair value.
 *  6. Market mood            risk_off = bad mood of the market (sentiment of the shares) + a war on + the average political risk. In a risk-off the safe
 *                            currency (beta > 0, the USD zone) gains and the risky ones (beta < 0) lose.
 *  7. Expectations and news  The expected policy moves the fair value BEFORE the meeting; news (data, statements) are rare hash events that jump the
 *                            rate at once, and half of the jump stays in the fair value for a while.
 *  8. Intervention           A zone central bank (FxProfile::strength) leans against a move away from the slow reference (the average of the last ~60 days)
 *                            once it is outside its band. It pays with RESERVES; when they are gone the defence stops and the rate falls to its fair value
 *                            (a currency crisis).
 *
 *  fair = PPP x (1 + trade + growth + capital + news - risk + safe haven); the rate goes FAIR_PULL_PPM of the way to it every day, plus a small noise.
 *  All terms are relative to the OTHER zones (differences from the average), so the sum of the zones does not drift.
 */

static constexpr int64_t MEETING_DAYS = 45;            ///< A zone central bank meets every 45 game days ...
static constexpr int64_t MEETING_STAGGER_DAYS = 11;    ///< ... zone i meets 11 x i days later than zone 0, so the meetings do not all fall on one day.
static constexpr int64_t GUIDE_DAYS_BEFORE = 15;       ///< The market expectation of the next policy rate is written down this many days before the meeting.
static constexpr int64_t POLICY_STEP_PPM = 2'500;      ///< A policy move is a multiple of 0.25%.
static constexpr int64_t POLICY_HOLD_PPM = 1'250;      ///< A gap to the aim smaller than 0.125% means: keep the rate.
static constexpr int64_t POLICY_MAX_STEPS = 3;         ///< At most 0.75% at one meeting.
static constexpr int64_t POLICY_SURPRISE_X = 3;        ///< A surprise of the policy rate moves the exchange rate by 3 x the surprise (a surprise of 0.25% = 0.75%).
static constexpr int64_t PRICE_LEVEL_MIN_PPM = 250'000;///< The relative price level of a zone stays between 0.25 and 4.0.
static constexpr int64_t PRICE_LEVEL_MAX_PPM = 4'000'000;
static constexpr int64_t MONEY_CAP = 1'000'000'000'000;///< Money flows are never counted above this (so the maths cannot overflow).
static constexpr int64_t TRADE_DAYS = 30;              ///< Exports and imports are averaged over about 30 game days.
static constexpr int64_t TRADE_FLOOR = 20'000;         ///< Added to the trade volume when the balance is worked out: a tiny trade has a tiny effect.
static constexpr int64_t TRADE_WEIGHT_PPM = 40'000;    ///< A trade balance of +100% (only exports) lifts the fair value by 4%.
static constexpr int64_t GDP_FAST_DAYS = 7;            ///< Income in the zone: fast average ...
static constexpr int64_t GDP_SLOW_DAYS = 180;          ///< ... against a slow average.
static constexpr int64_t GDP_WARMUP_DAYS = 30;         ///< For the first 30 days the slow average follows the fast one: no growth is measured yet.
static constexpr int64_t GDP_FLOOR = 50'000;           ///< Added to the slow average when the growth is worked out.
static constexpr int64_t GROWTH_CAP_PPM = 500'000;     ///< A growth of the income is counted up to +-50%.
static constexpr int64_t GROWTH_WEIGHT_PPM = 40'000;   ///< A growth 50% above the other zones lifts the fair value by 2%.
static constexpr int64_t RISK_ONE_IN_DAYS = 300;       ///< A political shock hits a zone about once in 300 game days.
static constexpr int64_t RISK_SHOCK_MIN_PPM = 50'000;  ///< Size of a shock: 5% .. 15% of risk.
static constexpr int64_t RISK_SHOCK_SPAN_PPM = 100'000;
static constexpr int64_t RISK_MAX_PPM = 500'000;
static constexpr int64_t RISK_DECAY_PPM_PER_DAY = 15'000; ///< Risk fades by 1.5% of itself a day (half in about 46 days).
static constexpr int64_t RISK_WEIGHT_PPM = 400'000;    ///< A risk of 10% lowers the fair value by 4%.
static constexpr int64_t MOOD_RISK_OFF_X = 4;          ///< A market mood of -15% (the worst) is a risk-off of 60%.
static constexpr int64_t WAR_RISK_OFF_PPM = 300'000;   ///< A war adds 30% risk-off.
static constexpr int64_t RISK_OFF_MIN_PPM = -300'000;  ///< Risk-off stays between -30% (risk-on) and 100%.
static constexpr int64_t SAFE_WEIGHT_PPM = 80'000;     ///< A risk-off of 100% moves a currency with beta 1.0 by 8%.
static constexpr int64_t CAPITAL_GAIN = 3;             ///< A policy rate 1% above the others draws capital that lifts the fair value by 3% in the long run.
static constexpr int64_t CAPITAL_DAYS = 45;            ///< The flow stock follows its goal with a time constant of 45 game days.
static constexpr int64_t CAPITAL_MAX_PPM = 200'000;
static constexpr int64_t NEWS_ONE_IN_DAYS = 30;        ///< A piece of news (data, a statement) hits a zone about once in 30 game days.
static constexpr int64_t NEWS_MIN_PPM = 5'000;         ///< It moves the rate at once by 0.5% .. 2.5%, up or down.
static constexpr int64_t NEWS_SPAN_PPM = 20'000;
static constexpr int64_t NEWS_FAIR_SHARE_PPM = 500'000;///< Half of the jump goes into the fair value ...
static constexpr int64_t NEWS_DECAY_PPM_PER_DAY = 20'000; ///< ... and fades by 2% of itself a day.
static constexpr int64_t NEWS_MAX_PPM = 100'000;
static constexpr int64_t ADJ_MAX_PPM = 400'000;        ///< All terms together move the fair value by 40% at most.
static constexpr int64_t FAIR_PULL_PPM = 50'000;       ///< Every day the rate goes 5% of the way to its fair value (half of a gap in about 13 days).
static constexpr int64_t NOISE_SIGMA_PPM = 4'000;      ///< Chance move of the rate in one game day: 0.4% (one standard deviation).
static constexpr int64_t REF_DAYS = 60;                ///< The reference of the central bank is the average of about 60 game days.
static constexpr int64_t RESERVE_COST_X = 4;           ///< Pushing the rate by 1% costs 4% of the start reserves.
static constexpr int64_t RESERVE_MAX_PPM = 2'000'000;  ///< Reserves never grow above twice the start.
static constexpr int64_t RESERVE_TRADE_PPM = 1'000;    ///< A trade surplus of 100% adds 0.1% of the start reserves a day (a deficit takes it).

/** How a currency behaves: its safe haven beta and how the central bank of the zone defends it. */
struct FxProfile {
	int64_t beta_ppm;          ///< Safe haven beta: +1.0 gains in a risk-off (USD), -1.0 loses (RUB). Units of ppm.
	int64_t strength_ppm;      ///< Part of the move beyond the band that the bank pushes back every day (0 = free float).
	int64_t band_ppm;          ///< The bank only acts when the rate is further than this from its reference.
	int64_t reserve_start_ppm; ///< Reserves at the start of a game (1.0 = PPM).
};

/** The profile of every zone, in the order of the currencies THB, USD, CNY, RUB (ZONE_CURRENCY_NAMES in zone_market.cpp). */
static constexpr FxProfile PROFILES[MAX_ZONES] = {
	{  -400'000, 150'000, 30'000, 1'000'000}, // THB: a managed float, a bit risky
	{ 1'000'000,       0,      0, 1'000'000}, // USD: free float, the safe haven
	{  -200'000, 400'000, 15'000, 1'500'000}, // CNY: tightly managed, big reserves
	{-1'000'000, 200'000, 40'000,   700'000}, // RUB: risky, thin reserves
};

/** What a zone remembers for the FX model. Everything is saved (SRK_ZONE_FX), except the display copy in FxParts. */
struct FxState {
	int64_t price_level_ppm = PPM;   ///< Relative price level (the average of the zones is 1.0).
	int64_t exports_avg = 0;         ///< Exports per game day, averaged (money units).
	int64_t imports_avg = 0;         ///< Imports per game day, averaged.
	int64_t gdp_fast = 0;            ///< Income of deliveries in the zone per game day, fast average.
	int64_t gdp_slow = 0;            ///< ... slow average.
	int64_t gdp_days = 0;            ///< Game days counted (warm-up).
	int64_t capital_ppm = 0;         ///< Effect of the capital flows on the fair value (signed).
	int64_t risk_ppm = 0;            ///< Political risk (0 and up).
	int64_t news_ppm = 0;            ///< The part of the news jumps that is still in the fair value (signed).
	int64_t reserve_ppm = PPM;       ///< Reserves of the central bank of the zone (1.0 = at the start).
	int64_t ref_ppm = 0;             ///< Slow reference of the rate (0 = take the rate).
	int64_t expected_policy_ppm = POLICY_START_PPM; ///< The policy rate that the market expected for the next meeting, written down GUIDE_DAYS_BEFORE days before it.
	int64_t exports_today = 0;       ///< The deliveries of the game day that is running (the game adds to these).
	int64_t imports_today = 0;
	int64_t gdp_today = 0;
};

/** The parts of the last fair value (for the console and the windows; not saved). All in ppm of the fair value. */
struct FxParts {
	int64_t ppp_ppm = PPM;   ///< The PPP anchor (1.0 = equal price level).
	int64_t trade = 0;
	int64_t growth = 0;
	int64_t capital = 0;
	int64_t news = 0;
	int64_t risk = 0;        ///< Negative: the risk lowers the fair value.
	int64_t safe = 0;
	int64_t fair_ppm = PPM;  ///< The fair value (rate).
	int64_t expected_policy_ppm = 0;
	int64_t surprise_ppm = 0;///< The policy surprise of today (0 when there was no meeting).
	int64_t news_jump_ppm = 0;///< The news jump of today.
	int64_t defended_ppm = 0;///< What the central bank pushed today (signed: + buys the currency).
	int64_t trade_balance_ppm = 0;
	int64_t growth_rel_ppm = 0;
};

/** Market facts that the FX model reads (the game fills it from the market events). */
struct FxWorld {
	int64_t mood_ppm = 0; ///< Sentiment of the whole market (-15% .. +15%): bad news = negative.
	bool war = false;     ///< A war is on.
};

/** The zone as the FX model sees it: the rate and the central bank numbers plus the state. */
struct FxZone {
	int64_t rate_ppm = PPM;
	int64_t inflation_ppm = INFL_START_PPM;
	int64_t policy_ppm = POLICY_START_PPM;
	FxState fx;
	FxParts parts;
};

inline int64_t MulPpm(int64_t x, int64_t ppm)
{
	return x / PPM * ppm + x % PPM * ppm / PPM;
}

/** The policy rate that the bank aims for: inflation + 1%. */
inline int64_t PolicyAim(int64_t infl_ppm)
{
	return std::clamp<int64_t>(infl_ppm + POLICY_OVER_INFL_PPM, POLICY_MIN_PPM, POLICY_MAX_PPM);
}

/** A decision of a meeting for a gap (aim - policy): 0 inside the hold band, else a multiple of 0.25%, at most POLICY_MAX_STEPS steps. */
inline int64_t RoundMove(int64_t gap)
{
	const int64_t mag = std::abs(gap);
	if (mag < POLICY_HOLD_PPM) return 0;
	const int64_t steps = std::min<int64_t>(POLICY_MAX_STEPS, std::max<int64_t>(1, (mag + POLICY_STEP_PPM / 2) / POLICY_STEP_PPM));
	return gap < 0 ? -steps * POLICY_STEP_PPM : steps * POLICY_STEP_PPM;
}

/** The policy rate that the market expects after the next meeting, given the policy rate now and the inflation now. */
inline int64_t ExpectedPolicy(int64_t policy_ppm, int64_t infl_ppm)
{
	return std::clamp<int64_t>(policy_ppm + RoundMove(PolicyAim(infl_ppm) - policy_ppm), POLICY_MIN_PPM, POLICY_MAX_PPM);
}

/** The start state of the FX model for a new zone. */
inline FxState FxInitState(int zone)
{
	FxState s;
	if (zone >= 0 && zone < MAX_ZONES) s.reserve_ppm = PROFILES[zone].reserve_start_ppm;
	return s;
}

inline int64_t Ewma(int64_t avg, int64_t x, int64_t days)
{
	return avg + (x - avg) / days;
}

/** The new inflation of a zone is made inside FxStepDay (NextInflation). One game day of the whole FX model for `n` zones. `world` is what the market looks like today. */
inline void FxStepDay(uint64_t seed, int64_t day, FxZone *z, int n, const FxWorld &world)
{
	if (n <= 0 || n > MAX_ZONES) return;
	const int64_t N = n;

	/* 1. Inflation, and the price level (driver 2). The levels are made relative (average 1.0) so they never run away. */
	int64_t sum_level = 0;
	for (int i = 0; i < n; i++) {
		z[i].inflation_ppm = NextInflation(seed, i, day, z[i].inflation_ppm);
		FxState &f = z[i].fx;
		f.price_level_ppm = std::clamp<int64_t>(f.price_level_ppm * (DAYS_PER_YEAR * PPM + z[i].inflation_ppm) / (DAYS_PER_YEAR * PPM), 1, PRICE_LEVEL_MAX_PPM * 4); // one day of the yearly inflation
		sum_level += f.price_level_ppm;
	}
	const int64_t mean_level = std::max<int64_t>(sum_level / N, 1);
	for (int i = 0; i < n; i++) {
		FxState &f = z[i].fx;
		f.price_level_ppm = std::clamp<int64_t>(f.price_level_ppm * PPM / mean_level, PRICE_LEVEL_MIN_PPM, PRICE_LEVEL_MAX_PPM);
	}

	/* 2. The policy meetings (driver 1) and the expectation (driver 7). */
	std::array<int64_t, MAX_ZONES> surprise{}, expected{};
	for (int i = 0; i < n; i++) {
		FxState &f = z[i].fx;
		const int64_t phase = (day + i * MEETING_STAGGER_DAYS) % MEETING_DAYS;
		const int64_t expected_now = ExpectedPolicy(z[i].policy_ppm, z[i].inflation_ppm);
		if (phase == MEETING_DAYS - GUIDE_DAYS_BEFORE) f.expected_policy_ppm = expected_now;
		if (phase == 0) {
			const int64_t new_policy = expected_now; // the same rule as the expectation, but with the aim of TODAY
			surprise[i] = new_policy - std::clamp<int64_t>(f.expected_policy_ppm, POLICY_MIN_PPM, POLICY_MAX_PPM);
			z[i].policy_ppm = new_policy;
			f.expected_policy_ppm = new_policy;
		}
		expected[i] = ExpectedPolicy(z[i].policy_ppm, z[i].inflation_ppm);
	}
	int64_t mean_expected = 0;
	for (int i = 0; i < n; i++) mean_expected += expected[i];
	mean_expected /= N;

	/* 3. Trade (driver 3) and growth (driver 5): averages of what the game counted. */
	std::array<int64_t, MAX_ZONES> balance{}, growth{};
	int64_t mean_growth = 0;
	for (int i = 0; i < n; i++) {
		FxState &f = z[i].fx;
		f.exports_avg = Ewma(f.exports_avg, std::clamp<int64_t>(f.exports_today, 0, MONEY_CAP), TRADE_DAYS);
		f.imports_avg = Ewma(f.imports_avg, std::clamp<int64_t>(f.imports_today, 0, MONEY_CAP), TRADE_DAYS);
		const int64_t total = f.exports_avg + f.imports_avg + TRADE_FLOOR;
		balance[i] = std::clamp<int64_t>((f.exports_avg - f.imports_avg) / total * PPM + (f.exports_avg - f.imports_avg) % total * PPM / total, -PPM, PPM);
		const int64_t gdp = std::clamp<int64_t>(f.gdp_today, 0, MONEY_CAP);
		f.gdp_fast = Ewma(f.gdp_fast, gdp, GDP_FAST_DAYS);
		if (f.gdp_days < GDP_WARMUP_DAYS) {
			f.gdp_slow = f.gdp_fast;
			f.gdp_days++;
			growth[i] = 0;
		} else {
			f.gdp_slow = Ewma(f.gdp_slow, gdp, GDP_SLOW_DAYS);
			const int64_t diff = f.gdp_fast - f.gdp_slow, base = f.gdp_slow + GDP_FLOOR;
			growth[i] = std::clamp<int64_t>(diff / base * PPM + diff % base * PPM / base, -GROWTH_CAP_PPM, GROWTH_CAP_PPM);
		}
		f.exports_today = f.imports_today = f.gdp_today = 0;
		mean_growth += growth[i];
	}
	mean_growth /= N;

	/* 4. Political risk (driver 5) and news (driver 7): rare hash events. */
	std::array<int64_t, MAX_ZONES> news_jump{};
	int64_t mean_risk = 0;
	for (int i = 0; i < n; i++) {
		FxState &f = z[i].fx;
		f.risk_ppm -= std::max<int64_t>(1, f.risk_ppm * RISK_DECAY_PPM_PER_DAY / PPM) * (f.risk_ppm > 0 ? 1 : 0);
		if (f.risk_ppm < 0) f.risk_ppm = 0;
		if (goldmath::Hash(seed, static_cast<uint64_t>(day), 0x5150 + static_cast<uint64_t>(i)) % static_cast<uint64_t>(RISK_ONE_IN_DAYS) == 0) {
			f.risk_ppm = std::min<int64_t>(RISK_MAX_PPM, f.risk_ppm + RISK_SHOCK_MIN_PPM + static_cast<int64_t>(goldmath::Hash(seed, static_cast<uint64_t>(day), 0x5151 + static_cast<uint64_t>(i)) % static_cast<uint64_t>(RISK_SHOCK_SPAN_PPM)));
		}
		mean_risk += f.risk_ppm;

		f.news_ppm -= f.news_ppm * NEWS_DECAY_PPM_PER_DAY / PPM;
		if (goldmath::Hash(seed, static_cast<uint64_t>(day), 0x4E57 + static_cast<uint64_t>(i)) % static_cast<uint64_t>(NEWS_ONE_IN_DAYS) == 0) {
			const int64_t size = NEWS_MIN_PPM + static_cast<int64_t>(goldmath::Hash(seed, static_cast<uint64_t>(day), 0x4E58 + static_cast<uint64_t>(i)) % static_cast<uint64_t>(NEWS_SPAN_PPM));
			news_jump[i] = (goldmath::Hash(seed, static_cast<uint64_t>(day), 0x4E59 + static_cast<uint64_t>(i)) & 1) != 0 ? size : -size;
			f.news_ppm = std::clamp<int64_t>(f.news_ppm + news_jump[i] * NEWS_FAIR_SHARE_PPM / PPM, -NEWS_MAX_PPM, NEWS_MAX_PPM);
		}
	}
	mean_risk /= N;

	/* 5. The mood of the market (driver 6). */
	const int64_t risk_off = std::clamp<int64_t>(-world.mood_ppm * MOOD_RISK_OFF_X + (world.war ? WAR_RISK_OFF_PPM : 0) + mean_risk, RISK_OFF_MIN_PPM, PPM);

	/* 6. Capital flows (driver 4), the fair value, the rate, the intervention (driver 8). */
	for (int i = 0; i < n; i++) {
		FxState &f = z[i].fx;
		FxParts &pt = z[i].parts;
		const FxProfile &pf = PROFILES[i];

		const int64_t goal = (expected[i] - mean_expected) * CAPITAL_GAIN;
		f.capital_ppm = std::clamp<int64_t>(f.capital_ppm + (goal - f.capital_ppm) / CAPITAL_DAYS, -CAPITAL_MAX_PPM, CAPITAL_MAX_PPM);

		pt.ppp_ppm = std::clamp<int64_t>(PPM * PPM / std::max<int64_t>(f.price_level_ppm, 1), RATE_MIN_PPM, RATE_MAX_PPM);
		pt.trade = balance[i] * TRADE_WEIGHT_PPM / PPM;
		pt.growth = (growth[i] - mean_growth) * GROWTH_WEIGHT_PPM / PPM;
		pt.capital = f.capital_ppm;
		pt.news = f.news_ppm;
		pt.risk = -f.risk_ppm * RISK_WEIGHT_PPM / PPM;
		pt.safe = pf.beta_ppm * risk_off / PPM * SAFE_WEIGHT_PPM / PPM;
		pt.trade_balance_ppm = balance[i];
		pt.growth_rel_ppm = growth[i] - mean_growth;
		pt.expected_policy_ppm = expected[i];
		pt.surprise_ppm = surprise[i];
		pt.news_jump_ppm = news_jump[i];
		const int64_t adj = std::clamp<int64_t>(pt.trade + pt.growth + pt.capital + pt.news + pt.risk + pt.safe, -ADJ_MAX_PPM, ADJ_MAX_PPM);
		pt.fair_ppm = std::clamp<int64_t>(MulPpm(pt.ppp_ppm, PPM + adj), RATE_MIN_PPM, RATE_MAX_PPM);

		int64_t r = z[i].rate_ppm;
		r += (pt.fair_ppm - r) * FAIR_PULL_PPM / PPM;
		const int64_t noise = goldmath::Gauss(goldmath::Hash(seed, static_cast<uint64_t>(day), 0x20E0 + static_cast<uint64_t>(i))) * NOISE_SIGMA_PPM / PPM;
		r += MulPpm(r, noise + news_jump[i] + surprise[i] * POLICY_SURPRISE_X);
		r = std::clamp<int64_t>(r, RATE_MIN_PPM, RATE_MAX_PPM);

		/* The central bank leans against a move away from its reference, as long as it has reserves. */
		if (f.ref_ppm <= 0) f.ref_ppm = z[i].rate_ppm;
		int64_t pushed = 0;
		if (pf.strength_ppm > 0) {
			const int64_t dev = (r - f.ref_ppm) * PPM / f.ref_ppm;
			if (dev < -pf.band_ppm) { // too weak: buy the currency with reserves
				int64_t want = (-dev - pf.band_ppm) * pf.strength_ppm / PPM;
				int64_t cost = want * RESERVE_COST_X;
				if (f.reserve_ppm <= 0) { want = 0; cost = 0; }
				else if (cost > f.reserve_ppm) { want = want * f.reserve_ppm / std::max<int64_t>(cost, 1); cost = f.reserve_ppm; }
				r += MulPpm(r, want);
				f.reserve_ppm -= cost;
				pushed = want;
			} else if (dev > pf.band_ppm) { // too strong: sell the currency, the reserves grow
				const int64_t want = (dev - pf.band_ppm) * pf.strength_ppm / PPM;
				r -= MulPpm(r, want);
				f.reserve_ppm = std::min<int64_t>(RESERVE_MAX_PPM, f.reserve_ppm + want * RESERVE_COST_X / 2);
				pushed = -want;
			}
		}
		f.reserve_ppm = std::clamp<int64_t>(f.reserve_ppm + balance[i] * RESERVE_TRADE_PPM / PPM, 0, RESERVE_MAX_PPM);
		pt.defended_ppm = pushed;
		r = std::clamp<int64_t>(r, RATE_MIN_PPM, RATE_MAX_PPM);
		f.ref_ppm += (r - f.ref_ppm) / REF_DAYS;
		z[i].rate_ppm = r;
	}
}

} // namespace zonemath

#endif /* ZONE_MARKET_MATH_H */

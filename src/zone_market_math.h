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
 * Rates: 1.0 = PPM. A zone with a bigger share of the population than an equal share has a stronger currency (the pull level), the rate moves by chance
 * every game day (a hash of the seed, the day and the zone: the same on every client) and is pulled back to that level by REVERT_PPM_PER_DAY.
 */

#ifndef ZONE_MARKET_MATH_H
#define ZONE_MARKET_MATH_H

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include "gold_market_math.h"

namespace zonemath {

static constexpr int64_t PPM = 1'000'000;
static constexpr int MAX_ZONES = 4;                     ///< At most this many zones.
static constexpr int WANT_ZONES = 4;                    ///< This many zones are made when the map has towns far enough apart.
static constexpr int64_t MIN_CAPITAL_DISTANCE = 24;     ///< Two capitals are at least this many tiles apart (distance x + y).
static constexpr int64_t RATE_SIGMA_PPM = 6'000;        ///< Chance move of the rate in one game day: 0.6% (one standard deviation).
static constexpr int64_t REVERT_PPM_PER_DAY = 5'000;    ///< Every game day 0.5% of the gap to the pull level is closed.
static constexpr int64_t RATE_MIN_PPM = 250'000;        ///< The rate stays between 0.25 and 4.0.
static constexpr int64_t RATE_MAX_PPM = 4'000'000;
static constexpr int64_t LEVEL_MIN_PPM = 600'000;       ///< The pull level stays between 0.6 and 1.6.
static constexpr int64_t LEVEL_MAX_PPM = 1'600'000;
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
static constexpr int64_t POLICY_REVERT_PPM_PER_DAY = 20'000; ///< Every game day 2% of the gap to the aim is closed.
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

/** The level that the rate of a zone is pulled to: an equal share of the people is 1.0, twice the share is 1.5, no people 0.6. */
inline int64_t LevelPpm(int64_t zone_pop, int64_t total_pop, int zones)
{
	if (total_pop <= 0 || zones <= 0) return PPM;
	const int64_t share = zone_pop * PPM / total_pop;
	const int64_t equal = PPM / zones;
	return std::clamp<int64_t>(PPM + (share - equal) * PPM / equal / 2, LEVEL_MIN_PPM, LEVEL_MAX_PPM);
}

/** One game day of the rate of a zone: a chance move, then the pull to the level. */
inline int64_t NextRate(uint64_t seed, int zone, int64_t day, int64_t rate, int64_t level)
{
	const int64_t g = goldmath::Gauss(goldmath::Hash(seed, static_cast<uint64_t>(day), 0x20E0 + static_cast<uint64_t>(zone)));
	const int64_t noise = g * RATE_SIGMA_PPM / PPM;                  // ppm of the rate
	int64_t r = rate + rate / PPM * noise + rate % PPM * noise / PPM;
	r += (level - r) * REVERT_PPM_PER_DAY / PPM;
	return std::clamp<int64_t>(r, RATE_MIN_PPM, RATE_MAX_PPM);
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

/** One game day of the yearly policy rate of a zone: it moves towards the inflation + POLICY_OVER_INFL_PPM. */
inline int64_t NextPolicy(int64_t policy, int64_t infl)
{
	const int64_t aim = std::clamp<int64_t>(infl + POLICY_OVER_INFL_PPM, POLICY_MIN_PPM, POLICY_MAX_PPM);
	return std::clamp<int64_t>(policy + (aim - policy) * POLICY_REVERT_PPM_PER_DAY / PPM, POLICY_MIN_PPM, POLICY_MAX_PPM);
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

} // namespace zonemath

#endif /* ZONE_MARKET_MATH_H */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file CargoMarketMath.h The pure maths of the cargo price: what a unit of cargo pays, from supply (what is delivered) and demand (the state of the economy).
 *
 * Nothing in this file knows OpenTTD (standard library only, so cargo_market_test.cpp can test it). Everything is integer arithmetic,
 * ratios in parts per million (ppm), because the result changes the income of the companies and every client of a network game must get
 * exactly the same numbers. No float or double.
 *
 * The idea, one cargo at a time:
 *   - Supply: the units that all the companies deliver per game day. A fast average (about a week) is compared with the normal level
 *     (a slow average, about half a year). More than normal = glut = the price falls; less than normal = shortage = the price rises.
 *     How strong that is depends on the cargo (elasticity): passengers and food react a lot, coal and ore little.
 *   - Demand: the outside factor (mood of the market from the news events, and the money in circulation) moves the price of all the cargo.
 *   - The price moves a part of the way to its goal every game day (like the gold price) and stays between MIN and MAX.
 * The income of a delivery is the income that OpenTTD calculates, multiplied by this price.
 */

#ifndef CARGO_MARKET_MATH_H
#define CARGO_MARKET_MATH_H

#include <algorithm>
#include <cstdint>

namespace cargomarket {

static constexpr int64_t PPM = 1'000'000;                ///< 1.0 in parts per million.

/* ---- Tuning: all the numbers that a designer may want to change ---------------------------------------------------------------------- */
static constexpr int BASELINE_DAYS = 30;                 ///< For this many game days after the first day, the normal level is only measured: the price follows the outside factor alone.
static constexpr int64_t FAST_DIV = 7;                   ///< The fast average of the deliveries follows the daily value by one this-many-th per day (about a week).
static constexpr int64_t REF_DIV = 180;                  ///< The normal level follows the daily value by one this-many-th per day (about half a year).
static constexpr int64_t REF_FLOOR_X100 = 5'000;         ///< The normal level counts as at least 50 units a day (x100), so a tiny trade does not give huge swings.
static constexpr int64_t PRICE_STEP_PPM = 100'000;       ///< Every day the price goes 10% of the way to its goal.
static constexpr int64_t SUPPLY_MIN_PPM = 700'000;       ///< Supply and shortage together never move the goal below 70% ...
static constexpr int64_t SUPPLY_MAX_PPM = 1'300'000;     ///< ... or above 130%.
static constexpr int64_t PRICE_MIN_PPM = 500'000;        ///< The price is never below 50% of the normal pay ...
static constexpr int64_t PRICE_MAX_PPM = 2'000'000;      ///< ... or above 200%.
static constexpr int64_t SENTIMENT_DIV = 3;              ///< The mood of the market (+-15% at most) moves the cargo price by a third of it (+-5%).
static constexpr int64_t MONEY_WEIGHT_PERCENT = 25;      ///< A quarter of the change of the money factor of the price index goes into the cargo price ...
static constexpr int64_t MONEY_MAX_PPM = 100'000;        ///< ... and at most +-10%.

/** What is known about one cargo. Saved: price_ppm, ref_x100, fast_x100, days. delivered_today is not saved (a load starts the day again). */
struct CargoState {
	int64_t price_ppm = PPM;      ///< What a unit pays now, 1.0 = the normal pay of OpenTTD.
	int64_t ref_x100 = 0;         ///< Normal level of the deliveries, units per day x 100.
	int64_t fast_x100 = 0;        ///< Recent level of the deliveries, units per day x 100.
	int32_t days = 0;             ///< Game days since the first day of this cargo; counted up to BASELINE_DAYS + 1.
	int64_t delivered_today = 0;  ///< Units delivered since midnight.
};

/** The outside factor: the mood of the market and the money in circulation, around PPM. */
inline int64_t OutsideFactor(int64_t sentiment_ppm, int64_t money_factor_ppm)
{
	const int64_t mood = std::clamp<int64_t>(sentiment_ppm, -300'000, 300'000) / SENTIMENT_DIV;
	const int64_t money = std::clamp<int64_t>((money_factor_ppm - PPM) * MONEY_WEIGHT_PERCENT / 100, -MONEY_MAX_PPM, MONEY_MAX_PPM);
	return std::clamp<int64_t>(PPM + mood + money, PPM / 2, 2 * PPM);
}

/**
 * The goal of the supply side: more delivered than normal pushes it below 1.0, less than normal above 1.0.
 * @param fast_x100 Recent deliveries per day x 100.
 * @param ref_x100 Normal deliveries per day x 100.
 * @param elasticity_ppm How strongly the price answers: 500,000 means +30% more than normal = -15% price.
 */
inline int64_t SupplyFactor(int64_t fast_x100, int64_t ref_x100, int64_t elasticity_ppm)
{
	const int64_t base = std::max(ref_x100, REF_FLOOR_X100);
	const int64_t excess = std::clamp<int64_t>((fast_x100 - ref_x100) * PPM / base, -PPM, PPM); // -100% .. +100%
	return std::clamp<int64_t>(PPM - excess * elasticity_ppm / PPM, SUPPLY_MIN_PPM, SUPPLY_MAX_PPM);
}

/** One game day of one cargo: the averages are updated, the price goes a part of the way to the goal, the day's counter is cleared. */
inline void UpdateDay(CargoState &s, int64_t elasticity_ppm, int64_t outside_ppm)
{
	const int64_t today_x100 = std::min<int64_t>(s.delivered_today, INT64_MAX / 1000) * 100;
	s.delivered_today = 0;
	if (s.days < BASELINE_DAYS + 1) s.days++;

	int64_t supply = PPM;
	if (s.days <= BASELINE_DAYS) {
		/* Baseline: the normal level is the plain average of the days so far. */
		s.ref_x100 = (s.ref_x100 * (s.days - 1) + today_x100) / s.days;
		s.fast_x100 = s.ref_x100;
	} else {
		s.fast_x100 += (today_x100 - s.fast_x100) / FAST_DIV;
		s.ref_x100 += (today_x100 - s.ref_x100) / REF_DIV;
		supply = SupplyFactor(s.fast_x100, s.ref_x100, elasticity_ppm);
	}
	const int64_t goal = std::clamp<int64_t>(supply * outside_ppm / PPM, PRICE_MIN_PPM, PRICE_MAX_PPM);
	s.price_ppm = std::clamp<int64_t>(s.price_ppm + (goal - s.price_ppm) * PRICE_STEP_PPM / PPM, PRICE_MIN_PPM, PRICE_MAX_PPM);
}

/** Income times price. Split in two parts so a big income cannot overflow. */
inline int64_t ApplyPrice(int64_t income, int64_t price_ppm)
{
	if (income == 0 || price_ppm == PPM) return income;
	return income / PPM * price_ppm + (income % PPM) * price_ppm / PPM;
}

} // namespace cargomarket

#endif /* CARGO_MARKET_MATH_H */

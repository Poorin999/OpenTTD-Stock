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
 *   - Local demand (step 2): the town that the cargo is delivered to makes the price a little higher or lower. A town that the retail
 *     investors have made rich (the demand boost) and a big town pay more; a tiny, poor town pays less. How much depends on the cargo:
 *     people and consumer goods follow the town, raw material hardly does. The factor is limited, so the loop retail wealth ->
 *     demand -> income -> dividends -> retail wealth stays stable.
 *   - Freight rate (step 3): the price of the transport service. Cargo that waits at the stations (the backlog) against what is carried per day
 *     means the service is scarce: the rate goes up. A fleet that grows faster than its own long average means more capacity: the rate goes
 *     down. It multiplies the income together with the price of the cargo. This is a brake that works against itself: a high rate pays for
 *     new vehicles, the backlog shrinks, the rate comes back.
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
static constexpr int64_t LEVEL_CAP_X100 = 1'000'000'000'000; ///< No level of deliveries counts above 10^10 units a day (x100), so level x PPM can never overflow an int64.
static constexpr int64_t STATE_BAND_PPM = 100'000;       ///< For the display only: recent deliveries more than 10% above / below the normal level count as a glut / a shortage.
static constexpr int64_t LOCAL_MIN_PPM = 900'000;        ///< The town of the delivery never lowers the pay by more than 10% ...
static constexpr int64_t LOCAL_MAX_PPM = 1'200'000;      ///< ... or raises it by more than 20%.
static constexpr int64_t LOCAL_BOOST_MAX_PPM = 150'000;  ///< The demand boost of a town counts up to +15% (the retail maximum 30% growth boost x 50%).
static constexpr int64_t PROSPERITY_NEUTRAL_PPM = 500'000; ///< A town of this prosperity (half of the 5,000 people of the retail rules) pays the plain price.
static constexpr int64_t PROSPERITY_SPREAD_PERCENT = 20; ///< The prosperity away from neutral counts 20%: the richest town +10%, a tiny town -10% (for a cargo of weight 1).
static constexpr int64_t LOCAL_WEIGHT_PEOPLE_PPM = 1'000'000; ///< Passengers and mail follow the town fully.
static constexpr int64_t LOCAL_WEIGHT_CONSUMER_PPM = 800'000; ///< Goods and food are bought by the town.
static constexpr int64_t LOCAL_WEIGHT_OTHER_PPM = 250'000;    ///< Raw material goes to industries: the town matters little.

static constexpr int64_t BACKLOG_NORMAL_DAYS = 2;        ///< Cargo waiting for this many days of normal carrying (the normal level) is a normal backlog.
static constexpr int64_t BACKLOG_DIV = 7;                ///< The backlog is smoothed: it follows the daily value by one this-many-th per day.
static constexpr int64_t BACKLOG_WEIGHT_PPM = 300'000;   ///< A backlog twice the normal (pressure +1.0) raises the rate goal by 30%; an empty one lowers it by 30%.
static constexpr int64_t FLEET_FAST_DIV = 30;            ///< The recent fleet size follows the daily count by one this-many-th per day (about a month).
static constexpr int64_t FLEET_SLOW_DIV = 360;           ///< The long fleet size follows by one this-many-th per day (about a year).
static constexpr int64_t FLEET_FLOOR_X100 = 1'000;       ///< The long fleet size counts as at least 10 vehicles (x100), so a tiny fleet does not swing the rate.
static constexpr int64_t FLEET_GROWTH_MAX_PPM = 200'000; ///< Fleet growth against the long average is counted up to +-20% ...
static constexpr int64_t FLEET_WEIGHT_PERCENT = 100;     ///< ... and it moves the rate goal by this share of it (growth +20% = rate -20%).
static constexpr int64_t FREIGHT_MIN_PPM = 700'000;      ///< The freight rate is never below 70% ...
static constexpr int64_t FREIGHT_MAX_PPM = 1'500'000;    ///< ... or above 150%.
static constexpr int64_t FREIGHT_STEP_PPM = 50'000;      ///< Every day the rate goes 5% of the way to its goal (slower than the price: a service changes slowly).
static constexpr int64_t BACKLOG_CAP_X100 = 1'000'000'000'000; ///< No backlog counts above 10^10 units (x100), so the products cannot overflow an int64.

/** The vehicles of all companies: a fast and a long average, in vehicles x 100. Saved (SRK_FLEET). */
struct FleetState {
	int64_t fast_x100 = 0;   ///< Recent fleet size (about a month).
	int64_t slow_x100 = 0;   ///< Long fleet size (about a year).
	bool started = false;    ///< False until the first count: both averages start at the first count, so the first day gives growth 0.
};

/** What is known about one cargo. Saved: price_ppm, ref_x100, fast_x100, days, freight_ppm, backlog_x100. delivered_today and waiting_today are not saved (a load starts the day again). */
struct CargoState {
	int64_t price_ppm = PPM;      ///< What a unit pays now, 1.0 = the normal pay of OpenTTD.
	int64_t ref_x100 = 0;         ///< Normal level of the deliveries, units per day x 100.
	int64_t fast_x100 = 0;        ///< Recent level of the deliveries, units per day x 100.
	int32_t days = 0;             ///< Game days since the first day of this cargo; counted up to BASELINE_DAYS + 1.
	int64_t delivered_today = 0;  ///< Units delivered since midnight.
	int64_t freight_ppm = PPM;    ///< The freight rate now, 1.0 = no change, FREIGHT_MIN_PPM .. FREIGHT_MAX_PPM.
	int64_t backlog_x100 = 0;     ///< Smoothed number of units waiting at all stations, x 100.
	int64_t waiting_today = 0;    ///< Units waiting at the stations when the day was counted (set by the caller before UpdateFreight).
};

/** The outside factor: the mood of the market and the money in circulation, around PPM. */
inline int64_t OutsideFactor(int64_t sentiment_ppm, int64_t money_factor_ppm)
{
	const int64_t mood = std::clamp<int64_t>(sentiment_ppm, -300'000, 300'000) / SENTIMENT_DIV;
	const int64_t money = std::clamp<int64_t>((money_factor_ppm - PPM) * MONEY_WEIGHT_PERCENT / 100, -MONEY_MAX_PPM, MONEY_MAX_PPM);
	return std::clamp<int64_t>(PPM + mood + money, PPM / 2, 2 * PPM);
}

/** How far the recent deliveries are above (+) or below (-) the normal level, -PPM .. +PPM. The levels are capped first, so the product cannot overflow. */
inline int64_t ExcessPpm(int64_t fast_x100, int64_t ref_x100)
{
	const int64_t fast = std::clamp<int64_t>(fast_x100, 0, LEVEL_CAP_X100);
	const int64_t ref = std::clamp<int64_t>(ref_x100, 0, LEVEL_CAP_X100);
	const int64_t base = std::max(ref, REF_FLOOR_X100);
	return std::clamp<int64_t>((fast - ref) * PPM / base, -PPM, PPM);
}

/**
 * The goal of the supply side: more delivered than normal pushes it below 1.0, less than normal above 1.0.
 * @param fast_x100 Recent deliveries per day x 100.
 * @param ref_x100 Normal deliveries per day x 100.
 * @param elasticity_ppm How strongly the price answers: 500,000 means +30% more than normal = -15% price.
 */
inline int64_t SupplyFactor(int64_t fast_x100, int64_t ref_x100, int64_t elasticity_ppm)
{
	return std::clamp<int64_t>(PPM - ExcessPpm(fast_x100, ref_x100) * elasticity_ppm / PPM, SUPPLY_MIN_PPM, SUPPLY_MAX_PPM);
}

/** One game day of one cargo: the averages are updated, the price goes a part of the way to the goal, the day's counter is cleared. */
inline void UpdateDay(CargoState &s, int64_t elasticity_ppm, int64_t outside_ppm)
{
	const int64_t today_x100 = std::min<int64_t>(s.delivered_today, LEVEL_CAP_X100 / 100) * 100;
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

/** One count of the fleet (vehicles of all companies). The first count starts both averages. */
inline void UpdateFleet(FleetState &f, int64_t vehicles)
{
	const int64_t now_x100 = std::clamp<int64_t>(vehicles, 0, LEVEL_CAP_X100 / 100) * 100;
	if (!f.started) { f.fast_x100 = f.slow_x100 = now_x100; f.started = true; return; }
	f.fast_x100 += (now_x100 - f.fast_x100) / FLEET_FAST_DIV;
	f.slow_x100 += (now_x100 - f.slow_x100) / FLEET_SLOW_DIV;
}

/** Fleet growth against the long average, -FLEET_GROWTH_MAX_PPM .. +FLEET_GROWTH_MAX_PPM (positive = more vehicles than usual). */
inline int64_t FleetGrowthPpm(const FleetState &f)
{
	const int64_t fast = std::clamp<int64_t>(f.fast_x100, 0, LEVEL_CAP_X100);
	const int64_t slow = std::clamp<int64_t>(f.slow_x100, 0, LEVEL_CAP_X100);
	return std::clamp<int64_t>((fast - slow) * PPM / std::max(slow, FLEET_FLOOR_X100), -FLEET_GROWTH_MAX_PPM, FLEET_GROWTH_MAX_PPM);
}

/**
 * The pressure of the backlog: how far the waiting cargo is above (+) or below (-) the normal backlog, -PPM .. +PPM.
 * The normal backlog is BACKLOG_NORMAL_DAYS days of the normal carrying (the normal level of deliveries, at least the floor of 50 units a day).
 */
inline int64_t BacklogPressurePpm(int64_t backlog_x100, int64_t ref_x100)
{
	const int64_t backlog = std::clamp<int64_t>(backlog_x100, 0, BACKLOG_CAP_X100);
	const int64_t normal = std::max(std::clamp<int64_t>(ref_x100, 0, LEVEL_CAP_X100), REF_FLOOR_X100) * BACKLOG_NORMAL_DAYS;
	return std::clamp<int64_t>((backlog - normal) * PPM / normal, -PPM, PPM);
}

/** The goal of the freight rate: backlog pushes it up, a growing fleet pushes it down. FREIGHT_MIN_PPM .. FREIGHT_MAX_PPM. */
inline int64_t FreightGoal(int64_t backlog_pressure_ppm, int64_t fleet_growth_ppm)
{
	const int64_t backlog = std::clamp<int64_t>(backlog_pressure_ppm, -PPM, PPM) * BACKLOG_WEIGHT_PPM / PPM;
	const int64_t fleet = std::clamp<int64_t>(fleet_growth_ppm, -FLEET_GROWTH_MAX_PPM, FLEET_GROWTH_MAX_PPM) * FLEET_WEIGHT_PERCENT / 100;
	return std::clamp<int64_t>(PPM + backlog - fleet, FREIGHT_MIN_PPM, FREIGHT_MAX_PPM);
}

/**
 * One game day of the freight rate of one cargo. Call it AFTER UpdateDay (it uses the updated normal level and day count) with waiting_today set.
 * During the baseline days only the backlog average is measured and the rate stays 1.0.
 */
inline void UpdateFreight(CargoState &s, int64_t fleet_growth_ppm)
{
	const int64_t waiting_x100 = std::min<int64_t>(std::max<int64_t>(s.waiting_today, 0), BACKLOG_CAP_X100 / 100) * 100;
	s.waiting_today = 0;
	if (s.days <= BASELINE_DAYS) { s.backlog_x100 = waiting_x100; s.freight_ppm = PPM; return; }
	s.backlog_x100 += (waiting_x100 - s.backlog_x100) / BACKLOG_DIV;
	const int64_t goal = FreightGoal(BacklogPressurePpm(s.backlog_x100, s.ref_x100), fleet_growth_ppm);
	s.freight_ppm = std::clamp<int64_t>(s.freight_ppm + (goal - s.freight_ppm) * FREIGHT_STEP_PPM / PPM, FREIGHT_MIN_PPM, FREIGHT_MAX_PPM);
}

/**
 * The local factor of a delivery: the demand of the town of the destination station.
 * @param weight_ppm How much this cargo follows the town, 0 .. PPM (0 = not at all).
 * @param demand_boost_ppm The demand boost of the town (RetailMarketManager::GetTownDemandBoostPpm), 0 and up.
 * @param prosperity_ppm How prosperous the town is, 0 .. PPM (retail::TownProsperityPpm).
 * @return PPM + weight x (boost + prosperity effect), between LOCAL_MIN_PPM and LOCAL_MAX_PPM.
 */
inline int64_t LocalFactor(int64_t weight_ppm, int64_t demand_boost_ppm, int64_t prosperity_ppm)
{
	const int64_t weight = std::clamp<int64_t>(weight_ppm, 0, PPM);
	if (weight == 0) return PPM;
	const int64_t boost = std::clamp<int64_t>(demand_boost_ppm, 0, LOCAL_BOOST_MAX_PPM);
	const int64_t prosperity = (std::clamp<int64_t>(prosperity_ppm, 0, PPM) - PROSPERITY_NEUTRAL_PPM) * PROSPERITY_SPREAD_PERCENT / 100;
	return std::clamp<int64_t>(PPM + weight * (boost + prosperity) / PPM, LOCAL_MIN_PPM, LOCAL_MAX_PPM);
}

/** For the display only: +1 = glut (more delivered than normal), -1 = shortage, 0 = normal or still measuring (the baseline days). */
inline int SupplyState(int64_t fast_x100, int64_t ref_x100, int32_t days)
{
	if (days <= BASELINE_DAYS) return 0;
	const int64_t excess = ExcessPpm(fast_x100, ref_x100);
	return excess > STATE_BAND_PPM ? 1 : (excess < -STATE_BAND_PPM ? -1 : 0);
}

/** Income times price. Split in two parts so a big income cannot overflow. */
inline int64_t ApplyPrice(int64_t income, int64_t price_ppm)
{
	if (income == 0 || price_ppm == PPM) return income;
	return income / PPM * price_ppm + (income % PPM) * price_ppm / PPM;
}

} // namespace cargomarket

#endif /* CARGO_MARKET_MATH_H */

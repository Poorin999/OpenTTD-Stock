/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file gold_market_math.h The parts of the gold price that are chance or slow waves, as pure functions of (game seed, game day): no state, nothing is
 * saved, every client of a network game gets the same numbers. Standard library only, so they can be tested outside the game (gold_market_test.cpp).
 *
 * Why they exist (found by simulating the old price): the old price was a smooth glide to a target that only news and trades moved. It had
 *  - a daily swing of 0.2% (a real gold price has about 0.7-1%),
 *  - a steady upward push (70% of the gold news pushed up),
 *  - a stock that mines filled and nothing emptied, so the price sank for good (-25%),
 * and nobody traded. Here: daily noise with calm and stormy spells, two slow waves (bull and bear phases), and the world takes gold out of the stock again.
 *
 * Since 2026-10-28 the live price is a RANDOM WALK (StepNoisePpm every 2 game minutes, RevertedPrice once a day), no longer a glide of 5% of the way
 * to a target; the slow waves (CyclePpm) are no longer part of the price (GoldReserveManager::PRICE_USES_CYCLE = false).
 */

#ifndef GOLD_MARKET_MATH_H
#define GOLD_MARKET_MATH_H

#include <algorithm>
#include <cstdint>

namespace goldmath {

static constexpr int64_t PPM = 1'000'000;

static constexpr int64_t NOISE_SIGMA_PPM = 5'500;       ///< Daily noise of the price: about 0.55% (one standard deviation) in a normal spell.
static constexpr int64_t VOL_BLOCK_DAYS = 15;           ///< The spell (calm / normal / stormy) changes every this many game days.
static constexpr int64_t CYCLE_LONG_AMP_PPM = 110'000;  ///< Long wave (bull / bear market): the target is up to 11% above or below its line.
static constexpr int64_t CYCLE_LONG_DAYS_MIN = 330;     ///< Its length: 330 .. 449 game days (decided by the seed).
static constexpr int64_t CYCLE_SHORT_AMP_PPM = 40'000;  ///< Short wave: 4%.
static constexpr int64_t CYCLE_SHORT_DAYS_MIN = 90;     ///< Its length: 90 .. 139 game days.
static constexpr int64_t STEP_SIGMA_DAY_PPM = 8'000;    ///< The REAL price: one standard deviation of a game day in a normal spell, 0.8% (with the calm / stormy spells the mean is about 0.88% a day, about 17% a year; real gold: 0.7-1% a day).
static constexpr int64_t STEPS_PER_DAY = 720;           ///< The live price moves every 2 game minutes (= GoldReserveManager::GLIDE_EVERY_MINUTES, a static_assert in gold_reserve.cpp keeps them equal).
static constexpr int64_t STEP_SIGMA_DIVISOR = 27;       ///< sqrt(STEPS_PER_DAY) = 26.8: 720 steps of 1/27 of the daily sigma together give the daily sigma (a random walk: the spread grows with the square root of time).
static constexpr int64_t REVERT_PPM_PER_DAY = 5'000;    ///< The only pull of the price: every game day 0.5% of the gap to the level that the economy explains (half life about 4.5 game months). The price is NOT led there on a fixed path: it moves with the news and the trades at once, by chance in between.
static constexpr int64_t CASH_SMOOTH_DAYS = 7;          ///< The cash in circulation that the price sees is an average over about this many game days (the real cash of the companies jumps with every income and expense; a real price follows slow money, not the till of a day).
static constexpr int64_t JUMP_ONE_IN_STEPS = 72'000;   ///< A rare jump of the price: one 2-minute step in this many (about one every 100 game days, 3-4 a year, like the days of real gold that move 3% or more).
static constexpr int64_t JUMP_MIN_PPM = 20'000;         ///< Size of a jump: 2.0% .. 4.5% (times how lively the market is at that time), up or down.
static constexpr int64_t JUMP_SPAN_PPM = 25'000;
static constexpr int64_t IMPACT_FADE_PPM_PER_STEP = 600; ///< The price impact of a trade fades by this share every 2 minutes: half of it is gone after about 1.6 game days (a real order impact is gone in minutes to hours, a news is not: it stays in pressure_ppm).
static constexpr int64_t STOCK_REBALANCE_PERMILLE = 8;  ///< Every game day the world moves the gold stock this many thousandths of the way back to the start stock.

/** A deterministic 64 bit hash (splitmix64 style) of the inputs. */
inline uint64_t Hash(uint64_t a, uint64_t b, uint64_t c = 0)
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

/** Roughly normal number with a standard deviation of PPM: the sum of three uniform numbers. Limited to +-3 sigma. */
inline int64_t Gauss(uint64_t hash)
{
	int64_t sum = 0;
	for (int i = 0; i < 3; i++) {
		const uint64_t part = (hash >> (i * 21)) & 0x1FFFFF;
		sum += static_cast<int64_t>((part * 2'000'001) >> 21) - 1'000'000; // uniform, no modulo bias
	}
	return std::clamp<int64_t>(sum, -3 * PPM, 3 * PPM);
}

/** How lively the market is in the spell of this day, in ppm (1.0 = normal): calm 0.6, normal 1.0, busy 1.5, stormy 2.2. Clusters of 15 days. */
inline int64_t VolatilityPpm(uint64_t seed, int64_t day)
{
	const uint64_t r = Hash(seed, static_cast<uint64_t>(day / VOL_BLOCK_DAYS), 0x50F7) % 100;
	return r < 25 ? 600'000 : (r < 80 ? PPM : (r < 95 ? 1'500'000 : 2'200'000));
}

/** The chance part of the price move of one game day, in ppm (positive = up). Average zero. */
inline int64_t DailyNoisePpm(uint64_t seed, int64_t day)
{
	const int64_t g = Gauss(Hash(seed, static_cast<uint64_t>(day), 0xD01));
	return g * NOISE_SIGMA_PPM / PPM * VolatilityPpm(seed, day) / PPM;
}

/** How lively the market is at this game minute, in ppm: the value of the spell (VolatilityPpm) glides into the value of the next spell over its 15 days, so the volatility changes without a step (a real market calms down and heats up gradually). */
inline int64_t SmoothVolatilityPpm(uint64_t seed, int64_t total_minutes)
{
	const int64_t block_minutes = VOL_BLOCK_DAYS * 1440;
	const int64_t b = total_minutes / block_minutes, frac = total_minutes % block_minutes;
	const int64_t v0 = VolatilityPpm(seed, b * VOL_BLOCK_DAYS), v1 = VolatilityPpm(seed, (b + 1) * VOL_BLOCK_DAYS);
	return v0 + (v1 - v0) * frac / block_minutes;
}

/**
 * The chance move of ONE 2-minute step of the live price, in ppm (positive = up). This is what makes the price a random walk like a real market:
 * every step has its own number (a hash of the seed and the game minute), so the path through the day is not a straight line, nobody can read the
 * end of the day from the morning, and the size follows the smooth calm / stormy spells (SmoothVolatilityPpm). 720 steps together have the daily
 * standard deviation STEP_SIGMA_DAY_PPM x spell. On top of that, rarely (JUMP_ONE_IN_STEPS) a jump of 2-4.5% (the fat tail of a real market: a few
 * days a year move far more than the normal noise), bigger when the market is stormy.
 */
inline int64_t StepNoisePpm(uint64_t seed, int64_t total_minutes)
{
	const int64_t vol = SmoothVolatilityPpm(seed, total_minutes);
	const int64_t g = Gauss(Hash(seed, static_cast<uint64_t>(total_minutes), 0x57E9));
	int64_t move = g * STEP_SIGMA_DAY_PPM * vol / (PPM * PPM * STEP_SIGMA_DIVISOR);
	const uint64_t j = Hash(seed, static_cast<uint64_t>(total_minutes), 0x70AB);
	if ((j >> 8) % static_cast<uint64_t>(JUMP_ONE_IN_STEPS) == 0) {
		const int64_t size = (JUMP_MIN_PPM + static_cast<int64_t>((j >> 40) % static_cast<uint64_t>(JUMP_SPAN_PPM + 1))) * vol / PPM;
		move += (j & 1) != 0 ? size : -size;
	}
	return move;
}

/** One 2-minute step of the fade of the price impact of the trades (IMPACT_FADE_PPM_PER_STEP). Always moves at least 1 towards 0, so it can not stall. */
inline int64_t FadedImpact(int64_t impact)
{
	if (impact == 0) return 0;
	int64_t d = impact * IMPACT_FADE_PPM_PER_STEP / PPM;
	if (d == 0) d = impact > 0 ? 1 : -1;
	return impact - d;
}

/** One 2-minute step of the average of the cash (exponential, about CASH_SMOOTH_DAYS days). avg <= 0 = not known: take the cash as it is. Always moves at least 1 towards the cash, so it can not stall. */
inline int64_t SmoothedCash(int64_t avg, int64_t cash)
{
	if (avg <= 0) return std::max<int64_t>(0, cash);
	const int64_t span = CASH_SMOOTH_DAYS * STEPS_PER_DAY;
	int64_t step = (cash - avg) / span;
	if (step == 0 && cash != avg) step = cash > avg ? 1 : -1;
	return avg + step;
}

/** One game day of the weak pull: the price moves REVERT_PPM_PER_DAY of the gap towards the level of the economy. Pure, so it can be tested. */
inline int64_t RevertedPrice(int64_t price, int64_t level)
{
	if (level <= 0) return price;
	return std::max<int64_t>(1, price + (level - price) * REVERT_PPM_PER_DAY / PPM);
}

/** The same weak pull, but every game HOUR (1/24 of the daily pull; a 2-minute step would round to 0 for a price in cents). No step at midnight. */
inline int64_t RevertedPriceHour(int64_t price, int64_t level)
{
	if (level <= 0) return price;
	return std::max<int64_t>(1, price + (level - price) * REVERT_PPM_PER_DAY / (PPM * 24));
}

/** The fade of the news pressure (3% a day) in the small steps of the price. Always moves at least 1 towards 0, so it can not stall. */
inline int64_t FadedPressureStep(int64_t pressure)
{
	if (pressure == 0) return 0;
	int64_t d = pressure * 3 / (100 * STEPS_PER_DAY);
	if (d == 0) d = pressure > 0 ? 1 : -1;
	return pressure - d;
}

/** The world moves the gold stock back to the start stock (STOCK_REBALANCE_PERMILLE a day) in the small steps of the price. Never passes the start stock. */
inline int64_t RebalancedStockStep(int64_t stock, int64_t start)
{
	const int64_t gap = start - stock;
	int64_t step = gap * STOCK_REBALANCE_PERMILLE / (1000 * STEPS_PER_DAY);
	if (step == 0 && gap != 0) step = gap > 0 ? 1 : -1;
	return stock + step;
}

/** A smooth wave from -PPM to +PPM: a triangle made round at the turns (3x - x^3) / 2. */
inline int64_t SmoothWave(int64_t t, int64_t period)
{
	if (period < 2) return 0;
	const int64_t x_t = ((t % period) + period) % period;
	const int64_t half = period / 2;
	const int64_t x = x_t < half ? -PPM + 2 * PPM * x_t / half : PPM - 2 * PPM * (x_t - half) / std::max<int64_t>(1, period - half);
	const int64_t cube = x * x / PPM * x / PPM;
	return (3 * x - cube) / 2;
}

/** The slow waves of the price: bull and bear phases. Two waves of different length, together at most +-15%. Mean zero. */
inline int64_t CyclePpm(uint64_t seed, int64_t day)
{
	const int64_t long_days = CYCLE_LONG_DAYS_MIN + static_cast<int64_t>(Hash(seed, 1, 0xC1) % 120);
	const int64_t short_days = CYCLE_SHORT_DAYS_MIN + static_cast<int64_t>(Hash(seed, 2, 0xC2) % 50);
	const int64_t long_phase = static_cast<int64_t>(Hash(seed, 3, 0xC3) % static_cast<uint64_t>(long_days));
	const int64_t short_phase = static_cast<int64_t>(Hash(seed, 4, 0xC4) % static_cast<uint64_t>(short_days));
	return CYCLE_LONG_AMP_PPM * SmoothWave(day + long_phase, long_days) / PPM + CYCLE_SHORT_AMP_PPM * SmoothWave(day + short_phase, short_days) / PPM;
}

/** One game day of the world's own supply and demand: the stock moves a little towards the start stock. */
inline int64_t RebalancedStock(int64_t stock, int64_t start_stock)
{
	const int64_t gap = start_stock - stock;
	int64_t step = gap * STOCK_REBALANCE_PERMILLE / 1000;
	if (step == 0 && gap != 0) step = gap > 0 ? 1 : -1;
	return stock + step;
}

} // namespace goldmath

#endif /* GOLD_MARKET_MATH_H */

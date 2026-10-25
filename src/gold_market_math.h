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

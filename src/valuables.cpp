/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file valuables.cpp The price model of the valuables (gold, silver, platinum, diamonds) of the trading terminal. */

#include "stdafx.h"
#include "valuables.h"
#include "stock_base.h"
#include "gold_reserve.h"
#include "settings_type.h"
#include "game_time/game_time_manager.h"
#include "timer/timer.h"

#include "safeguards.h"

ValuablesManager _valuables;

/** One million: 1.0 in ppm. */
static constexpr int64_t PPM = 1'000'000;

/** The valuables, in the order of #ValuableId. */
static constexpr ValuableInfo _valuable_info[VALUABLE_COUNT] = {
	/* code   name        unit      anchor link     lev  spread depth cycle    days  drift_max drift_step noise */
	{"XAU", "Gold",     "1 oz",    2000, 1'000'000, 20,  1'500, 20,        0,    1,    30'000,     2'000, 1'500},
	{"XAG", "Silver",   "10 oz",    250,   950'000, 15,  3'000, 15,  100'000,  300,   150'000,     9'000, 4'000},
	{"XPT", "Platinum", "1 oz",    1000,   800'000, 10,  3'000, 15,   80'000,  450,   180'000,     9'000, 3'000},
	{"XDI", "Diamonds", "1 carat",  600,   350'000,  5,  6'000, 10,  120'000,  700,   250'000,    10'000, 2'000},
};

const ValuableInfo &ValuablesManager::Info(size_t index)
{
	return _valuable_info[std::min<size_t>(index, VALUABLE_COUNT - 1)];
}

/** The leverage of a symbol as the divisor of the margin (10 for the shares of a company). */
int64_t GetStockMarginDivisor(size_t symbol)
{
	if (!IsValuableSymbol(symbol)) return STOCK_MARGIN_DIVISOR;
	return ValuablesManager::Info(symbol - STOCK_FIRST_VALUABLE).leverage;
}

/** Forget everything: a new game, or a savegame that is loaded. */
void ValuablesManager::Reset()
{
	this->drift_ppm.fill(0);
	this->noise_ppm.fill(0);
	this->rng_state = 0;
}

/**
 * The next random number (xorshift64*). The generator is seeded from the generation seed of the map the first time it is used, and
 * its state is saved, so it is the same on every client and different in every game.
 */
uint64_t ValuablesManager::Next()
{
	if (this->rng_state == 0) {
		uint64_t seed = static_cast<uint64_t>(_settings_game.game_creation.generation_seed) * 0x9E3779B97F4A7C15ULL + 0xD1B54A32D192ED03ULL;
		seed ^= seed >> 29;
		this->rng_state = seed != 0 ? seed : 0x2545F4914F6CDD1DULL;
	}
	uint64_t x = this->rng_state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	this->rng_state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

/** A random number from -max to +max (both included). */
int64_t ValuablesManager::RandomSigned(int64_t max)
{
	if (max <= 0) return 0;
	return static_cast<int64_t>((this->Next() >> 20) % static_cast<uint64_t>(2 * max + 1)) - max;
}

/** Every economy day: the fast noise of every valuable gets a fresh part; the old part fades. */
void ValuablesManager::StepFast()
{
	for (size_t k = 0; k < VALUABLE_COUNT; k++) {
		const ValuableInfo &info = Info(k);
		this->noise_ppm[k] = std::clamp<int64_t>(this->noise_ppm[k] * 85 / 100 + this->RandomSigned(info.noise_ppm), -4 * info.noise_ppm, 4 * info.noise_ppm);
	}
}

/** Every game day: the slow drift of every valuable takes a random step and is pulled back towards 0 a little. */
void ValuablesManager::StepSlow()
{
	for (size_t k = 0; k < VALUABLE_COUNT; k++) {
		const ValuableInfo &info = Info(k);
		int64_t drift = this->drift_ppm[k] + this->RandomSigned(info.drift_step_ppm) - this->drift_ppm[k] / 60;
		this->drift_ppm[k] = std::clamp<int64_t>(drift, -info.drift_max_ppm, info.drift_max_ppm);
	}
}

/** A smooth wave from -PPM to +PPM with the given period; day is the number of the game day, phase moves the wave. */
static int64_t SmoothWave(int64_t day, int64_t phase, int64_t period)
{
	if (period < 2) return 0;
	const int64_t t = (day + phase) % period;
	const int64_t half = period / 2;
	/* A triangle from -1 to +1 and back ... */
	const int64_t x = t < half ? -PPM + 2 * PPM * t / half : PPM - 2 * PPM * (t - half) / std::max<int64_t>(1, period - half);
	/* ... made round at the turns: (3x - x^3) / 2. */
	const int64_t cube = x * x / PPM * x / PPM;
	return (3 * x - cube) / 2;
}

/**
 * The price that the market of a valuable heads to, times #STOCK_PRICE_SCALE.
 * @param index Index of the valuable (0 .. VALUABLE_COUNT - 1).
 * @return The price of one unit; 0 while the gold market has not started.
 */
int64_t ValuablesManager::GetBaseFixed(size_t index) const
{
	if (index >= VALUABLE_COUNT) return 0;
	const int64_t gold = _gold_reserve.GetPriceCents();
	if (gold <= 0) return 0;
	const ValuableInfo &info = Info(index);

	/* How much dearer or cheaper gold is than at the start (1.0 = PPM), and the part of it that this valuable follows. */
	const int64_t gold_ppm = gold * PPM / GoldReserveManager::BASE_PRICE_CENTS;
	const int64_t follow_ppm = std::max<int64_t>(PPM / 10, PPM + info.link_ppm * (gold_ppm - PPM) / PPM);

	int64_t price = info.anchor * STOCK_PRICE_SCALE * follow_ppm / PPM;

	const int64_t day = static_cast<int64_t>(std::min<uint64_t>(GameTimeManager::Get().GetCurrentDayIndex(), INT32_MAX));
	const int64_t cycle = info.cycle_ppm * SmoothWave(day, static_cast<int64_t>(index) * 137, info.cycle_days) / PPM;

	price = price * (PPM + cycle) / PPM;
	price = price * (PPM + this->drift_ppm[index]) / PPM;
	price = price * (PPM + this->noise_ppm[index]) / PPM;
	return std::max<int64_t>(STOCK_MIN_PRICE * STOCK_PRICE_SCALE, price);
}

/** The drift takes one step per game day of the clock, like the gold market. */
static const GameTimeSubscription _valuables_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &)
{
	_valuables.StepSlow();
});

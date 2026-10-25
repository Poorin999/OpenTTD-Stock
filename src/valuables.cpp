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
	this->diamond_stock_carats = DIAMOND_STOCK_START;
	this->diamond_mine_allowance = DIAMOND_MINE_DAILY_MAX;
}

/** The price factor of the diamond stock: start stock / stock now, x0.5 .. x4 (half the stock = twice the price). */
int64_t ValuablesManager::DiamondScarcityPpm() const
{
	const int64_t floor_stock = DIAMOND_STOCK_START / DIAMOND_SCARCITY_FLOOR_DIV;
	return std::clamp<int64_t>(PPM * DIAMOND_STOCK_START / std::max(this->diamond_stock_carats, floor_stock), PPM / 2, DIAMOND_SCARCITY_FLOOR_DIV * PPM);
}

/** What mines may add: all of it up to the start stock, falling in a straight line to 0 at the cap (a full market does not want more). */
int64_t ValuablesManager::DiamondYieldPpm(int64_t stock_carats)
{
	if (stock_carats <= DIAMOND_STOCK_START) return PPM;
	if (stock_carats >= DIAMOND_STOCK_CAP) return 0;
	return PPM * (DIAMOND_STOCK_CAP - stock_carats) / (DIAMOND_STOCK_CAP - DIAMOND_STOCK_START);
}

void DiamondTerminalTrade(bool buy, uint32_t carats)
{
	_valuables.diamond_stock_carats = buy ? std::max<int64_t>(0, _valuables.diamond_stock_carats - carats) : _valuables.diamond_stock_carats + carats;
}

void DiamondsFromDelivery(uint count)
{
	if (count == 0) return;
	const int64_t room = ValuablesManager::DIAMOND_STOCK_CAP - _valuables.diamond_stock_carats;
	if (room <= 0) return; // A stock above the cap (should not happen) is not cut, it gets nothing more.
	int64_t carats = static_cast<int64_t>(count) * ValuablesManager::DIAMOND_CARATS_PER_UNIT * ValuablesManager::DiamondYieldPpm(_valuables.diamond_stock_carats) / PPM;
	carats = std::min({carats, room, _valuables.diamond_mine_allowance});
	if (carats <= 0) return;
	_valuables.diamond_stock_carats += carats;
	_valuables.diamond_mine_allowance -= carats;
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

/**
 * Every game hour: the slow drift of every valuable takes a small random step and is pulled back towards 0 a little.
 * It used to take one big step per game day, which showed as a jump of the price at midnight. 24 steps of 1/5 of the size (5 is about the
 * square root of 24) give the same spread over a day, and the pull is 1/24 of the daily pull. Nothing new is saved.
 */
void ValuablesManager::StepSlow()
{
	/* Diamond stock: the world takes its share every hour; the bucket of what mines may add fills by 1/24 of the daily maximum. */
	this->diamond_stock_carats = std::max<int64_t>(0, this->diamond_stock_carats - DIAMOND_DEMAND_DAILY / DRIFT_STEPS_PER_DAY);
	this->diamond_mine_allowance = std::min(DIAMOND_MINE_DAILY_MAX, this->diamond_mine_allowance + DIAMOND_MINE_DAILY_MAX / DRIFT_STEPS_PER_DAY);
	for (size_t k = 0; k < VALUABLE_COUNT; k++) {
		const ValuableInfo &info = Info(k);
		int64_t pull = this->drift_ppm[k] / (DRIFT_PULL_DAYS * DRIFT_STEPS_PER_DAY);
		if (pull == 0 && this->drift_ppm[k] != 0) pull = this->drift_ppm[k] > 0 ? 1 : -1; // Integer division must not stop the pull.
		const int64_t drift = this->drift_ppm[k] + this->RandomSigned(info.drift_step_ppm / DRIFT_STEP_DIVISOR) - pull;
		this->drift_ppm[k] = std::clamp<int64_t>(drift, -info.drift_max_ppm, info.drift_max_ppm);
	}
}

/** A smooth wave from -PPM to +PPM with the given period; t_now is the game time, phase moves the wave (both in the unit of the period, here game minutes). */
static int64_t SmoothWave(int64_t t_now, int64_t phase, int64_t period)
{
	if (period < 2) return 0;
	const int64_t t = (t_now + phase) % period;
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

	/* The cycle runs on game MINUTES, not on whole days, so it does not step at midnight. */
	constexpr int64_t MPD = GameTimeManager::MINUTES_PER_DAY;
	const int64_t minutes = static_cast<int64_t>(std::min<uint64_t>(GameTimeManager::Get().GetTotalGameMinutes(), static_cast<uint64_t>(INT32_MAX) * MPD));
	const int64_t cycle = info.cycle_ppm * SmoothWave(minutes, static_cast<int64_t>(index) * 137 * MPD, info.cycle_days * MPD) / PPM;

	price = price * (PPM + cycle) / PPM;
	price = price * (PPM + this->drift_ppm[index]) / PPM;
	price = price * (PPM + this->noise_ppm[index]) / PPM;
	if (index == static_cast<size_t>(ValuableId::Diamond)) price = price * this->DiamondScarcityPpm() / PPM; // The less diamonds in the market, the dearer.
	return std::max<int64_t>(STOCK_MIN_PRICE * STOCK_PRICE_SCALE, price);
}

/** The drift takes a small step every game hour of the clock (not one big step at midnight). */
static const GameTimeSubscription _valuables_hourly(GameTimeEvent::Hour, [](const GameTimeSnapshot &)
{
	_valuables.StepSlow();
});

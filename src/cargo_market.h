/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file cargo_market.h The price of cargo follows supply and demand (see CargoMarketMath.h for the rules).
 * Two hooks in economy.cpp (apply_overlay.py, patch_cargo_market): every final delivery is counted (CargoMarketOnDelivery), and the income
 * that OpenTTD calculates for a delivery is multiplied by the price of the cargo (CargoMarketAdjustIncome). Once per game day the prices move.
 * Gold and valuables are NOT touched: their price is the gold market.
 */

#ifndef CARGO_MARKET_H
#define CARGO_MARKET_H

#include "stdafx.h"
#include "CargoMarketMath.h"
#include "cargo_type.h"
#include "economy_type.h"
#include <array>

/** The state of the cargo prices: one entry per cargo type of OpenTTD. Saved in the 'STCK' chunk (SRK_CARGO_MARKET). */
struct CargoMarket {
	std::array<cargomarket::CargoState, NUM_CARGO> cargo{};

	void Reset() { this->cargo.fill({}); }
	void UpdateDaily();
	int64_t GetPricePpm(CargoType cargo_type) const;
};

extern CargoMarket _cargo_market;

/** A final delivery: count the units (supply). */
void CargoMarketOnDelivery(CargoType cargo_type, uint count);

/** The income of a delivery times the price of the cargo. Same result on every client. */
Money CargoMarketAdjustIncome(CargoType cargo_type, Money income);

#endif /* CARGO_MARKET_H */

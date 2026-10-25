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
 * The freight rate (step 3) needs no hook of its own: it is part of the factor in CargoMarketAdjustIncome.
 * A third hook (patch_cargo_local) multiplies the income of a final delivery by the demand of the town of the destination station (CargoMarketAdjustLocalIncome).
 * Gold and valuables are NOT touched: their price is the gold market.
 */

#ifndef CARGO_MARKET_H
#define CARGO_MARKET_H

#include "stdafx.h"
#include "CargoMarketMath.h"
#include "cargo_type.h"
#include "economy_type.h"
#include "station_type.h"
#include "town_type.h"
#include <array>

/** The state of the cargo prices: one entry per cargo type of OpenTTD. Saved in the 'STCK' chunk (SRK_CARGO_MARKET). */
struct CargoMarket {
	std::array<cargomarket::CargoState, NUM_CARGO> cargo{};
	cargomarket::FleetState fleet{}; ///< The vehicles of all companies (fast and long average), for the freight rate. Saved (SRK_FLEET).

	void Reset() { this->cargo.fill({}); this->fleet = {}; }
	void UpdateDaily();
	/** The price of the cargo from supply and demand (step 1), without the freight rate. */
	int64_t GetPricePpm(CargoType cargo_type) const;
	/** The freight rate of the cargo (step 3): the price of the transport service from the backlog at the stations and the size of the fleet, 70% .. 150%. */
	int64_t GetFreightPpm(CargoType cargo_type) const;
};

extern CargoMarket _cargo_market;

/** A final delivery: count the units (supply). */
void CargoMarketOnDelivery(CargoType cargo_type, uint count);

/** The income of a delivery times the price of the cargo and the freight rate. Same result on every client. */
Money CargoMarketAdjustIncome(CargoType cargo_type, Money income);

/** The local factor of a delivery to a town (1.0 = no change): the demand boost and the prosperity of the town, weighted by the kind of cargo. See CargoMarketMath.h. */
int64_t CargoMarketLocalFactorPpm(CargoType cargo_type, TownID town);

/** The income of a FINAL delivery times the local factor of the town of the destination station. Transfers and the estimates in windows do not use it. */
Money CargoMarketAdjustLocalIncome(CargoType cargo_type, StationID dest, Money income);

/** For the windows: true for the cargo that the gold market prices (it has no cargo price of its own). */
bool CargoMarketIsGoldCargo(CargoType cargo_type);

/** For the windows: the push of the news on the price goal of this cargo now, in ppm (0 when no news moves this cargo). */
int64_t CargoMarketNewsPushPpm(CargoType cargo_type);

#endif /* CARGO_MARKET_H */

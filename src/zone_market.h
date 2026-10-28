/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file zone_market.h Money zones, step B: zones with a currency and a moving rate; a delivery across a border pays an exchange fee. See zone_market_math.h. */

#ifndef ZONE_MARKET_H
#define ZONE_MARKET_H

#include <array>
#include <cstdint>
#include "company_type.h"
#include "station_type.h"
#include "tile_type.h"
#include "zone_market_math.h"

/** One zone: the capital (a tile, so the zone survives when the town is gone), the rate and the fees that it earned. */
struct ZoneState {
	TileIndex capital = INVALID_TILE;
	int64_t rate_ppm = zonemath::PPM; ///< Value of one unit of the zone currency in base units, in ppm. Saved (SRK_ZONE v2).
	int64_t fees = 0;                 ///< Exchange fees paid for deliveries INTO this zone, in money units. Saved (SRK_ZONE v3).
	int64_t population = 0;           ///< People living in the zone at the last day (not saved, made again every day).
	int64_t land_tax = 0;             ///< Land tax paid for the tiles inside the zone, all companies together (saved, SRK_ZONE_TAX).
	int64_t transport_tax = 0;        ///< Transport tax paid for the vehicles that stood inside the zone at the daily count (saved, SRK_ZONE_TAX).
	int64_t vat = 0;                  ///< VAT of the deliveries whose destination station is inside the zone, counted from the income of each delivery (saved, SRK_ZONE_TAX).
};

struct ZoneMarket {
	std::array<ZoneState, zonemath::MAX_ZONES> zones{};
	int count = 0; ///< Number of zones, 0 = not set up yet (the first game day makes them).

	void Reset();
	bool MakeZones();          ///< Choose the capitals from the towns of the map when there are no zones yet. True when zones exist afterwards.
	void RefreshPopulation();  ///< The people of every zone (and the total), made again.
	void UpdateDaily();
	int ZoneOfTile(TileIndex tile) const;
	int64_t FeePpmBetween(int from, int to) const;
};

extern ZoneMarket _zone_market;

/** Called for every final delivery (economy.cpp, hook ZONE_FEE_PATCH): charges the exchange fee when the cargo crossed a zone border. */
void ZoneFeeOnDelivery(CompanyID owner, int64_t income, StationID source, StationID dest);

/** The names of the currencies of the zones (English; the console prints the capital next to it). */
const char *ZoneCurrencyName(int zone);
const char *ZoneCurrencySymbol(int zone);

#endif /* ZONE_MARKET_H */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file km_system.cpp Kilometres on the map: console commands 'km', 'km_route' and 'km_vehicle'. Read only: nothing here changes the game state,
 * so it is safe in a network game. The maths (length of a tile, travel time) is in km_math.h, which also explains why one tile is 921.6 m and why
 * the vehicles need no change of speed.
 */

#include "stdafx.h"
#include <charconv>
#include <span>
#include <string>
#include <string_view>
#include "3rdparty/fmt/format.h"
#include "console_func.h"
#include "console_internal.h"
#include "map_func.h"
#include "station_base.h"
#include "vehicle_base.h"
#include "order_base.h"
#include "base_station_base.h"
#include "km_math.h"

#include "safeguards.h"

/** Read a whole number from the console text. */
static bool KmParseNumber(std::string_view text, int64_t &value)
{
	const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
	return r.ec == std::errc() && r.ptr == text.data() + text.size();
}

/** "12.3 km" from tenths of a kilometre. */
static std::string KmText(int64_t km_tenths)
{
	return fmt::format("{}.{} km", kmmath::KmWhole(km_tenths), kmmath::KmFraction(km_tenths));
}

/** "2 d 05 h 07 min (552 real s)" for a trip of this many tiles at this speed. */
static std::string KmTripText(int64_t tiles, int64_t speed_kmh)
{
	if (speed_kmh <= 0) return "never arrives (speed 0)";
	const kmmath::Duration d = kmmath::SplitMinutes(kmmath::TravelClockMinutes(tiles, speed_kmh));
	return fmt::format("{} d {:02} h {:02} min of game time (about {} s of real time at normal speed)", d.days, d.hours, d.minutes, kmmath::TravelRealSeconds(tiles, speed_kmh));
}

/** Print the distance between two tiles and the trip time at one speed, or at three typical speeds when none is given. */
static void KmPrintTrip(TileIndex a, TileIndex b, int64_t speed_kmh)
{
	const int64_t manhattan = DistanceManhattan(a, b);
	const int64_t straight = kmmath::TilesFromSquare(DistanceSquare(a, b));
	IConsolePrint(CC_DEFAULT, "distance on the map: {} tiles along the tracks / roads (the distance that pays), {} tiles in a straight line", manhattan, straight);
	IConsolePrint(CC_DEFAULT, "in kilometres: {} along the tracks / roads, {} in a straight line", KmText(kmmath::TilesToKmTenths(manhattan)), KmText(kmmath::TilesToKmTenths(straight)));
	if (speed_kmh > 0) {
		IConsolePrint(CC_DEFAULT, "at {} km/h: {}", speed_kmh, KmTripText(manhattan, speed_kmh));
		return;
	}
	for (const int64_t s : {48, 100, 160}) IConsolePrint(CC_DEFAULT, "at {} km/h: {}", s, KmTripText(manhattan, s));
}

/**
 * The place of an order, if it has one that is a station or a waypoint (implicit orders count: they are the stops that the vehicle made).
 * @param order The order.
 * @param tile Gets the tile.
 * @return True if the order has a place.
 */
static bool KmOrderTile(const Order *order, TileIndex &tile)
{
	if (!order->IsType(OT_GOTO_STATION) && !order->IsType(OT_IMPLICIT) && !order->IsType(OT_GOTO_WAYPOINT)) return false;
	const BaseStation *bs = BaseStation::GetIfValid(order->GetDestination().ToStationID());
	if (bs == nullptr) return false;
	tile = bs->xy;
	return true;
}

/**
 * The text that the orders window adds at the end of an order line: the distance in kilometres from the place of the order before it
 * (going round: the first order is measured from the last one). Orders without a place (depot, conditional ...) are skipped when looking
 * back; an order without a place gets no text. Hooked in by patch_order_km of apply_overlay.py.
 * @param v The vehicle.
 * @param order_index The index of the order in the list of the vehicle.
 * @return " (12.3 km)" or an empty string.
 */
std::string KmOrderSuffix(const Vehicle *v, VehicleOrderID order_index)
{
	if (v->orders == nullptr) return {};
	const VehicleOrderID count = v->GetNumOrders();
	if (count < 2 || order_index >= count) return {};
	TileIndex here;
	if (!KmOrderTile(v->GetOrder(order_index), here)) return {};
	for (VehicleOrderID step = 1; step < count; step++) {
		const VehicleOrderID prev = static_cast<VehicleOrderID>((order_index + count - step) % count);
		TileIndex there;
		if (!KmOrderTile(v->GetOrder(prev), there)) continue;
		return fmt::format(" ({})", KmText(kmmath::TilesToKmTenths(DistanceManhattan(there, here))));
	}
	return {};
}

/** 'km': the scale of the map. */
static bool ConKm(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Show the scale of the map in kilometres. Usage: 'km'. See also 'km_route <station id> <station id> [km/h]' and 'km_vehicle <vehicle id>'.");
		return true;
	}
	IConsolePrint(CC_DEFAULT, "one tile = {}.{} m (the length at which a speed in km/h agrees with the game clock)", kmmath::MM_PER_TILE / 1000, kmmath::MM_PER_TILE % 1000 / 100);
	IConsolePrint(CC_DEFAULT, "the map is {} x {} tiles = {} x {}", Map::SizeX(), Map::SizeY(), KmText(kmmath::TilesToKmTenths(Map::SizeX())), KmText(kmmath::TilesToKmTenths(Map::SizeY())));
	IConsolePrint(CC_DEFAULT, "a vehicle at 100 km/h covers {} tiles ({} km) in one game day and needs {} to cross one tile", kmmath::TilesPerGameDay(100), kmmath::KmPerGameDay(100), KmTripText(1, 100));
	IConsolePrint(CC_DEFAULT, "trains, road vehicles and ships follow this scale. Aircraft are not calibrated (the setting 'plane speed' divides their speed).");
	return true;
}

/** 'km_route <station> <station> [km/h]': the distance between two stations in kilometres and how long a vehicle needs. */
static bool ConKmRoute(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Distance between two stations in kilometres and the travel time. Usage: 'km_route <station id> <station id> [speed in km/h]'. Without a speed it shows 48, 100 and 160 km/h.");
		return true;
	}
	int64_t ida = 0, idb = 0, speed = 0;
	if (argv.size() < 3 || argv.size() > 4 || !KmParseNumber(argv[1], ida) || !KmParseNumber(argv[2], idb) || ida < 0 || idb < 0 || ida > 0xFFFF || idb > 0xFFFF) {
		IConsolePrint(CC_ERROR, "Usage: km_route <station id> <station id> [speed in km/h]");
		return true;
	}
	if (argv.size() == 4 && (!KmParseNumber(argv[3], speed) || speed < 1 || speed > 5000)) {
		IConsolePrint(CC_ERROR, "The speed must be a whole number from 1 to 5000 km/h.");
		return true;
	}
	const Station *a = Station::GetIfValid(StationID{static_cast<uint16_t>(ida)});
	const Station *b = Station::GetIfValid(StationID{static_cast<uint16_t>(idb)});
	if (a == nullptr || b == nullptr) {
		IConsolePrint(CC_ERROR, "'{}' is not a station of this game.", a == nullptr ? argv[1] : argv[2]);
		return true;
	}
	IConsolePrint(CC_DEFAULT, "{} (id {}) to {} (id {})", a->GetCachedName(), ida, b->GetCachedName(), idb);
	KmPrintTrip(a->xy, b->xy, speed);
	return true;
}

/** 'km_vehicle <vehicle id>': how far the vehicle is from the station it is heading to, and the time it needs at its top speed. */
static bool ConKmVehicle(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Distance of a vehicle to the station it is heading to, in kilometres, and the time it needs at its maximum speed. Usage: 'km_vehicle <vehicle id>' (the id is in the title of the vehicle window).");
		return true;
	}
	int64_t id = 0;
	if (argv.size() != 2 || !KmParseNumber(argv[1], id) || id < 0 || id > 0xFFFFFF) {
		IConsolePrint(CC_ERROR, "Usage: km_vehicle <vehicle id>");
		return true;
	}
	const Vehicle *v = Vehicle::GetIfValid(VehicleID{static_cast<uint32_t>(id)});
	if (v == nullptr || !v->IsPrimaryVehicle()) {
		IConsolePrint(CC_ERROR, "'{}' is not the id of a vehicle (the front engine of a train, a road vehicle, a ship or an aircraft).", argv[1]);
		return true;
	}
	const int64_t speed = v->GetDisplayMaxSpeed();
	IConsolePrint(CC_DEFAULT, "vehicle {}: maximum speed {} km/h{}", id, speed, v->type == VEH_AIRCRAFT ? " (aircraft: the times below are NOT calibrated)" : "");
	if (!v->current_order.IsType(OT_GOTO_STATION)) {
		IConsolePrint(CC_DEFAULT, "it is not heading to a station now (no station order is active).");
		return true;
	}
	const Station *dest = Station::GetIfValid(v->current_order.GetDestination().ToStationID());
	if (dest == nullptr) {
		IConsolePrint(CC_DEFAULT, "its destination station does not exist any more.");
		return true;
	}
	IConsolePrint(CC_DEFAULT, "heading to {}", dest->GetCachedName());
	KmPrintTrip(v->tile, dest->xy, speed);
	return true;
}

void RegisterKmConsoleCommands()
{
	IConsole::CmdRegister("km", ConKm);
	IConsole::CmdRegister("km_route", ConKmRoute);
	IConsole::CmdRegister("km_vehicle", ConKmVehicle);
}

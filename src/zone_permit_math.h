/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file zone_permit_math.h Building permits for foreign money zones: pure integer maths (no game types), the same on every client. See zone_permit.h. */

#ifndef ZONE_PERMIT_MATH_H
#define ZONE_PERMIT_MATH_H

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace permitmath {

constexpr int64_t VALID_DAYS = 180;          ///< A permit lasts this many GAME days (a game day is 24 real minutes, so 72 real hours).
constexpr int64_t MAX_BANKED_DAYS = 360;     ///< Renewing early adds time, but a permit never runs longer than this from today.
constexpr int64_t FEE_BASE = 50000;          ///< One-time fee for a permit, in money units, in a zone whose tax level is 100%.
constexpr int64_t SURCHARGE_PERMIT_PCT = 50; ///< Extra cost of construction in a foreign zone WITH a permit, in percent of the cost, at tax level 100%.
constexpr int64_t SURCHARGE_NONE_PCT = 400;  ///< Extra cost WITHOUT a permit (the build still works, but it costs 5 times as much).
constexpr int64_t SURCHARGE_PERMIT_MIN_PCT = 10; ///< A zone with a very low tax level still charges at least this much with a permit.

/** The tax level of a zone is a percent (100 = the base) and is kept inside 0 .. 300. */
constexpr int64_t ClampTax(int64_t tax_percent) { return std::clamp<int64_t>(tax_percent, 0, 300); }

/** The one-time fee of a permit for a zone with this tax level. */
constexpr int64_t Fee(int64_t tax_percent) { return FEE_BASE * ClampTax(tax_percent) / 100; }

/** Does a permit that ends on game day `expires_day` still hold on `today`? 0 = never had one. */
constexpr bool IsValid(int64_t expires_day, int64_t today) { return expires_day > today; }

/** New end day after buying: the time is added to what is left, but never more than MAX_BANKED_DAYS from today. */
constexpr int64_t Renew(int64_t expires_day, int64_t today)
{
	const int64_t start = IsValid(expires_day, today) ? expires_day : today;
	return std::min<int64_t>(start + VALID_DAYS, today + MAX_BANKED_DAYS);
}

/** Extra cost in percent of the cost of the command. */
constexpr int64_t SurchargePercent(bool has_permit, int64_t tax_percent)
{
	if (!has_permit) return SURCHARGE_NONE_PCT;
	return std::max<int64_t>(SURCHARGE_PERMIT_MIN_PCT, SURCHARGE_PERMIT_PCT * ClampTax(tax_percent) / 100);
}

/** Extra cost of one construction command (cost <= 0 gives 0: removing and refunds are never charged). */
constexpr int64_t Surcharge(int64_t cost, bool has_permit, int64_t tax_percent)
{
	return cost <= 0 ? 0 : cost * SurchargePercent(has_permit, tax_percent) / 100;
}

/** Days left on a permit (0 = none). */
constexpr int64_t DaysLeft(int64_t expires_day, int64_t today) { return IsValid(expires_day, today) ? expires_day - today : 0; }

/**
 * Whether a command is a construction on the map that the permit rule charges, from its name (the trait name of the command, for example "CmdBuildRailroadTrack").
 * Vehicles are not charged (their tile is the depot, and the depot was already charged when it was built).
 */
inline bool IsConstructionName(std::string_view name)
{
	if (name == "CmdLandscapeClear" || name == "CmdTerraformLand" || name == "CmdLevelLand" || name == "CmdPlantTree") return true;
	if (name.substr(0, 8) != "CmdBuild") return false;
	for (std::string_view bad : {"Vehicle", "Ship", "Aircraft", "Train", "Engine", "Wagon"}) {
		if (name.find(bad) != std::string_view::npos) return false;
	}
	return true;
}

} // namespace permitmath

#endif /* ZONE_PERMIT_MATH_H */

/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file zone_permit.h Building permits for foreign money zones. A company builds freely in the zone of its headquarters. In another zone every construction
 * costs more: with a permit (bought with CMD_ZONE_PERMIT, one-time fee, valid for permitmath::VALID_DAYS game days) a surcharge of 50% (scaled by the tax level of the
 * zone), without a permit 400%. The surcharge is added to the cost of the command in command.cpp (hook ZONE_PERMIT_PATCH). See zone_permit_math.h. */

#ifndef ZONE_PERMIT_H
#define ZONE_PERMIT_H

#include <array>
#include <cstdint>
#include "command_type.h"
#include "company_type.h"
#include "economy_type.h"
#include "tile_type.h"
#include "zone_market_math.h"

/** The permits of all companies. Saved as SRK_ZONE_PERMIT. */
struct ZonePermits {
	std::array<std::array<int64_t, zonemath::MAX_ZONES>, MAX_COMPANIES> expires{}; ///< Game day on which the permit of a company for a zone ends (0 = none).
	std::array<int64_t, zonemath::MAX_ZONES> fees{};       ///< Permit fees paid to the zone, in money units, all companies together.
	std::array<int64_t, zonemath::MAX_ZONES> surcharges{}; ///< Surcharges on constructions inside the zone, in money units, all companies together.

	void Reset();
	int64_t Today() const;                                     ///< The game day (the same clock as the zone rates).
	int64_t DaysLeft(CompanyID company, int zone) const;       ///< Days left on the permit, 0 = none.
	bool Has(CompanyID company, int zone) const { return this->DaysLeft(company, zone) > 0; }
};

extern ZonePermits _zone_permits;

CommandCost CmdZonePermit(DoCommandFlags flags, uint8_t zone);

DEF_CMD_TRAIT(CMD_ZONE_PERMIT, CmdZonePermit, {}, CommandType::MoneyManagement)

/**
 * Add the surcharge of a foreign zone to the cost of a command that has been executed (called by command.cpp, patched by apply_overlay.py, right before the money is
 * taken from the company). Returns the cost unchanged when the command is not a construction, costs nothing, or stands in the zone of the headquarters.
 * @param cmd The command.
 * @param tile Tile of the command (0 = none).
 * @param cost The cost of the command.
 */
CommandCost ZonePermitAdjustCost(Commands cmd, TileIndex tile, const CommandCost &cost);

#endif /* ZONE_PERMIT_H */

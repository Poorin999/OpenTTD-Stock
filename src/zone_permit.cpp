/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file zone_permit.cpp Building permits for foreign money zones. Integer maths only, the same on every client. */

#include "stdafx.h"
#include "zone_permit.h"
#include "zone_permit_math.h"
#include "zone_market.h"
#include "hq_gate.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "game_time/game_time_manager.h"

#include "table/strings.h"

#include "safeguards.h"

ZonePermits _zone_permits;

void ZonePermits::Reset()
{
	for (auto &row : this->expires) row.fill(0);
	this->fees.fill(0);
	this->surcharges.fill(0);
}

int64_t ZonePermits::Today() const
{
	return static_cast<int64_t>(GameTimeManager::Get().GetSnapshot().total_minutes / GameTimeManager::MINUTES_PER_DAY);
}

int64_t ZonePermits::DaysLeft(CompanyID company, int zone) const
{
	if (company.base() >= MAX_COMPANIES || zone < 0 || zone >= zonemath::MAX_ZONES) return 0;
	return permitmath::DaysLeft(this->expires[company.base()][zone], this->Today());
}

/**
 * Buy (or renew) the permit for building in a zone other than the one of the headquarters.
 * @param flags Type of operation.
 * @param zone Number of the zone, 0 based.
 * @return The one-time fee, or an error.
 */
CommandCost CmdZonePermit(DoCommandFlags flags, uint8_t zone)
{
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;
	if (_zone_market.count <= 0 || zone >= _zone_market.count) return CommandCost(STR_ZONE_PERMIT_ERROR_BAD_ZONE);
	if (CompanyNeedsHq(_current_company)) return CommandCost(STR_ZONE_PERMIT_ERROR_NO_HQ);
	if (_zone_market.ZoneOfCompany(_current_company) == zone) return CommandCost(STR_ZONE_PERMIT_ERROR_HOME);

	const int64_t fee = permitmath::Fee(_zone_market.zones[zone].tax_percent);
	if (flags.Test(DoCommandFlag::Execute)) {
		int64_t &expires = _zone_permits.expires[_current_company.base()][zone];
		expires = permitmath::Renew(expires, _zone_permits.Today());
		_zone_permits.fees[zone] += fee;
	}
	return CommandCost(EXPENSES_OTHER, Money(fee));
}

CommandCost ZonePermitAdjustCost(Commands cmd, TileIndex tile, const CommandCost &cost)
{
	if (tile == 0 || _zone_market.count <= 0 || !Company::IsValidID(_current_company)) return cost;
	const int64_t base = static_cast<int64_t>(cost.GetCost());
	if (base <= 0 || CompanyNeedsHq(_current_company)) return cost;
	if (!permitmath::IsConstructionName(GetCommandName(cmd))) return cost;

	const int here = _zone_market.ZoneOfTile(tile);
	const int home = _zone_market.ZoneOfCompany(_current_company);
	if (here < 0 || home < 0 || here == home) return cost;

	const int64_t extra = permitmath::Surcharge(base, _zone_permits.Has(_current_company, here), _zone_market.zones[here].tax_percent);
	if (extra <= 0) return cost;
	_zone_permits.surcharges[here] += extra;
	CommandCost out = cost;
	out.AddCost(Money(extra));
	return out;
}

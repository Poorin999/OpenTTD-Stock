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
#include "central_bank.h"
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
	if (GetUnborrowedLiquidCash(_current_company) < fee) return CommandCost(STR_ZONE_EXCHANGE_ERROR_NO_CASH); // Nothing is bought with borrowed money.
	if (flags.Test(DoCommandFlag::Execute)) {
		int64_t &expires = _zone_permits.expires[_current_company.base()][zone];
		expires = permitmath::Renew(expires, _zone_permits.Today());
		_zone_permits.fees[zone] += fee;
		/* The fee is taken HERE, not through the cost that the command returns (that way it was not taken in the game). */
		SubtractMoneyFromCompany(CommandCost(EXPENSES_OTHER, Money(fee)));
		RecordLedger(_current_company, LedgerItem::ZonePermit, -fee); // The account statement.
		BankOfZone(zone).trading_revenue_cents += fee * 100; // The fee is income of the bank of that zone, like the other fees.
	}
	return CommandCost(EXPENSES_OTHER);
}

/**
 * What the zone adds to (or takes off) the cost of a command: (1) the PRICE LEVEL of the zone of the tile (50% .. 200% of the average of the zones, from the inflation
 * of that zone) for every construction and every purchase of a vehicle, for every company; (2) the permit surcharge for a construction in a foreign zone (a percentage of
 * the cost that already follows the price level). Pure function of the state of the game, the same on every client. `count` = add the surcharge to the statistics.
 */
static int64_t ZoneExtraCost(Commands cmd, TileIndex tile, int64_t base, bool count)
{
	if (tile == 0 || base <= 0 || _zone_market.count <= 0 || !Company::IsValidID(_current_company)) return 0;
	const std::string_view name = GetCommandName(cmd);
	const bool construction = permitmath::IsConstructionName(name);
	const bool purchase = permitmath::IsVehiclePurchaseName(name);
	if (!construction && !purchase) return 0;

	const int here = _zone_market.ZoneOfTile(tile);
	if (here < 0) return 0;
	int64_t total = base;
	if (ZonePriceEnabled()) total = ZonePriceScale(base, tile);
	if (construction && !CompanyNeedsHq(_current_company)) {
		const int home = _zone_market.ZoneOfCompany(_current_company);
		if (home >= 0 && home != here) {
			const int64_t extra = permitmath::Surcharge(total, _zone_permits.Has(_current_company, here), _zone_market.zones[here].tax_percent);
			if (extra > 0 && count) _zone_permits.surcharges[here] += extra;
			total += extra;
		}
	}
	return total - base;
}

CommandCost ZonePermitAdjustCost(Commands cmd, TileIndex tile, const CommandCost &cost)
{
	const int64_t extra = ZoneExtraCost(cmd, tile, static_cast<int64_t>(cost.GetCost()), true);
	if (extra == 0) return cost;
	CommandCost out = cost;
	out.AddCost(Money(extra));
	return out;
}

void ZonePermitCheckTest(Commands cmd, TileIndex tile, CommandCost &res)
{
	if (res.Failed() || tile == 0 || _zone_market.count <= 0 || !Company::IsValidID(_current_company)) return;

	if (permitmath::IsConstructionName(GetCommandName(cmd))) {
		const int here = _zone_market.ZoneOfTile(tile);
		const int home = _zone_market.ZoneOfCompany(_current_company);
		if (here >= 0 && home >= 0) {
			const bool human = !Company::Get(_current_company)->is_ai;
			if (permitmath::BuildBlocked(human, !CompanyNeedsHq(_current_company), true, here != home, _zone_permits.Has(_current_company, here))) {
				res = CommandCost(STR_ZONE_PERMIT_ERROR_NEED);
				return;
			}
		}
	}
	/* The test run of the local client also knows the price level and the surcharge, so the check "enough cash" is made with the price that will be paid. */
	const int64_t extra = ZoneExtraCost(cmd, tile, static_cast<int64_t>(res.GetCost()), false);
	if (extra != 0) res.AddCost(Money(extra));
}

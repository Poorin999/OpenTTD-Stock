/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file company_role.cpp Roles of companies (step 2). Integer maths only, the same on every client. The rules and the table are in company_role_math.h. */

#include "stdafx.h"
#include "company_role.h"
#include "company_role_math.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "news_func.h"
#include "strings_func.h"
#include "vehicle_base.h"
#include "window_func.h"
#include "core/format.hpp"
#include "game_time/game_time_manager.h"
#include <array>

#include "table/strings.h"

#include "safeguards.h"

crole::Table _company_roles;

/** The kind of a vehicle type for the rules. False for the types that no company owns (effects, disasters). */
static bool KindOfType(VehicleType type, crole::VehicleKind &kind)
{
	switch (type) {
		case VEH_TRAIN:    kind = crole::VehicleKind::Train; return true;
		case VEH_ROAD:     kind = crole::VehicleKind::Road; return true;
		case VEH_SHIP:     kind = crole::VehicleKind::Ship; return true;
		case VEH_AIRCRAFT: kind = crole::VehicleKind::Aircraft; return true;
		default: return false;
	}
}

static int64_t Today()
{
	return static_cast<int64_t>(GameTimeManager::Get().GetSnapshot().total_minutes / GameTimeManager::MINUTES_PER_DAY);
}

int32_t CompanyRoleYear(CompanyID company)
{
	const Company *c = Company::GetIfValid(company);
	return c == nullptr ? 0 : static_cast<int32_t>(c->inaugurated_year.base());
}

crole::Role GetCompanyRole(CompanyID company)
{
	if (!Company::IsValidID(company) || company.base() >= crole::MAX_COMPANIES_N) return crole::Role::TransportOwner;
	return _company_roles.RoleOf(company.base(), CompanyRoleYear(company));
}

bool IsCompanyDealer(CompanyID company)
{
	if (!Company::IsValidID(company) || company.base() >= crole::MAX_COMPANIES_N) return false;
	return _company_roles.DealerOf(company.base(), CompanyRoleYear(company));
}

bool CompanyRoleBlocksBuild(CompanyID company, VehicleType type)
{
	crole::VehicleKind kind;
	if (!KindOfType(type, kind)) return false;
	return !crole::VehicleAllowed(GetCompanyRole(company), kind);
}

bool CompanyRoleBlocksStart(const Vehicle *v)
{
	if (v == nullptr) return false;
	crole::VehicleKind kind;
	if (!KindOfType(v->type, kind)) return false;
	return crole::StartBlocked(GetCompanyRole(v->owner), kind, v->vehstatus.Test(VehState::Stopped));
}

int CountVehiclesForbiddenFor(CompanyID company, crole::Role role)
{
	int n = 0;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->owner != company || !v->IsPrimaryVehicle()) continue; // A train is counted once (wagons are not primary vehicles).
		crole::VehicleKind kind;
		if (!KindOfType(v->type, kind)) continue;
		if (!crole::VehicleAllowed(role, kind)) n++;
	}
	return n;
}

/**
 * Change the role of the current company.
 * @param flags Type of operation.
 * @param role The new role (0 = transport owner, 1 = industry owner).
 * @return No cost, or an error.
 */
CommandCost CmdSetCompanyRole(DoCommandFlags flags, uint8_t role)
{
	if (!Company::IsValidID(_current_company) || _current_company.base() >= crole::MAX_COMPANIES_N) return CMD_ERROR;
	if (!crole::IsValidRole(role)) return CMD_ERROR;

	const uint8_t company = _current_company.base();
	const int32_t year = CompanyRoleYear(_current_company);
	const int forbidden = CountVehiclesForbiddenFor(_current_company, static_cast<crole::Role>(role));

	switch (_company_roles.CanSetRole(company, year, role, forbidden)) {
		case crole::SetRoleResult::Ok: break;
		case crole::SetRoleResult::BadRole: return CMD_ERROR;
		case crole::SetRoleResult::Unchanged: return CommandCost(STR_COMPANY_ROLE_ERROR_SAME);
		case crole::SetRoleResult::HasForbiddenVehicles: return CommandCost(STR_COMPANY_ROLE_ERROR_HAS_VEHICLES);
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		_company_roles.SetRole(company, year, role, forbidden, Today());
		InvalidateWindowClassesData(WC_CENTRAL_BANK, 0); // The company window (and the others of the mod) read the role.
	}
	return CommandCost(EXPENSES_OTHER);
}

/**
 * Switch Dealer mode of the current company.
 * @param flags Type of operation.
 * @param on True to switch it on.
 * @return No cost, or an error.
 */
CommandCost CmdSetDealerMode(DoCommandFlags flags, bool on)
{
	if (!Company::IsValidID(_current_company) || _current_company.base() >= crole::MAX_COMPANIES_N) return CMD_ERROR;

	const uint8_t company = _current_company.base();
	const int32_t year = CompanyRoleYear(_current_company);

	switch (_company_roles.CanSetDealer(company, year, on)) {
		case crole::SetDealerResult::Ok: break;
		case crole::SetDealerResult::Unchanged: return CommandCost(STR_COMPANY_ROLE_ERROR_SAME_DEALER);
		case crole::SetDealerResult::NotTransportOwner: return CommandCost(STR_COMPANY_ROLE_ERROR_DEALER_ROLE);
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		_company_roles.SetDealer(company, year, on, Today());
		InvalidateWindowClassesData(WC_CENTRAL_BANK, 0);
	}
	return CommandCost(EXPENSES_OTHER);
}

int CompanyRoleSweep()
{
	if (_company_roles.rows.empty()) return 0;
	return _company_roles.Sweep([](uint8_t company, const crole::Row &row) {
		/* The company: still there, and the same one that the row was made for (the game re-uses a company slot). */
		if (company >= MAX_COMPANIES || !Company::IsValidID(CompanyID{company})) return crole::Verdict::CompanyGone;
		if (CompanyRoleYear(CompanyID{company}) != row.owner_year) return crole::Verdict::CompanyGone;
		return crole::Verdict::Keep;
	});
}

/** What the net told each company last time (not saved: after a load the owner hears it once more, which is fine). */
static std::array<int, MAX_COMPANIES> _net_last_reported{};

int CompanyRoleDailyNet()
{
	if (_company_roles.rows.empty()) {
		_net_last_reported.fill(0);
		return 0;
	}

	std::array<int, MAX_COMPANIES> stopped{};  // Stopped in a depot (now or before).
	std::array<int, MAX_COMPANIES> outside{};  // Forbidden and not in a depot: only reported.
	int stopped_now = 0;

	for (Vehicle *v : Vehicle::Iterate()) {
		if (!v->IsPrimaryVehicle() || v->owner.base() >= crole::MAX_COMPANIES_N) continue;
		const crole::Role role = GetCompanyRole(v->owner);
		if (role == crole::Role::TransportOwner) continue; // Nothing is forbidden for it.
		crole::VehicleKind kind;
		if (!KindOfType(v->type, kind)) continue;

		const size_t c = v->owner.base();
		switch (crole::NetVerdict(role, kind, v->IsChainInDepot(), v->vehstatus.Test(VehState::Stopped))) {
			case crole::NetAction::Allowed: break;
			case crole::NetAction::AlreadyStopped: stopped[c]++; break;
			case crole::NetAction::StopInDepot:
				/* Same on every client: the vehicle stands in a depot and is only marked stopped. */
				v->vehstatus.Set(VehState::Stopped);
				stopped[c]++;
				stopped_now++;
				break;
			case crole::NetAction::NotifyOnly: outside[c]++; break;
		}
	}

	for (const Company *company : Company::Iterate()) {
		const size_t c = company->index.base();
		if (c >= MAX_COMPANIES) continue;
		const int total = stopped[c] + outside[c];
		if (total == _net_last_reported[c]) continue;
		_net_last_reported[c] = total;
		if (total == 0) continue;
		AddNewsItem(GetEncodedString(STR_JUST_RAW_STRING, fmt::format("{} is an industry owner and may only run road vehicles. {} train(s), ship(s) or aircraft are not allowed to run ({} stopped in depots, {} outside a depot). Sell them.",
			GetString(STR_COMPANY_NAME, company->index), total, stopped[c], outside[c])), NewsType::General, NewsStyle::Small, {});
	}

	if (stopped_now > 0) InvalidateWindowClassesData(WC_CENTRAL_BANK, 0);
	return stopped_now;
}

static const GameTimeSubscription _company_role_hourly(GameTimeEvent::Hour, [](const GameTimeSnapshot &) { CompanyRoleSweep(); }, 23);
static const GameTimeSubscription _company_role_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &) { CompanyRoleDailyNet(); }, 23);

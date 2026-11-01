/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file company_role_math.h Roles of companies (step 2 of the contract system): integer maths only and no game headers, so that company_role_test.cpp can test it alone.
 *
 * Two roles only:
 *   TransportOwner  plays as the game gives (every vehicle type) and may switch on DEALER MODE (a mode, not a role: the contract rules of the later steps).
 *   IndustryOwner   builds industries, warehouses and ROAD VEHICLES only. No trains, ships or aircraft.
 * Every company is a transport owner with Dealer mode off until it chooses otherwise (so an old savegame, and every AI company, keeps playing as before).
 *
 * OpenTTD cannot forbid a vehicle type per company, so the rule is kept in three places (company_role.cpp, apply_overlay.py patch_company_role):
 *   1. the BUILD command (CmdBuildVehicle) refuses a forbidden type;
 *   2. the START command refuses to start a forbidden vehicle that stands stopped;
 *   3. a daily NET looks at every vehicle: a forbidden one that stands in a depot is stopped, one that is outside is only reported.
 * Nothing of the player's is ever deleted or sold.
 *
 * A row also keeps the economy year in which its company was founded, so a new company that gets the slot of a bankrupt one does not inherit its role
 * (the same idea as industry_owner_math.h).
 */

#ifndef COMPANY_ROLE_MATH_H
#define COMPANY_ROLE_MATH_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace crole {

static constexpr uint8_t MAX_COMPANIES_N = 15; ///< Companies are 0 .. 14 (MAX_COMPANIES of OpenTTD).

/**
 * Whether a company must have no forbidden vehicle before it may change to the role that forbids them.
 * true: the change is refused ("sell the trains first"). false: the change is allowed and the daily net stops the vehicles.
 * A default of the designer (the spec does not say): refusing is clearer for the player than a fleet that is frozen in the depots.
 */
static constexpr bool REQUIRE_NO_FORBIDDEN_TO_SWITCH = true;

enum class Role : uint8_t {
	TransportOwner = 0, ///< All vehicle types. May switch Dealer mode on.
	IndustryOwner = 1,  ///< Road vehicles only.
};
static constexpr uint8_t ROLE_COUNT = 2;

/** The four kinds of vehicle that a company can own (the game's VehicleType without the effects and disasters). */
enum class VehicleKind : uint8_t { Train, Road, Ship, Aircraft };

/** May a company with this role own and build a vehicle of this kind? */
inline bool VehicleAllowed(Role role, VehicleKind kind)
{
	return role == Role::TransportOwner || kind == VehicleKind::Road;
}

/** True for a raw number that is a role. */
inline bool IsValidRole(uint8_t raw) { return raw < ROLE_COUNT; }

/** The START command: a forbidden vehicle that stands stopped may not be started. Stopping is always allowed (it can only help). */
inline bool StartBlocked(Role role, VehicleKind kind, bool vehicle_is_stopped)
{
	return vehicle_is_stopped && !VehicleAllowed(role, kind);
}

/** What the daily net does with one vehicle. */
enum class NetAction : uint8_t {
	Allowed,        ///< The vehicle may be owned by this role: nothing to do.
	AlreadyStopped, ///< Forbidden, but it stands stopped in a depot already.
	StopInDepot,    ///< Forbidden and in a depot but not stopped: stop it, so that it never starts.
	NotifyOnly,     ///< Forbidden and not in a depot: it is not touched (a stopped train would block the track); the owner is told.
};

inline NetAction NetVerdict(Role role, VehicleKind kind, bool in_depot, bool stopped)
{
	if (VehicleAllowed(role, kind)) return NetAction::Allowed;
	if (in_depot) return stopped ? NetAction::AlreadyStopped : NetAction::StopInDepot;
	return NetAction::NotifyOnly;
}

/** What happens when a company tries to change its role. */
enum class SetRoleResult : uint8_t {
	Ok,
	BadRole,              ///< The number is not a role.
	Unchanged,            ///< The company has this role already.
	HasForbiddenVehicles, ///< The new role forbids vehicles that the company owns (REQUIRE_NO_FORBIDDEN_TO_SWITCH).
};

/** What happens when a company tries to switch Dealer mode. */
enum class SetDealerResult : uint8_t {
	Ok,
	Unchanged,         ///< Dealer mode has that state already.
	NotTransportOwner, ///< An industry owner is not a dealer.
};

/** One company that is not the default (transport owner, Dealer mode off). */
struct Row {
	Role role = Role::TransportOwner;
	bool dealer = false;
	int64_t since_day = 0;  ///< Game day of the last change.
	int32_t owner_year = 0; ///< Economy year in which the company was founded.
	bool IsDefault() const { return this->role == Role::TransportOwner && !this->dealer; }
};

/** Why a row is dropped by a sweep. */
enum class Verdict : uint8_t { Keep, CompanyGone };

/** The saved form of one row (the game puts it in a StockRecord with kind SRK_COMPANY_ROLE). */
struct Record {
	uint8_t a = 0;   ///< Company number.
	uint8_t b = 0;   ///< Role.
	int64_t v1 = 0;  ///< Dealer mode (0 or 1).
	int64_t v2 = 0;  ///< Game day of the last change.
	int64_t v5 = 0;  ///< Founding year of the company.
};

/** The roles of the companies. Rows are kept in a std::map: the order of a walk is the same on every client. */
struct Table {
	std::map<uint8_t, Row> rows; ///< Key = company number.

	void Reset() { this->rows.clear(); }

	/** The row of this company, but only when it was made for the company that stands in the slot now. */
	const Row *Find(uint8_t company, int32_t owner_year) const
	{
		const auto it = this->rows.find(company);
		if (it == this->rows.end() || it->second.owner_year != owner_year) return nullptr;
		return &it->second;
	}

	Role RoleOf(uint8_t company, int32_t owner_year) const
	{
		const Row *r = this->Find(company, owner_year);
		return r == nullptr ? Role::TransportOwner : r->role;
	}

	/** Dealer mode: only a transport owner can be a dealer (a row that says otherwise is not believed). */
	bool DealerOf(uint8_t company, int32_t owner_year) const
	{
		const Row *r = this->Find(company, owner_year);
		return r != nullptr && r->role == Role::TransportOwner && r->dealer;
	}

	/** Would the change work? Pure: changes nothing. `forbidden_vehicles` = how many vehicles the company owns that the NEW role forbids. */
	SetRoleResult CanSetRole(uint8_t company, int32_t owner_year, uint8_t new_role_raw, int forbidden_vehicles) const
	{
		if (!IsValidRole(new_role_raw)) return SetRoleResult::BadRole;
		const Role now = this->RoleOf(company, owner_year);
		const Role next = static_cast<Role>(new_role_raw);
		if (now == next) return SetRoleResult::Unchanged;
		if (REQUIRE_NO_FORBIDDEN_TO_SWITCH && forbidden_vehicles > 0) return SetRoleResult::HasForbiddenVehicles;
		return SetRoleResult::Ok;
	}

	/** Do the change when it is allowed (same checks as CanSetRole). A change to industry owner switches Dealer mode off. A stale row is replaced. */
	SetRoleResult SetRole(uint8_t company, int32_t owner_year, uint8_t new_role_raw, int forbidden_vehicles, int64_t today)
	{
		const SetRoleResult r = this->CanSetRole(company, owner_year, new_role_raw, forbidden_vehicles);
		if (r != SetRoleResult::Ok) return r;
		Row row;
		row.role = static_cast<Role>(new_role_raw);
		row.dealer = false;
		row.since_day = today;
		row.owner_year = owner_year;
		if (row.IsDefault()) {
			this->rows.erase(company); // Back to the default: no row is kept.
		} else {
			this->rows[company] = row;
		}
		return SetRoleResult::Ok;
	}

	SetDealerResult CanSetDealer(uint8_t company, int32_t owner_year, bool on) const
	{
		if (this->RoleOf(company, owner_year) != Role::TransportOwner) return SetDealerResult::NotTransportOwner;
		if (this->DealerOf(company, owner_year) == on) return SetDealerResult::Unchanged;
		return SetDealerResult::Ok;
	}

	SetDealerResult SetDealer(uint8_t company, int32_t owner_year, bool on, int64_t today)
	{
		const SetDealerResult r = this->CanSetDealer(company, owner_year, on);
		if (r != SetDealerResult::Ok) return r;
		Row row;
		row.role = Role::TransportOwner;
		row.dealer = on;
		row.since_day = today;
		row.owner_year = owner_year;
		if (row.IsDefault()) {
			this->rows.erase(company);
		} else {
			this->rows[company] = row;
		}
		return SetDealerResult::Ok;
	}

	/** Forget the row of a company that is gone. Returns how many rows were dropped (0 or 1). */
	int ReleaseCompany(uint8_t company) { return static_cast<int>(this->rows.erase(company)); }

	/** Drop the rows of companies that are gone. `check(company, row)` returns a Verdict. Returns how many were dropped. */
	template <typename F>
	int Sweep(F &&check)
	{
		int n = 0;
		for (auto it = this->rows.begin(); it != this->rows.end();) {
			if (check(it->first, it->second) == Verdict::Keep) {
				++it;
				continue;
			}
			it = this->rows.erase(it);
			n++;
		}
		return n;
	}

	/** The rows as records for the savegame, in the order of the company number. Only companies that are not the default have a row. */
	std::vector<Record> ToRecords() const
	{
		std::vector<Record> out;
		for (const auto &kv : this->rows) {
			const Row &r = kv.second;
			Record rec;
			rec.a = kv.first;
			rec.b = static_cast<uint8_t>(r.role);
			rec.v1 = r.dealer ? 1 : 0;
			rec.v2 = r.since_day;
			rec.v5 = r.owner_year;
			out.push_back(rec);
		}
		return out;
	}

	/** Read one record of a savegame. A record that makes no sense is ignored (returns false). Dealer mode of an industry owner is dropped. */
	bool FromRecord(const Record &rec)
	{
		if (rec.a >= MAX_COMPANIES_N || !IsValidRole(rec.b)) return false;
		Row r;
		r.role = static_cast<Role>(rec.b);
		r.dealer = rec.v1 != 0 && r.role == Role::TransportOwner;
		r.since_day = std::max<int64_t>(rec.v2, 0);
		r.owner_year = static_cast<int32_t>(rec.v5);
		if (r.IsDefault()) return false; // Nothing to keep: the default needs no row.
		this->rows[rec.a] = r;
		return true;
	}
};

} // namespace crole

#endif /* COMPANY_ROLE_MATH_H */

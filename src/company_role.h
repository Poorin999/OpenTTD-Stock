/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file company_role.h Roles of companies (step 2 of the contract system). A company is a TRANSPORT OWNER (default; every vehicle type; may switch Dealer mode on) or an
 * INDUSTRY OWNER (road vehicles only). The rules and the table are in company_role_math.h. The table is saved as SRK_COMPANY_ROLE.
 * The build command and the start command of OpenTTD ask this file (apply_overlay.py, patch_company_role); a daily net stops what slipped through. */

#ifndef COMPANY_ROLE_H
#define COMPANY_ROLE_H

#include <cstdint>
#include "command_type.h"
#include "company_type.h"
#include "vehicle_type.h"
#include "company_role_math.h"

struct Vehicle;

/** The roles of all companies. Saved as SRK_COMPANY_ROLE; reset by StockMarket::Reset (so an old savegame has no rows: every company is a transport owner). */
extern crole::Table _company_roles;

/** Change the role of the current company. Refused when the new role forbids vehicles that the company owns. Same result on every client. */
CommandCost CmdSetCompanyRole(DoCommandFlags flags, uint8_t role);

/** Switch Dealer mode of the current company. Only a transport owner can. Same result on every client. */
CommandCost CmdSetDealerMode(DoCommandFlags flags, bool on);

DEF_CMD_TRAIT(CMD_SET_COMPANY_ROLE, CmdSetCompanyRole, {}, CommandType::CompanySetting)
DEF_CMD_TRAIT(CMD_SET_DEALER_MODE, CmdSetDealerMode, {}, CommandType::CompanySetting)

/** The role of a company (a transport owner when the company is not in the table or does not exist). */
crole::Role GetCompanyRole(CompanyID company);

/** True when the company is a transport owner with Dealer mode on. */
bool IsCompanyDealer(CompanyID company);

/** True when the role of this company forbids building a vehicle of this type (train, ship, aircraft for an industry owner). Called by CmdBuildVehicle (patch_company_role). */
bool CompanyRoleBlocksBuild(CompanyID company, VehicleType type);

/** True when this vehicle is forbidden for its owner and stands stopped, so that starting it is refused. Called by CmdStartStopVehicle (patch_company_role). */
bool CompanyRoleBlocksStart(const Vehicle *v);

/** How many vehicles the company owns that this role forbids. */
int CountVehiclesForbiddenFor(CompanyID company, crole::Role role);

/** Drop the rows of companies that are gone (bankrupt, bought, or a new company in the slot). Runs every game hour. Returns the number of rows dropped. */
int CompanyRoleSweep();

/** The daily net: a forbidden vehicle in a depot is stopped, one outside is reported (once per change). Runs every game day. Returns the number of vehicles stopped. */
int CompanyRoleDailyNet();

/** The economy year in which the company was founded (the identity that a row keeps). */
int32_t CompanyRoleYear(CompanyID company);

#endif /* COMPANY_ROLE_H */

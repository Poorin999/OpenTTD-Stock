/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file industry_owner.h Owners of industries (step 1 of the contract system). OpenTTD has no owner of an industry, so the mod keeps a table (industry_owner_math.h).
 * An industry that is not in the table belongs to the SYSTEM (NPC). A company buys the right of ownership with CMD_BUY_INDUSTRY_OWNERSHIP, for the output of a month times the
 * market price times 12, paid from UNBORROWED cash to the central bank of the zone of the industry. The table is saved as SRK_INDUSTRY_OWNER. */

#ifndef INDUSTRY_OWNER_H
#define INDUSTRY_OWNER_H

#include <cstdint>
#include "command_type.h"
#include "company_type.h"
#include "industry_type.h"
#include "cargo_type.h"
#include "industry_owner_math.h"

struct Industry;
struct Window;

/** The owners of all industries. Saved as SRK_INDUSTRY_OWNER; reset by StockMarket::Reset (so an old savegame without the record has no owners: all industries belong to the system). */
extern indowner::Table _industry_owners;

/** Buy the right of ownership of an industry that belongs to the system. Paid from unborrowed cash. Same result on every client. */
CommandCost CmdBuyIndustryOwnership(DoCommandFlags flags, IndustryID industry);

DEF_CMD_TRAIT(CMD_BUY_INDUSTRY_OWNERSHIP, CmdBuyIndustryOwnership, {}, CommandType::MoneyManagement)

/** The company that owns the industry, or CompanyID::Invalid() when it belongs to the system (NPC) or does not exist. */
CompanyID GetIndustryOwner(IndustryID industry);

/** True when this company owns the industry. */
bool IsIndustryOwnedBy(IndustryID industry, CompanyID company);

/** The upgrade level of the industry (0 for the system's industries and for every industry until step 12 upgrades it). */
int GetIndustryLevel(IndustryID industry);

/** What the right of ownership costs now, in money units: the output of one month x the market price of the cargo x 12, at least indowner::PRICE_FLOOR.
 * 0 when the industry produces nothing (a bank, a power station): it is not for sale. */
int64_t GetIndustryOwnershipPrice(const Industry *industry);

/** The market price of one unit of a cargo now, in 1/100 of a money unit (the unit of the contract price): the pay of the game for 100 units over 20 tiles, with the price and
 * the freight rate of the cargo prices window. 0 for a cargo that does not exist. Used by the automatic answer of the SYSTEM (step 4). */
int64_t GetCargoUnitPriceCents(CargoType cargo);

/** Drop the rows of industries that were closed and give the industries of companies that are gone (bankrupt, bought) back to the system. Runs every game hour. Returns the number of rows dropped. */
int IndustryOwnerSweep();

/** The identity of an industry for the table (type, tile, construction date). */
indowner::Identity IndustryIdentity(const Industry *industry);

/** Ask the player to confirm the purchase of the right of ownership of an industry (shows the owner and the price), then send CMD_BUY_INDUSTRY_OWNERSHIP.
 * @param parent The industry view window (its window number is the industry id; the answer comes back to it). */
void ShowIndustryBuyPrompt(Window *parent, IndustryID industry);

#endif /* INDUSTRY_OWNER_H */

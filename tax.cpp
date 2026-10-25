/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file tax.cpp The taxes of the companies, see tax.h. */

#include "stdafx.h"
#include "tax.h"
#include "openttd.h"
#include "game_time/game_time_manager.h"
#include "central_bank.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "core/backup_type.hpp"
#include "economy_func.h"
#include "map_func.h"
#include "tile_map.h"
#include "town.h"
#include "vehicle_base.h"

#include "safeguards.h"

/** Fixed amounts follow the price index of the game (inflation). */
static int64_t ScaleByPriceIndex(int64_t amount)
{
	return amount * GetPriceIndexPpm() / 1'000'000;
}

/**
 * Take a tax from the cash of a company: it is an expense of the company (the "Other" line of the finances window), a line of the
 * account statement and income of the own money of the bank. The cash may go below zero, like it does for any other expense.
 */
static void BookTax(CompanyID company, LedgerItem item, int64_t amount)
{
	if (amount <= 0 || !Company::IsValidID(company)) return;
	{
		AutoRestoreBackup cur_company(_current_company, company);
		SubtractMoneyFromCompany(CommandCost(EXPENSES_OTHER, Money(amount)));
	}
	RecordLedger(company, item, -amount);
	_central_bank.trading_revenue_cents += amount * 100;
}

void ChargeVat(CompanyID company, int64_t income)
{
	if (income <= 0) return;
	BookTax(company, LedgerItem::TaxVat, income * TAX_VAT_PERCENT / 100);
}

int64_t TakeInvestorTax(CompanyID company, LedgerItem item, int64_t profit)
{
	if (profit <= 0 || !Company::IsValidID(company)) return 0;
	int64_t percent = 0;
	LedgerItem tax_item = LedgerItem::End;
	switch (item) {
		case LedgerItem::RealizedProfit: percent = TAX_CAPITAL_GAINS_PERCENT; tax_item = LedgerItem::TaxCapitalGains; break;
		case LedgerItem::Dividends:      percent = TAX_DIVIDEND_PERCENT;      tax_item = LedgerItem::TaxDividend; break;
		default: return 0;
	}
	const int64_t tax = profit * percent / 100;
	if (tax <= 0) return 0;
	RecordLedger(company, tax_item, -tax);
	_central_bank.trading_revenue_cents += tax * 100;
	return tax;
}

/** Weight of a tile for the land tax, in 1/100 of a unit; 0 for a tile that the tax does not touch. A town of 10,000 people next to the tile adds +500%. */
static int64_t LandWeight(Tile tile)
{
	int64_t base;
	switch (GetTileType(tile)) {
		case MP_STATION: base = 4; break;
		case MP_OBJECT:  base = 3; break;
		case MP_CLEAR: case MP_RAILWAY: case MP_ROAD: case MP_TUNNELBRIDGE: case MP_WATER: base = 1; break;
		default: return 0; // houses and industries are owned by the town / the map, void, trees
	}
	int64_t percent = 100;
	const Town *t = ClosestTownFromTile(static_cast<TileIndex>(tile), 12);
	if (t != nullptr) percent += std::min<int64_t>(t->cache.population / 20, 500);
	return base * percent;
}

/** Land tax and transport tax, once per game day at 00:00. */
static const GameTimeSubscription _tax_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &)
{
	if (_game_mode != GM_NORMAL) return;

	/* Land: the weight of all tiles of every company in one pass over the map. */
	std::array<int64_t, MAX_COMPANIES> land{};
	for (const auto tile : Map::Iterate()) {
		switch (GetTileType(tile)) {
			case MP_CLEAR: case MP_RAILWAY: case MP_ROAD: case MP_TUNNELBRIDGE: case MP_WATER: case MP_STATION: case MP_OBJECT: break;
			default: continue;
		}
		const Owner owner = GetTileOwner(tile);
		if (!Company::IsValidID(owner)) continue;
		land[owner.base()] += LandWeight(tile);
	}

	/* Vehicles: every primary vehicle that is not stopped in a depot. */
	std::array<int64_t, MAX_COMPANIES> transport{};
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (!v->IsPrimaryVehicle() || v->IsStoppedInDepot() || !Company::IsValidID(v->owner)) continue;
		int64_t rate = 0;
		switch (v->type) {
			case VEH_TRAIN:    rate = TAX_TRAIN_DAY; break;
			case VEH_ROAD:     rate = TAX_ROADVEH_DAY; break;
			case VEH_SHIP:     rate = TAX_SHIP_DAY; break;
			case VEH_AIRCRAFT: rate = TAX_AIRCRAFT_DAY; break;
			default: break;
		}
		transport[v->owner.base()] += rate;
	}

	for (const Company *c : Company::Iterate()) {
		const size_t i = c->index.base();
		BookTax(c->index, LedgerItem::TaxLand, ScaleByPriceIndex(land[i] * TAX_LAND_PER_UNIT_DAY / 100));
		BookTax(c->index, LedgerItem::TaxTransport, ScaleByPriceIndex(transport[i]));
	}
});

/** Income tax on the profit of the quarter that has just closed (the economy.cpp patch closes it at the same day change, with a lower priority number). */
static const GameTimeSubscription _tax_quarter(GameTimeEvent::Day, [](const GameTimeSnapshot &now)
{
	if (_game_mode != GM_NORMAL) return;
	const uint64_t day = now.total_minutes / GameTimeManager::MINUTES_PER_DAY;
	if (day == 0 || day % GameTimeManager::DAYS_PER_QUARTER != 0) return;
	for (const Company *c : Company::Iterate()) {
		const CompanyEconomyEntry &last = c->old_economy[0];
		const int64_t profit = static_cast<int64_t>(last.income) + static_cast<int64_t>(last.expenses);
		BookTax(c->index, LedgerItem::TaxIncome, profit * TAX_INCOME_QUARTER_PERCENT / 100);
	}
}, 10);

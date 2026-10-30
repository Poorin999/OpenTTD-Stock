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
#include "CargoMarketMath.h"
#include "market_events.h"
#include "valuables.h"
#include "gold_reserve.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "core/backup_type.hpp"
#include "economy_func.h"
#include "map_func.h"
#include "tile_map.h"
#include "town.h"
#include "vehicle_base.h"
#include "zone_market.h"
#include "town_treasury.h"
#include "town_wallet_math.h"
#include <map>
#include <vector>

#include "safeguards.h"

/** Fixed amounts follow the price index of the game (inflation). */
static int64_t ScaleByPriceIndex(int64_t amount)
{
	return amount * GetPriceIndexPpm() / 1'000'000;
}

/**
 * Take a tax from the cash of a company: it is an expense of the company (the "Other" line of the finances window), a line of the
 * account statement and income of the own money of a bank: the bank of the zone where the tax is collected (zone 0 .. count - 1), or, with zone -1,
 * the bank of the zone of the headquarters of the company. The cash may go below zero, like it does for any other expense.
 */
static void BookTax(CompanyID company, LedgerItem item, int64_t amount, int zone = -1)
{
	if (amount <= 0 || !Company::IsValidID(company)) return;
	{
		AutoRestoreBackup cur_company(_current_company, company);
		SubtractMoneyFromCompany(CommandCost(EXPENSES_OTHER, Money(amount)));
	}
	RecordLedger(company, item, -amount);
	(zone >= 0 ? BankOfZone(zone) : BankOfCompany(company)).trading_revenue_cents += amount * 100;
}

/** An expense that is NOT a tax: the money goes out of the company cash (the "Other" line) and into the ledger, not to the central bank. */
static void BookFuel(CompanyID company, int64_t amount)
{
	if (amount <= 0 || !Company::IsValidID(company)) return;
	{
		AutoRestoreBackup cur_company(_current_company, company);
		SubtractMoneyFromCompany(CommandCost(EXPENSES_OTHER, Money(amount)));
	}
	RecordLedger(company, LedgerItem::FuelCost, -amount);
}

/** Income of the deliveries whose VAT was already charged one by one (ChargeVatPercent) in the payment that is running. One payment at a time, so one number is enough. */
static int64_t _vat_charged_income = 0;

int64_t ChargeVatPercent(CompanyID company, int64_t income, int64_t zone_percent, int zone)
{
	if (income <= 0) return 0;
	const int64_t vat = income * TAX_VAT_PERCENT * std::max<int64_t>(zone_percent, 0) / 10'000;
	_vat_charged_income += income;
	BookTax(company, LedgerItem::TaxVat, vat, zone);
	return vat;
}

void ChargeVat(CompanyID company, int64_t income)
{
	const int64_t rest = income - _vat_charged_income; // What the zone VAT of the single deliveries did not cover.
	_vat_charged_income = 0;
	if (rest <= 0) return;
	BookTax(company, LedgerItem::TaxVat, rest * TAX_VAT_PERCENT / 100);
}

void ChargeZoneFee(CompanyID company, int64_t amount, int zone)
{
	BookTax(company, LedgerItem::ZoneFee, amount, zone);
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
	const int64_t tax = profit * percent * ZoneTaxPercentOfCompany(company) / 10'000;
	if (tax <= 0) return 0;
	ZoneCountCompanyTax(company, tax);
	RecordLedger(company, tax_item, -tax);
	BankOfCompany(company).trading_revenue_cents += tax * 100;
	return tax;
}

/** Weight of a tile for the land tax, in 1/100 of a unit; 0 for a tile that the tax does not touch. A town of 10,000 people next to the tile adds +500%. */
static int64_t LandWeight(Tile tile, const Town **town_out = nullptr)
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
	if (town_out != nullptr) *town_out = t;
	if (t != nullptr) percent += std::min<int64_t>(t->cache.population / 20, 500);
	return base * percent;
}

/** Storage and insurance of the gold in the vault: 0.50% of its value a year (a real vault charges 0.1% .. 1%), taken daily from the cash of the company. */
static constexpr int64_t GOLD_STORAGE_BP_PER_YEAR = 50;

/** The storage fee of one game day for gold worth `value` (money units). Integer, rounds down; a vault worth less than 7,300 pays nothing that day. */
static int64_t GoldStorageFeeOfDay(int64_t value)
{
	return value <= 0 ? 0 : value * GOLD_STORAGE_BP_PER_YEAR / (10'000 * 365);
}

static void BookGoldStorage(CompanyID company, int64_t amount)
{
	if (amount <= 0 || !Company::IsValidID(company)) return;
	{
		AutoRestoreBackup cur_company(_current_company, company);
		SubtractMoneyFromCompany(CommandCost(EXPENSES_OTHER, Money(amount)));
	}
	RecordLedger(company, LedgerItem::GoldStorage, -amount);
}

/** Land tax and transport tax, once per game day at 00:00. */
static const GameTimeSubscription _tax_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &)
{
	if (_game_mode != GM_NORMAL) return;

	/* The capitals of the zones, to say in which zone a tile or a vehicle is. Slot MAX_ZONES = outside of all zones (no zones yet), at the base rate. */
	std::vector<zonemath::Pt> zone_caps;
	for (int i = 0; i < _zone_market.count; i++) {
		const TileIndex cap = _zone_market.zones[i].capital;
		zone_caps.push_back({static_cast<int64_t>(TileX(cap)), static_cast<int64_t>(TileY(cap))});
	}
	auto zone_of = [&zone_caps](TileIndex tile) -> int {
		if (zone_caps.empty()) return zonemath::MAX_ZONES;
		const int z = zonemath::ZoneOf({static_cast<int64_t>(TileX(tile)), static_cast<int64_t>(TileY(tile))}, zone_caps);
		return z >= 0 && z < zonemath::MAX_ZONES ? z : zonemath::MAX_ZONES;
	};
	using PerZone = std::array<int64_t, zonemath::MAX_ZONES + 1>;

	/* Land: the weight of all tiles of every company, per zone, in one pass over the map. */
	std::array<PerZone, MAX_COMPANIES> land{};
	/* Tax back to the towns (town_wallet_math.h TAX_BACK_PERCENT): per (town, zone) the weight x tax level of the land near the town, and the same for the vehicles. An ordered map: every client walks it in the same order. */
	using TownZone = std::pair<uint32_t, int>;
	std::map<TownZone, int64_t> town_land_raw, town_transport_raw;
	for (const auto tile : Map::Iterate()) {
		switch (GetTileType(tile)) {
			case MP_CLEAR: case MP_RAILWAY: case MP_ROAD: case MP_TUNNELBRIDGE: case MP_WATER: case MP_STATION: case MP_OBJECT: break;
			default: continue;
		}
		const Owner owner = GetTileOwner(tile);
		if (!Company::IsValidID(owner)) continue;
		const Town *near_town = nullptr;
		const int tile_zone = zone_of(static_cast<TileIndex>(tile));
		const int64_t weight = LandWeight(tile, &near_town);
		land[owner.base()][tile_zone] += weight;
		if (near_town != nullptr && weight > 0 && tile_zone < _zone_market.count) town_land_raw[{near_town->index.base(), tile_zone}] += weight * _zone_market.zones[tile_zone].tax_percent;
	}

	/* Vehicles: every primary vehicle that is not stopped in a depot; the transport tax counts in the zone where the vehicle is now. */
	std::array<PerZone, MAX_COMPANIES> transport{};
	std::array<int64_t, MAX_COMPANIES> fuel_base{}; // base fuel cost of the same vehicles (for an excess of 100%)
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (!v->IsPrimaryVehicle() || v->IsStoppedInDepot() || !Company::IsValidID(v->owner)) continue;
		int64_t rate = 0, fuel = 0;
		switch (v->type) {
			case VEH_TRAIN:    rate = TAX_TRAIN_DAY;    fuel = cargomarket::FUEL_TRAIN_DAY; break;
			case VEH_ROAD:     rate = TAX_ROADVEH_DAY;  fuel = cargomarket::FUEL_ROADVEH_DAY; break;
			case VEH_SHIP:     rate = TAX_SHIP_DAY;     fuel = cargomarket::FUEL_SHIP_DAY; break;
			case VEH_AIRCRAFT: rate = TAX_AIRCRAFT_DAY; fuel = cargomarket::FUEL_AIRCRAFT_DAY; break;
			default: break;
		}
		/* Fuel is burnt while the vehicle moves: one that stands (loading, waiting at a signal) at this moment burns none. The transport tax is per vehicle and does not care. */
		if (v->cur_speed == 0) fuel = 0;
		const int vehicle_zone = zone_of(v->tile);
		transport[v->owner.base()][vehicle_zone] += rate;
		if (vehicle_zone < _zone_market.count) {
			const Town *vt = ClosestTownFromTile(v->tile, 12);
			if (vt != nullptr) town_transport_raw[{vt->index.base(), vehicle_zone}] += rate * _zone_market.zones[vehicle_zone].tax_percent;
		}
		fuel_base[v->owner.base()] += fuel;
	}

	/* Fuel: only while the fuel index is above its plain price. The index is read from the news log at this game minute. */
	const int64_t fuel_index = FuelIndexWithOilPpm(_market_events.FuelIndexPpm(static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes()))); // News index changed by the oil market (valuables.cpp).

	for (const Company *c : Company::Iterate()) {
		const size_t i = c->index.base();
		BookFuel(c->index, ScaleByPriceIndex(cargomarket::FuelDailyCost(fuel_base[i], fuel_index)));
		/* The gold in the vault costs storage and insurance (the value at the market price of now). */
		if (_gold_reserve.GetPriceCents() > 0) BookGoldStorage(c->index, GoldStorageFeeOfDay(_gold_reserve.GetHoldings(c->index) * _gold_reserve.GetPriceCents() / 100));
		/* Every zone has its own tax level (percent of the base rate); the tax of the company is the sum over the zones where it owns land and runs vehicles. */
		for (int z = 0; z <= zonemath::MAX_ZONES; z++) {
			const bool real = z < _zone_market.count;
			const int64_t percent = real ? _zone_market.zones[z].tax_percent : 100;
			const int64_t land_z = ScaleByPriceIndex(land[i][z] * TAX_LAND_PER_UNIT_DAY * percent / 10'000);
			const int64_t transport_z = ScaleByPriceIndex(transport[i][z] * percent / 100);
			/* The tax goes to the bank of the zone where the land lies and the vehicle runs (outside of all zones: the bank of the company). */
			BookTax(c->index, LedgerItem::TaxLand, land_z, real ? z : -1);
			BookTax(c->index, LedgerItem::TaxTransport, transport_z, real ? z : -1);
			if (real) {
				_zone_market.zones[z].land_tax += land_z;
				_zone_market.zones[z].transport_tax += transport_z;
			}
		}
	}

	/* Tax back: a part of the land and transport tax that was just paid comes from the bank of the zone into the treasury of the town near the land / the vehicles. */
	std::map<TownZone, int64_t> tax_money;
	for (const auto &[key, raw] : town_land_raw) tax_money[key] += ScaleByPriceIndex(raw * TAX_LAND_PER_UNIT_DAY / 10'000);
	for (const auto &[key, raw] : town_transport_raw) tax_money[key] += ScaleByPriceIndex(raw / 100);
	for (const auto &[key, amount] : tax_money) {
		auto acc = _town_treasury.accounts.find(key.first);
		if (acc == _town_treasury.accounts.end() || !acc->second.started) continue; // a village without a treasury: the tax stays with the bank
		CentralBank &bank = BankOfZone(key.second);
		const int64_t back = std::min(townwallet::TaxBackOf(amount), std::max<int64_t>(0, bank.trading_revenue_cents / 100));
		if (back <= 0) continue;
		bank.trading_revenue_cents -= back * 100;
		acc->second.treasury += back;
		acc->second.tax_back += back;
	}
});

/** Income tax on the profit of the quarter that has just closed (the economy.cpp patch closes it at the same day change, with a lower priority number). */
static const GameTimeSubscription _tax_quarter(GameTimeEvent::Day, [](const GameTimeSnapshot &now)
{
	if (_game_mode != GM_NORMAL) return;
	const uint64_t day = now.total_minutes / GameTimeManager::MINUTES_PER_DAY;
	if (day == 0 || day % GameTimeManager::DAYS_PER_QUARTER != 0) return;
	const int64_t index_ppm = GetPriceIndexPpm();
	for (const Company *c : Company::Iterate()) {
		const CompanyEconomyEntry &last = c->old_economy[0];
		const int64_t profit = static_cast<int64_t>(last.income) + static_cast<int64_t>(last.expenses);
		const int64_t income_tax = IncomeTaxOfQuarter(profit, index_ppm) * ZoneTaxPercentOfCompany(c->index) / 100; // At the tax level of the zone of the headquarters.
		ZoneCountCompanyTax(c->index, income_tax);
		BookTax(c->index, LedgerItem::TaxIncome, income_tax);
	}
}, 10);

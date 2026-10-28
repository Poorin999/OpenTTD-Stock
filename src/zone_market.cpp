/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file zone_market.cpp Money zones, step B. Integer maths only, the same on every client. */

#include "stdafx.h"
#include "zone_market.h"
#include "tax.h"
#include "station_base.h"
#include "town.h"
#include "map_func.h"
#include "company_base.h"
#include "company_func.h"
#include "settings_type.h"
#include "game_time/game_time_manager.h"
#include "openttd.h"
#include "timer/timer.h"
#include "timer/timer_game_tick.h"
#include "safeguards.h"

ZoneMarket _zone_market;

static const char *const ZONE_CURRENCY_NAMES[zonemath::MAX_ZONES] = {"THB", "USD", "CNY", "RUB"};
static const char *const ZONE_CURRENCY_SYMBOLS[zonemath::MAX_ZONES] = {"THB", "USD", "CNY", "RUB"};

/** The tax level of each zone at the start of a new game, in percent of the base rates (tax.h). Change here and build again; the console command zone_tax changes it in a running game. */
static constexpr int64_t ZONE_TAX_DEFAULT_PERCENT[zonemath::MAX_ZONES] = {100, 80, 120, 140};

const char *ZoneCurrencyName(int zone) { return zone >= 0 && zone < zonemath::MAX_ZONES ? ZONE_CURRENCY_NAMES[zone] : ""; }
const char *ZoneCurrencySymbol(int zone) { return zone >= 0 && zone < zonemath::MAX_ZONES ? ZONE_CURRENCY_SYMBOLS[zone] : ""; }

/** The seed for the chance moves of the rates: the same on every client, different in every game. */
static uint64_t ZoneSeed()
{
	return static_cast<uint64_t>(_settings_game.game_creation.generation_seed) ^ 0x20E5EEDULL;
}

static zonemath::Pt PointOf(TileIndex tile)
{
	return {static_cast<int64_t>(TileX(tile)), static_cast<int64_t>(TileY(tile))};
}

void ZoneMarket::Reset()
{
	this->zones.fill({});
	this->count = 0;
}

int ZoneMarket::ZoneOfTile(TileIndex tile) const
{
	if (this->count <= 0 || tile == INVALID_TILE) return -1;
	std::vector<zonemath::Pt> caps;
	for (int i = 0; i < this->count; i++) caps.push_back(PointOf(this->zones[i].capital));
	return zonemath::ZoneOf(PointOf(tile), caps);
}

int ZoneMarket::ZoneOfCompany(CompanyID company) const
{
	if (!Company::IsValidID(company)) return -1;
	return this->ZoneOfTile(Company::Get(company)->location_of_HQ);
}

int64_t ZoneTaxPercentOfCompany(CompanyID company)
{
	const int z = _zone_market.ZoneOfCompany(company);
	return z >= 0 && z < _zone_market.count ? _zone_market.zones[z].tax_percent : 100;
}

void ZoneCountCompanyTax(CompanyID company, int64_t amount)
{
	const int z = _zone_market.ZoneOfCompany(company);
	if (z >= 0 && z < _zone_market.count && amount > 0) _zone_market.zones[z].company_tax += amount;
}

int64_t ZoneMarket::FeePpmBetween(int from, int to) const
{
	if (from < 0 || to < 0 || from == to || from >= this->count || to >= this->count) return 0;
	return zonemath::FeePpm(this->zones[from].rate_ppm, this->zones[to].rate_ppm);
}

bool ZoneMarket::MakeZones()
{
	if (this->count > 0) return true;
	/* The zones are made from the towns of the map. */
	std::vector<zonemath::Pt> pts;
	std::vector<int64_t> pop;
	std::vector<TileIndex> tiles;
	for (const Town *t : Town::Iterate()) {
		pts.push_back(PointOf(t->xy));
		pop.push_back(t->cache.population);
		tiles.push_back(t->xy);
	}
	const std::vector<int> caps = zonemath::PickCapitals(pts, pop, zonemath::WANT_ZONES);
	if (caps.size() < 2) return false; // One zone is no zone: nothing to charge. Try again later (towns may grow).
	for (size_t i = 0; i < caps.size(); i++) {
		this->zones[i] = {};
		this->zones[i].capital = tiles[caps[i]];
		this->zones[i].tax_percent = ZONE_TAX_DEFAULT_PERCENT[i];
	}
	this->count = static_cast<int>(caps.size());
	this->RefreshPopulation();
	return true;
}

void ZoneMarket::RefreshPopulation()
{
	std::vector<zonemath::Pt> caps;
	for (int i = 0; i < this->count; i++) caps.push_back(PointOf(this->zones[i].capital));
	for (int i = 0; i < this->count; i++) this->zones[i].population = 0;
	for (const Town *t : Town::Iterate()) {
		const int z = zonemath::ZoneOf(PointOf(t->xy), caps);
		if (z >= 0) this->zones[z].population += t->cache.population;
	}
}

void ZoneMarket::UpdateDaily()
{
	/* Normally the zones exist long before the first game day (see the tick timer below); this is only the safety net. */
	if (!this->MakeZones()) return;

	/* The people of every zone, the level of every rate, one step of every rate. */
	this->RefreshPopulation();
	int64_t total = 0;
	for (int i = 0; i < this->count; i++) total += this->zones[i].population;
	const int64_t day = static_cast<int64_t>(GameTimeManager::Get().GetSnapshot().total_minutes / GameTimeManager::MINUTES_PER_DAY);
	for (int i = 0; i < this->count; i++) {
		const int64_t level = zonemath::LevelPpm(this->zones[i].population, total, this->count);
		this->zones[i].rate_ppm = zonemath::NextRate(ZoneSeed(), i, day, this->zones[i].rate_ppm, level);
	}
}

void ZoneFeeOnDelivery(CompanyID owner, int64_t income, StationID source, StationID dest)
{
	if (_zone_market.count <= 0 || income <= 0) return;
	if (!Station::IsValidID(source) || !Station::IsValidID(dest)) return;
	const int from = _zone_market.ZoneOfTile(Station::Get(source)->xy);
	const int to = _zone_market.ZoneOfTile(Station::Get(dest)->xy);
	if (to >= 0 && to < _zone_market.count) _zone_market.zones[to].vat += ChargeVatPercent(owner, income, _zone_market.zones[to].tax_percent, to); // The VAT of this delivery at the tax level of the zone of the destination.
	const int64_t fee_ppm = _zone_market.FeePpmBetween(from, to);
	if (fee_ppm <= 0) return;
	const int64_t fee = zonemath::FeeAmount(income, fee_ppm);
	if (fee <= 0) return;
	ChargeZoneFee(owner, fee, to);
	_zone_market.zones[to].fees += fee;
}

/** Make the zones as soon as the game runs (a new game, or an old savegame that has none), not on the first game day: every 8 ticks while there are none. */
static const IntervalTimer<TimerGameTick> _zone_market_make({TimerGameTick::Priority::NONE, 8}, [](auto)
{
	if (_game_mode != GM_NORMAL || _zone_market.count > 0) return;
	_zone_market.MakeZones();
});

static const GameTimeSubscription _zone_market_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &) { _zone_market.UpdateDaily(); }, 26);

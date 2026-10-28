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
#include "central_bank.h"
#include "market_events.h"
#include "command_func.h"
#include "settings_type.h"
#include "game_time/game_time_manager.h"
#include "openttd.h"
#include "timer/timer.h"
#include "timer/timer_game_tick.h"
#include "table/strings.h"

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
	for (auto &row : this->holdings) row.fill(0);
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
		this->zones[i].fx = zonemath::FxInitState(static_cast<int>(i));
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

	/* The people of every zone (display, tax window), then one game day of the FX model (zonemath::FxStepDay): inflation, policy meetings, trade, growth,
	 * political risk, news, market mood, capital flows, the rate, and the defence of the zone central banks. */
	this->RefreshPopulation();
	const int64_t day = static_cast<int64_t>(GameTimeManager::Get().GetSnapshot().total_minutes / GameTimeManager::MINUTES_PER_DAY);
	std::array<zonemath::FxZone, zonemath::MAX_ZONES> fz{};
	for (int i = 0; i < this->count; i++) {
		fz[i].rate_ppm = this->zones[i].rate_ppm;
		fz[i].inflation_ppm = this->zones[i].inflation_ppm;
		fz[i].policy_ppm = this->zones[i].policy_ppm;
		fz[i].fx = this->zones[i].fx;
	}
	zonemath::FxWorld world;
	world.mood_ppm = _market_events.market_sentiment_ppm;
	world.war = _market_events.WarIsOn();
	zonemath::FxStepDay(ZoneSeed(), day, fz.data(), this->count, world);
	for (int i = 0; i < this->count; i++) {
		this->zones[i].rate_ppm = fz[i].rate_ppm;
		this->zones[i].inflation_ppm = fz[i].inflation_ppm;
		this->zones[i].policy_ppm = fz[i].policy_ppm;
		this->zones[i].fx = fz[i].fx;
		this->zones[i].parts = fz[i].parts;
	}

	/* Banknotes earn the policy rate of their zone every game day. */
	for (auto &row : this->holdings) {
		for (int i = 0; i < this->count; i++) row[i] += zonemath::DailyInterest(row[i], this->zones[i].policy_ppm);
	}
}

void ZoneRateTrade(int zone, bool buy, uint32_t volume)
{
	if (zone < 0 || zone >= _zone_market.count) return;
	int64_t &rate = _zone_market.zones[zone].rate_ppm;
	const int64_t before = rate;
	rate = zonemath::RateAfterTrade(rate, buy, volume);
	if (before > 0) _zone_market.zones[zone].fx.capital_ppm += (rate - before) * zonemath::PPM / before; // the trade of a player is a capital flow
}

/**
 * Exchange cash for banknotes of a zone, or banknotes back for cash (spread EXCHANGE_SPREAD_PPM). Paid from unborrowed cash only.
 * @param flags Type of operation.
 * @param zone Number of the zone, 0 based.
 * @param sell False: buy banknotes with `amount` money. True: sell `amount` banknotes.
 * @param amount Money (buy) or banknotes (sell).
 * @return The cost (a sell gives a negative cost = income), or an error.
 */
CommandCost CmdZoneExchange(DoCommandFlags flags, uint8_t zone, bool sell, Money amount)
{
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;
	if (_zone_market.count <= 0 || zone >= _zone_market.count) return CommandCost(STR_ZONE_EXCHANGE_ERROR_BAD_ZONE);
	const int64_t value = static_cast<int64_t>(amount);
	if (value <= 0) return CMD_ERROR;
	const int64_t rate = _zone_market.zones[zone].rate_ppm;
	int64_t &held = _zone_market.holdings[_current_company.base()][zone];

	if (!sell) {
		if (GetUnborrowedLiquidCash(_current_company) < value) return CommandCost(STR_ZONE_EXCHANGE_ERROR_NO_CASH);
		const int64_t notes = zonemath::AfterSpread(zonemath::NotesForMoney(value, rate));
		if (notes <= 0) return CMD_ERROR;
		if (flags.Test(DoCommandFlag::Execute)) {
			held += notes;
			SubtractMoneyFromCompany(CommandCost(EXPENSES_OTHER, Money(value))); // Taken here, not through the returned cost.
			RecordLedger(_current_company, LedgerItem::ZoneExchange, -value); // The account statement.
		}
		return CommandCost(EXPENSES_OTHER);
	}

	if (value > held) return CommandCost(STR_ZONE_EXCHANGE_ERROR_NO_NOTES);
	const int64_t gross = zonemath::MoneyForNotes(value, rate);
	const int64_t cash = zonemath::AfterSpread(gross);
	if (flags.Test(DoCommandFlag::Execute)) {
		held -= value;
		SubtractMoneyFromCompany(CommandCost(EXPENSES_OTHER, -Money(cash))); // A negative cost is income; paid here, not through the returned cost.
		RecordLedger(_current_company, LedgerItem::ZoneExchange, cash); // The account statement.
	}
	return CommandCost(EXPENSES_OTHER);
}

void ZoneFeeOnDelivery(CompanyID owner, int64_t income, StationID source, StationID dest)
{
	if (_zone_market.count <= 0 || income <= 0) return;
	if (!Station::IsValidID(source) || !Station::IsValidID(dest)) return;
	const int from = _zone_market.ZoneOfTile(Station::Get(source)->xy);
	const int to = _zone_market.ZoneOfTile(Station::Get(dest)->xy);
	if (to >= 0 && to < _zone_market.count) _zone_market.zones[to].vat += ChargeVatPercent(owner, income, _zone_market.zones[to].tax_percent, to); // The VAT of this delivery at the tax level of the zone of the destination.
	if (to >= 0 && to < _zone_market.count) _zone_market.zones[to].fx.gdp_today += income; // income of deliveries into the zone = its output
	if (from >= 0 && to >= 0 && from != to && from < _zone_market.count && to < _zone_market.count) {
		_zone_market.zones[from].fx.exports_today += income; // cargo crossing a border: export of the zone it came from, import of the zone it goes to
		_zone_market.zones[to].fx.imports_today += income;
	}
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

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
#include "currency.h"
#include "gfx_func.h"
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
		/* ONE policy rate per zone: the rate of the real central bank of the zone (the one that sets the loan rates) moves the exchange rate. 1 bp = 100 ppm. */
		const CentralBank &bank = BankOfZone(i);
		fz[i].external_policy_ppm = PolicyUpperBp(bank.ShiftBp()) * 100;
		fz[i].external_expected_ppm = PolicyUpperBp(bank.policy.ExpectedShiftBp() + BANK_ZONE_SPREAD_BP[i]) * 100;
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

	/* Banknotes earn NO interest (owner 2026-10-29): they are cash. A holder gains or loses only through the exchange rate. The policy rate still moves the rate (capital flows). */
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

/** Set to false and build again to switch the zone running costs off. */
static constexpr bool ZONE_RUNNING_COST_ENABLED = true;

/** Set to false and build again to switch the zone price level of construction and of the purchase of vehicles off (zone_permit.cpp calls ZonePriceScale). */
static constexpr bool ZONE_PRICE_ENABLED = true;

int64_t ZonePriceScale(int64_t cost, TileIndex tile)
{
	if (cost <= 0 || _zone_market.count <= 0) return cost;
	const int z = _zone_market.ZoneOfTile(tile);
	if (z < 0 || z >= _zone_market.count) return cost;
	return zonemath::ScaleByPercent(cost, zonemath::PriceLevelPercent(_zone_market.zones[z].fx.price_level_ppm)); // integer only, the same on every client
}

Money ZoneRunningCostScale(Money cost, TileIndex tile)
{
	if (!ZONE_RUNNING_COST_ENABLED) return cost;
	return Money(ZonePriceScale(static_cast<int64_t>(cost), tile));
}

bool ZonePriceEnabled() { return ZONE_PRICE_ENABLED; }

/** Set to false and build again to keep the one money look of OpenTTD in every zone. */
static constexpr bool ZONE_CURRENCY_LOOK_ENABLED = true;

/** Set to false and build again to show the same numbers as before in every zone (the look changes, the value is not converted). */
static constexpr bool ZONE_CURRENCY_CONVERT_ENABLED = true;

/** One way to write the sign of a currency. The first variant that the game font can draw is used, the last one is plain ASCII and always works. */
struct ZoneMoneyVariant { const char *prefix; const char *suffix; };

/** The look of the money of each zone, the way it is written in real life:
 *  THB  ฿1,000.50        (baht sign in front, comma for thousands, point for decimals)
 *  USD  $1,000.50
 *  CNY  ¥1,000.50        (yuan sign, same marks)
 *  RUB  1 000,50 ₽       (sign behind with a space, space for thousands, comma for decimals; руб. or RUB when the font has no ruble sign)
 *  symbol_pos 0 = in front, 1 = behind. */
struct ZoneMoneyLook { ZoneMoneyVariant variants[3]; uint8_t symbol_pos; const char *group; const char *decimal; };
static constexpr ZoneMoneyLook ZONE_MONEY_LOOK[zonemath::MAX_ZONES] = {
	{{{"\u0E3F", ""}, {"THB ", ""}, {"THB ", ""}}, 0, ",", "."},
	{{{"$", ""}, {"USD ", ""}, {"USD ", ""}}, 0, ",", "."},
	{{{"\u00A5", ""}, {"CNY ", ""}, {"CNY ", ""}}, 0, ",", "."},
	{{{"", " \u20BD"}, {"", " \u0440\u0443\u0431."}, {"", " RUB"}}, 1, " ", ","},
};

/** True when the game font has a picture for every non-ASCII sign of `s`. A sign that the font lacks is drawn as the "missing sign" picture, so a sign
 *  whose width equals that of an impossible character (U+FFFF) in both font sizes is taken as missing. When in doubt this says "missing": the next variant
 *  (an ISO code) is always readable. Not tested against every font of the game. */
static bool ZoneTextHasGlyphs(const char *s)
{
	const uint ref_normal = GetCharacterWidth(FS_NORMAL, 0xFFFF);
	const uint ref_small = GetCharacterWidth(FS_SMALL, 0xFFFF);
	const unsigned char *u = reinterpret_cast<const unsigned char *>(s);
	while (*u != 0) {
		char32_t c = *u;
		int extra = 0;
		if (c >= 0xF0) { c &= 0x07; extra = 3; } else if (c >= 0xE0) { c &= 0x0F; extra = 2; } else if (c >= 0xC0) { c &= 0x1F; extra = 1; }
		u++;
		for (; extra > 0 && (*u & 0xC0) == 0x80; extra--, u++) c = (c << 6) | (*u & 0x3F);
		if (c < 0x80) continue;
		if (GetCharacterWidth(FS_NORMAL, c) == ref_normal && GetCharacterWidth(FS_SMALL, c) == ref_small) return false;
	}
	return true;
}

/** The first variant of a zone whose signs the font can draw (the ASCII one at the end always can). */
static const ZoneMoneyVariant &ZonePickVariant(int zone)
{
	const ZoneMoneyLook &look = ZONE_MONEY_LOOK[zone];
	for (const ZoneMoneyVariant &v : look.variants) {
		if (ZoneTextHasGlyphs(v.prefix) && ZoneTextHasGlyphs(v.suffix)) return v;
	}
	return look.variants[2];
}

/** The rate at which money is SHOWN in a zone that is the home of the viewer (the zone of the headquarters of the local company) and for every domestic amount: a fixed
 *  value, NOT the exchange rate of the day. What things cost at home does not move when the exchange rate moves (a bus costs the same number of baht today and tomorrow);
 *  only money that belongs to ANOTHER zone (a window about a foreign town, the taxes of a foreign zone) is shown at the live rate of that zone. 1.0 = the number is the same as
 *  the internal money; change a value to give a currency its own scale (for example 35 baht to the dollar). Constant, so every client shows the same. */
static constexpr int64_t ZONE_DISPLAY_RATE_PPM[zonemath::MAX_ZONES] = {zonemath::PPM, zonemath::PPM, zonemath::PPM, zonemath::PPM};

/** An amount of a zone currency: whole units and hundredths. */
struct ZoneAmount { int64_t units; int cents; };

/**
 * Base money -> zone currency at the rate of the zone, integer only, rounded to the nearest hundredth.
 * rate_ppm is the value of ONE unit of the zone currency in base units (ppm), so the amount is magnitude / rate. Saturates instead of overflowing.
 */
static ZoneAmount ZoneConvertAmount(int64_t magnitude, int64_t rate_ppm)
{
	constexpr int64_t PPM = zonemath::PPM;
	if (rate_ppm <= 0 || magnitude <= 0) return {magnitude < 0 ? 0 : magnitude, 0};
	const int64_t q = magnitude / rate_ppm;
	const int64_t r = magnitude % rate_ppm;
	if (q > (INT64_MAX / PPM) - 2) return {INT64_MAX / 2, 0};
	int64_t units = q * PPM + r * PPM / rate_ppm;
	const int64_t rem = r * PPM % rate_ppm;
	int64_t cents = (rem * 100 + rate_ppm / 2) / rate_ppm;
	if (cents >= 100) { units++; cents -= 100; }
	return {units, static_cast<int>(cents)};
}

/** Digits of `n` with the group mark every three digits. */
static std::string ZoneGroupDigits(int64_t n, const char *group)
{
	const std::string digits = fmt::format("{}", n);
	std::string out;
	const size_t len = digits.size();
	for (size_t i = 0; i < len; i++) {
		if (i > 0 && (len - i) % 3 == 0) out += group;
		out += digits[i];
	}
	return out;
}

static constexpr int ZONE_MONEY_NO_SCOPE = -2;
static int _zone_money_scope = ZONE_MONEY_NO_SCOPE;

ZoneMoneyScope::ZoneMoneyScope(int zone) : previous(_zone_money_scope) { _zone_money_scope = zone; }
ZoneMoneyScope::~ZoneMoneyScope() { _zone_money_scope = this->previous; }

/** The zone of the headquarters of the local company (kept until the headquarters or a capital moves), -1 when there is none. */
static int ZoneOfLocalHq()
{
	if (!Company::IsValidID(_local_company)) return -1;
	const TileIndex hq = Company::Get(_local_company)->location_of_HQ;
	if (hq == INVALID_TILE) return -1;
	uint64_t key = static_cast<uint64_t>(_zone_market.count);
	for (int i = 0; i < _zone_market.count; i++) key = key * 1000003 + static_cast<uint64_t>(TileX(_zone_market.zones[i].capital)) * 4096 + static_cast<uint64_t>(TileY(_zone_market.zones[i].capital));
	static TileIndex cache_tile = INVALID_TILE;
	static uint64_t cache_key = 0;
	static int cache_zone = -1;
	if (hq != cache_tile || key != cache_key) {
		cache_tile = hq;
		cache_key = key;
		cache_zone = _zone_market.ZoneOfTile(hq);
	}
	return cache_zone;
}

/** The zone whose look the money has now: the scope when there is one, else the zone of the headquarters of the local company. */
static int ZoneForMoneyLook()
{
	if (_zone_money_scope != ZONE_MONEY_NO_SCOPE) return _zone_money_scope;
	return ZoneOfLocalHq();
}

/** Money is shown at the LIVE rate only when a scope names a zone that is not the home zone of the viewer: it is foreign money. Everything else is domestic: fixed rate. */
static bool ZoneMoneyIsForeign(int zone)
{
	return _zone_money_scope != ZONE_MONEY_NO_SCOPE && zone >= 0 && zone != ZoneOfLocalHq();
}

/** The currencies handed out by ZoneDisplayCurrency: [zone] shows money at the fixed rate (domestic), [MAX_ZONES + zone] at the live rate of the zone (foreign money). */
static std::array<CurrencySpec, 2 * zonemath::MAX_ZONES> _zone_specs;
static bool _zone_specs_made = false;

const CurrencySpec &ZoneDisplayCurrency(const CurrencySpec &base)
{
	if (!ZONE_CURRENCY_LOOK_ENABLED || _zone_market.count <= 0) return base;
	const int z = ZoneForMoneyLook();
	if (z < 0 || z >= _zone_market.count || z >= zonemath::MAX_ZONES) return base;
	if (!_zone_specs_made) { // Copied from the base currency once.
		for (size_t i = 0; i < _zone_specs.size(); i++) _zone_specs[i] = base;
		_zone_specs_made = true;
	}
	/* The signs follow what the font can draw (the language may change in a running game), so they are chosen again each time. When the value is not
	 * converted the game multiplies by the rate of the base currency itself; when it is, ZoneFormatMoney makes the whole text and the rate is not used. */
	const ZoneMoneyVariant &v = ZonePickVariant(z);
	CurrencySpec &spec = _zone_specs[(ZoneMoneyIsForeign(z) ? zonemath::MAX_ZONES : 0) + z];
	if (spec.prefix != v.prefix) spec.prefix = v.prefix;
	if (spec.suffix != v.suffix) spec.suffix = v.suffix;
	spec.symbol_pos = ZONE_MONEY_LOOK[z].symbol_pos;
	spec.rate = base.rate;
	return spec;
}

bool ZoneFormatMoney(const CurrencySpec *spec, int64_t magnitude, bool compact, std::string &out)
{
	if (!ZONE_CURRENCY_LOOK_ENABLED || !ZONE_CURRENCY_CONVERT_ENABLED || !_zone_specs_made || _zone_market.count <= 0) return false;
	if (spec < _zone_specs.data() || spec >= _zone_specs.data() + _zone_specs.size()) return false;
	const int index = static_cast<int>(spec - _zone_specs.data());
	const int z = index % zonemath::MAX_ZONES;
	const bool live = index >= zonemath::MAX_ZONES; // foreign money: the rate of the day; domestic money: the fixed display rate
	if (z >= _zone_market.count || _zone_market.zones[z].rate_ppm <= 0) return false;
	const int64_t rate = live ? _zone_market.zones[z].rate_ppm : ZONE_DISPLAY_RATE_PPM[z];
	if (magnitude < 0) magnitude = 0;

	const ZoneMoneyLook &look = ZONE_MONEY_LOOK[z];
	const ZoneMoneyVariant &v = ZonePickVariant(z);
	ZoneAmount a = ZoneConvertAmount(magnitude, rate);

	std::string number;
	const char *multiplier = "";
	if (compact && a.units >= 1'000'000'000) { // like the game: big amounts as K / M
		number = ZoneGroupDigits((a.units + 500'000) / 1'000'000, look.group);
		multiplier = "M";
	} else if (compact && a.units >= 1'000'000) {
		number = ZoneGroupDigits((a.units + 500) / 1'000, look.group);
		multiplier = "K";
	} else if (a.units >= 100) { // from 100 units up the hundredths are noise: round to whole units
		if (a.cents >= 50) a.units++;
		number = ZoneGroupDigits(a.units, look.group);
	} else { // small amounts keep their hundredths, like a price tag
		number = ZoneGroupDigits(a.units, look.group) + look.decimal + (a.cents < 10 ? "0" : "") + fmt::format("{}", a.cents);
	}
	out = std::string(v.prefix) + number + multiplier + v.suffix;
	return true;
}

/** Make the zones as soon as the game runs (a new game, or an old savegame that has none), not on the first game day: every 8 ticks while there are none. */
static const IntervalTimer<TimerGameTick> _zone_market_make({TimerGameTick::Priority::NONE, 8}, [](auto)
{
	if (_game_mode != GM_NORMAL || _zone_market.count > 0) return;
	_zone_market.MakeZones();
});

static const GameTimeSubscription _zone_market_daily(GameTimeEvent::Day, [](const GameTimeSnapshot &) { _zone_market.UpdateDaily(); }, 26);

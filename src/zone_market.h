/*
 * This file is part of OpenTTD-Stock (a mod of OpenTTD).
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file zone_market.h Money zones, step B: zones with a currency and a moving rate; a delivery across a border pays an exchange fee. See zone_market_math.h. */

#ifndef ZONE_MARKET_H
#define ZONE_MARKET_H

#include <array>
#include <cstdint>
#include <string>
#include "company_type.h"
#include "economy_type.h"
#include "station_type.h"
#include "tile_type.h"
#include "zone_market_math.h"

/** One zone: the capital (a tile, so the zone survives when the town is gone), the rate and the fees that it earned. */
struct ZoneState {
	TileIndex capital = INVALID_TILE;
	int64_t rate_ppm = zonemath::PPM; ///< Value of one unit of the zone currency in base units, in ppm. Saved (SRK_ZONE v2).
	int64_t fees = 0;                 ///< Exchange fees paid for deliveries INTO this zone, in money units. Saved (SRK_ZONE v3).
	int64_t population = 0;           ///< People living in the zone at the last day (not saved, made again every day).
	int64_t land_tax = 0;             ///< Land tax paid for the tiles inside the zone, all companies together (saved, SRK_ZONE_TAX).
	int64_t transport_tax = 0;        ///< Transport tax paid for the vehicles that stood inside the zone at the daily count (saved, SRK_ZONE_TAX).
	int64_t vat = 0;                  ///< VAT of the deliveries whose destination station is inside the zone, counted from the income of each delivery (saved, SRK_ZONE_TAX).
	int64_t company_tax = 0;          ///< Income tax, capital gains tax and dividend tax of the companies whose headquarters stand in this zone (saved, SRK_ZONE_TAX).
	int64_t inflation_ppm = zonemath::INFL_START_PPM; ///< Yearly inflation of the zone in ppm. Saved (SRK_ZONE v4, +1,000,000).
	int64_t policy_ppm = zonemath::POLICY_START_PPM;  ///< Yearly policy rate of the zone central bank in ppm (it moves the exchange rate; banknotes themselves earn no interest). Saved (SRK_ZONE v5, +1).
	zonemath::FxState fx;             ///< Memory of the FX model (price level, trade, growth, capital flow, risk, news, reserves). Saved (SRK_ZONE_FX).
	zonemath::FxParts parts;          ///< The parts of the last fair value (console, windows). Not saved.
	int64_t tax_percent = 100;        ///< The tax level of the zone in percent of the base rates of tax.h (100 = the base rate). Saved (SRK_ZONE_TAX, v5 = percent + 1).
};

struct ZoneMarket {
	std::array<ZoneState, zonemath::MAX_ZONES> zones{};
	int count = 0; ///< Number of zones, 0 = not set up yet (the first game day makes them).
	std::array<std::array<int64_t, zonemath::MAX_ZONES>, MAX_COMPANIES> holdings{}; ///< Banknotes (units of the zone currency) that a company holds, per zone. Saved (SRK_ZONE_HOLD).

	void Reset();
	bool MakeZones();          ///< Choose the capitals from the towns of the map when there are no zones yet. True when zones exist afterwards.
	void RefreshPopulation();  ///< The people of every zone (and the total), made again.
	void UpdateDaily();
	int ZoneOfTile(TileIndex tile) const;
	int64_t FeePpmBetween(int from, int to) const;
	int ZoneOfCompany(CompanyID company) const; ///< The zone of the headquarters of a company, -1 when it has none (or there are no zones).
};

extern ZoneMarket _zone_market;

/** Called for every final delivery (economy.cpp, hook ZONE_FEE_PATCH): charges the exchange fee when the cargo crossed a zone border. */
void ZoneFeeOnDelivery(CompanyID owner, int64_t income, StationID source, StationID dest);

/** Running costs of a vehicle follow the price level of the zone it stands in (called from the 4 vehicle files, hook ZONE_RUNNING_COST_PATCH). 100% = the average of the zones. Returns the cost unchanged when there are no zones. */
Money ZoneRunningCostScale(Money cost, TileIndex tile); ///< Money, not int64_t: CommandCost(EXPENSES_x, int64_t) is ambiguous in MSVC.

/** What a cost is in the zone of `tile`: the cost times the price level of the zone (50% .. 200%, 100% = the average of the zones). Returns the cost unchanged when there are no
 *  zones. Used for the running costs (above), for construction and for the purchase of vehicles (zone_permit.cpp, hook ZONE_PERMIT_PATCH). */
int64_t ZonePriceScale(int64_t cost, TileIndex tile);
/** False when the price level of construction and vehicle purchases is switched off (ZONE_PRICE_ENABLED in zone_market.cpp). */
bool ZonePriceEnabled();

struct CurrencySpec;

/** How money looks (symbol in front or behind) follows a zone: the zone of the headquarters of the company that looks at the screen, or the zone
 *  chosen by a ZoneMoneyScope (hook ZONE_CURRENCY_LOOK_PATCH in strings.cpp). The value is converted at the exchange rate of the zone by ZoneFormatMoney. Returns `base` when there are no zones. */
const CurrencySpec &ZoneDisplayCurrency(const CurrencySpec &base);

/** Money of a zone written the way people write it in that country (real exchange rate, symbol, group and decimal marks). Hook ZONE_CURRENCY_CONVERT_PATCH
 *  at the start of FormatGenericCurrency (strings.cpp).
 *  @param spec The currency that the game wants to draw with; only a currency made by ZoneDisplayCurrency is handled.
 *  @param magnitude The amount in base money units, NOT negative (the caller writes the minus sign and its colour).
 *  @param compact True: K / M for large amounts, like the game does.
 *  @param out The text, when the function returns true.
 *  @return False when `spec` is not a zone currency or the conversion is switched off: the caller then formats the money the normal way. */
bool ZoneFormatMoney(const CurrencySpec *spec, int64_t magnitude, bool compact, std::string &out);

/** While this object lives, money drawn by the game (GetString with a currency) uses the look of `zone` (-1 = the base currency). For windows about a town or a zone. */
struct ZoneMoneyScope {
	int previous;
	explicit ZoneMoneyScope(int zone);
	~ZoneMoneyScope();
	ZoneMoneyScope(const ZoneMoneyScope &) = delete;
	ZoneMoneyScope &operator=(const ZoneMoneyScope &) = delete;
};

/** The tax level (percent of the base rates) of the zone of the headquarters of a company: 100 when it has no zone. Used for the taxes of the company itself. */
int64_t ZoneTaxPercentOfCompany(CompanyID company);

/** Count a tax of a company (income, capital gains, dividend) for the zone of its headquarters, for the zone tax window. */
void ZoneCountCompanyTax(CompanyID company, int64_t amount);

/** A terminal trade of banknotes of a zone moves the real rate of the zone (called by StockMarket::ApplyImpact). */
void ZoneRateTrade(int zone, bool buy, uint32_t volume);

/** The names of the currencies of the zones (English; the console prints the capital next to it). */
const char *ZoneCurrencyName(int zone);
const char *ZoneCurrencySymbol(int zone);

#endif /* ZONE_MARKET_H */

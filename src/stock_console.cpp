/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file stock_console.cpp Console commands of the mod for testing: 'stock_dev on|off', 'stock_skip <time>', 'stock_event [kind]', 'cargo_prices' and 'gold_factors'.
 * They are registered by apply_overlay.py, which adds one call to IConsoleStdLibRegister() of console_cmds.cpp.
 */

#include "stdafx.h"
#include "commercial_bank.h"
#include <algorithm>
#include <charconv>
#include <vector>
#include <span>
#include <string>
#include <string_view>
#include "stock_dev.h"
#include "3rdparty/fmt/format.h"
#include "console_func.h"
#include "console_internal.h"
#include "network/network.h"
#include "window_func.h"
#include "game_time/game_time_manager.h"
#include "market_events.h"
#include "cargo_market.h"
#include "zone_market.h"
#include "zone_permit.h"
#include "zone_permit_math.h"
#include "industry_owner.h"
#include "company_role.h"
#include "contract.h"
#include "vehicle_base.h"
#include "industry.h"
#include "industry_map.h"
#include "industry_cmd.h"
#include "core/random_func.hpp"
#include "strings_func.h"
#include "tile_map.h"
#include "map_func.h"
#include "command_func.h"
#include "company_func.h"
#include "zone_map_gui.h"
#include "contract_gui.h"
#include "zone_borders.h"
#include "command_func.h"
#include "wallet_cmd.h"
#include "company_func.h"
#include "company_base.h"
#include "town.h"
#include "gold_reserve.h"
#include "town_treasury.h"
#include "town_wallet.h"
#include "valuables.h"
#include "cargotype.h"
#include "town.h"
#include "strings_func.h"

#include "table/strings.h"

#include "safeguards.h"

bool _stock_dev_tools = false;

/**
 * Move the game clock of the mod forward. Every system that listens to the clock (interest, retail investors, candles of all
 * timeframes, the gold price, the quarters of the company statistics) gets its events one by one, in time order, as if the time had passed.
 * The vehicles and the economy of OpenTTD itself do not move: only the clock does. Single player only, because the clients of a network
 * game would not agree about the time.
 * @param minutes Game minutes to skip.
 */
void SkipGameMinutes(uint64_t minutes)
{
	if (_networking || !_stock_dev_tools) return;
	GameTimeManager::Get().AdvanceMilliseconds(minutes * GameTimeManager::MS_PER_MINUTE);
	InvalidateWindowClassesData(WC_CENTRAL_BANK, 0);
	InvalidateWindowClassesData(WC_STOCK_MARKET, 0);
}

/** 'stock_dev on|off': switch the test tools. */
static bool ConStockDev(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Switch the test tools of the stock mod. Usage: 'stock_dev on' or 'stock_dev off'. The tools: the time skip buttons in the Central Bank window and the command 'stock_skip'.");
		return true;
	}
	if (argv.size() == 1) {
		IConsolePrint(CC_DEFAULT, "The test tools are {}.", _stock_dev_tools ? "on" : "off");
		return true;
	}
	if (argv[1] == "on" || argv[1] == "1") {
		if (_networking) {
			IConsolePrint(CC_ERROR, "The test tools work in single player only.");
			return true;
		}
		_stock_dev_tools = true;
		IConsolePrint(CC_DEFAULT, "The test tools are on. 'stock_skip 30' moves the game clock 30 days forward.");
	} else if (argv[1] == "off" || argv[1] == "0") {
		_stock_dev_tools = false;
		IConsolePrint(CC_DEFAULT, "The test tools are off.");
	} else {
		return false; // Shows the help.
	}
	InvalidateWindowClassesData(WC_CENTRAL_BANK, 0);
	return true;
}

/** 'stock_skip <number>[d|h]': move the game clock forward by days (default) or hours. */
static bool ConStockSkip(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Move the game clock forward (test tool, single player, after 'stock_dev on'). Usage: 'stock_skip <days>' or 'stock_skip <hours>h', for example 'stock_skip 30' or 'stock_skip 12h'. At most 400 days.");
		return true;
	}
	if (argv.size() != 2) return false;
	if (_networking) {
		IConsolePrint(CC_ERROR, "The time skip works in single player only.");
		return true;
	}
	if (!_stock_dev_tools) {
		IConsolePrint(CC_ERROR, "Switch the test tools on first: 'stock_dev on'.");
		return true;
	}

	std::string_view text = argv[1];
	uint64_t unit_minutes = 60 * 24;
	if (!text.empty() && (text.back() == 'h' || text.back() == 'd')) {
		if (text.back() == 'h') unit_minutes = 60;
		text.remove_suffix(1);
	}
	uint64_t value = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (error != std::errc() || end != text.data() + text.size() || value == 0 || value > 400 * 24 * 60 / unit_minutes) {
		IConsolePrint(CC_ERROR, "The time must be a whole number above 0, at most 400 days.");
		return true;
	}

	SkipGameMinutes(value * unit_minutes);
	const GameTimeSnapshot now = GameTimeManager::Get().GetSnapshot();
	IConsolePrint(CC_DEFAULT, "Skipped {} game minutes. The game time is now day {} of year {}, {:02}:{:02}.", value * unit_minutes, now.day, now.year, now.hour, now.minute);
	return true;
}

/** Turn the zone border lines of the game view on or off. */
static bool ConZoneLines(std::span<std::string_view> argv)
{
	if (argv.size() < 2 || argv[1] == "?") {
		IConsolePrint(CC_HELP, "Show or hide the borders of the money zones in the game view (coloured lines on the ground). Usage: 'zone_lines on|off'.");
		return true;
	}
	if (argv[1] == "on") SetZoneBordersEnabled(true);
	else if (argv[1] == "off") SetZoneBordersEnabled(false);
	else { IConsolePrint(CC_ERROR, "Usage: zone_lines on|off"); return true; }
	IConsolePrint(CC_DEFAULT, "Zone borders in the game view: {}", ZoneBordersEnabled() ? "on" : "off");
	return true;
}

/** Open the zones window: the map in coloured zones with the borders as lines. */
static bool ConZonesMap(std::span<std::string_view> argv)
{
	if (!argv.empty() && argv.size() >= 2 && argv[1] == "?") {
		IConsolePrint(CC_HELP, "Open the zones window: the map divided into the money zones, one colour per zone, the borders as lines. A click on the map scrolls the main view there.");
		return true;
	}
	if (_network_dedicated) {
		IConsolePrint(CC_ERROR, "There is no window on a dedicated server.");
		return true;
	}
	ShowZoneMapWindow();
	return true;
}

/** Print the money zones: capital, currency, rate, people and fees. Read only, works always. */
static bool ConZones(std::span<std::string_view> argv)
{
	if (!argv.empty() && argv.size() >= 2 && argv[1] == "?") {
		IConsolePrint(CC_HELP, "Show the money zones: capital, currency, exchange rate (1.00 = the base unit), people, and the exchange fees that deliveries into the zone paid. A delivery across a zone border pays a fee (1% + a quarter of the rate difference, at most 12%).");
		return true;
	}
	if (_zone_market.count <= 0) {
		IConsolePrint(CC_DEFAULT, "No zones yet: they are made on the first game day when the map has at least two towns far enough apart.");
		return true;
	}
	for (int i = 0; i < _zone_market.count; i++) {
		const ZoneState &z = _zone_market.zones[i];
		const Town *capital = ClosestTownFromTile(z.capital, UINT_MAX);
		const TileIndex cbt = CentralBankTile(i);
		IConsolePrint(CC_DEFAULT, "zone {}  capital {}  currency {} ({})  rate {}.{:03}  people {}  fees earned {}  tax level {}%  central bank {}", i + 1, capital != nullptr ? GetString(STR_TOWN_NAME, capital->index) : std::string("-"), ZoneCurrencyName(i), ZoneCurrencySymbol(i), z.rate_ppm / 1'000'000, (z.rate_ppm % 1'000'000) / 1'000, z.population, z.fees, z.tax_percent, cbt != INVALID_TILE ? fmt::format("at tile {},{}", TileX(cbt), TileY(cbt)) : std::string("not built yet"));
	}
	for (int a = 0; a < _zone_market.count; a++) {
		for (int b = 0; b < _zone_market.count; b++) {
			if (a == b) continue;
			const int64_t f = _zone_market.FeePpmBetween(a, b);
			IConsolePrint(CC_DEFAULT, "fee {} -> {}: {}.{:02}%", a + 1, b + 1, f / 10'000, (f % 10'000) / 100);
		}
	}
	return true;
}

/** The city wallet of the towns: treasury, debts, what the town paid for consumer cargo, business tax and grants. Read only. 'town_wallet' = the 20 biggest treasuries, 'town_wallet all' = every town. */
static bool ConTownWallet(std::span<std::string_view> argv)
{
	if (!argv.empty() && argv.size() >= 2 && argv[1] == "?") {
		IConsolePrint(CC_HELP, "Show the city wallet of the towns: treasury, loan from the central bank, municipal bonds, what the town paid for Goods / Food / Water, what it could not pay, business tax and grants. Usage: 'town_wallet' (20 towns with the biggest treasury) or 'town_wallet all'.");
		return true;
	}
	const bool all = argv.size() >= 2 && argv[1] == "all";
	std::vector<std::pair<int64_t, uint32_t>> order;
	for (const auto &[key, a] : _town_treasury.accounts) if (a.started) order.emplace_back(-a.treasury, key);
	std::sort(order.begin(), order.end());
	if (order.empty()) { IConsolePrint(CC_DEFAULT, "No town has a treasury yet."); return true; }
	size_t shown = 0;
	int64_t loans = 0, bonds = 0, paid = 0, lacked = 0, tax = 0, grants = 0;
	for (const auto &[neg, key] : order) {
		const TownAccount &a = _town_treasury.accounts[key];
		loans += a.loan_debt; bonds += a.bond_debt; paid += a.goods_paid; lacked += a.shortfall; tax += a.business_tax; grants += a.grants;
		if (!all && shown >= 20) continue;
		shown++;
		const TownID id{static_cast<uint16_t>(key)};
		const std::string name = Town::IsValidID(id) ? GetString(STR_TOWN_NAME, id) : std::string("-");
		IConsolePrint(CC_DEFAULT, "{}  treasury {}  loan {}  bonds {}  paid for goods {}  could not pay {}  business tax {}  grants {}  interest paid {}  fares {}  mail {}  unpaid fares/mail {}  wages back {}  tax back {}", name, a.treasury, a.loan_debt, a.bond_debt, a.goods_paid, a.shortfall, a.business_tax, a.grants, a.interest_paid, a.fares_paid, a.mail_paid, a.fare_shortfall, a.wages_back, a.tax_back);
	}
	IConsolePrint(CC_DEFAULT, "all {} towns: treasuries {}  loans {}  bonds {}  paid for goods {}  could not pay {}  business tax {}  grants {}", order.size(), _town_treasury.GetTotalTreasury(), loans, bonds, paid, lacked, tax, grants);
	return true;
}

/** Exchange cash for banknotes of a zone, or banknotes back for cash: 'zone_buy <zone 1-4> <money>', 'zone_sell <zone 1-4> <notes|all>'. */
static bool ConZoneExchange(std::span<std::string_view> argv, bool sell)
{
	if (argv.size() < 3) {
		IConsolePrint(CC_HELP, sell ? "Sell banknotes of a money zone for cash. Usage: 'zone_sell <zone 1-4> <notes|all>' (0.3% spread)." : "Buy banknotes of a money zone with cash. Usage: 'zone_buy <zone 1-4> <money>' (0.3% spread). Notes earn no interest: you gain or lose only by the exchange rate.");
		return true;
	}
	if (!Company::IsValidID(_local_company)) { IConsolePrint(CC_ERROR, "You need a company."); return true; }
	int zone = 0;
	const auto rz = std::from_chars(argv[1].data(), argv[1].data() + argv[1].size(), zone);
	if (rz.ec != std::errc() || zone < 1 || zone > _zone_market.count) { IConsolePrint(CC_ERROR, "No such zone."); return true; }
	int64_t amount = 0;
	if (sell && argv[2] == "all") {
		amount = _zone_market.holdings[_local_company.base()][zone - 1];
	} else {
		const auto ra = std::from_chars(argv[2].data(), argv[2].data() + argv[2].size(), amount);
		if (ra.ec != std::errc() || amount <= 0) { IConsolePrint(CC_ERROR, "The amount must be more than zero."); return true; }
	}
	if (amount <= 0) { IConsolePrint(CC_DEFAULT, "You hold no notes of that zone."); return true; }
	Command<CMD_ZONE_EXCHANGE>::Post(STR_ZONE_EXCHANGE_ERROR, static_cast<uint8_t>(zone - 1), sell, Money(amount));
	return true;
}

static bool ConZoneBuy(std::span<std::string_view> argv) { return ConZoneExchange(argv, false); }
static bool ConZoneSell(std::span<std::string_view> argv) { return ConZoneExchange(argv, true); }

/** Set the tax level of a zone: 'zone_tax <zone 1-4> <percent 0-300>', 100 = the base rates of tax.h. Single player only (it changes the game). */
static bool ConZoneTax(std::span<std::string_view> argv)
{
	if (argv.empty() || (argv.size() >= 2 && argv[1] == "?")) {
		IConsolePrint(CC_HELP, "Set the tax level of a money zone in percent of the base rates (100 = the base). Usage: 'zone_tax <zone> <percent>', zone 1-4, percent 0-300. It scales the land tax, transport tax and VAT of the zone, and the income, capital gains and dividend tax of the companies with headquarters there. Single player only. 'zones' shows the levels.");
		return true;
	}
	if (_networking) {
		IConsolePrint(CC_ERROR, "zone_tax is for single player only (it would differ between the players).");
		return true;
	}
	if (argv.size() != 3) {
		IConsolePrint(CC_ERROR, "Usage: zone_tax <zone> <percent>");
		return true;
	}
	int zone = 0;
	int percent = 0;
	const auto r1 = std::from_chars(argv[1].data(), argv[1].data() + argv[1].size(), zone);
	const auto r2 = std::from_chars(argv[2].data(), argv[2].data() + argv[2].size(), percent);
	if (r1.ec != std::errc() || r2.ec != std::errc() || zone < 1 || zone > _zone_market.count || percent < 0 || percent > 300) {
		IConsolePrint(CC_ERROR, "Zone must be 1 to {} and percent 0 to 300.", _zone_market.count);
		return true;
	}
	_zone_market.zones[zone - 1].tax_percent = percent;
	IConsolePrint(CC_DEFAULT, "zone {}: tax level {}%", zone, percent);
	return true;
}

/** Building permits: 'zone_permit' lists the zones with the fee and the days left, 'zone_permit <zone>' buys or renews the permit of the local company. Works in multiplayer (it is a command). */
static bool ConZonePermit(std::span<std::string_view> argv)
{
	if (argv.empty() || (argv.size() >= 2 && argv[1] == "?")) {
		IConsolePrint(CC_HELP, "Building permits of the money zones. Usage: 'zone_permit' (list) or 'zone_permit <zone>' (buy or renew the permit of your company for that zone). In the zone of your headquarters you build at the normal price; in another zone every construction costs more (with a permit +50% scaled by the tax level of the zone, without one +400%). A permit lasts 180 game days; the fee is paid once.");
		return true;
	}
	if (_game_mode != GM_NORMAL || _zone_market.count <= 0) {
		IConsolePrint(CC_ERROR, "The zones are not made yet.");
		return true;
	}
	if (argv.size() == 1) {
		const int home = _zone_market.ZoneOfCompany(_local_company);
		for (int z = 0; z < _zone_market.count; z++) {
			if (z == home) {
				IConsolePrint(CC_DEFAULT, "zone {} ({}): home zone, normal prices", z + 1, ZoneCurrencyName(z));
				continue;
			}
			const int64_t left = _zone_permits.DaysLeft(_local_company, z);
			IConsolePrint(CC_DEFAULT, "zone {} ({}): fee {}, {}", z + 1, ZoneCurrencyName(z), permitmath::Fee(_zone_market.zones[z].tax_percent),
				left > 0 ? fmt::format("permit valid for {} more days (construction +{}%)", left, permitmath::SurchargePercent(true, _zone_market.zones[z].tax_percent))
				         : fmt::format("no permit (construction +{}%)", permitmath::SurchargePercent(false, _zone_market.zones[z].tax_percent)));
		}
		return true;
	}
	int zone = 0;
	const auto r = std::from_chars(argv[1].data(), argv[1].data() + argv[1].size(), zone);
	if (r.ec != std::errc() || zone < 1 || zone > _zone_market.count) {
		IConsolePrint(CC_ERROR, "Zone must be 1 to {}.", _zone_market.count);
		return true;
	}
	Command<CMD_ZONE_PERMIT>::Post(STR_ZONE_PERMIT_ERROR_CAPTION, static_cast<uint8_t>(zone - 1));
	return true;
}

/** Read an industry from a console argument: an industry id, or the tile index of any tile of the industry (shown in the land area information window). */
static bool ParseIndustryArg(std::string_view arg, IndustryID &out)
{
	uint32_t value = 0;
	const auto [end, error] = std::from_chars(arg.data(), arg.data() + arg.size(), value);
	if (error != std::errc() || end != arg.data() + arg.size()) return false;
	if (value < 0xFFFF && Industry::IsValidID(IndustryID{static_cast<uint16_t>(value)})) {
		out = IndustryID{static_cast<uint16_t>(value)};
		return true;
	}
	if (value < Map::Size() && IsTileType(TileIndex{value}, MP_INDUSTRY)) {
		out = GetIndustryIndex(TileIndex{value});
		return true;
	}
	return false;
}

/** Owners of industries (step 1 of the contract system). 'industry_owner' lists the industries that a company owns, 'industry_owner <id>' shows one industry with its owner and price. Read only. */
static bool ConIndustryOwner(std::span<std::string_view> argv)
{
	if (argv.empty() || (argv.size() >= 2 && argv[1] == "?")) {
		IConsolePrint(CC_HELP, "Owners of industries. Usage: 'industry_owner' (the industries that a company owns, and what was paid to the system) or 'industry_owner <industry id or tile index>' (owner, upgrade level and the price of the right of ownership). An industry that nobody owns belongs to the system (NPC). Buy one with 'industry_buy <industry id or tile index>' (unborrowed cash only).");
		return true;
	}
	if (_game_mode != GM_NORMAL) {
		IConsolePrint(CC_ERROR, "This works in a running game only.");
		return true;
	}
	const auto describe = [](const Industry *i) {
		const CompanyID owner = GetIndustryOwner(i->index);
		const int64_t price = GetIndustryOwnershipPrice(i);
		IConsolePrint(CC_DEFAULT, "industry {} at {} x {}: owner {}, level {}, {}", i->index.base(), TileX(i->location.tile), TileY(i->location.tile),
			owner == CompanyID::Invalid() ? std::string("system") : fmt::format("company {}", owner.base() + 1), GetIndustryLevel(i->index),
			price > 0 ? fmt::format("price {}", price) : std::string("not for sale"));
	};
	if (argv.size() == 1) {
		int owned = 0;
		for (const Industry *i : Industry::Iterate()) {
			if (GetIndustryOwner(i->index) == CompanyID::Invalid()) continue;
			describe(i);
			owned++;
		}
		IConsolePrint(CC_DEFAULT, "{} industries have a company as owner; paid to the system so far: {}", owned, _industry_owners.system_income);
		return true;
	}
	IndustryID iid;
	if (!ParseIndustryArg(argv[1], iid)) {
		IConsolePrint(CC_ERROR, "'{}' is not an industry id or a tile index of an industry in this game.", argv[1]);
		return true;
	}
	describe(Industry::Get(iid));
	return true;
}

/** Found a new industry at a tile, paid by the local company like 'Fund new industry' in the game. 'industry_build' lists the types. Works in multiplayer (it is a command). */
static bool ConIndustryBuild(std::span<std::string_view> argv)
{
	if (argv.empty() || (argv.size() >= 2 && argv[1] == "?")) {
		IConsolePrint(CC_HELP, "Found a new industry. Usage: 'industry_build' (list the types), 'industry_build <type> <tile index>' (tile index is shown in the land area information window; the game's own rules for the place and the price apply). Then buy it with 'industry_buy <tile index>'.");
		return true;
	}
	if (_game_mode != GM_NORMAL) {
		IConsolePrint(CC_ERROR, "This works in a running game only.");
		return true;
	}
	if (argv.size() == 1) {
		for (IndustryType t = 0; t < NUM_INDUSTRYTYPES; t++) {
			const IndustrySpec *spec = GetIndustrySpec(t);
			if (!spec->enabled) continue;
			IConsolePrint(CC_DEFAULT, "{}: {}", t, GetString(spec->name));
		}
		return true;
	}
	uint32_t type = 0, tile = 0;
	const auto r1 = std::from_chars(argv[1].data(), argv[1].data() + argv[1].size(), type);
	if (argv.size() != 3 || r1.ec != std::errc() || r1.ptr != argv[1].data() + argv[1].size() || type >= NUM_INDUSTRYTYPES || !GetIndustrySpec(static_cast<IndustryType>(type))->enabled) {
		IConsolePrint(CC_ERROR, "Usage: 'industry_build <type> <tile index>'. 'industry_build' lists the types.");
		return true;
	}
	const auto r2 = std::from_chars(argv[2].data(), argv[2].data() + argv[2].size(), tile);
	if (r2.ec != std::errc() || r2.ptr != argv[2].data() + argv[2].size() || tile >= Map::Size()) {
		IConsolePrint(CC_ERROR, "'{}' is not a tile index of this map.", argv[2]);
		return true;
	}
	const IndustrySpec *indsp = GetIndustrySpec(static_cast<IndustryType>(type));
	const uint32_t seed = InteractiveRandom();
	const uint32_t layout = InteractiveRandomRange(static_cast<uint32_t>(indsp->layouts.size()));
	Command<CMD_BUILD_INDUSTRY>::Post(STR_ERROR_CAN_T_CONSTRUCT_THIS_INDUSTRY, TileIndex{tile}, static_cast<IndustryType>(type), layout, false, seed);
	return true;
}

/** Buy the right of ownership of an industry from the system with the local company. Works in multiplayer (it is a command). */
static bool ConIndustryBuy(std::span<std::string_view> argv)
{
	if (argv.size() != 2 || argv[1] == "?") {
		IConsolePrint(CC_HELP, "Buy the right of ownership of an industry from the system. Usage: 'industry_buy <industry id or tile index>'. The price is the output of one month times the market price times 12, paid from unborrowed cash. See 'industry_owner <id>' for the price.");
		return true;
	}
	if (_game_mode != GM_NORMAL) {
		IConsolePrint(CC_ERROR, "This works in a running game only.");
		return true;
	}
	IndustryID iid;
	if (!ParseIndustryArg(argv[1], iid)) {
		IConsolePrint(CC_ERROR, "'{}' is not an industry id or a tile index of an industry in this game.", argv[1]);
		return true;
	}
	Command<CMD_BUY_INDUSTRY_OWNERSHIP>::Post(STR_INDUSTRY_OWNER_ERROR_CAPTION, iid);
	return true;
}

/** Contracts (step 3 of the contract system). Helpers: whole numbers, and a price like 4, 4.5 or 4.25 read as 1/100 of a money unit (integers only, no floating point). */
static bool ContractParseUint(std::string_view s, uint32_t &out)
{
	if (s.empty()) return false;
	const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), out);
	return error == std::errc() && end == s.data() + s.size();
}

static bool ContractParsePrice(std::string_view s, uint32_t &cents)
{
	const size_t dot = s.find('.');
	uint32_t whole = 0;
	uint32_t frac = 0;
	if (!ContractParseUint(s.substr(0, dot), whole) || whole > 10'000'000) return false;
	if (dot != std::string_view::npos) {
		const std::string_view f = s.substr(dot + 1);
		if (f.empty() || f.size() > 2 || !ContractParseUint(f, frac)) return false;
		if (f.size() == 1) frac *= 10;
	}
	cents = whole * 100 + frac;
	return true;
}

static std::string ContractCargoName(uint8_t cargo)
{
	return cargo < NUM_CARGO && CargoSpec::Get(cargo)->IsValid() ? GetString(CargoSpec::Get(cargo)->name) : std::string("?");
}

static const char *ContractStatusName(contract::Status s)
{
	switch (s) {
		case contract::Status::Offered: return "offered";
		case contract::Status::Countered: return "countered";
		case contract::Status::Active: return "active";
		case contract::Status::Refused: return "refused";
		case contract::Status::Cancelled: return "cancelled";
		case contract::Status::Expired: return "expired";
		case contract::Status::Completed: return "completed";
	}
	return "?";
}

/** 'contract' lists the contracts of your company (as buyer or as owner of the industry). 'contract <industry id or tile index>' shows the output of an industry, what is booked and what is free. Read only. */
static bool ConContract(std::span<std::string_view> argv)
{
	if (argv.empty() || (argv.size() >= 2 && argv[1] == "?") || argv.size() > 2) {
		IConsolePrint(CC_HELP, "Contracts. Usage: 'contract' (your contracts and offers) or 'contract <industry id or tile index>' (output, booked and free output of an industry). Make one with contract_offer, answer with contract_counter / contract_accept / contract_refuse, end it with contract_cancel.");
		return true;
	}
	if (_game_mode != GM_NORMAL) {
		IConsolePrint(CC_ERROR, "This works in a running game only.");
		return true;
	}
	if (argv.size() == 2) {
		uint32_t value = 0;
		if (!ContractParseUint(argv[1], value) || value >= 0xFFFF || !Industry::IsValidID(IndustryID{static_cast<uint16_t>(value)})) {
			IConsolePrint(CC_ERROR, "'{}' is not an industry id of this game.", argv[1]);
			return true;
		}
		const Industry *i = Industry::Get(IndustryID{static_cast<uint16_t>(value)});
		int64_t ident = 0;
		int32_t built = 0;
		ContractIndustryIdent(i, ident, built);
		const CompanyID owner = GetIndustryOwner(i->index);
		IConsolePrint(CC_DEFAULT, "industry {}: owner {}", i->index.base(), owner == CompanyID::Invalid() ? std::string("system") : fmt::format("company {}", owner.base() + 1));
		for (const auto &p : i->produced) {
			if (!IsValidCargoType(p.cargo)) continue;
			const int64_t output = ContractMonthlyOutput(i, p.cargo);
			const int64_t booked = _contracts.Reserved(i->index.base(), ident, built, p.cargo);
			IConsolePrint(CC_DEFAULT, "  cargo {} ({}): output a month {}, booked {}, free {}, one buyer may book up to {}", static_cast<int>(p.cargo), ContractCargoName(p.cargo), output, booked,
				contract::FreeOf(output, booked), contract::BuyerCapOf(output));
		}
		return true;
	}
	if (!Company::IsValidID(_local_company)) {
		IConsolePrint(CC_ERROR, "You need a company.");
		return true;
	}
	const int32_t year = CompanyRoleYear(_local_company);
	int shown = 0;
	for (const auto &[id, c] : _contracts.rows) {
		uint8_t seller = contract::SYSTEM;
		int32_t seller_year = 0;
		ContractCurrentSeller(c, seller, seller_year);
		const bool is_buyer = c.buyer == _local_company.base() && c.buyer_year == year;
		const bool is_seller = seller == _local_company.base() && seller_year == year;
		if (!is_buyer && !is_seller) continue;
		IConsolePrint(CC_DEFAULT, "#{} {}: industry {} cargo {} ({}), {} a month at {}.{:02}, {} months{}, buyer company {}, seller {}, round {}{}, escrow {}{}", c.id, ContractStatusName(c.status), c.industry,
			static_cast<int>(c.cargo), ContractCargoName(c.cargo), c.terms.qty, c.terms.price_cents / 100, c.terms.price_cents % 100, c.terms.months,
			c.status == contract::Status::Active ? fmt::format(" ({} left)", c.months_left) : std::string(), c.buyer + 1,
			seller == contract::SYSTEM ? std::string("system") : fmt::format("company {}", seller + 1), c.round,
			contract::IsOpen(c.status) ? fmt::format(", waiting for the {}", c.turn == contract::Side::Seller ? "seller" : "buyer") : std::string(), c.escrow,
			c.status == contract::Status::Active ? fmt::format(", delivered {} this month, short {} month(s) in a row", c.delivered, c.miss_streak) : std::string());
		shown++;
	}
	IConsolePrint(CC_DEFAULT, "{} contract(s) shown", shown);
	return true;
}

/** 'contract_offer <industry id or tile index> <cargo id> <amount a month> <price per unit> <months>': offer a contract to the owner of an industry. Works in multiplayer (it is a command). */
static bool ConContractOffer(std::span<std::string_view> argv)
{
	if (argv.size() != 6 || argv[1] == "?") {
		IConsolePrint(CC_HELP, "Offer a contract. Usage: 'contract_offer <industry id or tile index> <cargo id> <amount a month> <price per unit, like 4.50> <months>'. The deposit (one month of the contract) is taken when the contract is accepted. See 'contract <industry id or tile index>' for the cargo ids and the free output.");
		return true;
	}
	if (_game_mode != GM_NORMAL) {
		IConsolePrint(CC_ERROR, "This works in a running game only.");
		return true;
	}
	uint32_t industry = 0, cargo = 0, qty = 0, price = 0, months = 0;
	if (!ContractParseUint(argv[1], industry) || industry >= 0xFFFF || !Industry::IsValidID(IndustryID{static_cast<uint16_t>(industry)})) {
		IConsolePrint(CC_ERROR, "'{}' is not an industry id of this game.", argv[1]);
		return true;
	}
	if (!ContractParseUint(argv[2], cargo) || cargo >= NUM_CARGO || !ContractParseUint(argv[3], qty) || !ContractParsePrice(argv[4], price) || !ContractParseUint(argv[5], months) || months > 255) {
		IConsolePrint(CC_ERROR, "Cargo id, amount, price and months must be numbers (the price may have 2 decimals).");
		return true;
	}
	Command<CMD_CONTRACT_OFFER>::Post(STR_CONTRACT_ERROR_CAPTION, IndustryID{static_cast<uint16_t>(industry)}, static_cast<uint8_t>(cargo), qty, price, static_cast<uint8_t>(months));
	return true;
}

/** 'contract_counter <contract id> <amount a month> <price per unit> <months>': answer with new terms (round 2 or 3). */
static bool ConContractCounter(std::span<std::string_view> argv)
{
	if (argv.size() != 5 || argv[1] == "?") {
		IConsolePrint(CC_HELP, "Answer a contract with new terms. Usage: 'contract_counter <contract id> <amount a month> <price per unit, like 4.50> <months>'. There are at most 3 rounds.");
		return true;
	}
	if (_game_mode != GM_NORMAL) {
		IConsolePrint(CC_ERROR, "This works in a running game only.");
		return true;
	}
	uint32_t id = 0, qty = 0, price = 0, months = 0;
	if (!ContractParseUint(argv[1], id) || !ContractParseUint(argv[2], qty) || !ContractParsePrice(argv[3], price) || !ContractParseUint(argv[4], months) || months > 255) {
		IConsolePrint(CC_ERROR, "The contract id, amount, price and months must be numbers (the price may have 2 decimals).");
		return true;
	}
	Command<CMD_CONTRACT_COUNTER>::Post(STR_CONTRACT_ERROR_CAPTION, id, qty, price, static_cast<uint8_t>(months));
	return true;
}

/** The three commands that need only a contract id: accept, refuse, cancel. */
template <Commands Cmd>
static bool ConContractById(std::span<std::string_view> argv, const char *help)
{
	if (argv.size() != 2 || argv[1] == "?") {
		IConsolePrint(CC_HELP, "{}", help);
		return true;
	}
	if (_game_mode != GM_NORMAL) {
		IConsolePrint(CC_ERROR, "This works in a running game only.");
		return true;
	}
	uint32_t id = 0;
	if (!ContractParseUint(argv[1], id)) {
		IConsolePrint(CC_ERROR, "'{}' is not a contract id.", argv[1]);
		return true;
	}
	Command<Cmd>::Post(STR_CONTRACT_ERROR_CAPTION, id);
	return true;
}

static bool ConContractAccept(std::span<std::string_view> argv) { return ConContractById<CMD_CONTRACT_ACCEPT>(argv, "Accept the contract that waits for you. Usage: 'contract_accept <contract id>'. The buyer pays the deposit (one month of the contract) from unborrowed cash."); }
static bool ConContractRefuse(std::span<std::string_view> argv) { return ConContractById<CMD_CONTRACT_REFUSE>(argv, "Refuse the contract that waits for you. Usage: 'contract_refuse <contract id>'."); }
/** Open the contracts window (step 4). */
static bool ConContractWindow(std::span<std::string_view> argv)
{
	if (argv.size() >= 2 && argv[1] == "?") {
		IConsolePrint(CC_HELP, "Open the contracts window: your offers and contracts, with buttons to accept, refuse, counter and cancel. Usage: 'contract_window'.");
		return true;
	}
	if (_game_mode != GM_NORMAL) {
		IConsolePrint(CC_ERROR, "This works in a running game only.");
		return true;
	}
	if (_network_dedicated) {
		IConsolePrint(CC_ERROR, "There is no window on a dedicated server.");
		return true;
	}
	ShowContractWindow();
	return true;
}

static bool ConContractCancel(std::span<std::string_view> argv) { return ConContractById<CMD_CONTRACT_CANCEL>(argv, "Withdraw an offer, or (as the buyer) end an active contract. Usage: 'contract_cancel <contract id>'. Ending an active contract early costs the deposit, unless 2 months in a row were short."); }

/** Roles of companies (step 2 of the contract system). 'company_role' lists every company with its role, Dealer mode and the vehicles that its role forbids;
 * 'company_role transport' / 'company_role industry' changes the role of the local company; 'company_dealer on|off' switches Dealer mode. Works in multiplayer (commands). */
static bool ConCompanyRole(std::span<std::string_view> argv)
{
	if (argv.size() >= 2 && argv[1] == "?") {
		IConsolePrint(CC_HELP, "Roles of companies. Usage: 'company_role' (list), 'company_role transport' or 'company_role industry' (change the role of your company). An industry owner may run road vehicles only. See also 'company_dealer on|off'.");
		return true;
	}
	if (_game_mode != GM_NORMAL) {
		IConsolePrint(CC_ERROR, "This works in a running game only.");
		return true;
	}
	if (argv.size() == 1) {
		for (const Company *c : Company::Iterate()) {
			const crole::Role role = GetCompanyRole(c->index);
			IConsolePrint(CC_DEFAULT, "company {}: {}{}, vehicles that the industry-owner role forbids: {}", c->index.base() + 1,
				role == crole::Role::IndustryOwner ? "industry owner" : "transport owner", IsCompanyDealer(c->index) ? " (Dealer mode ON)" : "",
				CountVehiclesForbiddenFor(c->index, crole::Role::IndustryOwner));
		}
		return true;
	}
	if (argv[1] != "transport" && argv[1] != "industry") {
		IConsolePrint(CC_ERROR, "'{}' is not a role. Use 'transport' or 'industry'.", argv[1]);
		return true;
	}
	Command<CMD_SET_COMPANY_ROLE>::Post(STR_COMPANY_ROLE_ERROR_CAPTION, static_cast<uint8_t>(argv[1] == "industry" ? crole::Role::IndustryOwner : crole::Role::TransportOwner));
	return true;
}

/** Dealer mode of the local company. */
static bool ConCompanyDealer(std::span<std::string_view> argv)
{
	if (argv.size() != 2 || (argv[1] != "on" && argv[1] != "off")) {
		IConsolePrint(CC_HELP, "Switch Dealer mode of your company (a transport owner only). Usage: 'company_dealer on' or 'company_dealer off'.");
		return true;
	}
	if (_game_mode != GM_NORMAL) {
		IConsolePrint(CC_ERROR, "This works in a running game only.");
		return true;
	}
	Command<CMD_SET_DEALER_MODE>::Post(STR_COMPANY_ROLE_DEALER_ERROR_CAPTION, argv[1] == "on");
	return true;
}

static bool ConCargoPrices(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Show the cargo prices of the mod. Usage: 'cargo_prices [town id]'. Per cargo: price now (100% = the normal pay of OpenTTD), the freight rate (the price of the service: waiting cargo and fleet size), normal and recent deliveries per day, game days counted. Then the fuel index and the push of the news on each group of cargo. With a town id: also the local factor of that town for each cargo (the demand of the town of the destination station) and the price a delivery there pays.");
		return true;
	}
	/* Optional town id: show the local factor of that town too. */
	bool with_town = false;
	TownID town{};
	if (argv.size() >= 2) {
		const std::string_view text = argv[1];
		uint32_t value = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (error != std::errc() || end != text.data() + text.size() || value >= 0xFFFF || !Town::IsValidID(TownID{static_cast<uint16_t>(value)})) {
			IConsolePrint(CC_ERROR, "'{}' is not a town id of this game.", text);
			return true;
		}
		town = TownID{static_cast<uint16_t>(value)};
		with_town = true;
		IConsolePrint(CC_DEFAULT, "Local factor of the town {} (people follow the town fully, raw material little):", GetString(STR_TOWN_NAME, town));
	}
	for (const CargoSpec *cs : CargoSpec::Iterate()) {
		const size_t i = static_cast<size_t>(cs->Index());
		if (i >= _cargo_market.cargo.size()) continue;
		const cargomarket::CargoState &c = _cargo_market.cargo[i];
		IConsolePrint(CC_DEFAULT, "{}: price {}.{:02}%  freight {}.{:02}%  normal {}.{:02}/day  recent {}.{:02}/day  waiting {}.{:02}  days {}", GetString(cs->name),
				c.price_ppm / 10000, c.price_ppm % 10000 / 100, c.freight_ppm / 10000, c.freight_ppm % 10000 / 100, c.ref_x100 / 100, c.ref_x100 % 100,
				c.fast_x100 / 100, c.fast_x100 % 100, c.backlog_x100 / 100, c.backlog_x100 % 100, c.days);
		if (with_town) {
			const int64_t local = CargoMarketLocalFactorPpm(cs->Index(), town);
			const int64_t total = c.price_ppm * c.freight_ppm / cargomarket::PPM * local / cargomarket::PPM;
			IConsolePrint(CC_DEFAULT, "    local {}.{:02}%  paid there {}.{:02}%", local / 10000, local % 10000 / 100, total / 10000, total % 10000 / 100);
		}
	}
	/* The push of the news on the price goal of each group of cargo (coal shortage, bad harvest ...), now. */
	const int64_t minute = static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes());
	{
		/* The fuel index (news FuelSpike / FuelDrop): above 1.0 every running vehicle pays an extra daily cost and the income of a delivery rises by 60% of the excess. */
		const int64_t fuel = _market_events.FuelIndexPpm(minute);
		const int64_t surcharge = cargomarket::FuelIncomeFactorPpm(fuel) - cargomarket::PPM;
		IConsolePrint(CC_DEFAULT, "fuel index {}.{:02}%  extra daily cost {}.{:02}% of the base cost per vehicle  income surcharge +{}.{:02}%", fuel / 10000, fuel % 10000 / 100,
				cargomarket::FuelExcessPpm(fuel) / 10000, cargomarket::FuelExcessPpm(fuel) % 10000 / 100, surcharge / 10000, surcharge % 10000 / 100);
	}
	for (size_t g = static_cast<size_t>(MarketCargoGroup::Oil); g < static_cast<size_t>(MarketCargoGroup::Count); g++) {
		const int64_t push = _market_events.CargoPushPpm(static_cast<MarketCargoGroup>(g), minute);
		IConsolePrint(CC_DEFAULT, "news push on {}: {}{}.{:02}%", MarketCargoGroupName(static_cast<MarketCargoGroup>(g)), push < 0 ? "-" : "+", (push < 0 ? -push : push) / 10000, (push < 0 ? -push : push) % 10000 / 100);
	}
	return true;
}

/** Print what moves the gold price: every factor of the target, the limit, and the price now. Read only, works always. */
static bool ConGoldFactors(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Show what moves the gold price. Usage: 'gold_factors'. Prints the factors of the target price (inflation, cash, demand, scarcity, safe haven), the target before and after the limit, the price now, the price at midnight and the market stock.");
		return true;
	}
	if (_gold_reserve.GetPriceCents() <= 0) {
		IConsolePrint(CC_ERROR, "The gold market has not started yet.");
		return true;
	}
	const GoldFactors f = _gold_reserve.GetFactors();
	auto pct = [](int64_t ppm) { return fmt::format("{}.{:02}%", ppm / 10000, (ppm < 0 ? -ppm : ppm) % 10000 / 100); };
	auto money = [](int64_t cents) { return fmt::format("{}.{:02}", cents / 100, (cents < 0 ? -cents : cents) % 100); };
	IConsolePrint(CC_DEFAULT, "inflation {}  cash {}  demand {}  scarcity {}  safe haven {}", pct(f.inflation_ppm), pct(f.cash_ppm), pct(f.demand_ppm), pct(f.scarcity_ppm), pct(f.haven_ppm));
	IConsolePrint(CC_DEFAULT, "slow waves (bull/bear) {} (not part of the price when PRICE_USES_CYCLE is false)  chance move of today {} ppm (0: the price has its own random step every 2 minutes)", pct(f.cycle_ppm), f.noise_ppm);
	IConsolePrint(CC_DEFAULT, "anchor (base x inflation) {}  target before limit {}  target {}", money(f.anchor_cents), money(f.raw_cents), money(f.target_cents));
	IConsolePrint(CC_DEFAULT, "price now {}  price at midnight {}  pressure {} ppm (news) + trade impact {} ppm  market stock {} oz  history days {}", money(_gold_reserve.GetPriceCents()), money(_gold_reserve.GetDayStartCents()), _gold_reserve.pressure_ppm, _gold_reserve.impact_ppm, _gold_reserve.market_stock_oz, _gold_reserve.history_cents.size());
	IConsolePrint(CC_DEFAULT, "diamond stock {} carats  diamond scarcity {}", _valuables.diamond_stock_carats, pct(_valuables.DiamondScarcityPpm()));
	IConsolePrint(CC_DEFAULT, "oil stock {} barrels  oil scarcity {}  oil price index {}  fuel pass {}%", _valuables.oil_stock_barrels, pct(_valuables.OilScarcityPpm()), pct(OilPriceIndexPpm()), ValuablesManager::OIL_FUEL_PASS_PERCENT);
	return true;
}

/** Register the commands. Called from IConsoleStdLibRegister(). */
/** 'stock_event [kind]': make a random market event now (test tool). Without a number the kind is drawn by weight. */
static bool ConStockEvent(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Make a market news event now (test tool, single player, after 'stock_dev on'). Usage: 'stock_event' (a random kind) or 'stock_event <0-44>' (a kind: 0 boom, 1 recession, 2 inflation, 3 rate cut hope, 4 war, 5 peace, 6 bank scare, 7 gold mine found (REAL: the ounces that mines delivered), 8 mine strike, 9 central banks buy gold, 10 jewellery demand, 11 fuel up, 12 fuel down, 13 technology, 14 tourism, 15 storm, 16 company revenue up, 17 company profit down, 18 company distress, 19 company profit up, 20 company revenue down, 21 company loss, 22 company value up, 23 company value down, 24 company bankruptcy, 25 rate hike, 26 central banks sell gold (REAL: the bank really sells half of its gold), 27 jewellery demand drop, 28 profit taking, 29 coal shortage, 30 coal glut, 31 harvest failure, 32 bumper harvest, 33 construction boom, 34 construction slump, 35 diamond mine found, 36 diamond demand surge, 37 town boom, 38 town grew, 39 town shrank, 40 town milestone, 41 storm, 42 meteorite, 43 drought, 44 drought over). Company kinds (16-24) are made only when TRUE for a company now, otherwise a kind is drawn by weight. Kinds 38-44 are made about the first town that has a treasury and have their real effects (treasury, income, growth). The game itself only draws war and peace by chance: every other news is true (see market_events.h).");
		return true;
	}
	if (argv.size() > 2) return false;
	if (_networking) {
		IConsolePrint(CC_ERROR, "The event test tool works in single player only.");
		return true;
	}
	if (!_stock_dev_tools) {
		IConsolePrint(CC_ERROR, "Switch the test tools on first: 'stock_dev on'.");
		return true;
	}

	int kind = -1;
	if (argv.size() == 2) {
		uint32_t value = 0;
		const std::string_view text = argv[1];
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (error != std::errc() || end != text.data() + text.size() || value >= MARKET_EVENT_KIND_COUNT) {
			IConsolePrint(CC_ERROR, "The kind must be a whole number from 0 to 44.");
			return true;
		}
		kind = static_cast<int>(value);
	}

	const MarketEvent e = FireMarketEventNow(kind);
	IConsolePrint(CC_DEFAULT, "Event {} made: kind {}, severity {}, gold {} ppm, shares {} ppm.", e.id, static_cast<int>(e.kind), static_cast<int>(e.severity), e.gold_ppm, e.share_ppm);
	return true;
}

/** Print how the four groups of citizens do with gold, all towns added up. Read only. */
static bool ConCitizenGold(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Show how the citizens trade gold. Usage: 'citizen_gold'. For each group (hoarders, speculators, overtraders, balanced savers) of all towns: gold held, cash, result of closed sales, won / lost sales and the money that went through their orders.");
		return true;
	}
	static const char *const names[citizengold::STYLE_COUNT] = { "hoarders", "speculators", "overtraders", "balanced", };
	for (size_t i = 0; i < citizengold::STYLE_COUNT; i++) {
		const citizengold::Book b = _town_treasury.GetStyleTotals(static_cast<citizengold::Style>(i));
		IConsolePrint(CC_DEFAULT, "{}: gold {}.{:02} oz  cash {}  result {}  sales won {} lost {}  turnover {}", names[i], b.gold_cz / 100, b.gold_cz % 100, b.cash, b.profit, b.wins, b.losses, b.turnover);
	}
	IConsolePrint(CC_DEFAULT, "towns with an account {}, treasuries {}", _town_treasury.accounts.size(), _town_treasury.GetTotalTreasury());
	return true;
}

void RegisterKmConsoleCommands(); /* km_system.cpp */

void RegisterStockConsoleCommands()
{
	IConsole::CmdRegister("stock_dev", ConStockDev);
	IConsole::CmdRegister("stock_skip", ConStockSkip);
	IConsole::CmdRegister("stock_event", ConStockEvent);
	IConsole::CmdRegister("cargo_prices", ConCargoPrices);
	IConsole::CmdRegister("zones", ConZones);
	IConsole::CmdRegister("town_wallet", ConTownWallet);
	IConsole::CmdRegister("zones_map", ConZonesMap);
	IConsole::CmdRegister("zone_tax", ConZoneTax);
	IConsole::CmdRegister("zone_permit", ConZonePermit);
	IConsole::CmdRegister("industry_owner", ConIndustryOwner);
	IConsole::CmdRegister("industry_buy", ConIndustryBuy);
	IConsole::CmdRegister("industry_build", ConIndustryBuild);
	IConsole::CmdRegister("company_role", ConCompanyRole);
	IConsole::CmdRegister("company_dealer", ConCompanyDealer);
	IConsole::CmdRegister("contract", ConContract);
	IConsole::CmdRegister("contract_offer", ConContractOffer);
	IConsole::CmdRegister("contract_counter", ConContractCounter);
	IConsole::CmdRegister("contract_accept", ConContractAccept);
	IConsole::CmdRegister("contract_refuse", ConContractRefuse);
	IConsole::CmdRegister("contract_cancel", ConContractCancel);
	IConsole::CmdRegister("contract_window", ConContractWindow);
	IConsole::CmdRegister("zone_lines", ConZoneLines);
	IConsole::CmdRegister("zone_buy", ConZoneBuy);
	IConsole::CmdRegister("zone_sell", ConZoneSell);
	IConsole::CmdRegister("gold_factors", ConGoldFactors);
	IConsole::CmdRegister("citizen_gold", ConCitizenGold);
	RegisterKmConsoleCommands();
}

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
#include <charconv>
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
#include "gold_reserve.h"
#include "town_treasury.h"
#include "valuables.h"
#include "cargotype.h"
#include "strings_func.h"

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

/** Print the price of every cargo: what a unit pays now against the normal pay, and the deliveries behind it. Read only, works always. */
static bool ConCargoPrices(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Show the cargo prices of the mod. Usage: 'cargo_prices'. Per cargo: price now (100% = the normal pay of OpenTTD), normal and recent deliveries per day, game days counted. Then the push of the news on each group of cargo.");
		return true;
	}
	for (const CargoSpec *cs : CargoSpec::Iterate()) {
		const size_t i = static_cast<size_t>(cs->Index());
		if (i >= _cargo_market.cargo.size()) continue;
		const cargomarket::CargoState &c = _cargo_market.cargo[i];
		IConsolePrint(CC_DEFAULT, "{}: price {}.{:02}%  normal {}.{:02}/day  recent {}.{:02}/day  days {}", GetString(cs->name),
				c.price_ppm / 10000, c.price_ppm % 10000 / 100, c.ref_x100 / 100, c.ref_x100 % 100, c.fast_x100 / 100, c.fast_x100 % 100, c.days);
	}
	/* The push of the news on the price goal of each group of cargo (coal shortage, bad harvest ...), now. */
	const int64_t minute = static_cast<int64_t>(GameTimeManager::Get().GetTotalGameMinutes());
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
	IConsolePrint(CC_DEFAULT, "slow waves (bull/bear) {}  chance move of today {} ppm", pct(f.cycle_ppm), f.noise_ppm);
	IConsolePrint(CC_DEFAULT, "anchor (base x inflation) {}  target before limit {}  target {}", money(f.anchor_cents), money(f.raw_cents), money(f.target_cents));
	IConsolePrint(CC_DEFAULT, "price now {}  price at midnight {}  pressure {} ppm  market stock {} oz  history days {}", money(_gold_reserve.GetPriceCents()), money(_gold_reserve.GetDayStartCents()), _gold_reserve.pressure_ppm, _gold_reserve.market_stock_oz, _gold_reserve.history_cents.size());
	IConsolePrint(CC_DEFAULT, "diamond stock {} carats  diamond scarcity {}", _valuables.diamond_stock_carats, pct(_valuables.DiamondScarcityPpm()));
	return true;
}

/** Register the commands. Called from IConsoleStdLibRegister(). */
/** 'stock_event [kind]': make a random market event now (test tool). Without a number the kind is drawn by weight. */
static bool ConStockEvent(std::span<std::string_view> argv)
{
	if (argv.empty()) {
		IConsolePrint(CC_HELP, "Make a market news event now (test tool, single player, after 'stock_dev on'). Usage: 'stock_event' (a random kind) or 'stock_event <0-37>' (a kind: 0 boom, 1 recession, 2 inflation, 3 rate cut hope, 4 war, 5 peace, 6 bank scare, 7 gold mine found, 8 mine strike, 9 central banks buy gold, 10 jewellery demand, 11 fuel up, 12 fuel down, 13 technology, 14 tourism, 15 storm, 16 company revenue up, 17 company profit down, 18 company distress, 19 company profit up, 20 company revenue down, 21 company loss, 22 company value up, 23 company value down, 24 company bankruptcy, 25 rate hike, 26 central banks sell gold, 27 jewellery demand drop, 28 profit taking, 29 coal shortage, 30 coal glut, 31 harvest failure, 32 bumper harvest, 33 construction boom, 34 construction slump, 35 diamond mine found, 36 diamond demand surge, 37 town boom). Company kinds (16-24) are made only when TRUE for a company now, otherwise a kind is drawn by weight.");
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
			IConsolePrint(CC_ERROR, "The kind must be a whole number from 0 to 37.");
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

void RegisterStockConsoleCommands()
{
	IConsole::CmdRegister("stock_dev", ConStockDev);
	IConsole::CmdRegister("stock_skip", ConStockSkip);
	IConsole::CmdRegister("stock_event", ConStockEvent);
	IConsole::CmdRegister("cargo_prices", ConCargoPrices);
	IConsole::CmdRegister("gold_factors", ConGoldFactors);
	IConsole::CmdRegister("citizen_gold", ConCitizenGold);
}

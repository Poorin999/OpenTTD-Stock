/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file citizen_terminal.h The citizens in the Trading Terminal: the speculators and the overtraders of a town open real leveraged positions (long or short)
 * in the symbols of the terminal (the companies and the valuables) with margin, commission, swap, stop loss and take profit, like a company does (stock_base.h).
 * Pure logic, standard library only (so citizen_terminal_test.cpp can run it outside the game). The game side (prices, fills, market impact, taxes, saving)
 * is in town_treasury.cpp. The hoarders and the balanced savers keep buying physical gold (citizen_gold.h).
 *
 * All maths is integer; the dice are a hash of (game seed, town, group, game hour), so nothing but the positions has to be saved.
 * Prices and volumes are those of the terminal: a position of volume V (units, one share / ounce / carat each) entered at price P is worth P x V.
 */

#ifndef CITIZEN_TERMINAL_H
#define CITIZEN_TERMINAL_H

#include "citizen_gold.h"
#include <vector>

namespace citizengold {

/* The terminal rules (must equal stock_base.h; town_treasury.cpp checks this with static_assert). */
static constexpr int64_t TERM_COMMISSION_PPM = 1'000;  ///< Commission when a position is opened: 0.1% of its value.
static constexpr int64_t TERM_SWAP_PPM_PER_DAY = 100;  ///< Swap per game day: 0.01% of the value.
static constexpr int64_t TERM_STOP_OUT_PERCENT = 50;   ///< Below this margin level (equity / used margin) the worst position is closed by force.

/* Habits. */
static constexpr int64_t TERM_MAX_VOLUME = 100;        ///< Largest order of a citizen group, in units: small, so the many towns together move a price, not one town.
static constexpr size_t  TERM_SPEC_MAX_POSITIONS = 2;  ///< Speculator: at most this many open positions.
static constexpr int64_t TERM_SPEC_ENTER_PPM = 12'000; ///< Speculator: price 1.2% away from the 3 day average, in the direction of the 3 day vs the 10 day average: enters.
static constexpr int64_t TERM_SPEC_EXIT_PPM = 15'000;  ///< Speculator: leaves when the price falls back 1.5% through the 3 day average.
static constexpr int64_t TERM_SPEC_SL_PPM = 30'000;    ///< Speculator: stop loss 3% from the entry price.
static constexpr int64_t TERM_SPEC_TP_PPM = 60'000;    ///< Speculator: take profit 6% from the entry price.
static constexpr int64_t TERM_SPEC_MARGIN_PERCENT = 50; ///< Speculator: puts this share of its free margin into one position.
static constexpr size_t  TERM_OVER_MAX_POSITIONS = 3;  ///< Overtrader: at most this many open positions.
static constexpr int64_t TERM_OVER_MARGIN_PERCENT = 30; ///< Overtrader: puts this share of its free margin into one position.
static constexpr int64_t TERM_OVER_CLOSE_PERMILLE = 500; ///< Overtrader: when it acts and has positions, half of the time it closes one instead of opening.
static constexpr size_t  TERM_MAX_SYMBOLS = 64;        ///< More than the symbols of the terminal.

/** One open position of a citizen group of a town. */
struct TermPos {
	Style style = Style::Speculator; ///< The group that owns it.
	uint8_t symbol = 0;              ///< Symbol of the terminal.
	bool is_long = true;             ///< Long (bought) or short (sold).
	int64_t volume = 0;              ///< Units.
	int64_t entry = 0;               ///< Price per unit at which it was opened (money units).
	int64_t sl = 0;                  ///< Stop loss price, 0 = none.
	int64_t tp = 0;                  ///< Take profit price, 0 = none.
};

/** What a group sees of one symbol. Prices in money units. */
struct TermQuote {
	bool valid = false;     ///< The symbol exists and has a price.
	int64_t bid = 0;        ///< Price at which a long is closed.
	int64_t ask = 0;        ///< Price at which a long is opened.
	int64_t mid = 0;
	int64_t fast = 0;       ///< Average of the last 3 daily closes; 0 = not enough history.
	int64_t slow = 0;       ///< Average of the last 10 daily closes; 0 = not enough history.
	int64_t divisor = 10;   ///< Leverage: margin = value / divisor.
};

/** Floating profit of a position if it is closed now (negative = loss). */
inline int64_t TermProfit(const TermPos &p, const TermQuote &q)
{
	return p.is_long ? (q.bid - p.entry) * p.volume : (p.entry - q.ask) * p.volume;
}

/** The margin that a position holds. */
inline int64_t TermMargin(const TermPos &p, int64_t divisor)
{
	return divisor > 0 ? p.entry * p.volume / divisor : 0;
}

/** True when the stop loss or the take profit of a position has been reached. */
inline bool TermLevelHit(const TermPos &p, const TermQuote &q)
{
	if (p.is_long) return (p.sl > 0 && q.bid <= p.sl) || (p.tp > 0 && q.bid >= p.tp);
	return (p.sl > 0 && q.ask >= p.sl) || (p.tp > 0 && q.ask <= p.tp);
}

/** The most units that a margin budget buys at a price (ask for a long, bid for a short is close enough: the caller passes the price). */
inline int64_t TermUnitsFor(int64_t margin_budget, int64_t price, int64_t divisor)
{
	if (margin_budget <= 0 || price <= 0 || divisor <= 0) return 0;
	return std::min(TERM_MAX_VOLUME, margin_budget * divisor / price);
}

/** What a group decided to do in this hour. */
struct TermAction {
	enum class Kind : uint8_t { None, Open, Close } kind = Kind::None;
	size_t close_index = 0;   ///< Close: index in the list of positions that was given.
	uint8_t symbol = 0;       ///< Open: symbol.
	bool is_long = true;      ///< Open: direction.
	int64_t units = 0;        ///< Open: volume.
	int64_t sl_ppm = 0;       ///< Open: stop loss distance from the entry price, 0 = none.
	int64_t tp_ppm = 0;       ///< Open: take profit distance from the entry price, 0 = none.
};

/**
 * What a group does in this game hour. The group looks at one symbol per hour (the dice pick it), so the work per hour stays small.
 * @param style Speculator or Overtrader (the other groups do nothing here).
 * @param mine The open positions of this group.
 * @param free_margin Equity minus used margin of the group, in money units.
 * @param cooldown Hours before the group may open again.
 * @param quotes The symbols (the index is the symbol number).
 * @param symbols Number of entries of quotes.
 * @param dice The hash of this group in this hour (see Mix).
 */
inline TermAction DecideTerminal(Style style, const std::vector<TermPos> &mine, int64_t free_margin, int32_t cooldown, const TermQuote *quotes, size_t symbols, uint64_t dice)
{
	TermAction act;
	if (style != Style::Speculator && style != Style::Overtrader) return act;

	if (style == Style::Speculator) {
		/* A position whose trend broke is closed. */
		for (size_t i = 0; i < mine.size(); i++) {
			const TermPos &p = mine[i];
			if (p.symbol >= symbols || !quotes[p.symbol].valid || quotes[p.symbol].fast <= 0) continue;
			const TermQuote &q = quotes[p.symbol];
			const int64_t dev = (q.mid - q.fast) * PPM / q.fast;
			if ((p.is_long && dev <= -TERM_SPEC_EXIT_PPM) || (!p.is_long && dev >= TERM_SPEC_EXIT_PPM)) {
				act.kind = TermAction::Kind::Close;
				act.close_index = i;
				return act;
			}
		}
		if (cooldown > 0 || mine.size() >= TERM_SPEC_MAX_POSITIONS) return act;
	} else {
		if (Pick(dice, 1000) >= OVER_TRADE_PERMILLE) return act;
		if (!mine.empty() && Pick(Mix(dice, 5), 1000) < TERM_OVER_CLOSE_PERMILLE) {
			act.kind = TermAction::Kind::Close;
			act.close_index = static_cast<size_t>(Pick(Mix(dice, 6), static_cast<int64_t>(mine.size())));
			return act;
		}
		if (mine.size() >= TERM_OVER_MAX_POSITIONS) return act;
	}

	/* One symbol of the valid ones. */
	size_t valid = 0;
	for (size_t i = 0; i < symbols; i++) if (quotes[i].valid) valid++;
	if (valid == 0) return act;
	size_t pick = static_cast<size_t>(Pick(Mix(dice, 7), static_cast<int64_t>(valid)));
	size_t sym = 0;
	for (size_t i = 0; i < symbols; i++) {
		if (!quotes[i].valid) continue;
		if (pick == 0) { sym = i; break; }
		pick--;
	}
	const TermQuote &q = quotes[sym];
	if (q.ask <= 0 || q.bid <= 0) return act;

	bool is_long;
	int64_t share, sl = 0, tp = 0;
	if (style == Style::Speculator) {
		if (q.fast <= 0 || q.slow <= 0) return act;
		const int64_t dev = (q.mid - q.fast) * PPM / q.fast;
		if (dev >= TERM_SPEC_ENTER_PPM && q.fast > q.slow) is_long = true;
		else if (dev <= -TERM_SPEC_ENTER_PPM && q.fast < q.slow) is_long = false;
		else return act;
		share = TERM_SPEC_MARGIN_PERCENT; sl = TERM_SPEC_SL_PPM; tp = TERM_SPEC_TP_PPM;
	} else {
		is_long = Pick(Mix(dice, 1), 2) == 0;
		if (q.fast > 0 && Pick(Mix(dice, 2), 1000) < OVER_CHASE_PERMILLE) is_long = q.mid >= q.fast; // chases the last move
		share = TERM_OVER_MARGIN_PERCENT;
	}
	const int64_t units = TermUnitsFor(free_margin * share / 100, is_long ? q.ask : q.bid, q.divisor);
	if (units <= 0) return act;
	act.kind = TermAction::Kind::Open;
	act.symbol = static_cast<uint8_t>(sym);
	act.is_long = is_long;
	act.units = units;
	act.sl_ppm = sl;
	act.tp_ppm = tp;
	return act;
}

} // namespace citizengold

#endif /* CITIZEN_TERMINAL_H */

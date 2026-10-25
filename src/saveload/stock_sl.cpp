/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file stock_sl.cpp Code handling saving and loading of the trading terminal. */

#include "../stdafx.h"

#include "saveload.h"
#include "compat/stock_sl_compat.h"

#include "../stock_base.h"
#include "../central_bank.h"
#include "../economy_news.h"
#include "../market_events.h"
#include "../wallet_base.h"
#include "../gold_reserve.h"
#include "../valuables.h"
#include "../town_treasury.h"
#include "../citizen_gold.h"
#include "../cargo_market.h"
#include "../game_time/game_time_manager.h"

#include "../safeguards.h"

/** Kinds of records in the chunk. Where a record says company, the number can also be the symbol of a valuable (STOCK_FIRST_VALUABLE and up, see STOCK_SYMBOLS). */
enum StockRecordKind : uint8_t {
	SRK_CANDLE = 0,   ///< Closed candle: a = company, ym = month, v1..v4 = open/high/low/close.
	SRK_RUNNING = 1,  ///< Running candle, same fields as SRK_CANDLE.
	SRK_PRICE = 2,    ///< Daily price: a = company, v1 = price. Only read, for savegames from before SRK_MARKET.
	SRK_POSITION = 4, ///< Open position: a = owner, b = target, ym = id, v1 = volume, v2 = price, v3 = SL, v4 = TP, v5 = type | expiry << 8.
	SRK_PENDING = 5,  ///< Pending order, same fields as SRK_POSITION.
	SRK_META = 6,     ///< Misc: ym = next order id, v1 = last seen quarter, v2 = days until the dividend is paid, v3 = quarter of that dividend.
	SRK_VALUATION = 7, ///< Valuation: a = company, v1 = book value, v2 = earnings, v3 = number of quarters.
	SRK_MARKET = 9,   ///< Market state: a = company, v1 = current price times STOCK_PRICE_SCALE, v2 = valuation price times STOCK_PRICE_SCALE, v3 = market impact.
	SRK_DAILY = 11,   ///< Closed daily candle: a = company, ym = economy date, v1..v4 = open/high/low/close.
	SRK_DAILY_RUNNING = 12, ///< Running daily candle, same fields.
	SRK_DAILY_ON = 13, ///< The Daily view was turned on: daily candles are recorded.
	SRK_BALANCE = 10, ///< Trading account of older savegames: a = company, v1 = deposited money. Only read.
	SRK_WALLET = 14,  ///< Wallet: a = owner, b = number, v1 = balance, v2 = kind.
	SRK_TIME = 19,    ///< The game clock: v1 = milliseconds since the start of the game, v2 = game year at the start (0 in older savegames).
	SRK_TIMEFRAME = 20, ///< Candle of game time: a = company, b = timeframe | 0x80 when still running, ym = samples, v1 = candle number, v2..v5 = open/high/low/close times STOCK_PRICE_SCALE.
	SRK_TIMEFRAME_VOL = 21, ///< Traded volume of the SRK_TIMEFRAME row just before it: a = company, v1 = volume. Only written when it is not 0.
	SRK_LEDGER = 22,  ///< Line of the account statement: a = company, b = line (LedgerItem), ym = economy year, v1 = income, v2 = expense.
	SRK_GOLD = 24,    ///< Gold market: v1 = price in 1/100 money units, v2 = baseline cash supply, v3 = ounces of the central bank, v4 = demand pressure in ppm, v5 = ounces for sale in the market plus 1 (0 = savegame from before the stock).
	SRK_VALUABLES = 29, ///< State of the price model of a valuable: a = index of the valuable, v1 = slow drift in ppm, v2 = fast noise in ppm, v3 = state of the random number generator (the same in every row), v4 of the diamond row = diamond stock in carats + 1 (0 = old savegame).
	SRK_POLICY = 28,  ///< Monetary policy: ym = bit 0 set when a decision was measured, bit 1 set in emergency mode, v1 = shift in bp, v2 = price index at the last decision in ppm, v3 = game day of the last decision plus 1 (0 = none), v4 = measured inflation in bp, v5 = measured growth in bp.
	SRK_EVENT = 32,   ///< Random market event (news): a = kind, b = target company (255 = none), ym = id, v1 = game minute, v2 = push on gold in ppm, v3 = push on the sentiment of the shares in ppm, v4 = severity | variant << 8, v5 = real number of a company story in ppm (0 in older savegames). Written oldest first.
	SRK_EVENT_STATE = 33, ///< State of the events: a = 255: v1 = random number generator, v2 = sentiment of the whole market in ppm, v3 = id of the next event, v4 = game minute of the last event plus 1 (0 = none). a = 0 .. 31: v1 = sentiment of that symbol in ppm.
	SRK_PAYROLL = 31, ///< Transport Payrolls report: ym = bit 0 set for the baseline, v1 = game day, v2 = average vehicles x100, v3 = actual x100, v4 = forecast x100, v5 = push on the gold demand in ppm. Written oldest first.
	SRK_BANK_LOSS = 30, ///< Debts that the central bank wrote off (bankruptcy): v1 = total written off, v2 = the part that the budget could not pay, money units.
	SRK_BANK_BUDGET = 26, ///< Budget of the central bank from trading: v1 = commission and spread of the gold trades in 1/100 money units.
	SRK_GOLD_HISTORY = 27, ///< Five daily gold prices for the chart: ym = number of the record (0, 1, 2 ...), v1..v5 = prices in 1/100 money units, oldest first (0 = no price).
	SRK_GOLD_HOLD = 25, ///< Gold of a company: a = company, v1 = ounces, v2 = what the gold cost (0 = unknown).
	SRK_BANK = 18,    ///< Loan at the central bank: a = company, v1 = debt, v2 = interest that has been added.
	SRK_BANK_RISK = 23, ///< Rate and penalty of the loan at the central bank: a = company, v1 = penalty level | in default << 8 | days paid on time << 16, v2 = rest of the daily interest, v3 = annual rate in bp, v4 = number of the game day charged last.
	SRK_POSITION_EXT = 15, ///< More data of an open position: a = owner, ym = id, v1 = trailing stop distance, v2 = month of opening.
	SRK_STATS = 16,   ///< Trading results of a company: a = company, v1 = closed trades, v2 = winning trades, v3 = total profit.
	SRK_STATS_MONTH = 17, ///< Profit of one month: a = company, ym = month, v1 = profit, v2 = trades, v3 = winning trades.
	SRK_HISTORY = 8,  ///< Closed trade: a = owner, b = target, ym = id, v1 = volume, v2 = open price, v3 = close price, v4 = profit, v5 = type | reason << 8 | closed month << 16.
	SRK_CARGO_MARKET = 35, ///< Price of one cargo: a = cargo type, ym = game days counted (0 .. 31), v1 = price in ppm, v2 = normal deliveries per day x100, v3 = recent deliveries per day x100. Written only for a cargo with a state.
	SRK_TOWN = 34,    ///< Account of a town: ym = index of the town, b = 1 when the groups have their start wealth, v4 = treasury, v5 = spent on development. v1..v3 (ounces, cost, savings) are only read from older savegames and go to the balanced group. Taxes collected are not saved.
	SRK_TOWN_POS = 37, ///< Open terminal position of a citizen group: a = group (citizengold::Style), b = symbol | 0x80 when long, ym = index of the town, v1 = volume, v2 = entry price, v3 = stop loss, v4 = take profit.
	SRK_TOWN_BOOK = 36, ///< One group of citizens of a town: a = group (citizengold::Style), ym = index of the town, v1 = gold in hundredths of an ounce, v2 = what it cost, v3 = cash, v4 = profit of closed sales, v5 = wins | losses << 24 | cooldown << 48.
};

/** One saved record of the trading terminal. */
struct StockRecord {
	uint8_t kind = 0;
	uint8_t a = 0;
	uint8_t b = 0;
	int32_t ym = 0;
	int64_t v1 = 0;
	int64_t v2 = 0;
	int64_t v3 = 0;
	int64_t v4 = 0;
	int64_t v5 = 0;
};

static const SaveLoad _stock_desc[] = {
	SLE_VAR(StockRecord, kind, SLE_UINT8),
	SLE_VAR(StockRecord, a,    SLE_UINT8),
	SLE_VAR(StockRecord, b,    SLE_UINT8),
	SLE_VAR(StockRecord, ym,   SLE_INT32),
	SLE_VAR(StockRecord, v1,   SLE_INT64),
	SLE_VAR(StockRecord, v2,   SLE_INT64),
	SLE_VAR(StockRecord, v3,   SLE_INT64),
	SLE_VAR(StockRecord, v4,   SLE_INT64),
	SLE_VAR(StockRecord, v5,   SLE_INT64),
};

static StockRecord CandleRecord(uint8_t kind, uint8_t company, const StockCandle &candle)
{
	StockRecord r;
	r.kind = kind;
	r.a = company;
	r.ym = candle.ym;
	r.v1 = candle.open;
	r.v2 = candle.high;
	r.v3 = candle.low;
	r.v4 = candle.close;
	r.v5 = candle.volume;
	return r;
}

static StockRecord OrderRecord(uint8_t kind, const StockOrder &o)
{
	StockRecord r;
	r.kind = kind;
	r.a = o.owner;
	r.b = o.target;
	r.ym = static_cast<int32_t>(o.id);
	r.v1 = o.volume;
	r.v2 = o.price;
	r.v3 = o.sl;
	r.v4 = o.tp;
	r.v5 = static_cast<int64_t>(o.type) | (static_cast<int64_t>(o.expiry) << 8) | (static_cast<int64_t>(o.wallet) << 40);
	return r;
}

struct STCKChunkHandler : ChunkHandler {
	STCKChunkHandler() : ChunkHandler('STCK', CH_TABLE) {}

	void Save() const override
	{
		SlTableHeader(_stock_desc);

		int index = 0;
		auto write = [&](StockRecord record) {
			SlSetArrayIndex(index++);
			SlObject(&record, _stock_desc);
		};

		StockRecord meta;
		meta.kind = SRK_META;
		meta.ym = static_cast<int32_t>(_stock_market.next_id);
		meta.v1 = _stock_market.dividend_quarter_id;
		meta.v2 = _stock_market.dividend_delay;
		meta.v3 = _stock_market.dividend_quarter;
		write(meta);

		{
			StockRecord t;
			t.kind = SRK_TIME;
			const GameTimeSaveState clock = GameTimeManager::Get().GetSaveState();
			t.v1 = static_cast<int64_t>(clock.total_ms);
			t.v2 = clock.start_year;
			write(t);
		}

		/* The candles of game time (1h, 4h, 12h). Every candle is one row, with a second row for its volume when there is one. */
		for (uint8_t i = 0; i < STOCK_SYMBOLS; i++) {
			for (const TimeframeRecord &tr : _stock_market.frames[i].Export()) {
				StockRecord r;
				r.kind = SRK_TIMEFRAME;
				r.a = i;
				r.b = static_cast<uint8_t>(tr.timeframe | (tr.running ? 0x80 : 0));
				r.ym = static_cast<int32_t>(std::min<uint32_t>(tr.bucket.samples, INT32_MAX));
				r.v1 = static_cast<int64_t>(tr.bucket.index);
				r.v2 = tr.bucket.open;
				r.v3 = tr.bucket.high;
				r.v4 = tr.bucket.low;
				r.v5 = tr.bucket.close;
				write(r);
				if (tr.bucket.volume != 0) {
					StockRecord v;
					v.kind = SRK_TIMEFRAME_VOL;
					v.a = i;
					v.v1 = tr.bucket.volume;
					write(v);
				}
			}
		}

		for (const Wallet &w : _wallets.wallets) {
			StockRecord r;
			r.kind = SRK_WALLET;
			r.a = w.owner;
			r.b = w.id;
			r.v1 = w.balance;
			r.v2 = static_cast<int64_t>(w.kind);
			r.v3 = static_cast<int64_t>(w.leverage) + 1; // 0 in older savegames: the default level.
			write(r);
		}

		if (_stock_market.record_daily) {
			StockRecord on;
			on.kind = SRK_DAILY_ON;
			write(on);
		}

		for (uint8_t i = 0; i < STOCK_SYMBOLS; i++) {
			for (const StockCandle &candle : _stock_market.candles[i]) write(CandleRecord(SRK_CANDLE, i, candle));
			if (_stock_market.has_current[i]) write(CandleRecord(SRK_RUNNING, i, _stock_market.current[i]));
			if (_stock_market.price_fp[i] > 0) {
				StockRecord r;
				r.kind = SRK_MARKET;
				r.a = i;
				r.v1 = _stock_market.price_fp[i];
				r.v2 = _stock_market.base_fp[i];
				r.v3 = _stock_market.impact[i];
				write(r);
			}
			for (const StockCandle &candle : _stock_market.daily[i]) write(CandleRecord(SRK_DAILY, i, candle));
			if (_stock_market.has_daily_current[i]) write(CandleRecord(SRK_DAILY_RUNNING, i, _stock_market.daily_current[i]));
			if (i >= MAX_COMPANIES) continue; // A valuable has no valuation.
			const StockValuation &valuation = _stock_market.valuation[i];
			if (valuation.quarters > 0) {
				StockRecord r;
				r.kind = SRK_VALUATION;
				r.a = i;
				r.v1 = valuation.book;
				r.v2 = valuation.earnings;
				r.v3 = valuation.quarters;
				write(r);
			}
		}

		/* The price model of the valuables: one row each, the state of the random number generator is the same in every row. */
		if (_valuables.rng_state != 0 || _valuables.diamond_stock_carats != ValuablesManager::DIAMOND_STOCK_START) {
			for (size_t k = 0; k < VALUABLE_COUNT; k++) {
				StockRecord r;
				r.kind = SRK_VALUABLES;
				r.a = static_cast<uint8_t>(k);
				r.v1 = _valuables.drift_ppm[k];
				r.v2 = _valuables.noise_ppm[k];
				r.v3 = static_cast<int64_t>(_valuables.rng_state);
				if (k == static_cast<size_t>(ValuableId::Diamond)) r.v4 = _valuables.diamond_stock_carats + 1; // 0 = a savegame from before: the start stock.
				write(r);
			}
		}

		for (const StockOrder &o : _stock_market.positions) write(OrderRecord(SRK_POSITION, o));
		for (const StockOrder &o : _stock_market.pending) write(OrderRecord(SRK_PENDING, o));

		for (const StockOrder &o : _stock_market.positions) {
			if (o.trail == 0 && o.open_ym == 0) continue;
			StockRecord r;
			r.kind = SRK_POSITION_EXT;
			r.a = o.owner;
			r.ym = static_cast<int32_t>(o.id);
			r.v1 = o.trail;
			r.v2 = o.open_ym;
			write(r);
		}

		for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
			const BankAccount &b = _central_bank.accounts[i];
			if (b.debt == 0 && b.interest_paid == 0) continue;
			StockRecord r;
			r.kind = SRK_BANK;
			r.a = i;
			r.v1 = b.debt;
			r.v2 = b.interest_paid;
			write(r);
		}

		/* The penalty status of the loans: without it a default would be forgotten when the game is loaded. */
		for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
			const BankAccount &b = _central_bank.accounts[i];
			if (b.penalty_level == 0 && !b.in_default && b.clean_days == 0 && b.carry == 0 && b.rate_bp == 0 && b.last_charged_day < 0) continue;
			StockRecord r;
			r.kind = SRK_BANK_RISK;
			r.a = i;
			r.v1 = static_cast<int64_t>(b.penalty_level) | (b.in_default ? int64_t{1} << 8 : 0) | (static_cast<int64_t>(b.clean_days) << 16);
			r.v2 = b.carry;
			r.v3 = b.rate_bp;
			r.v4 = b.last_charged_day;
			write(r);
		}

		if (_central_bank.policy.last_decision_day >= 0) {
			const MonetaryPolicy &p = _central_bank.policy;
			StockRecord r;
			r.kind = SRK_POLICY;
			r.ym = (p.measured ? 1 : 0) | (p.emergency ? 2 : 0); // Bit 0: measured. Bit 1: emergency mode.
			r.v1 = p.shift_bp;
			r.v2 = p.prev_index_ppm;
			r.v3 = p.last_decision_day + 1;
			r.v4 = p.inflation_bp;
			r.v5 = p.growth_bp;
			write(r);
		}

		for (auto it = _payroll_news.releases.rbegin(); it != _payroll_news.releases.rend(); ++it) {
			StockRecord r;
			r.kind = SRK_PAYROLL;
			r.ym = it->baseline ? 1 : 0;
			r.v1 = it->day;
			r.v2 = it->avg_x100;
			r.v3 = it->actual_x100;
			r.v4 = it->forecast_x100;
			r.v5 = it->gold_ppm;
			write(r);
		}

		{
			StockRecord st;
			st.kind = SRK_EVENT_STATE;
			st.a = 255;
			st.v1 = static_cast<int64_t>(_market_events.rng_state);
			st.v2 = _market_events.market_sentiment_ppm;
			st.v3 = _market_events.next_id;
			st.v4 = _market_events.last_minute + 1;
			write(st);
			for (size_t i = 0; i < MARKET_SENTIMENT_SLOTS; i++) {
				if (_market_events.sentiment_ppm[i] == 0) continue;
				StockRecord ss;
				ss.kind = SRK_EVENT_STATE;
				ss.a = static_cast<uint8_t>(i);
				ss.v1 = _market_events.sentiment_ppm[i];
				write(ss);
			}
		}

		for (auto it = _market_events.log.rbegin(); it != _market_events.log.rend(); ++it) {
			StockRecord r;
			r.kind = SRK_EVENT;
			r.a = static_cast<uint8_t>(it->kind);
			r.b = it->target;
			r.ym = static_cast<int32_t>(it->id);
			r.v1 = it->minute;
			r.v2 = it->gold_ppm;
			r.v3 = it->share_ppm;
			r.v4 = static_cast<int64_t>(it->severity) | (static_cast<int64_t>(it->variant) << 8);
			r.v5 = it->fact_ppm;
			write(r);
		}

		for (size_t i = 0; i < _cargo_market.cargo.size(); i++) {
			const cargomarket::CargoState &c = _cargo_market.cargo[i];
			if (c.days == 0) continue;
			StockRecord r;
			r.kind = SRK_CARGO_MARKET;
			r.a = static_cast<uint8_t>(i);
			r.ym = c.days;
			r.v1 = c.price_ppm;
			r.v2 = c.ref_x100;
			r.v3 = c.fast_x100;
			write(r);
		}

		for (const auto &[key, a] : _town_treasury.accounts) {
			if (!a.started && a.treasury == 0 && a.spent == 0) continue;
			StockRecord r;
			r.kind = SRK_TOWN;
			r.b = a.started ? 1 : 0;
			r.ym = static_cast<int32_t>(key);
			r.v4 = a.treasury;
			r.v5 = a.spent;
			write(r);
			for (size_t i = 0; i < citizengold::STYLE_COUNT; i++) {
				const citizengold::Book &bk = a.books[i];
				StockRecord br;
				br.kind = SRK_TOWN_BOOK;
				br.a = static_cast<uint8_t>(i);
				br.ym = static_cast<int32_t>(key);
				br.v1 = bk.gold_cz;
				br.v2 = bk.cost;
				br.v3 = bk.cash;
				br.v4 = bk.profit;
				br.v5 = (static_cast<int64_t>(std::clamp<int32_t>(bk.wins, 0, 0xFFFFFF))) | (static_cast<int64_t>(std::clamp<int32_t>(bk.losses, 0, 0xFFFFFF)) << 24) |
						(static_cast<int64_t>(std::clamp<int32_t>(bk.cooldown, 0, 255)) << 48);
				write(br);
			}
			for (const citizengold::TermPos &tp : a.positions) {
				StockRecord pr;
				pr.kind = SRK_TOWN_POS;
				pr.a = static_cast<uint8_t>(tp.style);
				pr.b = static_cast<uint8_t>((tp.symbol & 0x7F) | (tp.is_long ? 0x80 : 0));
				pr.ym = static_cast<int32_t>(key);
				pr.v1 = tp.volume;
				pr.v2 = tp.entry;
				pr.v3 = tp.sl;
				pr.v4 = tp.tp;
				write(pr);
			}
		}

		if (_central_bank.written_off != 0 || _central_bank.uncovered_loss != 0) {
			StockRecord r;
			r.kind = SRK_BANK_LOSS;
			r.v1 = _central_bank.written_off;
			r.v2 = _central_bank.uncovered_loss;
			write(r);
		}

		{
			/* Always written: a savegame without this record is from before the bank had its own money and gets the start capital. */
			StockRecord r;
			r.kind = SRK_BANK_BUDGET;
			r.v1 = _central_bank.trading_revenue_cents;
			write(r);
		}

		if (_gold_reserve.price_cents > 0) {
			StockRecord r;
			r.kind = SRK_GOLD;
			r.v1 = _gold_reserve.price_cents;
			r.v2 = _gold_reserve.baseline_supply;
			r.v3 = _gold_reserve.bank_reserve_oz;
			r.v4 = _gold_reserve.pressure_ppm;
			r.v5 = _gold_reserve.market_stock_oz + 1;
			write(r);
		}
		/* The daily prices of the gold chart, five to a record. */
		for (size_t i = 0; i < _gold_reserve.history_cents.size(); i += 5) {
			StockRecord r;
			r.kind = SRK_GOLD_HISTORY;
			r.ym = static_cast<int32_t>(i / 5);
			int64_t *fields[5] = {&r.v1, &r.v2, &r.v3, &r.v4, &r.v5};
			for (size_t k = 0; k < 5 && i + k < _gold_reserve.history_cents.size(); k++) *fields[k] = _gold_reserve.history_cents[i + k];
			write(r);
		}
		for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
			if (_gold_reserve.holdings_oz[i] == 0) continue;
			StockRecord r;
			r.kind = SRK_GOLD_HOLD;
			r.a = i;
			r.v1 = _gold_reserve.holdings_oz[i];
			r.v2 = _gold_reserve.holdings_cost[i];
			write(r);
		}

		for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
			for (const auto &[year, row] : _ledger.years[i]) {
				for (size_t item = 0; item < LEDGER_ITEMS; item++) {
					if (row[item].income == 0 && row[item].expense == 0) continue;
					StockRecord r;
					r.kind = SRK_LEDGER;
					r.a = i;
					r.b = static_cast<uint8_t>(item);
					r.ym = year;
					r.v1 = row[item].income;
					r.v2 = row[item].expense;
					write(r);
				}
			}
		}

		for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
			const StockStats &s = _stock_market.stats[i];
			if (s.closed == 0 && s.months.empty()) continue;
			StockRecord r;
			r.kind = SRK_STATS;
			r.a = i;
			r.v1 = s.closed;
			r.v2 = s.wins;
			r.v3 = s.total_profit;
			write(r);
			for (const StockMonthProfit &m : s.months) {
				StockRecord mr;
				mr.kind = SRK_STATS_MONTH;
				mr.a = i;
				mr.ym = m.ym;
				mr.v1 = m.profit;
				mr.v2 = m.trades;
				mr.v3 = m.wins;
				write(mr);
			}
		}

		for (const StockClosedTrade &t : _stock_market.history) {
			/* The month of opening is saved as the number of months before the closing, plus one; 0 means not known. */
			const int64_t open_months = t.open_ym > 0 ? std::clamp<int64_t>(static_cast<int64_t>(t.closed_ym) - t.open_ym + 1, 1, 0x7FFF) : 0;
			StockRecord r;
			r.kind = SRK_HISTORY;
			r.a = t.owner;
			r.b = t.target;
			r.ym = static_cast<int32_t>(t.id);
			r.v1 = t.volume;
			r.v2 = t.open_price;
			r.v3 = t.close_price;
			r.v4 = t.profit;
			r.v5 = static_cast<int64_t>(t.type) | (static_cast<int64_t>(t.reason) << 8) | (static_cast<int64_t>(t.closed_ym) << 16) | (static_cast<int64_t>(t.wallet) << 40) | (open_months << 48);
			write(r);
		}
	}

	void Load() const override
	{
		const std::vector<SaveLoad> slt = SlCompatTableHeader(_stock_desc, _stock_sl_compat);

		_stock_market.Reset();

		bool have_stats = false;
		std::array<std::vector<TimeframeRecord>, STOCK_SYMBOLS> frame_rows{}; ///< Candles of game time, imported after the last row.
		int index;
		while ((index = SlIterateArray()) != -1) {
			StockRecord r;
			SlObject(&r, slt);

			switch (r.kind) {
				case SRK_META:
					_stock_market.next_id = static_cast<uint32_t>(std::max<int32_t>(r.ym, 1));
					_stock_market.dividend_quarter_id = static_cast<int32_t>(std::max<int64_t>(r.v1, 0));
					_stock_market.dividend_delay = static_cast<int32_t>(std::clamp<int64_t>(r.v2, 0, STOCK_DIVIDEND_DELAY_DAYS));
					_stock_market.dividend_quarter = static_cast<uint8_t>(std::clamp<int64_t>(r.v3, 0, 4));
					break;

				case SRK_CANDLE:
				case SRK_RUNNING: {
					if (r.a >= STOCK_SYMBOLS) break;
					StockCandle candle{r.ym, r.v1, r.v2, r.v3, r.v4};
					candle.volume = std::max<int64_t>(r.v5, 0);
					if (r.kind == SRK_CANDLE) {
						_stock_market.candles[r.a].push_back(candle);
					} else {
						_stock_market.current[r.a] = candle;
						_stock_market.has_current[r.a] = true;
					}
					break;
				}

				case SRK_PRICE:
					/* Older savegames: the price is the same as the valuation price, without market impact. */
					if (r.a < MAX_COMPANIES && r.v1 > 0) {
						_stock_market.price_fp[r.a] = r.v1 * STOCK_PRICE_SCALE;
						_stock_market.base_fp[r.a] = r.v1 * STOCK_PRICE_SCALE;
					}
					break;

				case SRK_MARKET:
					if (r.a < STOCK_SYMBOLS && r.v1 > 0 && r.v2 > 0) {
						_stock_market.price_fp[r.a] = r.v1;
						_stock_market.base_fp[r.a] = r.v2;
						_stock_market.impact[r.a] = std::clamp<int64_t>(r.v3, -STOCK_IMPACT_MAX_PPM, STOCK_IMPACT_MAX_PPM);
					}
					break;

				case SRK_DAILY_ON:
					_stock_market.record_daily = true;
					break;

				case SRK_DAILY:
				case SRK_DAILY_RUNNING: {
					if (r.a >= STOCK_SYMBOLS) break;
					StockCandle candle{r.ym, r.v1, r.v2, r.v3, r.v4};
					candle.volume = std::max<int64_t>(r.v5, 0);
					if (r.kind == SRK_DAILY) {
						_stock_market.daily[r.a].push_back(candle);
					} else {
						_stock_market.daily_current[r.a] = candle;
						_stock_market.has_daily_current[r.a] = true;
					}
					break;
				}

				case SRK_BALANCE:
					/* Older savegames had one trading balance per company: it goes to the first trading wallet. */
					if (r.a < MAX_COMPANIES) {
						_wallets.EnsureDefaults(CompanyID{r.a});
						Wallet *w = _wallets.FindFirst(CompanyID{r.a}, WalletKind::Trading);
						if (w != nullptr) w->balance += std::max<int64_t>(r.v1, 0);
					}
					break;

				case SRK_WALLET: {
					if (r.a >= MAX_COMPANIES || r.b == WALLET_CASH) break;
					const WalletKind kind = static_cast<WalletKind>(std::clamp<int64_t>(r.v2, 0, static_cast<int64_t>(WalletKind::End)));
					if (kind != WalletKind::Trading && kind != WalletKind::Fuel) break;
					Wallet *existing = _wallets.Find(CompanyID{r.a}, r.b);
					if (existing == nullptr) {
						Wallet w;
						w.id = r.b;
						w.owner = r.a;
						w.kind = kind;
						w.balance = std::max<int64_t>(r.v1, 0);
						w.leverage = (r.v3 >= 1 && r.v3 <= WALLET_LEVERAGE_COUNT) ? static_cast<uint8_t>(r.v3 - 1) : WALLET_LEVERAGE_DEFAULT;
						_wallets.wallets.push_back(w);
					} else {
						existing->kind = kind;
						existing->balance = std::max<int64_t>(r.v1, 0);
						existing->leverage = (r.v3 >= 1 && r.v3 <= WALLET_LEVERAGE_COUNT) ? static_cast<uint8_t>(r.v3 - 1) : WALLET_LEVERAGE_DEFAULT;
					}
					break;
				}

				case SRK_VALUATION:
					if (r.a < MAX_COMPANIES) {
						StockValuation &valuation = _stock_market.valuation[r.a];
						valuation.book = r.v1;
						valuation.earnings = r.v2;
						valuation.quarters = static_cast<uint8_t>(std::clamp<int64_t>(r.v3, 0, static_cast<int64_t>(STOCK_EARNINGS_QUARTERS)));
					}
					break;

				case SRK_POSITION:
				case SRK_PENDING: {
					if (r.a >= MAX_COMPANIES || r.b >= STOCK_SYMBOLS) break;
					const int64_t order_type = r.v5 & 0xFF;
					if (order_type >= static_cast<int64_t>(StockOrderType::End)) break;
					StockOrder o;
					o.id = static_cast<uint32_t>(r.ym);
					o.owner = r.a;
					o.target = r.b;
					o.type = static_cast<StockOrderType>(order_type);
					o.volume = static_cast<uint32_t>(r.v1);
					o.price = r.v2;
					o.sl = r.v3;
					o.tp = r.v4;
					o.wallet = static_cast<uint8_t>((r.v5 >> 40) & 0xFF);
					if (r.kind == SRK_PENDING) o.expiry = static_cast<uint32_t>(std::clamp<int64_t>((r.v5 >> 8) & 0xFFFFFFFF, 0, STOCK_MAX_EXPIRY_DAYS));
					(r.kind == SRK_POSITION ? _stock_market.positions : _stock_market.pending).push_back(o);
					break;
				}

				case SRK_HISTORY: {
					if (r.a >= MAX_COMPANIES || r.b >= STOCK_SYMBOLS) break;
					const int64_t type = r.v5 & 0xFF;
					const int64_t reason = (r.v5 >> 8) & 0xFF;
					if (type != static_cast<int64_t>(StockOrderType::Buy) && type != static_cast<int64_t>(StockOrderType::Sell)) break;
					if (reason >= static_cast<int64_t>(StockCloseReason::End)) break;
					StockClosedTrade t;
					t.id = static_cast<uint32_t>(r.ym);
					t.owner = r.a;
					t.target = r.b;
					t.type = static_cast<StockOrderType>(type);
					t.reason = static_cast<StockCloseReason>(reason);
					t.volume = static_cast<uint32_t>(r.v1);
					t.open_price = r.v2;
					t.close_price = r.v3;
					t.profit = r.v4;
					t.closed_ym = static_cast<int32_t>((r.v5 >> 16) & 0xFFFFFF);
					t.wallet = static_cast<uint8_t>((r.v5 >> 40) & 0xFF);
					const int64_t open_months = (r.v5 >> 48) & 0x7FFF;
					t.open_ym = open_months > 0 ? static_cast<int32_t>(std::max<int64_t>(t.closed_ym - (open_months - 1), 1)) : 0;
					_stock_market.history.push_back(t);
					break;
				}

				case SRK_POSITION_EXT:
					for (StockOrder &o : _stock_market.positions) {
						if (o.id != static_cast<uint32_t>(r.ym) || o.owner != r.a) continue;
						o.trail = std::max<int64_t>(r.v1, 0);
						o.open_ym = static_cast<int32_t>(std::max<int64_t>(r.v2, 0));
						break;
					}
					break;

				case SRK_TIME: {
					GameTimeSaveState clock;
					clock.total_ms = static_cast<uint64_t>(std::max<int64_t>(r.v1, 0));
					clock.start_year = r.v2 > 0 ? static_cast<int>(std::min<int64_t>(r.v2, GameTimeManager::MAX_START_YEAR)) : GameTimeManager::DEFAULT_START_YEAR;
					GameTimeManager::Get().LoadSaveState(clock);
					break;
				}

				case SRK_TIMEFRAME: {
					if (r.a >= STOCK_SYMBOLS) break;
					TimeframeRecord tr;
					tr.timeframe = r.b & 0x7F;
					tr.running = (r.b & 0x80) != 0;
					tr.bucket.samples = static_cast<uint32_t>(std::max<int32_t>(r.ym, 0));
					tr.bucket.index = static_cast<uint64_t>(std::max<int64_t>(r.v1, 0));
					tr.bucket.open = r.v2;
					tr.bucket.high = r.v3;
					tr.bucket.low = r.v4;
					tr.bucket.close = r.v5;
					frame_rows[r.a].push_back(tr);
					break;
				}

				case SRK_TIMEFRAME_VOL:
					if (r.a >= STOCK_SYMBOLS || frame_rows[r.a].empty()) break;
					frame_rows[r.a].back().bucket.volume = std::max<int64_t>(r.v1, 0);
					break;

				case SRK_BANK:
					if (r.a >= MAX_COMPANIES) break;
					_central_bank.accounts[r.a].debt = std::max<int64_t>(r.v1, 0);
					_central_bank.accounts[r.a].interest_paid = std::max<int64_t>(r.v2, 0);
					break;

				case SRK_BANK_RISK: {
					if (r.a >= MAX_COMPANIES) break;
					BankAccount &b = _central_bank.accounts[r.a];
					b.penalty_level = static_cast<uint8_t>(std::clamp<int64_t>(r.v1 & 0xFF, 0, BANK_PENALTY_LEVEL_MAX));
					b.in_default = ((r.v1 >> 8) & 1) != 0;
					b.clean_days = static_cast<uint16_t>(std::clamp<int64_t>((r.v1 >> 16) & 0xFFFF, 0, UINT16_MAX));
					b.carry = std::clamp<int64_t>(r.v2, 0, BANK_INTEREST_DIVISOR - 1);
					b.rate_bp = std::clamp<int64_t>(r.v3, 0, BANK_PENALTY_MAX_BP + BANK_PENALTY_EMERGENCY_EXTRA_BP);
					b.last_charged_day = std::max<int64_t>(r.v4, -1);
					break;
				}

				case SRK_POLICY: {
					MonetaryPolicy &p = _central_bank.policy;
					p.shift_bp = std::clamp<int64_t>(r.v1, POLICY_SHIFT_MIN_BP, POLICY_EMERGENCY_SHIFT_MAX_BP);
					p.prev_index_ppm = std::max<int64_t>(r.v2, 0);
					p.last_decision_day = std::max<int64_t>(r.v3 - 1, -1);
					p.inflation_bp = std::clamp<int64_t>(r.v4, -2000, 5000);
					p.growth_bp = std::clamp<int64_t>(r.v5, -10'000, 400'000);
					p.measured = (r.ym & 1) != 0;
					p.emergency = (r.ym & 2) != 0;
					break;
				}

				case SRK_VALUABLES:
					if (r.a >= VALUABLE_COUNT) break;
					_valuables.drift_ppm[r.a] = std::clamp<int64_t>(r.v1, -ValuablesManager::Info(r.a).drift_max_ppm, ValuablesManager::Info(r.a).drift_max_ppm);
					_valuables.noise_ppm[r.a] = std::clamp<int64_t>(r.v2, -4 * ValuablesManager::Info(r.a).noise_ppm, 4 * ValuablesManager::Info(r.a).noise_ppm);
					if (r.v3 != 0) _valuables.rng_state = static_cast<uint64_t>(r.v3);
					if (r.a == static_cast<size_t>(ValuableId::Diamond) && r.v4 > 0) _valuables.diamond_stock_carats = r.v4 - 1;
					break;

				case SRK_PAYROLL: {
					PayrollRelease rel;
					rel.baseline = (r.ym & 1) != 0;
					rel.day = std::max<int64_t>(r.v1, 0);
					rel.avg_x100 = std::max<int64_t>(r.v2, 0);
					rel.actual_x100 = r.v3;
					rel.forecast_x100 = r.v4;
					rel.gold_ppm = std::clamp<int64_t>(r.v5, -PAYROLL_GOLD_MAX_PPM, PAYROLL_GOLD_MAX_PPM);
					_payroll_news.releases.push_front(rel); // Records come oldest first: the newest ends up in front.
					if (_payroll_news.releases.size() > PAYROLL_KEEP) _payroll_news.releases.pop_back();
					_payroll_news.last_day = _payroll_news.releases.front().day;
					break;
				}

				case SRK_EVENT: {
					if (r.a >= MARKET_EVENT_KIND_COUNT) break;
					MarketEvent ev;
					ev.kind = static_cast<MarketEventKind>(r.a);
					ev.target = r.b;
					ev.id = static_cast<uint32_t>(std::max<int32_t>(r.ym, 1));
					ev.minute = std::max<int64_t>(r.v1, 0);
					ev.gold_ppm = std::clamp<int64_t>(r.v2, -MARKET_EVENT_GOLD_MAX_PPM, MARKET_EVENT_GOLD_MAX_PPM);
					ev.share_ppm = std::clamp<int64_t>(r.v3, -MARKET_EVENT_SHARE_MAX_PPM, MARKET_EVENT_SHARE_MAX_PPM);
					ev.severity = static_cast<uint8_t>(std::clamp<int64_t>(r.v4 & 0xFF, 1, 3));
					ev.variant = static_cast<uint8_t>(std::clamp<int64_t>((r.v4 >> 8) & 0xFF, 0, 11));
					ev.fact_ppm = std::clamp<int64_t>(r.v5, -MARKET_FACT_CAP_PPM, MARKET_FACT_CAP_PPM);
					_market_events.log.push_front(ev); // Records come oldest first: the newest ends up in front.
					if (_market_events.log.size() > MARKET_EVENT_KEEP) _market_events.log.pop_back();
					break;
				}

				case SRK_EVENT_STATE:
					if (r.a == 255) {
						_market_events.rng_state = static_cast<uint64_t>(r.v1);
						_market_events.market_sentiment_ppm = std::clamp<int64_t>(r.v2, -MARKET_SENTIMENT_MAX_PPM, MARKET_SENTIMENT_MAX_PPM);
						_market_events.next_id = static_cast<uint32_t>(std::clamp<int64_t>(r.v3, 1, UINT32_MAX));
						_market_events.last_minute = std::max<int64_t>(r.v4, 0) - 1;
					} else if (r.a < MARKET_SENTIMENT_SLOTS) {
						_market_events.sentiment_ppm[r.a] = std::clamp<int64_t>(r.v1, -MARKET_SENTIMENT_MAX_PPM, MARKET_SENTIMENT_MAX_PPM);
					}
					break;

				case SRK_CARGO_MARKET:
					if (r.a < _cargo_market.cargo.size()) {
						cargomarket::CargoState &c = _cargo_market.cargo[r.a];
						c.days = std::clamp<int32_t>(r.ym, 0, cargomarket::BASELINE_DAYS + 1);
						c.price_ppm = std::clamp<int64_t>(r.v1, cargomarket::PRICE_MIN_PPM, cargomarket::PRICE_MAX_PPM);
						c.ref_x100 = std::max<int64_t>(r.v2, 0);
						c.fast_x100 = std::max<int64_t>(r.v3, 0);
					}
					break;

				case SRK_TOWN: {
					if (r.ym < 0 || r.ym > 0xFFFF) break;
					TownAccount &a = _town_treasury.accounts[static_cast<uint32_t>(r.ym)];
					a.started = r.b != 0 || r.v1 != 0 || r.v3 != 0;
					a.treasury = std::max<int64_t>(r.v4, 0);
					a.spent = std::max<int64_t>(r.v5, 0);
					/* An older savegame kept one pot: its gold (whole ounces) and savings go to the balanced group. */
					if (r.v1 != 0 || r.v3 != 0) {
						citizengold::Book &bal = a.books[static_cast<size_t>(citizengold::Style::Balanced)];
						bal.gold_cz += std::max<int64_t>(r.v1, 0) * citizengold::CZ_PER_OZ;
						bal.cost += std::max<int64_t>(r.v2, 0);
						bal.cash += std::max<int64_t>(r.v3, 0);
					}
					break;
				}

				case SRK_TOWN_BOOK: {
					if (r.ym < 0 || r.ym > 0xFFFF || r.a >= citizengold::STYLE_COUNT) break;
					citizengold::Book &bk = _town_treasury.accounts[static_cast<uint32_t>(r.ym)].books[r.a];
					bk.gold_cz = std::max<int64_t>(r.v1, 0);
					bk.cost = std::max<int64_t>(r.v2, 0);
					bk.cash = std::max<int64_t>(r.v3, 0);
					bk.profit = r.v4;
					bk.wins = static_cast<int32_t>(r.v5 & 0xFFFFFF);
					bk.losses = static_cast<int32_t>((r.v5 >> 24) & 0xFFFFFF);
					bk.cooldown = static_cast<int32_t>((r.v5 >> 48) & 0xFF);
					break;
				}

				case SRK_TOWN_POS: {
					if (r.ym < 0 || r.ym > 0xFFFF || r.a >= citizengold::STYLE_COUNT || (r.b & 0x7F) >= STOCK_SYMBOLS || r.v1 <= 0 || r.v2 <= 0) break;
					citizengold::TermPos tp;
					tp.style = static_cast<citizengold::Style>(r.a);
					tp.symbol = r.b & 0x7F;
					tp.is_long = (r.b & 0x80) != 0;
					tp.volume = r.v1;
					tp.entry = r.v2;
					tp.sl = std::max<int64_t>(r.v3, 0);
					tp.tp = std::max<int64_t>(r.v4, 0);
					_town_treasury.accounts[static_cast<uint32_t>(r.ym)].positions.push_back(tp);
					break;
				}

				case SRK_BANK_LOSS:
					_central_bank.written_off = std::max<int64_t>(r.v1, 0);
					_central_bank.uncovered_loss = std::clamp<int64_t>(r.v2, 0, _central_bank.written_off);
					break;

				case SRK_BANK_BUDGET:
					_central_bank.trading_revenue_cents = std::max<int64_t>(r.v1, 0);
					break;

				case SRK_GOLD:
					_gold_reserve.price_cents = std::max<int64_t>(r.v1, 0);
					_gold_reserve.baseline_supply = std::max<int64_t>(r.v2, 0);
					_gold_reserve.bank_reserve_oz = std::max<int64_t>(r.v3, 0);
					_gold_reserve.market_stock_oz = r.v5 <= 0 ? GoldReserveManager::MARKET_STOCK_START : r.v5 - 1;
					_gold_reserve.pressure_ppm = std::clamp<int64_t>(r.v4, -GoldReserveManager::PRESSURE_MAX_PPM, GoldReserveManager::PRESSURE_MAX_PPM);
					break;

				case SRK_GOLD_HISTORY: {
					const int64_t prices[5] = {r.v1, r.v2, r.v3, r.v4, r.v5};
					for (const int64_t price : prices) {
						if (price > 0 && _gold_reserve.history_cents.size() < GoldReserveManager::HISTORY_MAX_DAYS) _gold_reserve.history_cents.push_back(price);
					}
					break;
				}

				case SRK_GOLD_HOLD:
					if (r.a < MAX_COMPANIES) {
						_gold_reserve.holdings_oz[r.a] = std::max<int64_t>(r.v1, 0);
						_gold_reserve.holdings_cost[r.a] = std::max<int64_t>(r.v2, 0); // v2 = cost of the gold; 0 in older savegames.
					}
					break;

				case SRK_LEDGER:
					if (r.a >= MAX_COMPANIES || r.b >= LEDGER_ITEMS) break;
					_ledger.years[r.a][r.ym][r.b] = LedgerEntry{std::max<int64_t>(r.v1, 0), std::max<int64_t>(r.v2, 0)};
					break;

				case SRK_STATS:
					if (r.a >= MAX_COMPANIES) break;
					have_stats = true;
					_stock_market.stats[r.a].closed = static_cast<uint32_t>(std::max<int64_t>(r.v1, 0));
					_stock_market.stats[r.a].wins = static_cast<uint32_t>(std::max<int64_t>(r.v2, 0));
					_stock_market.stats[r.a].total_profit = r.v3;
					break;

				case SRK_STATS_MONTH: {
					if (r.a >= MAX_COMPANIES) break;
					StockMonthProfit m;
					m.ym = r.ym;
					m.profit = r.v1;
					m.trades = static_cast<uint32_t>(std::max<int64_t>(r.v2, 0));
					m.wins = static_cast<uint32_t>(std::max<int64_t>(r.v3, 0));
					_stock_market.stats[r.a].months.push_back(m);
					break;
				}

				default: break;
			}
		}

		/* The candles of game time. A list that is not valid is dropped; the candles start again from the next price. */
		for (size_t i = 0; i < STOCK_SYMBOLS; i++) {
			if (!frame_rows[i].empty()) _stock_market.frames[i].Import(frame_rows[i]);
		}

		/* Savegames from before the results were saved: count the trades of the history. */
		if (!have_stats) _stock_market.RebuildStats();

		/* Savegames from before the wallets: the trades have no wallet. They belong to the first trading wallet. */
		auto migrate = [](uint8_t owner, uint8_t &wallet) {
			if (wallet != WALLET_CASH || owner >= MAX_COMPANIES) return;
			_wallets.EnsureDefaults(CompanyID{owner});
			const Wallet *w = _wallets.FindFirst(CompanyID{owner}, WalletKind::Trading);
			if (w != nullptr) wallet = w->id;
		};
		for (StockOrder &o : _stock_market.positions) migrate(o.owner, o.wallet);
		for (StockOrder &o : _stock_market.pending) migrate(o.owner, o.wallet);
		for (StockClosedTrade &t : _stock_market.history) migrate(t.owner, t.wallet);

		std::sort(_wallets.wallets.begin(), _wallets.wallets.end(), [](const Wallet &a, const Wallet &b) {
			return a.owner != b.owner ? a.owner < b.owner : a.id < b.id;
		});
	}
};

static const STCKChunkHandler STCK;
static const ChunkHandlerRef stock_chunk_handlers[] = {
	STCK,
};

extern const ChunkHandlerTable _stock_chunk_handlers(stock_chunk_handlers);


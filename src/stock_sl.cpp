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

#include "../safeguards.h"

/** Kinds of records in the chunk. */
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
	SRK_BALANCE = 10, ///< Trading account: a = company, v1 = deposited money.
	SRK_HISTORY = 8,  ///< Closed trade: a = owner, b = target, ym = id, v1 = volume, v2 = open price, v3 = close price, v4 = profit, v5 = type | reason << 8 | closed month << 16.
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
	r.v5 = static_cast<int64_t>(o.type) | (static_cast<int64_t>(o.expiry) << 8);
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

		if (_stock_market.record_daily) {
			StockRecord on;
			on.kind = SRK_DAILY_ON;
			write(on);
		}

		for (uint8_t i = 0; i < MAX_COMPANIES; i++) {
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
			if (_stock_market.trading_balance[i] != 0) {
				StockRecord r;
				r.kind = SRK_BALANCE;
				r.a = i;
				r.v1 = _stock_market.trading_balance[i];
				write(r);
			}
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

		for (const StockOrder &o : _stock_market.positions) write(OrderRecord(SRK_POSITION, o));
		for (const StockOrder &o : _stock_market.pending) write(OrderRecord(SRK_PENDING, o));

		for (const StockClosedTrade &t : _stock_market.history) {
			StockRecord r;
			r.kind = SRK_HISTORY;
			r.a = t.owner;
			r.b = t.target;
			r.ym = static_cast<int32_t>(t.id);
			r.v1 = t.volume;
			r.v2 = t.open_price;
			r.v3 = t.close_price;
			r.v4 = t.profit;
			r.v5 = static_cast<int64_t>(t.type) | (static_cast<int64_t>(t.reason) << 8) | (static_cast<int64_t>(t.closed_ym) << 16);
			write(r);
		}
	}

	void Load() const override
	{
		const std::vector<SaveLoad> slt = SlCompatTableHeader(_stock_desc, _stock_sl_compat);

		_stock_market.Reset();

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
					if (r.a >= MAX_COMPANIES) break;
					StockCandle candle{r.ym, r.v1, r.v2, r.v3, r.v4};
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
					if (r.a < MAX_COMPANIES && r.v1 > 0 && r.v2 > 0) {
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
					if (r.a >= MAX_COMPANIES) break;
					StockCandle candle{r.ym, r.v1, r.v2, r.v3, r.v4};
					if (r.kind == SRK_DAILY) {
						_stock_market.daily[r.a].push_back(candle);
					} else {
						_stock_market.daily_current[r.a] = candle;
						_stock_market.has_daily_current[r.a] = true;
					}
					break;
				}

				case SRK_BALANCE:
					if (r.a < MAX_COMPANIES) _stock_market.trading_balance[r.a] = std::max<int64_t>(r.v1, 0);
					break;

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
					if (r.a >= MAX_COMPANIES || r.b >= MAX_COMPANIES) break;
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
					if (r.kind == SRK_PENDING) o.expiry = static_cast<uint32_t>(std::clamp<int64_t>(r.v5 >> 8, 0, STOCK_MAX_EXPIRY_DAYS));
					(r.kind == SRK_POSITION ? _stock_market.positions : _stock_market.pending).push_back(o);
					break;
				}

				case SRK_HISTORY: {
					if (r.a >= MAX_COMPANIES || r.b >= MAX_COMPANIES) break;
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
					t.closed_ym = static_cast<int32_t>(r.v5 >> 16);
					_stock_market.history.push_back(t);
					break;
				}

				default: break;
			}
		}
	}
};

static const STCKChunkHandler STCK;
static const ChunkHandlerRef stock_chunk_handlers[] = {
	STCK,
};

extern const ChunkHandlerTable _stock_chunk_handlers(stock_chunk_handlers);

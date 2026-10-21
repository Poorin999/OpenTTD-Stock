/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file stock_base.h Data of the in-game trading terminal: prices, candles, open positions and pending orders. */

#ifndef STOCK_BASE_H
#define STOCK_BASE_H

#include "company_type.h"
#include "economy_type.h"
#include "wallet_base.h"
#include <array>
#include <vector>

/** Number of (virtual) shares every company has. The mid price of one share is the company value divided by this. */
static constexpr int64_t STOCK_SHARES_PER_COMPANY = 100'000;

/** One LOT is this many shares. Volumes are stored in hundredths of a LOT, so one volume unit is exactly one share. */
static constexpr int64_t STOCK_SHARES_PER_LOT = 100;

/** Largest size of a single order, as a percentage of the shares of a company. */
static constexpr int64_t STOCK_MAX_ORDER_PERCENT = 100;

/** Largest volume of a single order, in hundredths of a LOT. 10% of 100,000 shares is 10,000 (100.00 LOT). */
static constexpr uint32_t STOCK_MAX_VOLUME = static_cast<uint32_t>(STOCK_SHARES_PER_COMPANY * STOCK_MAX_ORDER_PERCENT / 100);

/** Parts per million: the unit of the fees and of the market impact. */
static constexpr int64_t STOCK_PPM = 1'000'000;

/** Prices are tracked internally in this many steps per money unit, so that small price moves are not lost to rounding. */
static constexpr int64_t STOCK_PRICE_SCALE = 1000;

/** The lowest mid price of a share, in money units. */
static constexpr int64_t STOCK_MIN_PRICE = 2;

/** The margin needed to open a position is the value of the position divided by this (leverage 1:10). */
static constexpr int64_t STOCK_MARGIN_DIVISOR = 10;

/** Margin call: below this margin level (equity / used margin, in percent) the terminal warns the player. */
static constexpr int64_t STOCK_MARGIN_CALL_LEVEL = 100;

/** Stop out: below this margin level the positions with the biggest loss are closed by force. */
static constexpr int64_t STOCK_STOP_OUT_LEVEL = 50;

/** Valuation model: a company is worth its book value plus this many years of earnings, i.e. a P/E ratio of 15 (within the limits in StockPriceFromValuation). */
static constexpr int64_t STOCK_EARNINGS_MULTIPLE = 15;

/** Undervalued market: when the market cap is below this percentage of the book value, the spread starts to tighten. */
static constexpr int64_t STOCK_UNDERVALUED_PERCENT = 80;

/** Undervalued market: the spread never becomes smaller than this percentage of the normal spread. */
static constexpr int64_t STOCK_UNDERVALUED_MIN_SPREAD_PERCENT = 25;

/** Dividends: this share of the net earnings of a company is distributed to the holders of its shares, in percent. */
static constexpr int64_t STOCK_PAYOUT_PERCENT = 30;

/** Valuation model: the earnings are the net result of this many completed quarters, i.e. one year. */
static constexpr size_t STOCK_EARNINGS_QUARTERS = 4;

/** Dividends are paid when the third and when the fourth quarter of the economy year has been completed. */
static constexpr uint8_t STOCK_DIVIDEND_QUARTER_FIRST = 3;

/** Dividends are paid this many economy days after a quarter has ended, so that the company statistics have been rolled over for sure. */
static constexpr int32_t STOCK_DIVIDEND_DELAY_DAYS = 2;

/** Commission, charged when a trade is opened: this many parts per million of the value of the position (0.1%). */
static constexpr int64_t STOCK_COMMISSION_PPM = 1'000;

/** Swap (overnight financing), charged every economy day for every open position: parts per million of the value of the position (0.01%). */
static constexpr int64_t STOCK_SWAP_PPM_PER_DAY = 100;

/** Market impact: an order of the full size of the company moves the price by this many times its share of the company. 2 means that an order for 10% of the shares moves the price by 20%. */
static constexpr int64_t STOCK_IMPACT_FACTOR = 2;

/** Market impact: the accumulated impact can not move the price by more than this, in parts per million (30%). */
static constexpr int64_t STOCK_IMPACT_MAX_PPM = 300'000;

/** Market impact: every day the accumulated impact fades by one this-many-th, so the price slowly returns to the valuation. */
static constexpr int64_t STOCK_IMPACT_DECAY_DIV = 8;

/** The market is checked every this many game ticks (an economy day has 74 ticks), for prices, stop levels and pending orders. */
static constexpr uint32_t STOCK_TICK_INTERVAL = 8;

/** Every check the price moves this-many-th of the way to the price of the valuation, so the price glides instead of jumping once per day. */
static constexpr int64_t STOCK_GLIDE_DIV = 4;

/** Longest lifetime of a pending order, in economy days. */
static constexpr uint32_t STOCK_MAX_EXPIRY_DAYS = 3650;

/** Maximum number of closed trades that are remembered per company; the oldest ones are forgotten first. */
static constexpr size_t STOCK_MAX_HISTORY_PER_COMPANY = 100;

/** Number of months (the newest ones) of which the profit is remembered per company, for the portfolio summary. */
static constexpr size_t STOCK_MAX_STATS_MONTHS = 24;

/** Maximum number of positions plus pending orders per company. */
static constexpr size_t STOCK_MAX_ORDERS_PER_COMPANY = 40;

/** Maximum number of closed monthly candles that are remembered per company. */
static constexpr size_t STOCK_MAX_CANDLES = 240;

/** Maximum number of daily candles that are remembered per company: 365 per year for 150 years, so in practice all of them. Daily candles are only recorded after the Daily view was turned on. */
static constexpr size_t STOCK_MAX_DAILY_CANDLES = 365 * 150;

/** Kind of an order. The first two are executed at once (market execution), the others are pending orders. */
enum class StockOrderType : uint8_t {
	Buy,       ///< Market execution: buy at the Ask price.
	Sell,      ///< Market execution: sell (short) at the Bid price.
	BuyLimit,  ///< Buy when the Ask price falls to the given price.
	SellLimit, ///< Sell when the Bid price rises to the given price.
	BuyStop,   ///< Buy when the Ask price rises to the given price.
	SellStop,  ///< Sell when the Bid price falls to the given price.
	End,       ///< End marker.
};

/** Whether the order is (or becomes) a long position. */
inline bool IsStockBuy(StockOrderType type)
{
	return type == StockOrderType::Buy || type == StockOrderType::BuyLimit || type == StockOrderType::BuyStop;
}

/** One open position (when stored in StockMarket::positions) or one pending order (StockMarket::pending). */
struct StockOrder {
	uint32_t id = 0;                         ///< Unique number of the order.
	uint8_t owner = 0;                       ///< Company that owns the order.
	uint8_t target = 0;                      ///< Company whose shares (symbol) are traded.
	StockOrderType type = StockOrderType::Buy; ///< Buy or Sell for positions, any type for pending orders.
	uint32_t volume = 0;                     ///< Volume in hundredths of a LOT.
	int64_t price = 0;                       ///< Entry price of a position, or trigger price of a pending order.
	int64_t sl = 0;                          ///< Stop Loss, 0 when not set.
	int64_t tp = 0;                          ///< Take Profit, 0 when not set.
	uint32_t expiry = 0;                     ///< Pending orders only: economy days left before the order expires; 0 means it never expires.
	uint8_t wallet = 0;                      ///< Trading wallet of the owner that pays the fees of the order and gets its results.
	int64_t trail = 0;                       ///< Positions only: trailing stop distance in price units; 0 when the trailing stop is off.
	int32_t open_ym = 0;                     ///< Positions only: economy year * 12 + month in which the position was opened; 0 when not known.
};

/** One candle of the chart; covers one economy month. */
struct StockCandle {
	int32_t ym = 0;      ///< Economy year * 12 + month.
	int64_t open = 0;    ///< Price at the start of the month.
	int64_t high = 0;    ///< Highest price of the month.
	int64_t low = 0;     ///< Lowest price of the month.
	int64_t close = 0;   ///< Price at the end of the month (or the latest price for the running month).
	int64_t volume = 0;  ///< Shares traded in this period by the orders of the players (the volume bars of the chart).
};

/** Why a position was closed. */
enum class StockCloseReason : uint8_t {
	Manual,      ///< The player closed it.
	StopLoss,    ///< The Stop Loss was reached.
	TakeProfit,  ///< The Take Profit was reached.
	StopOut,     ///< Closed by force because of a margin call.
	Delisted,    ///< The company whose shares were traded disappeared.
	End,         ///< End marker.
};

/** One closed trade, as shown in the history. A partial close creates one entry for the closed part. */
struct StockClosedTrade {
	uint32_t id = 0;                              ///< Number of the trade (same as when it was open).
	uint8_t owner = 0;                            ///< Company that made the trade.
	uint8_t target = 0;                           ///< Company whose shares (symbol) were traded.
	StockOrderType type = StockOrderType::Buy;    ///< Buy (long) or Sell (short).
	StockCloseReason reason = StockCloseReason::Manual; ///< Why it was closed.
	uint32_t volume = 0;                          ///< Closed volume in hundredths of a LOT.
	int64_t open_price = 0;                       ///< Price at which it was opened.
	int64_t close_price = 0;                      ///< Price at which it was closed.
	int64_t profit = 0;                           ///< Realised profit; negative for a loss.
	int32_t closed_ym = 0;                        ///< Economy year * 12 + month of the closing.
	uint8_t wallet = 0;                           ///< Trading wallet that the result was booked in.
	int32_t open_ym = 0;                          ///< Economy year * 12 + month of the opening; 0 when not known (savegames from before).
};

/** The profit of one company in one economy month, part of #StockStats. */
struct StockMonthProfit {
	int32_t ym = 0;        ///< Economy year * 12 + month.
	int64_t profit = 0;    ///< Realised profit of the trades closed in that month; negative for a loss.
	uint32_t trades = 0;   ///< Number of trades closed in that month.
	uint32_t wins = 0;     ///< Number of those trades that made a profit.
};

/**
 * The trading results of one company. They are counted when a trade is closed, so they stay complete after the
 * oldest trades have been dropped from the history.
 */
struct StockStats {
	int64_t total_profit = 0;                ///< Realised profit of all closed trades; negative for a loss.
	uint32_t closed = 0;                     ///< Number of closed trades (a partial close counts as one trade).
	uint32_t wins = 0;                       ///< Number of closed trades with a profit above zero.
	std::vector<StockMonthProfit> months{};  ///< The newest #STOCK_MAX_STATS_MONTHS months with trades, the oldest first.
};

/** What the ranking of the companies is sorted by. */
enum class StockRankSort : uint8_t {
	MarketCap, ///< Market capitalisation, the highest first.
	Price,     ///< Price of one share, the highest first.
	Change,    ///< Change of the price since the last month, the best first.
};

/** One line of the ranking of the companies. */
struct StockRankEntry {
	CompanyID company{};         ///< The company.
	int64_t price = 0;           ///< Price of one share.
	int64_t market_cap = 0;      ///< Market capitalisation.
	int64_t change_tenths = 0;   ///< Change of the price since the last month, in tenths of a percent.
};

/** Valuation of one company, refreshed once per day. */
struct StockValuation {
	int64_t book = 0;      ///< Book value: the company value (assets minus loan).
	int64_t earnings = 0;  ///< Net result of the last year; annualised when fewer than four quarters are known.
	uint8_t quarters = 0;  ///< Number of completed quarters behind #earnings (0..4). 0 means there are no results yet.
};

/** The figures of the summary row: P/E, EPS and dividend yield. */
struct StockSummary {
	bool has_earnings = false;    ///< False when the company has no completed quarter yet: nothing can be shown.
	bool has_pe = false;          ///< False when earnings or book value are not positive: P/E is not meaningful.
	int64_t eps = 0;              ///< Earnings per share; negative for a loss.
	int64_t pe_tenths = 0;        ///< Price / earnings ratio, in tenths.
	int64_t yield_tenths = 0;     ///< Dividend yield, in tenths of a percent.
};

/** All state of the trading terminal. Saved in the 'STCK' chunk. */
struct StockMarket {
	std::array<std::vector<StockCandle>, MAX_COMPANIES> candles{}; ///< Closed candles per company.
	std::array<StockCandle, MAX_COMPANIES> current{};              ///< The running candle per company.
	std::array<bool, MAX_COMPANIES> has_current{};                 ///< Whether #current is valid.
	std::array<int64_t, MAX_COMPANIES> price_fp{};                 ///< Current mid price per share, times #STOCK_PRICE_SCALE. 0 when unknown.
	std::array<int64_t, MAX_COMPANIES> base_fp{};                  ///< Mid price per share according to the valuation, times #STOCK_PRICE_SCALE, refreshed once per day.
	std::array<int64_t, MAX_COMPANIES> impact{};                   ///< Accumulated market impact of the orders, in parts per million; positive when more was bought than sold.
	std::array<StockValuation, MAX_COMPANIES> valuation{};         ///< Valuation per company, refreshed once per day.
	std::array<std::vector<StockCandle>, MAX_COMPANIES> daily{};   ///< Closed daily candles per company; ym holds the economy date. Only filled when #record_daily is set.
	std::array<StockCandle, MAX_COMPANIES> daily_current{};        ///< The running daily candle per company.
	std::array<bool, MAX_COMPANIES> has_daily_current{};           ///< Whether #daily_current is valid.
	bool record_daily = false;                                     ///< Whether daily candles are recorded; set once when the player opens the Daily view.
	std::vector<StockClosedTrade> history{};                       ///< Closed trades of all companies, the oldest first.
	std::array<StockStats, MAX_COMPANIES> stats{};                 ///< Trading results per company (the portfolio summary).
	std::vector<StockOrder> positions{};                           ///< Open positions.
	std::vector<StockOrder> pending{};                             ///< Pending orders.
	uint32_t next_id = 1;                                          ///< Number of the next order.

	int32_t dividend_quarter_id = 0;  ///< Economy year * 4 + quarter that was last seen; 0 when not known yet.
	int32_t dividend_delay = 0;       ///< Economy days to wait before the dividend of the ended quarter is paid; 0 when nothing is due.
	uint8_t dividend_quarter = 0;     ///< Number (1..4) of the quarter that has ended and whose dividend is due.

	void Reset();
	void UpdateDaily();
	void UpdateTick();
	void CheckExecutions();

	StockOrder *FindPosition(uint32_t id, CompanyID owner);
	StockOrder *FindPending(uint32_t id, CompanyID owner);
	size_t CountOrders(CompanyID owner) const;
	size_t CountWalletOrders(CompanyID owner, uint8_t wallet) const;
	void AddHistory(const StockOrder &position, int64_t close_price, int64_t profit, StockCloseReason reason);
	int64_t ClosePosition(size_t index, uint32_t volume, int64_t close_price, StockCloseReason reason);
	void ApplyImpact(CompanyID target, bool buy, uint32_t volume);
	void RecordStats(const StockClosedTrade &trade);
	void RebuildStats();

private:
	void HandleRemovedCompanies();
	void ApplyTrailingStops();
	void CheckPositions();
	void CheckPending();
	void CheckMarginCall();
	void ChargeSwap();
	void ExpireOrders();
	void HandleDividends();
	void PayDividends(size_t quarters);
};

extern StockMarket _stock_market;

int64_t GetStockPrice(CompanyID company);
int64_t GetStockMarketCap(CompanyID company);
StockSummary GetStockSummary(CompanyID company);
int64_t GetStockBid(CompanyID company);
int64_t GetStockAsk(CompanyID company);
int64_t GetStockExecutionPrice(CompanyID company, bool buy, uint32_t volume);
int64_t GetStockCommission(int64_t value);

int32_t GetStockCurrentMonth();
int64_t GetStockReferencePrice(CompanyID company);
std::vector<StockRankEntry> GetStockRanking(StockRankSort sort);

int64_t GetStockProfit(const StockOrder &position);
int64_t GetStockMargin(const StockOrder &position);
int64_t GetStockUsedMargin(CompanyID owner, uint8_t wallet);
int64_t GetStockFloatingProfit(CompanyID owner, uint8_t wallet);
int64_t GetStockEquity(CompanyID owner, uint8_t wallet);
int64_t GetStockFreeMargin(CompanyID owner, uint8_t wallet);
int64_t GetStockMarginLevel(CompanyID owner, uint8_t wallet);
void PayStockProfit(CompanyID owner, uint8_t wallet, int64_t profit);

#endif /* STOCK_BASE_H */


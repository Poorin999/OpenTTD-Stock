/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file stock_widget.h Types related to the trading terminal widgets. */

#ifndef WIDGETS_STOCK_WIDGET_H
#define WIDGETS_STOCK_WIDGET_H

/** Widgets of the #StockWindow class (the terminal). */
enum StockWidgets : WidgetID {
	WID_ST_NEW_ORDER,        ///< Button that opens the New Order window.
	WID_ST_HISTORY,          ///< Toggle button: show the history of closed trades instead of the open trades.
	WID_ST_DEPOSIT,          ///< Button: move money from the company cash to the trading balance.
	WID_ST_WITHDRAW,         ///< Button: move money from the trading balance back to the company cash.
	WID_ST_TF_DAY,           ///< Toggle button: chart with daily candles; recording starts when it is first used.
	WID_ST_TF_MONTH,         ///< Toggle button: chart with monthly candles.
	WID_ST_TF_YEAR,          ///< Toggle button: chart with yearly candles.
	WID_ST_ACCOUNT,          ///< Balance, equity, margin and free margin.
	WID_ST_WATCH,            ///< Market Watch: symbols with Bid and Ask.
	WID_ST_TITLE,            ///< Selected symbol with its Bid, Ask and change.
	WID_ST_SUMMARY,          ///< Summary row of the selected symbol: P/E, EPS and dividend yield.
	WID_ST_CHART,            ///< The candlestick chart.
	WID_ST_TOOLBOX,          ///< Toolbox: open positions and pending orders.
	WID_ST_TOOL_SCROLLBAR,   ///< Scrollbar of the toolbox.
	WID_ST_CLOSE_ALL,        ///< Button: close all open positions of the selected wallet at once.
	WID_ST_WALLET,           ///< Panel: the selected wallet with its balance; click to switch to the next wallet.
	WID_ST_WALLET_NEW,       ///< Button: create a new trading wallet.
	WID_ST_WALLET_DELETE,    ///< Button: delete the selected trading wallet.
	WID_ST_PORTFOLIO,        ///< Toggle: the toolbox shows the portfolio summary (profit and loss, win rate, profit per month).
	WID_ST_RANKING,          ///< Toggle: the toolbox shows the ranking of the companies.
	WID_ST_BANK,             ///< Toggle: the toolbox shows the loan at the central bank.
	WID_ST_WATCH_SCROLLBAR,  ///< Scrollbar of the Market Watch.
};

/** Widgets of the #StockOrderWindow class. */
enum StockOrderWidgets : WidgetID {
	WID_SO_PREV,             ///< Select the previous symbol.
	WID_SO_SYMBOL,           ///< Name of the selected symbol.
	WID_SO_NEXT,             ///< Select the next symbol.
	WID_SO_PRICES,           ///< Current Bid and Ask.
	WID_SO_TYPE_MARKET,      ///< Order type: Market Execution. The type buttons must stay in this order.
	WID_SO_TYPE_BUY_LIMIT,   ///< Order type: Buy Limit.
	WID_SO_TYPE_SELL_LIMIT,  ///< Order type: Sell Limit.
	WID_SO_TYPE_BUY_STOP,    ///< Order type: Buy Stop.
	WID_SO_TYPE_SELL_STOP,   ///< Order type: Sell Stop.
	WID_SO_VOLUME,           ///< Edit box: volume in LOT.
	WID_SO_PRICE,            ///< Edit box: price of a pending order.
	WID_SO_SL,               ///< Edit box: Stop Loss.
	WID_SO_TP,               ///< Edit box: Take Profit.
	WID_SO_EXPIRY,           ///< Edit box: lifetime of a pending order in days.
	WID_SO_INFO,             ///< Value and margin of the order.
	WID_SO_SELL,             ///< Sell by market.
	WID_SO_BUY,              ///< Buy by market.
	WID_SO_PLACE,            ///< Place a pending order.
};

#endif /* WIDGETS_STOCK_WIDGET_H */


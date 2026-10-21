/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file stock_cmd.cpp Commands to open, modify and close trades in the trading terminal. */

#include "stdafx.h"
#include "stock_cmd.h"
#include "stock_base.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "window_func.h"

#include "table/strings.h"

#include "safeguards.h"

/**
 * Check that a Stop Loss and a Take Profit are on the correct side of a reference price.
 * @param buy Whether the position is long.
 * @param reference Entry price for a new order, or the price at which the position would be closed now.
 * @param sl Stop Loss, 0 for none.
 * @param tp Take Profit, 0 for none.
 * @return True when valid.
 */
static bool AreStopsValid(bool buy, int64_t reference, int64_t sl, int64_t tp)
{
	if (sl < 0 || tp < 0) return false;
	if (buy) return !((sl != 0 && sl >= reference) || (tp != 0 && tp <= reference));
	return !((sl != 0 && sl <= reference) || (tp != 0 && tp >= reference));
}

/**
 * Open a trade: either a market execution (Buy / Sell at the current price) or a pending order.
 * A market order is executed at once; its commission is charged now and it needs free margin.
 * A pending order is only stored. Its commission is charged when it is executed, and it can get a lifetime.
 * Profit and loss are settled when the position is closed.
 * @param flags Type of operation.
 * @param target Company whose shares (symbol) are traded. Must not be the company giving the command.
 * @param type Order type.
 * @param volume Volume in hundredths of a LOT (100 = 1.00 LOT).
 * @param price Trigger price; only used for pending orders.
 * @param sl Stop Loss, 0 for none.
 * @param tp Take Profit, 0 for none.
 * @param expiry_days Pending orders only: number of economy days after which the order expires; 0 for an order that never expires.
 * @param wallet Trading wallet that holds the margin, pays the commission and the swap and gets the results of the trade.
 * @return Zero cost (the commission is taken from the wallet), or an error.
 */
CommandCost CmdStockOpen(DoCommandFlags flags, CompanyID target, StockOrderType type, uint32_t volume, Money price_in, Money sl_in, Money tp_in, uint32_t expiry_days, uint8_t wallet)
{
	const int64_t price = price_in, sl = sl_in, tp = tp_in;
	const Company *owner = Company::GetIfValid(_current_company);
	if (owner == nullptr) return CMD_ERROR;
	if (!Company::IsValidID(target)) return CMD_ERROR;
	if (target == _current_company) return CommandCost(STR_STOCK_ERROR_OWN_COMPANY);
	if (type >= StockOrderType::End) return CMD_ERROR;
	if (volume == 0 || volume > STOCK_MAX_VOLUME) return CommandCost(STR_STOCK_ERROR_BAD_VOLUME);
	if (_stock_market.CountOrders(_current_company) >= STOCK_MAX_ORDERS_PER_COMPANY) return CommandCost(STR_STOCK_ERROR_TOO_MANY);
	if (!IsTradingWallet(_current_company, wallet)) return CommandCost(STR_WALLET_ERROR_BAD_WALLET);

	const int64_t bid = GetStockBid(target);
	const int64_t ask = GetStockAsk(target);
	const bool market = type == StockOrderType::Buy || type == StockOrderType::Sell;
	const bool buy = IsStockBuy(type);

	int64_t entry = 0;
	if (market) {
		/* The market price, moved by the market impact of this order. */
		entry = GetStockExecutionPrice(target, buy, volume);
		expiry_days = 0;
	} else {
		entry = price;
		if (entry <= 0) return CommandCost(STR_STOCK_ERROR_BAD_PRICE);
		/* A pending order must lie on the correct side of the current price. */
		switch (type) {
			case StockOrderType::BuyLimit:  if (entry >= ask) return CommandCost(STR_STOCK_ERROR_BAD_PRICE); break;
			case StockOrderType::BuyStop:   if (entry <= ask) return CommandCost(STR_STOCK_ERROR_BAD_PRICE); break;
			case StockOrderType::SellLimit: if (entry <= bid) return CommandCost(STR_STOCK_ERROR_BAD_PRICE); break;
			case StockOrderType::SellStop:  if (entry >= bid) return CommandCost(STR_STOCK_ERROR_BAD_PRICE); break;
			default: break;
		}
		if (expiry_days > STOCK_MAX_EXPIRY_DAYS) return CommandCost(STR_STOCK_ERROR_BAD_EXPIRY);
	}

	/* Stop Loss and Take Profit must be on the correct side of the entry price. */
	if (!AreStopsValid(buy, entry, sl, tp)) return CommandCost(STR_STOCK_ERROR_BAD_STOPS);

	int64_t commission = 0;
	if (market) {
		StockOrder probe;
		probe.price = entry;
		probe.volume = volume;
		commission = GetStockCommission(entry * static_cast<int64_t>(volume));
		/* The wallet must be able to hold the margin of the position and to pay the commission. */
		if (GetStockUsedMargin(_current_company, wallet) + GetStockMargin(probe) + commission > GetStockEquity(_current_company, wallet)) {
			return CommandCost(STR_STOCK_ERROR_NO_MARGIN);
		}
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		StockOrder order;
		order.id = _stock_market.next_id++;
		order.owner = _current_company.base();
		order.target = target.base();
		order.type = type;
		order.volume = volume;
		order.price = entry;
		order.sl = sl;
		order.tp = tp;
		order.expiry = expiry_days;
		order.wallet = wallet;
		order.open_ym = market ? GetStockCurrentMonth() : 0;
		(market ? _stock_market.positions : _stock_market.pending).push_back(order);

		if (market) {
			PayStockProfit(_current_company, wallet, -commission);
			_stock_market.ApplyImpact(target, buy, volume);
			/* The new price may already reach the levels of other trades. */
			_stock_market.CheckExecutions();
		}
		InvalidateWindowData(WC_STOCK_MARKET, 0);
	}

	/* The commission is taken from the wallet above, not from the money of the company, so the command itself costs nothing. */
	return CommandCost();
}

/**
 * Close an open position, completely or partially, or cancel a pending order.
 * The profit or loss of the closed volume is added to the company money and written to the history; a partial
 * close keeps the rest of the position open with the same Stop Loss and Take Profit.
 * @param flags Type of operation.
 * @param id Number of the position or order.
 * @param close_volume Volume to close, in hundredths of a LOT. At least the volume of the position closes it completely.
 *                     Ignored for a pending order, which is always cancelled completely.
 * @return Zero cost, or an error.
 */
CommandCost CmdStockClose(DoCommandFlags flags, uint32_t id, uint32_t close_volume)
{
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;

	StockOrder *position = _stock_market.FindPosition(id, _current_company);
	if (position != nullptr) {
		if (close_volume == 0) return CommandCost(STR_STOCK_ERROR_BAD_VOLUME);
		if (!Company::IsValidID(CompanyID{position->target})) return CMD_ERROR;

		if (flags.Test(DoCommandFlag::Execute)) {
			const uint32_t volume = std::min(close_volume, position->volume);
			/* Closing a long position sells shares, closing a short position buys them. */
			const int64_t price = GetStockExecutionPrice(CompanyID{position->target}, !IsStockBuy(position->type), volume);
			_stock_market.ClosePosition(position - _stock_market.positions.data(), volume, price, StockCloseReason::Manual);
			_stock_market.CheckExecutions();
			InvalidateWindowData(WC_STOCK_MARKET, 0);
		}
		return CommandCost(EXPENSES_OTHER);
	}

	StockOrder *order = _stock_market.FindPending(id, _current_company);
	if (order != nullptr) {
		if (flags.Test(DoCommandFlag::Execute)) {
			_stock_market.pending.erase(_stock_market.pending.begin() + (order - _stock_market.pending.data()));
			InvalidateWindowData(WC_STOCK_MARKET, 0);
		}
		return CommandCost(EXPENSES_OTHER);
	}

	return CommandCost(STR_STOCK_ERROR_NO_ORDER);
}

/**
 * Change an open position or a pending order.
 * For an open position the Stop Loss and Take Profit can be changed; they must be on the correct side of the price
 * at which the position would be closed now, otherwise it would close at once. The price is ignored.
 * For a pending order the trigger price, the Stop Loss and the Take Profit can be changed; the price must lie on
 * the correct side of the current price for the type of the order, and the stops on the correct side of the new price.
 * @param flags Type of operation.
 * @param id Number of the position or pending order.
 * @param sl New Stop Loss, 0 for none.
 * @param tp New Take Profit, 0 for none.
 * @param price New trigger price of a pending order; ignored for an open position.
 * @param trail New trailing stop distance of an open position, in price units; 0 switches the trailing stop off.
 *              Must be 0 for a pending order. While it is set, the Stop Loss follows the price (see StockMarket::ApplyTrailingStops).
 * @return Zero cost, or an error.
 */
CommandCost CmdStockModify(DoCommandFlags flags, uint32_t id, Money sl_in, Money tp_in, Money price_in, Money trail_in)
{
	const int64_t sl = sl_in, tp = tp_in, price = price_in, trail = trail_in;
	if (trail < 0) return CMD_ERROR;
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;

	StockOrder *position = _stock_market.FindPosition(id, _current_company);
	if (position != nullptr) {
		const CompanyID target{position->target};
		if (!Company::IsValidID(target)) return CMD_ERROR;

		const bool buy = IsStockBuy(position->type);
		const int64_t reference = buy ? GetStockBid(target) : GetStockAsk(target);
		if (!AreStopsValid(buy, reference, sl, tp)) return CommandCost(STR_STOCK_ERROR_BAD_STOPS);

		if (flags.Test(DoCommandFlag::Execute)) {
			position->sl = sl;
			position->tp = tp;
			position->trail = trail;
			InvalidateWindowData(WC_STOCK_MARKET, 0);
		}
		return CommandCost(EXPENSES_OTHER);
	}

	StockOrder *order = _stock_market.FindPending(id, _current_company);
	if (order == nullptr) return CommandCost(STR_STOCK_ERROR_NO_ORDER);

	const CompanyID target{order->target};
	if (!Company::IsValidID(target)) return CMD_ERROR;
	if (price <= 0) return CommandCost(STR_STOCK_ERROR_BAD_PRICE);
	if (trail != 0) return CommandCost(STR_STOCK_ERROR_BAD_STOPS); // Only an open position can trail.

	/* The same rules as when the order was placed. */
	const int64_t bid = GetStockBid(target);
	const int64_t ask = GetStockAsk(target);
	switch (order->type) {
		case StockOrderType::BuyLimit:  if (price >= ask) return CommandCost(STR_STOCK_ERROR_BAD_PRICE); break;
		case StockOrderType::BuyStop:   if (price <= ask) return CommandCost(STR_STOCK_ERROR_BAD_PRICE); break;
		case StockOrderType::SellLimit: if (price <= bid) return CommandCost(STR_STOCK_ERROR_BAD_PRICE); break;
		case StockOrderType::SellStop:  if (price >= bid) return CommandCost(STR_STOCK_ERROR_BAD_PRICE); break;
		default: return CMD_ERROR;
	}
	if (!AreStopsValid(IsStockBuy(order->type), price, sl, tp)) return CommandCost(STR_STOCK_ERROR_BAD_STOPS);

	if (flags.Test(DoCommandFlag::Execute)) {
		order->price = price;
		order->sl = sl;
		order->tp = tp;
		InvalidateWindowData(WC_STOCK_MARKET, 0);
	}

	return CommandCost(EXPENSES_OTHER);
}

/**
 * Move money between the company cash and the first trading wallet. This is the command of the old terminal, kept so
 * that scripts and savegames that use it keep working; the terminal itself uses CmdWalletTransfer.
 * @param flags Type of operation.
 * @param deposit True to deposit (cash to wallet), false to withdraw.
 * @param amount Amount of money, more than zero.
 * @return Zero cost, or an error.
 */
CommandCost CmdStockTransfer(DoCommandFlags flags, bool deposit, Money amount)
{
	const Wallet *wallet = _wallets.FindFirst(_current_company, WalletKind::Trading);
	if (wallet == nullptr) return CommandCost(STR_WALLET_ERROR_BAD_WALLET);
	return deposit ? CmdWalletTransfer(flags, WALLET_CASH, wallet->id, amount) : CmdWalletTransfer(flags, wallet->id, WALLET_CASH, amount);
}

/**
 * Start recording daily candles. Nothing is recorded before the first player opens the Daily view, so the
 * game does not keep data nobody looks at. Once turned on it stays on; there is no history from before.
 * The command needs at least one argument: the command system looks at the first argument of every command
 * and a command without arguments does not compile (internal compiler error with MSVC).
 * @param flags Type of operation.
 * @param enable Must be true; false does nothing, because recording is never turned off again.
 * @return Zero cost, or an error.
 */
CommandCost CmdStockEnableDaily(DoCommandFlags flags, bool enable)
{
	if (!Company::IsValidID(_current_company)) return CMD_ERROR;
	if (flags.Test(DoCommandFlag::Execute) && enable && !_stock_market.record_daily) {
		_stock_market.record_daily = true;
		InvalidateWindowData(WC_STOCK_MARKET, 0);
	}
	return CommandCost();
}

